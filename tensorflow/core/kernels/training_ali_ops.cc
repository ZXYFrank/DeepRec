/* Copyright 2016 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#define EIGEN_USE_THREADS
#if GOOGLE_CUDA
#define EIGEN_USE_GPU
#endif  // GOOGLE_CUDA
#include "tensorflow/core/lib/bfloat16/bfloat16.h"

#include <algorithm>

#include "tensorflow/core/framework/bounds_check.h"
#include "tensorflow/core/framework/embedding/intra_thread_copy_id_allocator.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/kernels/kv_variable_ops.h"
#include "tensorflow/core/kernels/training_op_helpers.h"
#include "tensorflow/core/kernels/training_ali_op_helpers.h"
#include "tensorflow/core/kernels/training_ali_ops.h"
#include "tensorflow/core/kernels/variable_ops.h"
#include "tensorflow/core/util/work_sharder.h"

#ifdef TENSORFLOW_USE_SYCL
#include "tensorflow/core/common_runtime/sycl/sycl_util.h"
#endif  // TENSORFLOW_USE_SYCL

#if GOOGLE_CUDA
#include "tensorflow/core/kernels/gpu_device_array.h"
#include "tensorflow/core/kernels/training_ali_ops_gpu.h"
#include "tensorflow/core/platform/stream_executor.h"
#endif  // GOOGLE_CUDA

namespace tensorflow {

using CPUDevice = Eigen::ThreadPoolDevice;
using GPUDevice = Eigen::GpuDevice;
using SYCLDevice = Eigen::SyclDevice;

namespace functor {
template <typename T>
struct ApplyAdagradDecay<CPUDevice, T> {
  void operator()(const CPUDevice& d, typename TTypes<T>::Flat var,
                  typename TTypes<T>::Flat accum,
                  typename TTypes<T>::ConstScalar lr,
                  typename TTypes<T>::ConstFlat grad,
                  bool need_decay,
                  typename TTypes<T>::ConstScalar decay_rate,
                  typename TTypes<T>::ConstScalar decay_baseline) {
    if (need_decay) {
      accum.device(d) = (accum * decay_rate()).cwiseMax(decay_baseline());
    }
    accum.device(d) += grad.square();
    var.device(d) -= grad * lr() * accum.rsqrt();
  }
};

}

template <typename TKey, typename T, typename Tstep,
          bool indices_as_pointer, bool has_counts>
class KvSparseApplyAdagradOp : public OpKernel {
 public:
  explicit KvSparseApplyAdagradOp(OpKernelConstruction* ctx) : OpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("use_locking", &use_exclusive_lock_));
  }

  void Compute(OpKernelContext* ctx) override NO_THREAD_SAFETY_ANALYSIS {
    auto locks =
        MaybeLockEmbeddingVariableInputMutexesInOrder<TKey, T>(ctx, use_exclusive_lock_, {0, 1});

    EmbeddingVar<TKey, T>* var = NULL;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 0, &var));
    core::ScopedUnref unref_var(var);
    EmbeddingVar<TKey, T>* accum = NULL;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 1, &accum));
    core::ScopedUnref unref_accum(accum);

    const Tensor& lr = ctx->input(2);
    OP_REQUIRES(ctx, IsLegacyScalar(lr.shape()),
                errors::InvalidArgument("lr is not a scalar: ",
                                        lr.shape().DebugString()));
    const Tensor& grad = ctx->input(3);
    const Tensor& indices = ctx->input(4);
    OP_REQUIRES(ctx, TensorShapeUtils::IsVector(indices.shape()),
                errors::InvalidArgument("indices must be one-dimensional"));

    const Tensor& global_step = ctx->input(5);
    OP_REQUIRES(
      ctx, IsLegacyScalar(global_step.shape()),
      errors::InvalidArgument(
        "global_step is not a scalar: ", global_step.shape().DebugString()));

    int64 inner_dim = 1;
    TensorShape var_shape({var->ValueLen()});
    for (int d = 0; d < var_shape.dims(); d++) {
      OP_REQUIRES(ctx, var_shape.dim_size(d) == grad.dim_size(d + 1),
                  errors::InvalidArgument(strings::StrCat(
                      "var and grad must match in dimension ", d + 1)));
      inner_dim *= grad.dim_size(d + 1);
    }

    const int64 N = indices.dim_size(0);
    OP_REQUIRES(
        ctx, grad.dim_size(0) == N,
        errors::InvalidArgument(
            "grad must be the same size as indices in the first dimension."));

    OP_REQUIRES(ctx, inner_dim > 0,
                errors::InvalidArgument(
                    "Inner dimension should be greater than zero."));
    int64* indices_counts = nullptr;
    std::function<int64(int64*, int64)> get_count_fn = 0;
    if (has_counts) {
      const Tensor& counts_tensor = ctx->input(6);
      indices_counts = (int64*)counts_tensor.data();
      get_count_fn = [](int64* counts, int64 index) {return counts[index];};
    } else {
      get_count_fn = [](int64* counts, int64 index) {return 1;};
    }

    if (N > 0) {
      if (inner_dim > 0) {
        auto indices_vec = indices.vec<TKey>();
        auto grad_flat = grad.flat_outer_dims<T>();
        T lr_scalar = lr.scalar<T>()();
        Tstep gs = global_step.scalar<Tstep>()();
        auto do_work = [this, ctx, &indices_vec, var, accum, &grad_flat,
            &gs, &lr_scalar, indices_counts, get_count_fn]
            (int64 start_i, int64 limit_i) {
          for (int64 i = start_i; i < limit_i; i++) {
            const TKey index = indices_vec(i);
            ValuePtr<T>* value_ptr = nullptr;
            bool is_filter = false;
            int64 count = get_count_fn(indices_counts, i);
            OP_REQUIRES_OK(ctx, var->LookupOrCreateKey(index, &value_ptr,
                           &is_filter, indices_as_pointer, count));
            var->UpdateVersion(value_ptr, gs);
            if (is_filter) {
              auto a = accum->flat(value_ptr, index);
              auto g = grad_flat.template chip<0>(i);
              auto v = var->flat(value_ptr, index);
              a += g.square();
              v -= g.constant(lr_scalar) * g * a.rsqrt();
            }
          }
        };
        const int64 cost = 1000; //very unreliable estimate for cost per step.
        auto worker_threads = *(ctx->device()->tensorflow_cpu_worker_threads());
        Shard(worker_threads.num_threads, worker_threads.workers, N, cost, do_work);

        if (has_counts && !indices_as_pointer) {
          const Tensor& indices_counts = ctx->input(6);
          var->UpdateCache(indices, indices_counts);
        }
      }
    }
  }

 private:
  bool use_exclusive_lock_;
};

#define REGISTER_KERNELS(Tindices, T, Tstep)                         \
  REGISTER_KERNEL_BUILDER(Name("KvResourceSparseApplyAdagrad")       \
                              .Device(DEVICE_CPU)                    \
                              .TypeConstraint<T>("T")                \
                              .TypeConstraint<Tindices>("Tindices")  \
                              .TypeConstraint<Tstep>("Tstep"),       \
                          KvSparseApplyAdagradOp<Tindices, T, Tstep, false, false>); \
  REGISTER_KERNEL_BUILDER(Name("_OPT_KvResourceSparseApplyAdagrad")  \
                              .Device(DEVICE_CPU)                    \
                              .TypeConstraint<T>("T")                \
                              .TypeConstraint<Tindices>("Tindices")  \
                              .TypeConstraint<Tstep>("Tstep"),       \
                          KvSparseApplyAdagradOp<Tindices, T, Tstep, true, false>);\
  REGISTER_KERNEL_BUILDER(Name("KvResourceSparseApplyAdagradWithCounts")       \
                              .Device(DEVICE_CPU)                    \
                              .TypeConstraint<T>("T")                \
                              .TypeConstraint<Tindices>("Tindices")  \
                              .TypeConstraint<Tstep>("Tstep"),       \
                          KvSparseApplyAdagradOp<Tindices, T, Tstep, false, true>); \
  REGISTER_KERNEL_BUILDER(Name("_OPT_KvResourceSparseApplyAdagradWithCounts")  \
                              .Device(DEVICE_CPU)                    \
                              .TypeConstraint<T>("T")                \
                              .TypeConstraint<Tindices>("Tindices")  \
                              .TypeConstraint<Tstep>("Tstep"),       \
                          KvSparseApplyAdagradOp<Tindices, T, Tstep, true, true>);
#define REGISTER_CPU_KERNELS(T)        \
  REGISTER_KERNELS(int32, T, int32);   \
  REGISTER_KERNELS(int64, T, int32);   \
  REGISTER_KERNELS(int32, T, int64);   \
  REGISTER_KERNELS(int64, T, int64);

TF_CALL_float(REGISTER_CPU_KERNELS);

#undef REGISTER_CPU_KERNELS
#undef REGISTER_KERNELS

#if GOOGLE_CUDA
template <typename Device, typename TKey, typename T,
          typename Tstep, bool indices_as_pointer, bool has_counts>
class KvSparseApplyAdagradGPUOp : public OpKernel {
 public:
  explicit KvSparseApplyAdagradGPUOp(OpKernelConstruction* ctx) : OpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("use_locking", &use_exclusive_lock_));

    int num_worker_threads = ctx->device()
                             ->tensorflow_cpu_worker_threads()
                             ->num_threads;
    thread_copy_id_alloc_.reset(
        new IntraThreadCopyIdAllocator(num_worker_threads));
  }

  void ApplyGradients(
      EmbeddingVar<TKey, T>* var,
      EmbeddingVar<TKey, T>* accum, T** v, T**a,
      T lr_scalar, const T* grad_base,
      const int64 task_size,
      se::Stream* stream,
      EventMgr* event_mgr,
      const Eigen::GpuDevice& gpu_device) {
    // Send pointers of embeddings to GPU
    T** dev_v = (T**)var->GetBuffer(task_size * 2);
    T** dev_a = dev_v + task_size;
    CHECK(dev_a);
    CHECK(dev_v);
    DeviceMemoryBase dev_v_ptr(dev_v, sizeof(T*) * task_size * 2);
    stream->ThenMemcpy(&dev_v_ptr, v, sizeof(T*) * task_size * 2);

    int block_size = 128;
    int embedding_dim = var->ValueLen();
    functor::KvSparseApplyAdagradHbm<GPUDevice, TKey, T>()(
            block_size, embedding_dim,
            dev_a, dev_v, grad_base,
            lr_scalar, task_size, gpu_device);
    SyncWithEventMgr(stream, event_mgr);
  }

  void Compute(OpKernelContext* ctx) override NO_THREAD_SAFETY_ANALYSIS {
    auto locks =
        MaybeLockEmbeddingVariableInputMutexesInOrder<TKey, T>(ctx, use_exclusive_lock_, {0, 1});

    EmbeddingVar<TKey, T>* var = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 0, &var));
    core::ScopedUnref unref_var(var);
    EmbeddingVar<TKey, T>* accum = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 1, &accum));
    core::ScopedUnref unref_accum(accum);

    const Tensor& lr = ctx->input(2);
    OP_REQUIRES(ctx, IsLegacyScalar(lr.shape()),
                errors::InvalidArgument("lr is not a scalar: ",
                                        lr.shape().DebugString()));
    const Tensor& grad = ctx->input(3);
    const Tensor& indices = ctx->input(4);
    OP_REQUIRES(ctx, TensorShapeUtils::IsVector(indices.shape()),
                errors::InvalidArgument("indices must be one-dimensional"));

    const Tensor& global_step = ctx->input(5);
    OP_REQUIRES(
      ctx, IsLegacyScalar(global_step.shape()),
      errors::InvalidArgument(
        "global_step is not a scalar: ", global_step.shape().DebugString()));

    int64 inner_dim = 1;
    TensorShape var_shape({var->ValueLen()});
    for (int d = 0; d < var_shape.dims(); d++) {
      OP_REQUIRES(ctx, var_shape.dim_size(d) == grad.dim_size(d + 1),
                  errors::InvalidArgument(strings::StrCat(
                      "var and grad must match in dimension ", d + 1)));
      inner_dim *= grad.dim_size(d + 1);
    }

    const int64 N = indices.dim_size(0);
    OP_REQUIRES(
        ctx, grad.dim_size(0) == N,
        errors::InvalidArgument(
            "grad must be the same size as indices in the first dimension."));

    OP_REQUIRES(ctx, inner_dim > 0,
                errors::InvalidArgument(
                    "Inner dimension should be greater than zero."));

    if (N > 0) {
      if (inner_dim > 0) {
        auto indices_flat = indices.flat<TKey>();
        auto grad_flat = grad.flat_outer_dims<T>();
        Tstep gs = global_step.scalar<Tstep>()();
        T lr_scalar = lr.scalar<T>()();
        if (var->IsSingleHbm()) {
          const TKey* key_base = &indices_flat(0);
          const T* grad_base = &grad_flat(0);
          const Device& device = ctx->eigen_device<Device>();

          functor::KvSparseApplyAdagrad<Device, TKey, T>()(
              N, ctx->get_allocator(AllocatorAttributes()), var, accum,
              key_base, grad_base, lr_scalar, gs, device);
        } else {
          Tensor indices_temp_host(indices.dtype(), indices.shape());
          const Tensor* indices_host_ptr = nullptr;
          //Copy ids from GPU to CPU for CPU Lookup.
          auto stream = ctx->op_device_context()->stream();
          auto event_mgr = ctx->device()->tensorflow_gpu_device_info()->event_mgr;
          if (!indices_as_pointer) {
            indices_host_ptr = &indices_temp_host;
            se::DeviceMemoryBase gpu_src(
                const_cast<TKey*>(&indices_flat(0)), N * sizeof(TKey));
            stream->ThenMemcpy(indices_host_ptr->data(), gpu_src, N * sizeof(TKey));
            SyncWithEventMgr(stream, event_mgr);
          } else {
            indices_host_ptr = &indices;
          }

          int counts_index = has_counts ? 6 : -1;
          T** v = new T*[N * 2];
          T** a = v + N;
          std::vector<std::pair<EmbeddingVar<TKey, T>*, T**>> vars(2);
          vars[0] = std::pair<EmbeddingVar<TKey, T>*, T**>(var, v);
          vars[1] = std::pair<EmbeddingVar<TKey, T>*, T**>(accum, a);
          GetEmbeddingPointers(ctx, vars, (TKey*)indices_host_ptr->data(),
                               gs, indices_as_pointer,
                               counts_index, N, thread_copy_id_alloc_.get());

          ApplyGradients(
              var, accum, v, a,
              lr_scalar,
              &grad_flat(0), N,
              stream, event_mgr,
              ctx->eigen_device<GPUDevice>());

          if (has_counts && !indices_as_pointer) {
            const Tensor& counts_tensor = ctx->input(counts_index);
              var->UpdateCache(*indices_host_ptr, counts_tensor);
          }

          delete[] v;
        }
      }
    }
  }

 private:
  bool use_exclusive_lock_;
  std::unique_ptr<IntraThreadCopyIdAllocator> thread_copy_id_alloc_;
};

namespace functor {
#define DECLARE_GPU_SPEC(TKey, T)                             \
  template <>                                                 \
  void KvSparseApplyAdagrad<GPUDevice, TKey, T>::operator()(  \
      int32 num_items,                                        \
      Allocator* alloc,                                       \
      EmbeddingVar<TKey, T>* var,                             \
      EmbeddingVar<TKey, T>* accum,                           \
      const TKey* key_base,                                   \
      const T* grad,                                          \
      T lr,                                                   \
      int64 gs,                                               \
      const GPUDevice& device);                               \
  extern template struct KvSparseApplyAdagrad<GPUDevice, TKey, T>;
DECLARE_GPU_SPEC(int32, float);
DECLARE_GPU_SPEC(int32, double);
DECLARE_GPU_SPEC(int64, float);
DECLARE_GPU_SPEC(int64, double);
#undef DECLARE_GPU_SPEC
}  // namespace functor

#define REGISTER_KERNELS(Tindices, T, Tstep)                         \
  REGISTER_KERNEL_BUILDER(Name("KvResourceSparseApplyAdagrad")       \
                              .Device(DEVICE_GPU)                    \
                              .TypeConstraint<T>("T")                \
                              .HostMemory("lr")                      \
                              .HostMemory("global_step")             \
                              .TypeConstraint<Tindices>("Tindices")  \
                              .TypeConstraint<Tstep>("Tstep"),       \
                          KvSparseApplyAdagradGPUOp<GPUDevice, Tindices, T, Tstep, false, false>);\
  REGISTER_KERNEL_BUILDER(Name("_OPT_KvResourceSparseApplyAdagrad")  \
                              .Device(DEVICE_GPU)                    \
                              .TypeConstraint<T>("T")                \
                              .HostMemory("indices")                 \
                              .HostMemory("lr")                      \
                              .HostMemory("global_step")             \
                              .TypeConstraint<Tindices>("Tindices")  \
                              .TypeConstraint<Tstep>("Tstep"),       \
                          KvSparseApplyAdagradGPUOp<GPUDevice, Tindices, T, Tstep, true, false>);\
  REGISTER_KERNEL_BUILDER(Name("KvResourceSparseApplyAdagradWithCounts")       \
                              .Device(DEVICE_GPU)                    \
                              .TypeConstraint<T>("T")                \
                              .HostMemory("lr")                      \
                              .HostMemory("global_step")             \
                              .HostMemory("indices_counts")          \
                              .TypeConstraint<Tindices>("Tindices")  \
                              .TypeConstraint<Tstep>("Tstep"),       \
                          KvSparseApplyAdagradGPUOp<GPUDevice, Tindices, T, Tstep, false, true>);\
  REGISTER_KERNEL_BUILDER(Name("_OPT_KvResourceSparseApplyAdagradWithCounts")  \
                              .Device(DEVICE_GPU)                    \
                              .TypeConstraint<T>("T")                \
                              .HostMemory("indices")                 \
                              .HostMemory("lr")                      \
                              .HostMemory("global_step")             \
                              .HostMemory("indices_counts")          \
                              .TypeConstraint<Tindices>("Tindices")  \
                              .TypeConstraint<Tstep>("Tstep"),       \
                          KvSparseApplyAdagradGPUOp<GPUDevice, Tindices, T, Tstep, true, true>);
#define REGISTER_GPU_KERNELS(T)        \
  REGISTER_KERNELS(int32, T, int32);   \
  REGISTER_KERNELS(int64, T, int32);   \
  REGISTER_KERNELS(int32, T, int64);   \
  REGISTER_KERNELS(int64, T, int64);

TF_CALL_float(REGISTER_GPU_KERNELS);
#undef REGISTER_GPU_KERNELS
#undef REGISTER_KERNELS
#endif  // GOOGLE_CUDA

// Note, this op works on cpu only.
template <typename Device, typename TKey, typename T,
          bool has_l2_shrinkage, bool indices_as_pointer, bool has_counts>
class KvSparseApplyFtrlOp : public OpKernel {
 public:
  explicit KvSparseApplyFtrlOp(OpKernelConstruction* ctx) : OpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("use_locking", &use_exclusive_lock_));
  }

  void Compute(OpKernelContext* ctx) override NO_THREAD_SAFETY_ANALYSIS {
    auto locks =
        MaybeLockEmbeddingVariableInputMutexesInOrder<TKey, T>(ctx, use_exclusive_lock_, {0, 1, 2});

    EmbeddingVar<TKey, T>* var_ = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 0, &var_));
    core::ScopedUnref unref_var(var_);
    EmbeddingVar<TKey, T>* accum_ = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 1, &accum_));
    core::ScopedUnref unref_accum(accum_);
    EmbeddingVar<TKey, T>* linear_ = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 2, &linear_));
    core::ScopedUnref unref_linear(linear_);

    const Tensor& grad = ctx->input(3);
    const Tensor& indices = ctx->input(4);
    OP_REQUIRES(ctx, TensorShapeUtils::IsVector(indices.shape()),
                errors::InvalidArgument("indices must be one-dimensional"));

    const Tensor& lr = ctx->input(5);
    OP_REQUIRES(ctx,
                TensorShapeUtils::IsScalar(lr.shape()) &&
                    lr.scalar<T>()() > static_cast<T>(0),
                errors::InvalidArgument("lr is not a positive scalar: ",
                                        lr.shape().DebugString()));

    const Tensor& l1 = ctx->input(6);
    OP_REQUIRES(ctx,
                TensorShapeUtils::IsScalar(l1.shape()) &&
                    l1.scalar<T>()() >= static_cast<T>(0),
                errors::InvalidArgument("l1 regularization strength is not a "
                                        "non-negative scalar: ",
                                        l1.shape().DebugString()));
    const Tensor& l2 = ctx->input(7);
    OP_REQUIRES(ctx,
                TensorShapeUtils::IsScalar(l2.shape()) &&
                    l2.scalar<T>()() >= static_cast<T>(0),
                errors::InvalidArgument("l2 regularization strength is not a "
                                        "non-negative scalar: ",
                                        l2.shape().DebugString()));
    const int lr_power_index = has_l2_shrinkage ? 9 : 8;
    const Tensor& lr_power = ctx->input(lr_power_index);
    OP_REQUIRES(ctx,
                TensorShapeUtils::IsScalar(lr_power.shape()) &&
                    lr_power.scalar<T>()() <= static_cast<T>(0),
                errors::InvalidArgument("lr_power is not a "
                                        "non-positive scalar: ",
                                        lr_power.shape().DebugString()));
    int64 inner_dim = 1;
    TensorShape var_shape({var_->ValueLen()});
    for (int d = 0; d < var_shape.dims(); d++) {
      OP_REQUIRES(ctx, var_shape.dim_size(d) == grad.dim_size(d + 1),
                  errors::InvalidArgument(strings::StrCat(
                      "var and grad must match in dimension ", d + 1)));
      inner_dim *= grad.dim_size(d + 1);
    }
    const int64 N = indices.dim_size(0);
    OP_REQUIRES(
        ctx, grad.dim_size(0) == N,
        errors::InvalidArgument(
            "grad must be the same size as indices in the first dimension."));

    OP_REQUIRES(ctx, inner_dim > 0,
                errors::InvalidArgument(
                    "Inner dimension should be greater than zero."));

    const Tensor* l2_shrinkage;
    if (has_l2_shrinkage) {
      l2_shrinkage = &ctx->input(8);
      OP_REQUIRES(
          ctx,
          TensorShapeUtils::IsScalar(l2_shrinkage->shape()) &&
              l2_shrinkage->scalar<T>()() >= static_cast<T>(0),
          errors::InvalidArgument("l2 shrinkage regularization strength "
                                  "is not a non-negative scalar: ",
                                  l2_shrinkage->shape().DebugString()));
    }
    int64* indices_counts = nullptr;
    std::function<int64(int64*, int64)> get_count_fn = 0;
    if (has_counts) {
      const int counts_input_index = has_l2_shrinkage ? 10 : 9;
      const Tensor& counts_tensor = ctx->input(counts_input_index);
      indices_counts = (int64*)counts_tensor.data();
      get_count_fn = [](int64* counts, int64 index) {return counts[index];};
    } else {
      get_count_fn = [](int64* counts, int64 index) {return 1;};
    }

    if (N > 0) {
      if (inner_dim > 0) {
        auto indices_vec = indices.vec<TKey>();
        auto grad_flat = grad.flat_outer_dims<T>();
        T lr_scalar = lr.scalar<T>()();
        T l1_scalar = l1.scalar<T>()();
        T l2_scalar = l2.scalar<T>()();
        T l2_shrinkage_scalar = 0.0;
        if (has_l2_shrinkage) {
          l2_shrinkage_scalar = l2_shrinkage->scalar<T>()();
        }
        T lr_power_scalar = lr_power.scalar<T>()();
        auto do_work = [this, ctx, inner_dim, &var_,
                       &indices_vec, &accum_, &linear_, &grad_flat,
                       &lr_scalar, &l1_scalar, &l2_scalar, &lr_power,
                       &l2_shrinkage_scalar, &lr_power_scalar,
                       get_count_fn, indices_counts]
            (int64 start_i, int64 limit_i) {
          for (int64 i = start_i; i < limit_i; i++) {
            const TKey index = indices_vec(i);
            ValuePtr<T>* value_ptr = nullptr;
            bool is_filter = false;
            int64 count = get_count_fn(indices_counts, i);
            OP_REQUIRES_OK(ctx, var_->LookupOrCreateKey(index, &value_ptr,
                           &is_filter, indices_as_pointer, count));
            if (is_filter) {
              auto var = var_->flat(value_ptr, index);
              auto accum = accum_->flat(value_ptr, index);
              auto linear = linear_->flat(value_ptr, index);
              auto grad = grad_flat.template chip<0>(i);

// Use a macro to implement the computation here due to the templating of the
// eigen tensor library.
#define COMPUTE_FTRL(grad_to_use)                                              \
  auto new_accum = accum + grad_to_use.square();                               \
  if (lr_power_scalar == static_cast<T>(-0.5)) {                               \
    linear +=                                                                  \
        grad_to_use - (new_accum.sqrt() - accum.sqrt()) / lr_scalar * var;     \
  } else {                                                                     \
    linear += grad_to_use - (new_accum.pow(-lr_power_scalar) -                 \
                             accum.pow(-lr_power_scalar)) /                    \
                                lr_scalar * var;                               \
  }                                                                            \
  Eigen::Tensor<T, 0, Eigen::RowMajor, long int> linear_sqrsum =               \
            linear.square().sum().sqrt();                                      \
  T linear_norm = linear_sqrsum(0);                                            \
  if (linear_norm > l1_scalar) {                                               \
    if (lr_power_scalar == static_cast<T>(-0.5)) {                             \
       auto eta_rec = new_accum.sqrt() / lr_scalar;                            \
       auto coef = (l1_scalar - linear_norm)  /                                \
                     ((eta_rec + static_cast<T>(2) * l2_scalar) * linear_norm);\
       var = coef * linear;                                                    \
    } else {                                                                   \
      auto eta_rec = new_accum.pow(-lr_power_scalar) / lr_scalar;              \
      auto coef = (l1_scalar - linear_norm)  /                                 \
                    ((eta_rec + static_cast<T>(2) * l2_scalar) * linear_norm); \
      var = coef * linear;                                                     \
    }                                                                          \
  } else {                                                                     \
    var = var.constant(static_cast<T>(0));                                     \
  }                                                                            \
  accum += grad.square();
              if (has_l2_shrinkage) {
                auto grad_with_shrinkage =
                    grad + static_cast<T>(2) * l2_shrinkage_scalar * var;
                COMPUTE_FTRL(grad_with_shrinkage);
              } else {
                COMPUTE_FTRL(grad);
              }
            }
          }
#undef COMPUTE_FTRL
        };

        const int64 cost = 4500; //very unreliable estimate for cost per step.
        auto worker_threads = *(ctx->device()->tensorflow_cpu_worker_threads());
        Shard(worker_threads.num_threads, worker_threads.workers, N, cost, do_work);

        if (has_counts && !indices_as_pointer) {
          const int counts_input_index = has_l2_shrinkage ? 10 : 9;
          const Tensor& indices_counts = ctx->input(counts_input_index);
          var_->UpdateCache(indices, indices_counts);
        }
      }
    }

    MaybeForwardRefInputToRefOutput(ctx, 0, 0);
  }

 private:
  bool use_exclusive_lock_;
};

#define REGISTER_KERNELS(Tindices, T)                                         \
  REGISTER_KERNEL_BUILDER(                                                    \
      Name("KvResourceSparseApplyFtrl")                                       \
          .Device(DEVICE_CPU)                                                 \
          .TypeConstraint<T>("T")                                             \
          .TypeConstraint<Tindices>("Tindices"),                              \
      KvSparseApplyFtrlOp<CPUDevice, Tindices, T, /*has_l2_shrinkage=*/false, false, false>);\
  REGISTER_KERNEL_BUILDER(                                                    \
      Name("_OPT_KvResourceSparseApplyFtrl")                                  \
          .Device(DEVICE_CPU)                                                 \
          .TypeConstraint<T>("T")                                             \
          .TypeConstraint<Tindices>("Tindices"),                              \
      KvSparseApplyFtrlOp<CPUDevice, Tindices, T, /*has_l2_shrinkage=*/false, true, false>);\
  REGISTER_KERNEL_BUILDER(                                                    \
      Name("KvResourceSparseApplyFtrlWithCounts")                                       \
          .Device(DEVICE_CPU)                                                 \
          .TypeConstraint<T>("T")                                             \
          .TypeConstraint<Tindices>("Tindices"),                              \
      KvSparseApplyFtrlOp<CPUDevice, Tindices, T, /*has_l2_shrinkage=*/false, false, true>);\
  REGISTER_KERNEL_BUILDER(                                                    \
      Name("_OPT_KvResourceSparseApplyFtrlWithCounts")                                  \
          .Device(DEVICE_CPU)                                                 \
          .TypeConstraint<T>("T")                                             \
          .TypeConstraint<Tindices>("Tindices"),                              \
      KvSparseApplyFtrlOp<CPUDevice, Tindices, T, /*has_l2_shrinkage=*/false, true, true>);

#define REGISTER_CPU_KERNELS(T) \
  REGISTER_KERNELS(int64, T);   \
  REGISTER_KERNELS(int32, T);

TF_CALL_float(REGISTER_CPU_KERNELS);

#undef REGISTER_CPU_KERNELS
#undef REGISTER_KERNELS

#define REGISTER_KERNELS(Tindices, T)                                        \
  REGISTER_KERNEL_BUILDER(                                                   \
      Name("KvResourceSparseApplyFtrlV2")                                    \
          .Device(DEVICE_CPU)                                                \
          .TypeConstraint<T>("T")                                            \
          .TypeConstraint<Tindices>("Tindices"),                             \
      KvSparseApplyFtrlOp<CPUDevice, Tindices, T, /*has_l2_shrinkage=*/true, false, false>);\
  REGISTER_KERNEL_BUILDER(                                                   \
      Name("_OPT_KvResourceSparseApplyFtrlV2")                               \
          .Device(DEVICE_CPU)                                                \
          .TypeConstraint<T>("T")                                            \
          .TypeConstraint<Tindices>("Tindices"),                             \
      KvSparseApplyFtrlOp<CPUDevice, Tindices, T, /*has_l2_shrinkage=*/true, true, false>)\
  REGISTER_KERNEL_BUILDER(                                                   \
      Name("KvResourceSparseApplyFtrlV2WithCounts")                                    \
          .Device(DEVICE_CPU)                                                \
          .TypeConstraint<T>("T")                                            \
          .TypeConstraint<Tindices>("Tindices"),                             \
      KvSparseApplyFtrlOp<CPUDevice, Tindices, T, /*has_l2_shrinkage=*/true, false, true>);\
  REGISTER_KERNEL_BUILDER(                                                   \
      Name("_OPT_KvResourceSparseApplyFtrlV2WithCounts")                               \
          .Device(DEVICE_CPU)                                                \
          .TypeConstraint<T>("T")                                            \
          .TypeConstraint<Tindices>("Tindices"),                             \
      KvSparseApplyFtrlOp<CPUDevice, Tindices, T, /*has_l2_shrinkage=*/true, true, true>);

#define REGISTER_CPU_KERNELS(T) \
  REGISTER_KERNELS(int64, T);   \
  REGISTER_KERNELS(int32, T);

TF_CALL_float(REGISTER_CPU_KERNELS);

#undef REGISTER_CPU_KERNELS
#undef REGISTER_KERNELS

#if GOOGLE_CUDA
template <typename Device, typename TKey, typename T, bool has_l2_shrinkage, bool indices_as_pointer>
class KvSparseApplyFtrlOpGPU : public OpKernel {
 public:
  explicit KvSparseApplyFtrlOpGPU(OpKernelConstruction* ctx) : OpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("use_locking", &use_exclusive_lock_));
  }

  void Compute(OpKernelContext* ctx) override NO_THREAD_SAFETY_ANALYSIS {
    EmbeddingVar<TKey, T>* var_ = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 0, &var_));
    EmbeddingVar<TKey, T>* accum_ = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 1, &accum_));
    EmbeddingVar<TKey, T>* linear_ = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 2, &linear_));

    const Tensor& grad = ctx->input(3);
    const Tensor& indices = ctx->input(4);
    OP_REQUIRES(ctx, TensorShapeUtils::IsVector(indices.shape()),
                errors::InvalidArgument("indices must be one-dimensional"));

    const Tensor& lr = ctx->input(5);
    OP_REQUIRES(ctx,
                TensorShapeUtils::IsScalar(lr.shape()) &&
                    lr.scalar<T>()() > static_cast<T>(0),
                errors::InvalidArgument("lr is not a positive scalar: ",
                                        lr.shape().DebugString()));

    const Tensor& l1 = ctx->input(6);
    OP_REQUIRES(ctx,
                TensorShapeUtils::IsScalar(l1.shape()) &&
                    l1.scalar<T>()() >= static_cast<T>(0),
                errors::InvalidArgument("l1 regularization strength is not a "
                                        "non-negative scalar: ",
                                        l1.shape().DebugString()));
    const Tensor& l2 = ctx->input(7);
    OP_REQUIRES(ctx,
                TensorShapeUtils::IsScalar(l2.shape()) &&
                    l2.scalar<T>()() >= static_cast<T>(0),
                errors::InvalidArgument("l2 regularization strength is not a "
                                        "non-negative scalar: ",
                                        l2.shape().DebugString()));
    const int lr_power_index = has_l2_shrinkage ? 9 : 8;
    const Tensor& lr_power = ctx->input(lr_power_index);
    OP_REQUIRES(ctx,
                TensorShapeUtils::IsScalar(lr_power.shape()) &&
                    lr_power.scalar<T>()() <= static_cast<T>(0),
                errors::InvalidArgument("lr_power is not a "
                                        "non-positive scalar: ",
                                        lr_power.shape().DebugString()));
    int64 inner_dim = 1;
    TensorShape var_shape({var_->ValueLen()});
    for (int d = 0; d < var_shape.dims(); d++) {
      OP_REQUIRES(ctx, var_shape.dim_size(d) == grad.dim_size(d + 1),
                  errors::InvalidArgument(strings::StrCat(
                      "var and grad must match in dimension ", d + 1)));
      inner_dim *= grad.dim_size(d + 1);
    }
    const int64 N = indices.dim_size(0);
    OP_REQUIRES(
        ctx, grad.dim_size(0) == N,
        errors::InvalidArgument(
            "grad must be the same size as indices in the first dimension."));

    OP_REQUIRES(ctx, inner_dim > 0,
                errors::InvalidArgument(
                    "Inner dimension should be greater than zero."));

    const Tensor* l2_shrinkage;
    if (has_l2_shrinkage) {
      l2_shrinkage = &ctx->input(8);
      OP_REQUIRES(
          ctx,
          TensorShapeUtils::IsScalar(l2_shrinkage->shape()) &&
              l2_shrinkage->scalar<T>()() >= static_cast<T>(0),
          errors::InvalidArgument("l2 shrinkage regularization strength "
                                  "is not a non-negative scalar: ",
                                  l2_shrinkage->shape().DebugString()));
    }

    if (N > 0) {
      if (inner_dim > 0) {
        auto indices_flat = indices.flat<TKey>();
        auto grad_flat = grad.flat<T>();
        T lr_scalar = lr.scalar<T>()();
        T l1_scalar = l1.scalar<T>()();
        T l2_scalar = l2.scalar<T>()();
        T l2_shrinkage_scalar = 0.0;
        if (has_l2_shrinkage) {
          l2_shrinkage_scalar = l2_shrinkage->scalar<T>()();
        }
        T lr_power_scalar = lr_power.scalar<T>()();
        const TKey* key_base = &indices_flat(0);
        const T* grad_base = &grad_flat(0);
        const Device& device = ctx->eigen_device<Device>();

        functor::KvSparseApplyFtrl<Device, TKey, T>()(
            N, ctx->get_allocator(AllocatorAttributes()), var_, accum_, linear_,
            key_base, grad_base, lr_scalar, l1_scalar, l2_scalar, lr_power_scalar,
            has_l2_shrinkage, l2_shrinkage_scalar, device);
      }
    }

    MaybeForwardRefInputToRefOutput(ctx, 0, 0);
  }

 private:
  bool use_exclusive_lock_;
};

namespace functor {
#define DECLARE_GPU_SPEC(TKey, T)                                          \
  template <>                                                              \
  void KvSparseApplyFtrl<GPUDevice, TKey, T>::operator()(                  \
      int32 num_items,                                                     \
      Allocator* alloc,                                                    \
      EmbeddingVar<TKey, T>* var,                                          \
      EmbeddingVar<TKey, T>* accum,                                        \
      EmbeddingVar<TKey, T>* linear,                                       \
      const TKey* key_base,                                                \
      const T* grad,                                                       \
      T lr,                                                                \
      T l1,                                                                \
      T l2,                                                                \
      T lr_power,                                                          \
      bool has_l2_shrinkage,                                               \
      T l2_shrinkage,                                                      \
      const GPUDevice& device);                                            \
  extern template struct KvSparseApplyFtrl<GPUDevice, TKey, T>;
DECLARE_GPU_SPEC(int32, float);
DECLARE_GPU_SPEC(int32, double);
DECLARE_GPU_SPEC(int64, float);
DECLARE_GPU_SPEC(int64, double);
#undef DECLARE_GPU_SPEC
}  // namespace functor

#define REGISTER_KERNELS(Tindices, T)                                      \
  REGISTER_KERNEL_BUILDER(                                                 \
      Name("KvResourceSparseApplyFtrl")                                    \
          .Device(DEVICE_GPU)                                              \
          .TypeConstraint<T>("T")                                          \
          .HostMemory("lr")                                                \
          .HostMemory("l1")                                                \
          .HostMemory("l2")                                                \
          .HostMemory("lr_power")                                          \
          .TypeConstraint<Tindices>("Tindices"),                           \
      KvSparseApplyFtrlOpGPU<GPUDevice, Tindices, T, false, false>);
#define REGISTER_GPU_KERNELS(T)                                            \
  REGISTER_KERNELS(int64, T);                                              \
  REGISTER_KERNELS(int32, T);
TF_CALL_float(REGISTER_GPU_KERNELS);
TF_CALL_double(REGISTER_GPU_KERNELS);
#undef REGISTER_GPU_KERNELS
#undef REGISTER_KERNELS

#define REGISTER_KERNELS(Tindices, T)                                      \
  REGISTER_KERNEL_BUILDER(                                                 \
      Name("KvResourceSparseApplyFtrlV2")                                  \
          .Device(DEVICE_GPU)                                              \
          .TypeConstraint<T>("T")                                          \
          .HostMemory("lr")                                                \
          .HostMemory("l1")                                                \
          .HostMemory("l2")                                                \
          .HostMemory("lr_power")                                          \
          .HostMemory("l2_shrinkage")                                      \
          .TypeConstraint<Tindices>("Tindices"),                           \
      KvSparseApplyFtrlOpGPU<GPUDevice, Tindices, T, true, false>);
#define REGISTER_GPU_KERNELS(T)                                            \
  REGISTER_KERNELS(int64, T);                                              \
  REGISTER_KERNELS(int32, T);
TF_CALL_float(REGISTER_GPU_KERNELS);
TF_CALL_double(REGISTER_GPU_KERNELS);
#undef REGISTER_GPU_KERNELS
#undef REGISTER_KERNELS
#endif  // GOOGLE_CUDA

// Note, this op works on cpu only.
template <typename Device, typename T, typename Tstep>
class ApplyAdagradDecayOp : public OpKernel {
 public:
  explicit ApplyAdagradDecayOp(OpKernelConstruction* ctx) : OpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("use_locking", &use_exclusive_lock_));
  }

  void Compute(OpKernelContext* ctx) override {
    const bool sparse = false;
    auto locks = MaybeLockVariableInputMutexesInOrder<Device, T>(
      ctx, use_exclusive_lock_, sparse, {0, 1});

    Tensor var;
    OP_REQUIRES_OK(ctx, GetInputTensorFromVariable<Device, T>(
      ctx, 0, use_exclusive_lock_, false, &var));

    OP_REQUIRES(
      ctx, var.IsInitialized(),
      errors::FailedPrecondition(
        "Attempting to use uninitialized variables: ", requested_input(0)));

    Tensor accum;
    OP_REQUIRES_OK(ctx, GetInputTensorFromVariable<Device, T>(
        ctx, 1, use_exclusive_lock_, false, &accum));
    OP_REQUIRES(
      ctx, accum.IsInitialized(),
      errors::FailedPrecondition(
        "Attempting to use uninitialized variables: ", requested_input(1)));
    OP_REQUIRES(
      ctx, var.shape().IsSameSize(accum.shape()),
      errors::InvalidArgument(
        "var and accum do not have the same shape",
        var.shape().DebugString(), " ", accum.shape().DebugString()));

    Tensor accum_decay_power;
    OP_REQUIRES_OK(ctx, GetInputTensorFromVariable<Device, Tstep>(
        ctx, 2, use_exclusive_lock_, false, &accum_decay_power));
    OP_REQUIRES(
      ctx, accum_decay_power.IsInitialized(),
      errors::FailedPrecondition(
        "Attempting to use uninitialized variables: ", requested_input(2)));

    const Tensor& lr = ctx->input(3);
    OP_REQUIRES(
      ctx, IsLegacyScalar(lr.shape()),
      errors::InvalidArgument(
        "lr is not a scalar: ", lr.shape().DebugString()));

    const Tensor& decay_step = ctx->input(4);
    OP_REQUIRES(
      ctx, IsLegacyScalar(decay_step.shape()),
      errors::InvalidArgument(
        "decay_step is not a scalar: ", decay_step.shape().DebugString()));

    const Tensor& decay_rate = ctx->input(5);
    OP_REQUIRES(
      ctx, IsLegacyScalar(decay_rate.shape()),
      errors::InvalidArgument(
        "decay_rate is not a scalar: ", decay_rate.shape().DebugString()));

    const Tensor& decay_baseline = ctx->input(6);
    OP_REQUIRES(
      ctx, IsLegacyScalar(decay_baseline.shape()),
      errors::InvalidArgument(
        "init accum is not a scalar: ", decay_baseline.shape().DebugString()));

    const Tensor& global_step = ctx->input(7);
    OP_REQUIRES(
      ctx, IsLegacyScalar(global_step.shape()),
      errors::InvalidArgument(
        "global_step is not a scalar: ", global_step.shape().DebugString()));

    const Tensor& grad = ctx->input(8);
    OP_REQUIRES(
      ctx, var.shape().IsSameSize(grad.shape()),
      errors::InvalidArgument(
        "var and grad do not have the same shape",
        var.shape().DebugString(), " ", grad.shape().DebugString()));

    bool need_decay = false;
    auto accum_decay_power_flat = accum_decay_power.flat<Tstep>();
    Tstep global_step_scalar = global_step.scalar<Tstep>()();
    Tstep decay_step_scalar = decay_step.scalar<Tstep>()();
    if (global_step_scalar / decay_step_scalar > accum_decay_power_flat(0)) {
      accum_decay_power_flat(0) += 1;
      need_decay = true;
    }

    const Device& device = ctx->template eigen_device<Device>();
    functor::ApplyAdagradDecay<Device, T>()(
      device, var.flat<T>(), accum.flat<T>(), lr.scalar<T>(),
      grad.flat<T>(), need_decay, decay_rate.scalar<T>(),
      decay_baseline.scalar<T>());

    MaybeForwardRefInputToRefOutput(ctx, 0, 0);
  }

 private:
  bool use_exclusive_lock_;
};

#define REGISTER_KERNELS(D, T, Tstep)                                        \
  REGISTER_KERNEL_BUILDER(Name("ApplyAdagradDecay")                          \
                              .Device(DEVICE_##D)                            \
                              .TypeConstraint<T>("T")                        \
                              .TypeConstraint<Tstep>("Tstep"),               \
                          ApplyAdagradDecayOp<D##Device, T, Tstep>);         \
  REGISTER_KERNEL_BUILDER(Name("ResourceApplyAdagradDecay")                  \
                              .HostMemory("var")                             \
                              .HostMemory("accum")                           \
                              .HostMemory("accum_decay_power")               \
                              .Device(DEVICE_##D)                            \
                              .TypeConstraint<T>("T")                        \
                              .TypeConstraint<Tstep>("Tstep"),               \
                          ApplyAdagradDecayOp<D##Device, T, Tstep>);

#define REGISTER_CPU_KERNELS(T)        \
  REGISTER_KERNELS(CPU, T, int32);     \
  REGISTER_KERNELS(CPU, T, int64);

TF_CALL_half(REGISTER_CPU_KERNELS);
TF_CALL_float(REGISTER_CPU_KERNELS);
TF_CALL_double(REGISTER_CPU_KERNELS);

#undef REGISTER_CPU_KERNELS
#undef REGISTER_KERNELS

// Note, this op works on cpu only.
template <typename T, typename Tindex, typename Tstep>
class SparseApplyAdagradDecayOp : public OpKernel {
 public:
  explicit SparseApplyAdagradDecayOp(OpKernelConstruction* ctx) : OpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("use_locking", &use_exclusive_lock_));
  }

  void Compute(OpKernelContext* ctx) override NO_THREAD_SAFETY_ANALYSIS {
    const bool sparse = true;
    auto locks = MaybeLockVariableInputMutexesInOrder<CPUDevice, T>(
      ctx, use_exclusive_lock_, sparse, {0, 1});

    Tensor var;
    OP_REQUIRES_OK(ctx, GetInputTensorFromVariable<CPUDevice, T>(
        ctx, 0, use_exclusive_lock_, true, &var));
    OP_REQUIRES(
      ctx, var.IsInitialized(),
      errors::FailedPrecondition(
        "Attempting to use uninitialized variables: ", requested_input(0)));
    OP_REQUIRES(
      ctx, TensorShapeUtils::IsVectorOrHigher(var.shape()),
      errors::InvalidArgument("var must be at least 1 dimensional"));

    Tensor accum;
    OP_REQUIRES_OK(ctx, GetInputTensorFromVariable<CPUDevice, T>(
        ctx, 1, use_exclusive_lock_, true, &accum));
    OP_REQUIRES(
      ctx, accum.IsInitialized(),
      errors::FailedPrecondition(
        "Attempting to use uninitialized variables: ", requested_input(1)));
    OP_REQUIRES(
      ctx, var.shape().IsSameSize(accum.shape()),
      errors::InvalidArgument(
        "var and accum do not have the same shape",
        var.shape().DebugString(), " ", accum.shape().DebugString()));

    Tensor accum_decay_power;
    OP_REQUIRES_OK(ctx, GetInputTensorFromVariable<CPUDevice, Tstep>(
        ctx, 2, use_exclusive_lock_, true, &accum_decay_power));
    OP_REQUIRES(
      ctx, accum_decay_power.IsInitialized(),
      errors::FailedPrecondition(
        "Attempting to use uninitialized variables: ", requested_input(2)));

    const Tensor& lr = ctx->input(3);
    OP_REQUIRES(
      ctx, IsLegacyScalar(lr.shape()),
      errors::InvalidArgument(
        "lr is not a scalar: ", lr.shape().DebugString()));

    const Tensor& decay_step = ctx->input(4);
    OP_REQUIRES(
      ctx, IsLegacyScalar(decay_step.shape()),
      errors::InvalidArgument(
        "decay_step is not a scalar: ", decay_step.shape().DebugString()));

    const Tensor& decay_rate = ctx->input(5);
    OP_REQUIRES(
      ctx, IsLegacyScalar(decay_rate.shape()),
      errors::InvalidArgument(
        "decay_rate is not a scalar: ", decay_rate.shape().DebugString()));

    const Tensor& decay_baseline = ctx->input(6);
    OP_REQUIRES(
      ctx, IsLegacyScalar(decay_baseline.shape()),
      errors::InvalidArgument(
        "init accum is not a scalar: ", decay_baseline.shape().DebugString()));

    const Tensor& global_step = ctx->input(7);
    OP_REQUIRES(
      ctx, IsLegacyScalar(global_step.shape()),
      errors::InvalidArgument(
        "global_step is not a scalar: ", global_step.shape().DebugString()));

    const Tensor& grad = ctx->input(8);
    const Tensor& indices = ctx->input(9);
    OP_REQUIRES(ctx, TensorShapeUtils::IsVector(indices.shape()),
                errors::InvalidArgument("indices must be one-dimensional"));

    int64 inner_dim = 1;
    for (int d = 1; d < var.dims(); d++) {
      OP_REQUIRES(
        ctx, var.dim_size(d) == grad.dim_size(d),
        errors::InvalidArgument(
          strings::StrCat("var and grad must match in dimension ", d)));
      inner_dim *= grad.dim_size(d);
    }
    OP_REQUIRES(
      ctx, inner_dim > 0,
      errors::InvalidArgument("Inner dimension should be greater than zero."));

    const int64 N = indices.dim_size(0);
    OP_REQUIRES(
      ctx, grad.dim_size(0) == N,
      errors::InvalidArgument(
        "grad must be the same size as indices in the first dimension."));

    if (N > 0) {
      auto indices_vec = indices.vec<Tindex>();
      auto accum_decay_power_flat = accum_decay_power.flat<Tstep>();
      T lr_scalar = lr.scalar<T>()();
      Tstep global_step_scalar = global_step.scalar<Tstep>()();
      Tstep decay_step_scalar = decay_step.scalar<Tstep>()();
      T decay_rate_scalar = decay_rate.scalar<T>()();
      T decay_baseline_scalar = decay_baseline.scalar<T>()();

      if (inner_dim > 1) {
        const int64 first_dim_size = var.dim_size(0);
        auto var_flat = var.flat_outer_dims<T>();
        auto accum_flat = accum.flat_outer_dims<T>();
        auto grad_flat = grad.flat_outer_dims<T>();
        auto do_work = [this, &indices_vec, &first_dim_size, ctx,
            &accum_flat, &grad_flat, &var_flat, &global_step_scalar,
            &decay_step_scalar, &accum_decay_power_flat, &decay_rate_scalar,
            &decay_baseline_scalar, &lr_scalar] (int64 start_i, int64 limit_i) {
          for (int64 i = start_i; i < limit_i; i++) {
            const Tindex index = internal::SubtleMustCopy(indices_vec(i));
            OP_REQUIRES(ctx, FastBoundsCheck(index, first_dim_size),
                        errors::InvalidArgument(
                            strings::StrCat("Index ", index, " at offset ", i,
                                            " in indices is out of range")));
            auto a = accum_flat.template chip<0>(index);
            auto g = grad_flat.template chip<0>(i);
            auto v = var_flat.template chip<0>(index);
            if (global_step_scalar / decay_step_scalar > accum_decay_power_flat(index)) {
              a *= a.constant(decay_rate_scalar);
              a = a.cwiseMax(decay_baseline_scalar);
              accum_decay_power_flat(index) += 1;
            }
            a += g.square();
            v -= g.constant(lr_scalar) * g * a.rsqrt();
          }
        };
        const int64 cost = 1000;
        auto worker_threads = *(ctx->device()->tensorflow_cpu_worker_threads());
        Shard(worker_threads.num_threads, worker_threads.workers, N, cost, do_work);
      } else {
        auto var_flat = var.flat<T>();
        auto accum_flat = accum.flat<T>();
        auto grad_flat = grad.flat<T>();
        const int64 first_dim_size = accum_flat.size();
        auto do_work = [this, ctx, &indices_vec, &first_dim_size, &accum_flat, &grad_flat,
            &global_step_scalar, &decay_step_scalar, &accum_decay_power_flat,
            &decay_rate_scalar, &decay_baseline_scalar, &lr_scalar, &var_flat]
                (int64 start_i, int64 limit_i) {
          for (int64 i = start_i; i < limit_i; i++) {
            const Tindex index = internal::SubtleMustCopy(indices_vec(i));
            OP_REQUIRES(ctx, FastBoundsCheck(index, first_dim_size),
                        errors::InvalidArgument(
                            strings::StrCat("Index ", index, " at offset ", i,
                                            " in indices is out of range")));
            T& a = accum_flat(index);
            const T& g = grad_flat(i);
            if (global_step_scalar / decay_step_scalar > accum_decay_power_flat(index)) {
              a *= decay_rate_scalar;
              if (a < decay_baseline_scalar) {
                a = decay_baseline_scalar;
              }
              accum_decay_power_flat(index) += 1;
            }
            a += g * g;
            var_flat(index) -= lr_scalar * g / Eigen::numext::sqrt(a);
          }
        };
        const int64 cost = 1000;
        auto worker_threads = *(ctx->device()->tensorflow_cpu_worker_threads());
        Shard(worker_threads.num_threads, worker_threads.workers, N, cost, do_work);
      }
    }

    MaybeForwardRefInputToRefOutput(ctx, 0, 0);
  }

 private:
  bool use_exclusive_lock_;
};

#define REGISTER_KERNELS(T, Tindices, Tstep)                                      \
  REGISTER_KERNEL_BUILDER(Name("SparseApplyAdagradDecay")                         \
                              .Device(DEVICE_CPU)                                 \
                              .TypeConstraint<T>("T")                             \
                              .TypeConstraint<Tindices>("Tindices")               \
                              .TypeConstraint<Tstep>("Tstep"),                    \
                          SparseApplyAdagradDecayOp<T, Tindices, Tstep>);         \
  REGISTER_KERNEL_BUILDER(Name("ResourceSparseApplyAdagradDecay")                 \
                              .Device(DEVICE_CPU)                                 \
                              .HostMemory("var")                                  \
                              .HostMemory("accum")                                \
                              .HostMemory("accum_decay_power")                    \
                              .TypeConstraint<T>("T")                             \
                              .TypeConstraint<Tindices>("Tindices")               \
                              .TypeConstraint<Tstep>("Tstep"),                    \
                          SparseApplyAdagradDecayOp<T, Tindices, Tstep>);

#define REGISTER_CPU_KERNELS(T)        \
  REGISTER_KERNELS(T, int32, int32);   \
  REGISTER_KERNELS(T, int32, int64);   \
  REGISTER_KERNELS(T, int64, int32);   \
  REGISTER_KERNELS(T, int64, int64);   \

TF_CALL_half(REGISTER_CPU_KERNELS);
TF_CALL_float(REGISTER_CPU_KERNELS);
TF_CALL_double(REGISTER_CPU_KERNELS);

#undef REGISTER_CPU_KERNELS
#undef REGISTER_KERNELS

// Note, this op works on cpu only.
template <typename T, typename Tindex, typename Tstep,
          bool indices_as_pointer, bool has_counts>
class KvSparseApplyAdagradDecayOp : public OpKernel {
 public:
  explicit KvSparseApplyAdagradDecayOp(OpKernelConstruction* ctx) : OpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("use_locking", &use_exclusive_lock_));
  }

  void Compute(OpKernelContext* ctx) override NO_THREAD_SAFETY_ANALYSIS {
    auto locks = MaybeLockEmbeddingVariableInputMutexesInOrder<Tindex, T>(
      ctx, use_exclusive_lock_, {0, 1, 2});

    EmbeddingVar<Tindex, T>* var = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 0, &var));
    core::ScopedUnref unref_var(var);

    EmbeddingVar<Tindex, T>* accum = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 1, &accum));
    core::ScopedUnref unref_accum(accum);

    EmbeddingVar<Tindex, T>* accum_decay_power_var = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 2, &accum_decay_power_var));
    core::ScopedUnref unref_accum_decay_power_var(accum_decay_power_var);

    const Tensor& lr = ctx->input(3);
    OP_REQUIRES(
      ctx, IsLegacyScalar(lr.shape()),
      errors::InvalidArgument(
        "lr is not a scalar: ", lr.shape().DebugString()));

    const Tensor& decay_step = ctx->input(4);
    OP_REQUIRES(
      ctx, IsLegacyScalar(decay_step.shape()),
      errors::InvalidArgument(
        "decay_step is not a scalar: ", decay_step.shape().DebugString()));

    const Tensor& decay_rate = ctx->input(5);
    OP_REQUIRES(
      ctx, IsLegacyScalar(decay_rate.shape()),
      errors::InvalidArgument(
        "decay_rate is not a scalar: ", decay_rate.shape().DebugString()));

    const Tensor& decay_baseline = ctx->input(6);
    OP_REQUIRES(
      ctx, IsLegacyScalar(decay_baseline.shape()),
      errors::InvalidArgument(
        "init accum is not a scalar: ", decay_baseline.shape().DebugString()));

    const Tensor& global_step = ctx->input(7);
    OP_REQUIRES(
      ctx, IsLegacyScalar(global_step.shape()),
      errors::InvalidArgument(
        "global_step is not a scalar: ", global_step.shape().DebugString()));

    const Tensor& grad = ctx->input(8);
    const Tensor& indices = ctx->input(9);
    OP_REQUIRES(ctx, TensorShapeUtils::IsVector(indices.shape()),
                errors::InvalidArgument("indices must be one-dimensional"));

    int64 inner_dim = 1;
    TensorShape var_shape({var->ValueLen()});
    for (int d = 0; d < var_shape.dims(); d++) {
      OP_REQUIRES(ctx, var_shape.dim_size(d) == grad.dim_size(d + 1),
                  errors::InvalidArgument(strings::StrCat(
                      "var and grad must match in dimension ", d + 1)));
      inner_dim *= grad.dim_size(d + 1);
    }
    OP_REQUIRES(
      ctx, inner_dim > 0,
      errors::InvalidArgument("Inner dimension should be greater than zero."));

    const int64 N = indices.dim_size(0);
    OP_REQUIRES(
      ctx, grad.dim_size(0) == N,
      errors::InvalidArgument(
        "grad must be the same size as indices in the first dimension."));
    int64* indices_counts = nullptr;
    std::function<int64(int64*, int64)> get_count_fn = 0;
    if (has_counts) {
      const Tensor& counts_tensor = ctx->input(10);
      indices_counts = (int64*)counts_tensor.data();
      get_count_fn = [](int64* counts, int64 index) {return counts[index];};
    } else {
      get_count_fn = [](int64* counts, int64 index) {return 1;};
    }

    if (N > 0) {
      auto indices_vec = indices.vec<Tindex>();
      T lr_scalar = lr.scalar<T>()();
      Tstep gs = global_step.scalar<Tstep>()();
      Tstep decay_step_scalar = decay_step.scalar<Tstep>()();
      T decay_rate_scalar = decay_rate.scalar<T>()();
      T decay_baseline_scalar = decay_baseline.scalar<T>()();

      if (inner_dim > 0) {
        auto grad_flat = grad.flat_outer_dims<T>();
        auto do_work = [this, ctx, &indices_vec, &var, &accum, &gs,
            &grad_flat, accum_decay_power_var, &decay_step_scalar,
            &decay_rate_scalar, &decay_baseline_scalar, &lr_scalar,
            get_count_fn, indices_counts]
            (int64 start_i, int64 limit_i) {
          for (int64 i = start_i; i < limit_i; i++) {
            const Tindex index = indices_vec(i);
            ValuePtr<T>* value_ptr = nullptr;
            bool is_filter = false;
            int64 count = get_count_fn(indices_counts, i);
            OP_REQUIRES_OK(ctx, var->LookupOrCreateKey(index, &value_ptr,
                           &is_filter, indices_as_pointer, count));
            var->UpdateVersion(value_ptr, gs);
            if (is_filter) {
              auto a = accum->flat(value_ptr, index);

              auto g = grad_flat.template chip<0>(i);

              auto v = var->flat(value_ptr, index);
              auto accum_decay_power = accum_decay_power_var->flat(value_ptr, index);

              if (gs / decay_step_scalar > accum_decay_power(0)) {
                a *= a.constant(decay_rate_scalar);
                a = a.cwiseMax(decay_baseline_scalar);
                accum_decay_power(0) += 1;
              }
              a += g.square();
              v -= g.constant(lr_scalar) * g * a.rsqrt();
            }
          }
        };
        const int64 cost = 1000;
        auto worker_threads = *(ctx->device()->tensorflow_cpu_worker_threads());
        Shard(worker_threads.num_threads, worker_threads.workers, N, cost, do_work);
        if (has_counts && !indices_as_pointer) {
          const Tensor& indices_counts = ctx->input(10);
          var->UpdateCache(indices, indices_counts);
        }
      }
    }

    MaybeForwardRefInputToRefOutput(ctx, 0, 0);
  }

 private:
  bool use_exclusive_lock_;
};

#define REGISTER_KERNELS(T, Tindices, Tstep)                               \
  REGISTER_KERNEL_BUILDER(Name("KvResourceSparseApplyAdagradDecay")        \
                              .Device(DEVICE_CPU)                          \
                              .HostMemory("var")                           \
                              .HostMemory("accum")                         \
                              .HostMemory("accum_decay_power")             \
                              .TypeConstraint<T>("T")                      \
                              .TypeConstraint<Tindices>("Tindices")        \
                              .TypeConstraint<Tstep>("Tstep"),             \
                          KvSparseApplyAdagradDecayOp<T, Tindices, Tstep, false, false>);\
  REGISTER_KERNEL_BUILDER(Name("_OPT_KvResourceSparseApplyAdagradDecay")   \
                              .Device(DEVICE_CPU)                          \
                              .HostMemory("var")                           \
                              .HostMemory("accum")                         \
                              .HostMemory("accum_decay_power")             \
                              .TypeConstraint<T>("T")                      \
                              .TypeConstraint<Tindices>("Tindices")        \
                              .TypeConstraint<Tstep>("Tstep"),             \
                          KvSparseApplyAdagradDecayOp<T, Tindices, Tstep, true, false>);\
  REGISTER_KERNEL_BUILDER(Name("KvResourceSparseApplyAdagradDecayWithCounts")        \
                              .Device(DEVICE_CPU)                          \
                              .HostMemory("var")                           \
                              .HostMemory("accum")                         \
                              .HostMemory("accum_decay_power")             \
                              .TypeConstraint<T>("T")                      \
                              .TypeConstraint<Tindices>("Tindices")        \
                              .TypeConstraint<Tstep>("Tstep"),             \
                          KvSparseApplyAdagradDecayOp<T, Tindices, Tstep, false, true>);\
  REGISTER_KERNEL_BUILDER(Name("_OPT_KvResourceSparseApplyAdagradDecayWithCounts")   \
                              .Device(DEVICE_CPU)                          \
                              .HostMemory("var")                           \
                              .HostMemory("accum")                         \
                              .HostMemory("accum_decay_power")             \
                              .TypeConstraint<T>("T")                      \
                              .TypeConstraint<Tindices>("Tindices")        \
                              .TypeConstraint<Tstep>("Tstep"),             \
                          KvSparseApplyAdagradDecayOp<T, Tindices, Tstep, true, true>);

#define REGISTER_CPU_KERNELS(T)        \
  REGISTER_KERNELS(T, int64, int32);   \
  REGISTER_KERNELS(T, int64, int64);   \
  REGISTER_KERNELS(T, int32, int32);   \
  REGISTER_KERNELS(T, int32, int64);   \

TF_CALL_float(REGISTER_CPU_KERNELS);

#undef REGISTER_CPU_KERNELS
#undef REGISTER_KERNELS

template <typename Device, typename T, typename Tindex,
          bool indices_as_pointer, bool has_counts>
class KvSparseApplyAdamOp : public OpKernel {
 public:
  explicit KvSparseApplyAdamOp(OpKernelConstruction* ctx) : OpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("use_locking", &use_exclusive_lock_));
  }

  void Compute(OpKernelContext* ctx) override NO_THREAD_SAFETY_ANALYSIS {
    auto locks = MaybeLockEmbeddingVariableInputMutexesInOrder<Tindex, T>(ctx, use_exclusive_lock_,
                                                      {0, 1, 2});
    EmbeddingVar<Tindex, T>* var = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 0, &var));
    core::ScopedUnref unref_var(var);

    EmbeddingVar<Tindex, T>* m = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 1, &m));
    core::ScopedUnref unref_m(m);

    EmbeddingVar<Tindex, T>* v = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 2, &v));
    core::ScopedUnref unref_v(v);

    const Tensor& beta1_power = ctx->input(3);
    const Tensor& beta2_power = ctx->input(4);
    const Tensor& lr = ctx->input(5);
    const Tensor& beta1 = ctx->input(6);
    const Tensor& beta2 = ctx->input(7);
    const Tensor& epsilon = ctx->input(8);
    const Tensor& grad = ctx->input(9);
    const Tensor& indices = ctx->input(10);
    const Tensor& global_step = ctx->input(11);

    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(beta1_power.shape()),
        errors::InvalidArgument("beta1_power is not a scalar: ",
                                beta1_power.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(beta2_power.shape()),
        errors::InvalidArgument("beta2_power is not a scalar: ",
                                beta2_power.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(lr.shape()),
        errors::InvalidArgument("lr is not a scalar: ",
                                lr.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(beta1.shape()),
        errors::InvalidArgument("beta1 is not a scalar: ",
                                beta1.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(beta2.shape()),
        errors::InvalidArgument("beta2 is not a scalar: ",
                                beta2.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(epsilon.shape()),
        errors::InvalidArgument("epsilon is not a scalar: ",
                                epsilon.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsVector(indices.shape()),
        errors::InvalidArgument("indices must be one-dimensional"));

    int64 inner_dim = 1;
    TensorShape var_shape({var->ValueLen()});
    for (int d = 0; d < var_shape.dims(); d++) {
      OP_REQUIRES(ctx, var_shape.dim_size(d) == grad.dim_size(d + 1),
                  errors::InvalidArgument(strings::StrCat(
                      "var and grad must match in dimension ", d + 1)));
      inner_dim *= grad.dim_size(d + 1);
    }
    OP_REQUIRES(
        ctx, inner_dim > 0,
        errors::InvalidArgument(
            "Inner dimension should be greater than zero."));

    OP_REQUIRES(
      ctx, IsLegacyScalar(global_step.shape()),
      errors::InvalidArgument(
        "global_step is not a scalar: ", global_step.shape().DebugString()));

    const int64 N = indices.dim_size(0);
    OP_REQUIRES(
        ctx, grad.dim_size(0) == N,
        errors::InvalidArgument(
            "grad must be the same size as indices in the first dimension."));
    int64* indices_counts = nullptr;
    std::function<int64(int64*, int64)> get_count_fn = 0;
    if (has_counts) {
      const Tensor& counts_tensor = ctx->input(12);
      indices_counts = (int64*)counts_tensor.data();
      get_count_fn = [](int64* counts, int64 index) {return counts[index];};
    } else {
      get_count_fn = [](int64* counts, int64 index) {return 1;};
    }

    if (N > 0) {
      T beta1_power_scalar = beta1_power.scalar<T>()();
      T beta2_power_scalar = beta2_power.scalar<T>()();
      T lr_scalar = lr.scalar<T>()();
      T beta1_scalar = beta1.scalar<T>()();
      T beta2_scalar = beta2.scalar<T>()();
      T epsilon_scalar = epsilon.scalar<T>()();
      const T alpha = lr_scalar *
          Eigen::numext::sqrt(static_cast<T>(1) - beta2_power_scalar) /
          (static_cast<T>(1) - beta1_power_scalar);

      auto DoWork = [this, ctx, inner_dim, &var, &m, &v, &grad, &indices,
           &beta1_power_scalar, &beta2_power_scalar, &lr_scalar, &beta1_scalar,
           &beta2_scalar, &epsilon_scalar, &alpha, &global_step,
           get_count_fn, indices_counts] (int64 start_i, int64 limit_i) {
        if (inner_dim > 0) {
          auto grad_flat = grad.flat_outer_dims<T>();
          auto indices_vec = indices.vec<Tindex>();

          int64 gs = global_step.scalar<int64>()();

          for (int64 i = start_i; i < limit_i; i++) {
            const Tindex index = indices_vec(i);
            ValuePtr<T>* value_ptr = nullptr;
            bool is_filter =false;
            int64 count = get_count_fn(indices_counts, i);
            OP_REQUIRES_OK(ctx, var->LookupOrCreateKey(index, &value_ptr,
                           &is_filter, indices_as_pointer, count));
            var->UpdateVersion(value_ptr, gs);
            if (is_filter) {
              auto var_i = var->flat(value_ptr, index);
              auto m_a = m->flat(value_ptr, index);
              auto v_a = v->flat(value_ptr, index);

              auto g = grad_flat.template chip<0>(i);
              m_a += (g - m_a) * (static_cast<T>(1) - beta1_scalar);
              v_a += (g.square() - v_a) * (static_cast<T>(1) - beta2_scalar);
              var_i -= (m_a * alpha) / (v_a.sqrt() + epsilon_scalar);
            }
          }
        }
      };

      const int64 cost = 1000;
      auto worker_threads = *(ctx->device()->tensorflow_cpu_worker_threads());
      Shard(worker_threads.num_threads, worker_threads.workers, N, cost, DoWork);
      if (has_counts && !indices_as_pointer) {
        const Tensor& indices_counts = ctx->input(12);
        var->UpdateCache(indices, indices_counts);
      }
    }
  }

 private:
  bool use_exclusive_lock_;
};

#define REGISTER_KERNELS(T, Tindices)                                 \
  REGISTER_KERNEL_BUILDER(Name("KvResourceSparseApplyAdam")           \
                              .Device(DEVICE_CPU)                     \
                              .TypeConstraint<T>("T")                 \
                              .TypeConstraint<Tindices>("Tindices"),  \
                          KvSparseApplyAdamOp<CPUDevice, T, Tindices, false, false>);\
  REGISTER_KERNEL_BUILDER(Name("_OPT_KvResourceSparseApplyAdam")      \
                              .Device(DEVICE_CPU)                     \
                              .TypeConstraint<T>("T")                 \
                              .TypeConstraint<Tindices>("Tindices"),  \
                          KvSparseApplyAdamOp<CPUDevice, T, Tindices, true, false>);\
  REGISTER_KERNEL_BUILDER(Name("KvResourceSparseApplyAdamWithCounts")           \
                              .Device(DEVICE_CPU)                     \
                              .TypeConstraint<T>("T")                 \
                              .TypeConstraint<Tindices>("Tindices"),  \
                          KvSparseApplyAdamOp<CPUDevice, T, Tindices, false, true>);\
  REGISTER_KERNEL_BUILDER(Name("_OPT_KvResourceSparseApplyAdamWithCounts")      \
                              .Device(DEVICE_CPU)                     \
                              .TypeConstraint<T>("T")                 \
                              .TypeConstraint<Tindices>("Tindices"),  \
                          KvSparseApplyAdamOp<CPUDevice, T, Tindices, true, true>);

#define REGISTER_CPU_KERNELS(T) \
  REGISTER_KERNELS(T, int32);   \
  REGISTER_KERNELS(T, int64);

TF_CALL_float(REGISTER_CPU_KERNELS);

#undef REGISTER_CPU_KERNELS
#undef REGISTER_KERNELS

#if GOOGLE_CUDA
template <typename Device, typename T, typename Tindex,
          bool indices_as_pointer, bool has_counts>
class KvSparseApplyAdamGPUOp : public OpKernel {
 public:
  explicit KvSparseApplyAdamGPUOp(OpKernelConstruction* ctx) : OpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("use_locking", &use_exclusive_lock_));

    int num_worker_threads = ctx->device()
                             ->tensorflow_cpu_worker_threads()
                             ->num_threads;
    thread_copy_id_alloc_.reset(
        new IntraThreadCopyIdAllocator(num_worker_threads));
  }

  void ApplyGradients(
      EmbeddingVar<Tindex, T>* var,
      EmbeddingVar<Tindex, T>* m,
      EmbeddingVar<Tindex, T>* v,
      T** var_ptr, T** m_ptr, T** v_ptr,
      T alpha, T beta1, T beta2,
      T epsilon, const T* grad_base,
      const int64 task_size,
      se::Stream* stream,
      EventMgr* event_mgr,
      const Eigen::GpuDevice& gpu_device) {
    // Send pointers of embeddings to GPU
    T** dev_var_ptr = (T**)var->GetBuffer(task_size * 3);
    T** dev_m_ptr = dev_var_ptr + task_size;
    T** dev_v_ptr = dev_m_ptr + task_size;
    CHECK(dev_var_ptr);
    CHECK(dev_m_ptr);
    CHECK(dev_v_ptr);

    DeviceMemoryBase dst_ptr(dev_var_ptr, sizeof(T*) * task_size * 3);
    stream->ThenMemcpy(&dst_ptr, var_ptr, sizeof(T*) * task_size * 3);

    int block_size = 128;
    int embedding_dim = var->ValueLen();

    functor::KvSparseApplyAdamHbm<GPUDevice, Tindex, T>()(
        block_size, embedding_dim,
        dev_var_ptr, dev_m_ptr, dev_v_ptr, grad_base,
        alpha, beta1, beta2, epsilon,
        task_size, gpu_device);
    SyncWithEventMgr(stream, event_mgr);
  }

  void Compute(OpKernelContext* ctx) override NO_THREAD_SAFETY_ANALYSIS {
    auto locks = MaybeLockEmbeddingVariableInputMutexesInOrder<Tindex, T>(ctx, use_exclusive_lock_,
                                                      {0, 1, 2});
    EmbeddingVar<Tindex, T>* var = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 0, &var));
    core::ScopedUnref unref_var(var);

    EmbeddingVar<Tindex, T>* m = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 1, &m));
    core::ScopedUnref unref_m(m);

    EmbeddingVar<Tindex, T>* v = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 2, &v));
    core::ScopedUnref unref_v(v);

    const Tensor& beta1_power = ctx->input(3);
    const Tensor& beta2_power = ctx->input(4);
    const Tensor& lr = ctx->input(5);
    const Tensor& beta1 = ctx->input(6);
    const Tensor& beta2 = ctx->input(7);
    const Tensor& epsilon = ctx->input(8);
    const Tensor& grad = ctx->input(9);
    const Tensor& indices = ctx->input(10);
    const Tensor& global_step = ctx->input(11);

    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(beta1_power.shape()),
        errors::InvalidArgument("beta1_power is not a scalar: ",
                                beta1_power.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(beta2_power.shape()),
        errors::InvalidArgument("beta2_power is not a scalar: ",
                                beta2_power.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(lr.shape()),
        errors::InvalidArgument("lr is not a scalar: ",
                                lr.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(beta1.shape()),
        errors::InvalidArgument("beta1 is not a scalar: ",
                                beta1.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(beta2.shape()),
        errors::InvalidArgument("beta2 is not a scalar: ",
                                beta2.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(epsilon.shape()),
        errors::InvalidArgument("epsilon is not a scalar: ",
                                epsilon.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsVector(indices.shape()),
        errors::InvalidArgument("indices must be one-dimensional"));
    
    OP_REQUIRES(ctx, !var->IsSingleHbm(), errors::InvalidArgument(
        "Adam optimizer doesn't support EV with single-level HBM storage."));

    int64 inner_dim = 1;
    TensorShape var_shape({var->ValueLen()});
    for (int d = 0; d < var_shape.dims(); d++) {
      OP_REQUIRES(ctx, var_shape.dim_size(d) == grad.dim_size(d + 1),
                  errors::InvalidArgument(strings::StrCat(
                      "var and grad must match in dimension ", d + 1)));
      inner_dim *= grad.dim_size(d + 1);
    }
    OP_REQUIRES(
        ctx, inner_dim > 0,
        errors::InvalidArgument(
            "Inner dimension should be greater than zero."));

    OP_REQUIRES(
      ctx, IsLegacyScalar(global_step.shape()),
      errors::InvalidArgument(
        "global_step is not a scalar: ", global_step.shape().DebugString()));

    const int64 N = indices.dim_size(0);
    OP_REQUIRES(
        ctx, grad.dim_size(0) == N,
        errors::InvalidArgument(
            "grad must be the same size as indices in the first dimension."));

    if (N > 0) {
      if (inner_dim > 0) {
        auto indices_flat = indices.flat<Tindex>();
        auto grad_flat = grad.flat_outer_dims<T>();
        int64 gs = global_step.scalar<int64>()();
        T beta1_power_scalar = beta1_power.scalar<T>()();
        T beta2_power_scalar = beta2_power.scalar<T>()();
        T lr_scalar = lr.scalar<T>()();
        T beta1_scalar = beta1.scalar<T>()();
        T beta2_scalar = beta2.scalar<T>()();
        T epsilon_scalar = epsilon.scalar<T>()();
        T alpha = lr_scalar *
            Eigen::numext::sqrt(static_cast<T>(1) - beta2_power_scalar) /
            (static_cast<T>(1) - beta1_power_scalar);

        int counts_index = has_counts ? 12 : -1;
        T** var_ptr = new T*[N * 3];
        T** m_ptr = var_ptr + N;
        T** v_ptr = m_ptr + N;
        std::vector<std::pair<EmbeddingVar<Tindex, T>*, T**>> vars(3);
        vars[0] = std::pair<EmbeddingVar<Tindex, T>*, T**>(var, var_ptr);
        vars[1] = std::pair<EmbeddingVar<Tindex, T>*, T**>(m, m_ptr);
        vars[2] = std::pair<EmbeddingVar<Tindex, T>*, T**>(v, v_ptr);
        GetEmbeddingPointers(ctx, vars, indices_flat.data(),
                             gs, indices_as_pointer,
                             counts_index, N, thread_copy_id_alloc_.get());

        auto stream = ctx->op_device_context()->stream();
        auto event_mgr = ctx->device()->tensorflow_gpu_device_info()->event_mgr;
        ApplyGradients(
            var, m, v, var_ptr,
            m_ptr, v_ptr, alpha,
            beta1_scalar, beta2_scalar,
            epsilon_scalar, &grad_flat(0), N,
            stream, event_mgr,
            ctx->eigen_gpu_device());

        if (has_counts && !indices_as_pointer) {
          const Tensor& counts_tensor = ctx->input(counts_index);
          var->UpdateCache(indices, counts_tensor);
        }

        delete[] var_ptr;
      }
    }
  }

 private:
  bool use_exclusive_lock_;
  std::unique_ptr<IntraThreadCopyIdAllocator> thread_copy_id_alloc_;
  std::function<T*(T*, Tindex, int64, int64, int64)> get_default_v_fn_;
};

#define REGISTER_KERNELS(T, Tindices)                                 \
  REGISTER_KERNEL_BUILDER(Name("KvResourceSparseApplyAdam")             \
                              .Device(DEVICE_GPU)                     \
                              .HostMemory("indices")                 \
                              .HostMemory("lr")                      \
                              .HostMemory("beta1_power")             \
                              .HostMemory("beta2_power")             \
                              .HostMemory("beta1")                   \
                              .HostMemory("beta2")                   \
                              .HostMemory("epsilon")                 \
                              .HostMemory("global_step")             \
                              .TypeConstraint<T>("T")                 \
                              .TypeConstraint<Tindices>("Tindices"),  \
                          KvSparseApplyAdamGPUOp<GPUDevice, T, Tindices, false, false>); \
  REGISTER_KERNEL_BUILDER(Name("_OPT_KvResourceSparseApplyAdam")           \
                              .Device(DEVICE_GPU)                     \
                              .HostMemory("indices")                 \
                              .HostMemory("lr")                      \
                              .HostMemory("beta1_power")             \
                              .HostMemory("beta2_power")             \
                              .HostMemory("beta1")                   \
                              .HostMemory("beta2")                   \
                              .HostMemory("epsilon")                 \
                              .HostMemory("global_step")             \
                              .TypeConstraint<T>("T")                 \
                              .TypeConstraint<Tindices>("Tindices"),  \
                          KvSparseApplyAdamGPUOp<GPUDevice, T, Tindices, true, false>);\
  REGISTER_KERNEL_BUILDER(Name("KvResourceSparseApplyAdamWithCounts")             \
                              .Device(DEVICE_GPU)                     \
                              .HostMemory("indices")                 \
                              .HostMemory("lr")                      \
                              .HostMemory("beta1_power")             \
                              .HostMemory("beta2_power")             \
                              .HostMemory("beta1")                   \
                              .HostMemory("beta2")                   \
                              .HostMemory("epsilon")                 \
                              .HostMemory("global_step")             \
                              .HostMemory("indices_counts")          \
                              .TypeConstraint<T>("T")                 \
                              .TypeConstraint<Tindices>("Tindices"),  \
                          KvSparseApplyAdamGPUOp<GPUDevice, T, Tindices, false, true>); \
   REGISTER_KERNEL_BUILDER(Name("_OPT_KvResourceSparseApplyAdamWithCounts")           \
                              .Device(DEVICE_GPU)                     \
                              .HostMemory("indices")                 \
                              .HostMemory("lr")                      \
                              .HostMemory("beta1_power")             \
                              .HostMemory("beta2_power")             \
                              .HostMemory("beta1")                   \
                              .HostMemory("beta2")                   \
                              .HostMemory("epsilon")                 \
                              .HostMemory("global_step")             \
                              .HostMemory("indices_counts")          \
                              .TypeConstraint<T>("T")                 \
                              .TypeConstraint<Tindices>("Tindices"),  \
                          KvSparseApplyAdamGPUOp<GPUDevice, T, Tindices, true, true>);
#define REGISTER_GPU_KERNELS(T) \
  REGISTER_KERNELS(T, int32);   \
  REGISTER_KERNELS(T, int64);

TF_CALL_float(REGISTER_GPU_KERNELS);

#undef REGISTER_GPU_KERNELS
#undef REGISTER_KERNELS
#endif  // GOOGLE_CUDA

namespace functor {
template <typename T>
struct ApplyAdamAsync<CPUDevice, T> {
  void operator()(const CPUDevice& d, typename TTypes<T>::Flat var,
                  typename TTypes<T>::Flat m, typename TTypes<T>::Flat v,
                  typename TTypes<T>::Scalar beta1_power,
                  typename TTypes<T>::Scalar beta2_power,
                  typename TTypes<T>::ConstScalar lr,
                  typename TTypes<T>::ConstScalar beta1,
                  typename TTypes<T>::ConstScalar beta2,
                  typename TTypes<T>::ConstScalar epsilon,
                  typename TTypes<T>::ConstFlat grad, bool use_nesterov) {
    auto alpha = lr() * Eigen::numext::sqrt(T(1) - beta2_power()) /
                 (T(1) - beta1_power());

    // beta1 == μ
    // beta2 == ν
    // v     == n
    // var   == θ
    m.device(d) = m * beta1() + grad * (T(1) - beta1());
    v.device(d) = v * beta2() + grad.square() * (T(1) - beta2());
    if (use_nesterov) {
      var.device(d) -= ((grad * (T(1) - beta1()) + beta1() * m) * alpha) /
                       (v.sqrt() + epsilon());
    } else {
      var.device(d) -= (m * alpha) / (v.sqrt() + epsilon());
    }

    // update beta1_power && beta2_power
    beta1_power.device(d) = beta1_power * beta1();
    beta2_power.device(d) = beta2_power * beta2();
  }
};
} // namespace functor

template <typename Device, typename T>
class ApplyAdamAsyncOp : public OpKernel {
 public:
  explicit ApplyAdamAsyncOp(OpKernelConstruction* ctx) : OpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("use_locking", &use_exclusive_lock_));
    OP_REQUIRES_OK(ctx, ctx->GetAttr("use_nesterov", &use_nesterov_));
  }

  void Compute(OpKernelContext* ctx) override {
    const bool sparse = false;
    auto locks = MaybeLockVariableInputMutexesInOrder<Device, T>(
      ctx, use_exclusive_lock_, sparse, {0, 1, 2, 3, 4});

    Tensor var;
    OP_REQUIRES_OK(ctx, GetInputTensorFromVariable<Device, T>(
                            ctx, 0, use_exclusive_lock_, false, &var));
    Tensor m;
    OP_REQUIRES_OK(ctx, GetInputTensorFromVariable<Device, T>(
                            ctx, 1, use_exclusive_lock_, false, &m));
    Tensor v;
    OP_REQUIRES_OK(ctx, GetInputTensorFromVariable<Device, T>(
                            ctx, 2, use_exclusive_lock_, false, &v));
    Tensor beta1_power;
    OP_REQUIRES_OK(ctx, GetInputTensorFromVariable<Device, T>(
                            ctx, 3, use_exclusive_lock_, false, &beta1_power));
    Tensor beta2_power;
    OP_REQUIRES_OK(ctx, GetInputTensorFromVariable<Device, T>(
                            ctx, 4, use_exclusive_lock_, false, &beta2_power));

    OP_REQUIRES(
        ctx, var.IsInitialized(),
        errors::FailedPrecondition(
            "Attempting to use uninitialized variables: ", requested_input(0)));
    OP_REQUIRES(
        ctx, m.IsInitialized(),
        errors::FailedPrecondition(
            "Attempting to use uninitialized variables: ", requested_input(1)));
    OP_REQUIRES(
        ctx, v.IsInitialized(),
        errors::FailedPrecondition(
            "Attempting to use uninitialized variables: ", requested_input(2)));
    OP_REQUIRES(
        ctx, beta1_power.IsInitialized(),
        errors::FailedPrecondition(
            "Attempting to use uninitialized variables: ", requested_input(3)));
    OP_REQUIRES(
        ctx, beta2_power.IsInitialized(),
        errors::FailedPrecondition(
            "Attempting to use uninitialized variables: ", requested_input(4)));

    const Tensor& lr = ctx->input(5);
    const Tensor& beta1 = ctx->input(6);
    const Tensor& beta2 = ctx->input(7);
    const Tensor& epsilon = ctx->input(8);

    OP_REQUIRES(ctx, TensorShapeUtils::IsScalar(lr.shape()),
                errors::InvalidArgument("lr is not a scalar : ",
                                        lr.shape().DebugString()));
    OP_REQUIRES(ctx, TensorShapeUtils::IsScalar(beta1.shape()),
                errors::InvalidArgument("beta1 is not a scalar: ",
                                        beta1.shape().DebugString()));
    OP_REQUIRES(ctx, TensorShapeUtils::IsScalar(beta2.shape()),
                errors::InvalidArgument("beta2 is not a scalar: ",
                                        beta2.shape().DebugString()));
    OP_REQUIRES(ctx, TensorShapeUtils::IsScalar(epsilon.shape()),
                errors::InvalidArgument("epsilon is not a scalar: ",
                                        epsilon.shape().DebugString()));

    const Tensor& grad = ctx->input(9);
    OP_REQUIRES(ctx, var.shape().IsSameSize(m.shape()),
                errors::InvalidArgument("var and m do not have the same shape",
                                        var.shape().DebugString(), " ",
                                        m.shape().DebugString()));
    OP_REQUIRES(ctx, var.shape().IsSameSize(v.shape()),
                errors::InvalidArgument("var and v do not have the same shape",
                                        var.shape().DebugString(), " ",
                                        v.shape().DebugString()));
    OP_REQUIRES(
        ctx, var.shape().IsSameSize(grad.shape()),
        errors::InvalidArgument("var and grad do not have the same shape",
                                var.shape().DebugString(), " ",
                                grad.shape().DebugString()));

    const Device& device = ctx->template eigen_device<Device>();
    functor::ApplyAdamAsync<Device, T>()(
        device, var.flat<T>(), m.flat<T>(), v.flat<T>(),
        beta1_power.scalar<T>(), beta2_power.scalar<T>(), lr.scalar<T>(),
        beta1.scalar<T>(), beta2.scalar<T>(), epsilon.scalar<T>(),
        grad.flat<T>(), use_nesterov_);

    MaybeForwardRefInputToRefOutput(ctx, 0, 0);
  }

 private:
  bool use_exclusive_lock_;
  bool use_nesterov_;
};

#define REGISTER_KERNELS(D, T)                                          \
  REGISTER_KERNEL_BUILDER(                                              \
      Name("ApplyAdamAsync").Device(DEVICE_##D).TypeConstraint<T>("T"), \
      ApplyAdamAsyncOp<D##Device, T>);                                  \
  REGISTER_KERNEL_BUILDER(Name("ResourceApplyAdamAsync")                \
                              .Device(DEVICE_##D)                       \
                              .TypeConstraint<T>("T"),                  \
                          ApplyAdamAsyncOp<D##Device, T>);
#define REGISTER_CPU_KERNELS(T) REGISTER_KERNELS(CPU, T);

TF_CALL_half(REGISTER_CPU_KERNELS);
TF_CALL_bfloat16(REGISTER_CPU_KERNELS);
TF_CALL_float(REGISTER_CPU_KERNELS);
TF_CALL_double(REGISTER_CPU_KERNELS);

#undef REGISTER_CPU_KERNELS

#if GOOGLE_CUDA || TENSORFLOW_USE_ROCM
// Forward declarations of the functor specializations for GPU.
namespace functor {
#define DECLARE_GPU_SPEC(T)\
  template <> \
  void ApplyAdamAsync<GPUDevice, T>::operator()(const GPUDevice& d, \
                   typename TTypes<T>::Flat var, \
                  typename TTypes<T>::Flat m, typename TTypes<T>::Flat v, \
                  typename TTypes<T>::Scalar beta1_power, \
                  typename TTypes<T>::Scalar beta2_power, \
                  typename TTypes<T>::ConstScalar lr, \
                  typename TTypes<T>::ConstScalar beta1, \
                  typename TTypes<T>::ConstScalar beta2, \
                  typename TTypes<T>::ConstScalar epsilon, \
                  typename TTypes<T>::ConstFlat grad, bool use_nesterov); \
  extern template struct ApplyAdamAsync<GPUDevice, T>;

DECLARE_GPU_SPEC(Eigen::half)
DECLARE_GPU_SPEC(float)
DECLARE_GPU_SPEC(double)
#undef DECLARE_GPU_SPEC
} // end of namespace functor

#define REGISTER_GPU_KERNELS(T) REGISTER_KERNELS(GPU, T);

TF_CALL_half(REGISTER_GPU_KERNELS);
TF_CALL_float(REGISTER_GPU_KERNELS);
TF_CALL_double(REGISTER_GPU_KERNELS);

#undef REGISTER_GPU_KERNELS
#endif // end of GOOGLE_CUDA || TENSORFLOW_USE_ROCM
#undef REGISTER_KERNELS

namespace functor {
template <typename T, typename Tindex>
struct SparseApplyAdamAsync<CPUDevice, T, Tindex> {
  Status operator()(const CPUDevice &d, typename TTypes<T>::Matrix var,
                  typename TTypes<T>::Matrix m, typename TTypes<T>::Matrix v,
                  typename TTypes<T>::Scalar beta1_power_scalar,
                  typename TTypes<T>::Scalar beta2_power_scalar,
                  typename TTypes<T>::ConstScalar lr_scalar,
                  typename TTypes<T>::ConstScalar beta1_scalar,
                  typename TTypes<T>::ConstScalar beta2_scalar,
                  typename TTypes<T>::ConstScalar epsilon_scalar,
                  typename TTypes<T>::ConstMatrix grad,
                  typename TTypes<Tindex>::ConstVec indices_vec,
                  bool apply_sparse_rmsprop, int64 inner_dim) {
    const int64 N = indices_vec.dimension(0);
    if (N <= 0) return Status::OK();

    const T lr = lr_scalar();
    const T beta1 = beta1_scalar();
    const T beta2 = beta2_scalar();
    const T epsilon = epsilon_scalar();
    const int64 first_dim_size = static_cast<int64>(var.dimension(0));
    // Validate all the indices are in range
    for (int64 i = 0; i < N; i++) {
      const Tindex index = indices_vec(i);
      if (index < 0 || index >= first_dim_size) {
        return errors::InvalidArgument(strings::StrCat("Index ", index,
                      " at offset ", i, " in indices is out of range"));
      }
    }

    if (apply_sparse_rmsprop) {
      auto do_work = [this, &indices_vec, &v, &m, &grad, &beta2, &beta1,
                      &epsilon, &lr, &var] (int64 start_i, int64 limit_i) {
        for (Tindex i = start_i; i < limit_i; i++) {
          const Tindex index = indices_vec(i);

          auto v_ = v.template chip<0>(index);
          auto m_ = m.template chip<0>(index);
          auto grad_ = grad.template chip<0>(i);

          v_ = v_ * v_.constant(beta2) +
                grad_.square() * grad_.constant(T(1) - beta2);
          m_ = m_ * m_.constant(beta1) +
                  (v_ + v_.constant(epsilon)).rsqrt() *
                      v_.constant(lr) * grad_;

          auto v = var.template chip<0>(index);
          v -= m_;
        }
      };
      const int in_bytes = inner_dim * sizeof(T) * 4;
      const int out_bytes = inner_dim * sizeof(T) * 3;
      const int cycles = inner_dim * (Eigen::TensorOpCost::AddCost<int>() * 5 +
                                      Eigen::TensorOpCost::MulCost<int>() * 6);
      const Eigen::TensorOpCost cost(in_bytes, out_bytes, cycles);
      d.parallelFor(N, cost, do_work);
    } else {
      const T alpha = lr *
          Eigen::numext::sqrt(static_cast<T>(1) - beta2_power_scalar()) /
          (static_cast<T>(1) - beta1_power_scalar());

      auto do_work = [this, inner_dim, &var, &m, &v, &grad, &indices_vec,
            &beta1_power_scalar, &beta2_power_scalar, &lr, &beta1, &beta2,
            &epsilon, &alpha] (int64 start_i, int64 limit_i) {
        if (inner_dim > 1) {
          for (Tindex i = static_cast<Tindex>(start_i); i < static_cast<Tindex>(limit_i); i++) {
            const Tindex index = internal::SubtleMustCopy(indices_vec(i));
            auto m_a = m.template chip<0>(index);
            auto v_a = v.template chip<0>(index);
            auto g = grad.template chip<0>(i);
            auto var_i = var.template chip<0>(index);

            m_a = m_a * beta1 + g * (static_cast<T>(1) - beta1);
            v_a = v_a * beta2 + g.square() * (static_cast<T>(1) - beta2);
            var_i -= (m_a * alpha) / (v_a.sqrt() + epsilon);
          }
        } else {
          for (Tindex i = static_cast<Tindex>(start_i); i < static_cast<Tindex>(limit_i); i++) {
            const Tindex index = internal::SubtleMustCopy(indices_vec(i));
            const T& g = grad(i);
            T& m_a = m(index);
            T& v_a = v(index);
            m_a = m_a * beta1 + g * (static_cast<T>(1) - beta1);
            v_a = v_a * beta2 + g * g * (static_cast<T>(1) - beta2);
            var(index) -= (m_a * alpha) / (Eigen::numext::sqrt(v_a) + epsilon);
          }
        }
      };

      const int in_bytes = inner_dim * sizeof(T) * 4;
      const int out_bytes = inner_dim * sizeof(T) * 3;
      const int cycles = inner_dim * (Eigen::TensorOpCost::AddCost<T>() * 6 +
                                      Eigen::TensorOpCost::MulCost<T>() * 6 +
                                      Eigen::TensorOpCost::DivCost<T>());
      const Eigen::TensorOpCost cost(in_bytes, out_bytes, cycles);
      d.parallelFor(N, cost, do_work);

      beta1_power_scalar() *= beta1;
      beta2_power_scalar() *= beta2;
    }
    return Status::OK();
  }
};
} // namespace functor

template <typename Device, typename T, typename Tindex>
class SparseApplyAdamAsyncOp : public OpKernel {
 public:
  explicit SparseApplyAdamAsyncOp(OpKernelConstruction* ctx) : OpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("use_locking", &use_exclusive_lock_));
    OP_REQUIRES_OK(ctx, ctx->GetAttr("apply_sparse_rmsprop", &apply_sparse_rmsprop_));
  }

  void Compute(OpKernelContext* ctx) override NO_THREAD_SAFETY_ANALYSIS {
    const bool sparse = true;
    auto locks = MaybeLockVariableInputMutexesInOrder<Device, T>(
      ctx, use_exclusive_lock_, sparse, {0, 1, 2, 3, 4});
    Tensor var;
    OP_REQUIRES_OK(ctx, GetInputTensorFromVariable<Device, T>(
                            ctx, 0, use_exclusive_lock_, true, &var));
    Tensor m;
    OP_REQUIRES_OK(ctx, GetInputTensorFromVariable<Device, T>(
                            ctx, 1, use_exclusive_lock_, true, &m));
    Tensor v;
    OP_REQUIRES_OK(ctx, GetInputTensorFromVariable<Device, T>(
                            ctx, 2, use_exclusive_lock_, true, &v));
    Tensor beta1_power;
    OP_REQUIRES_OK(ctx, GetInputTensorFromVariable<Device, T>(
                            ctx, 3, use_exclusive_lock_, true, &beta1_power));
    Tensor beta2_power;
    OP_REQUIRES_OK(ctx, GetInputTensorFromVariable<Device, T>(
                            ctx, 4, use_exclusive_lock_, true, &beta2_power));

    OP_REQUIRES(
        ctx, var.IsInitialized(),
        errors::FailedPrecondition(
            "Attempting to use uninitialized variables: ", requested_input(0)));
    OP_REQUIRES(
        ctx, m.IsInitialized(),
        errors::FailedPrecondition(
            "Attempting to use uninitialized variables: ", requested_input(1)));
    OP_REQUIRES(
        ctx, v.IsInitialized(),
        errors::FailedPrecondition(
            "Attempting to use uninitialized variables: ", requested_input(2)));
    OP_REQUIRES(
        ctx, beta1_power.IsInitialized(),
        errors::FailedPrecondition(
            "Attempting to use uninitialized variables: ", requested_input(3)));
    OP_REQUIRES(
        ctx, beta2_power.IsInitialized(),
        errors::FailedPrecondition(
            "Attempting to use uninitialized variables: ", requested_input(4)));
    OP_REQUIRES(
        ctx, var.shape().IsSameSize(m.shape()),
        errors::InvalidArgument("var and m do not have the same shape",
                                var.shape().DebugString(), " ",
                                m.shape().DebugString()));
    OP_REQUIRES(
        ctx, var.shape().IsSameSize(v.shape()),
        errors::InvalidArgument("var and v do not have the same shape",
                                var.shape().DebugString(), " ",
                                v.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsVectorOrHigher(var.shape()),
        errors::InvalidArgument("var must be at least 1 dimensional"));

    const Tensor& lr = ctx->input(5);
    const Tensor& beta1 = ctx->input(6);
    const Tensor& beta2 = ctx->input(7);
    const Tensor& epsilon = ctx->input(8);
    const Tensor& grad = ctx->input(9);
    const Tensor& indices = ctx->input(10);

    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(lr.shape()),
        errors::InvalidArgument("lr is not a scalar: ",
                                lr.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(beta1.shape()),
        errors::InvalidArgument("beta1 is not a scalar: ",
                                beta1.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(beta2.shape()),
        errors::InvalidArgument("beta2 is not a scalar: ",
                                beta2.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(epsilon.shape()),
        errors::InvalidArgument("epsilon is not a scalar: ",
                                epsilon.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsVector(indices.shape()),
        errors::InvalidArgument("indices must be one-dimensional"));

    int64 inner_dim = 1;
    for (int d = 1; d < var.dims(); d++) {
      OP_REQUIRES(
          ctx, var.dim_size(d) == grad.dim_size(d),
          errors::InvalidArgument(strings::StrCat(
                "var and grad must match in dimension ", d)));
      inner_dim *= grad.dim_size(d);
    }
    OP_REQUIRES(
        ctx, inner_dim > 0,
        errors::InvalidArgument(
            "Inner dimension should be greater than zero."));

    const int64 N = indices.dim_size(0);
    OP_REQUIRES(
        ctx, grad.dim_size(0) == N,
        errors::InvalidArgument(
            "grad must be the same size as indices in the first dimension."));

    const Device& device = ctx->template eigen_device<Device>();
    OP_REQUIRES_OK(ctx, functor::SparseApplyAdamAsync<Device, T, Tindex>()(
      device, var.flat_outer_dims<T>(), m.flat_outer_dims<T>(),
      v.flat_outer_dims<T>(), beta1_power.scalar<T>(), beta2_power.scalar<T>(),
      lr.scalar<T>(), beta1.scalar<T>(), beta2.scalar<T>(), epsilon.scalar<T>(),
      grad.flat_outer_dims<T>(), indices.vec<Tindex>(), apply_sparse_rmsprop_,
      inner_dim));

    MaybeForwardRefInputToRefOutput(ctx, 0, 0);
  }

 private:
  bool use_exclusive_lock_;
  bool apply_sparse_rmsprop_;
};

#define REGISTER_KERNELS(D, T, Tindices)                                   \
  REGISTER_KERNEL_BUILDER(Name("SparseApplyAdamAsync")                     \
                              .Device(DEVICE_##D)                          \
                              .TypeConstraint<T>("T")                      \
                              .TypeConstraint<Tindices>("Tindices"),       \
                          SparseApplyAdamAsyncOp<D##Device, T, Tindices>); \
  REGISTER_KERNEL_BUILDER(Name("ResourceSparseApplyAdamAsync")             \
                              .Device(DEVICE_##D)                          \
                              .TypeConstraint<T>("T")                      \
                              .TypeConstraint<Tindices>("Tindices"),       \
                          SparseApplyAdamAsyncOp<D##Device, T, Tindices>);
#define REGISTER_CPU_KERNELS(T)                                            \
  REGISTER_KERNELS(CPU, T, int32);                                         \
  REGISTER_KERNELS(CPU, T, int64);

TF_CALL_half(REGISTER_CPU_KERNELS);
TF_CALL_bfloat16(REGISTER_CPU_KERNELS);
TF_CALL_float(REGISTER_CPU_KERNELS);
TF_CALL_double(REGISTER_CPU_KERNELS);

#undef REGISTER_CPU_KERNELS

#if GOOGLE_CUDA || TENSORFLOW_USE_ROCM
// Forward declarations of the functor specializations for GPU.
namespace functor {
#define DECLARE_GPU_SPEC(T, Tindex)                                                \
  template <>                                                                      \
  Status SparseApplyAdamAsync<GPUDevice, T, Tindex>::operator()(const GPUDevice &d,\
                  typename TTypes<T>::Matrix var,                                  \
                  typename TTypes<T>::Matrix m, typename TTypes<T>::Matrix v,      \
                  typename TTypes<T>::Scalar beta1_power,                          \
                  typename TTypes<T>::Scalar beta2_power,                          \
                  typename TTypes<T>::ConstScalar lr,                              \
                  typename TTypes<T>::ConstScalar beta1,                           \
                  typename TTypes<T>::ConstScalar beta2,                           \
                  typename TTypes<T>::ConstScalar epsilon,                         \
                  typename TTypes<T>::ConstMatrix grad,                            \
                  typename TTypes<Tindex>::ConstVec indices_vec,                   \
                  bool apply_sparse_rmsprop, int64 inner_dim);                     \
  extern template struct SparseApplyAdamAsync<GPUDevice, T, Tindex>;

DECLARE_GPU_SPEC(Eigen::half, int32);
DECLARE_GPU_SPEC(Eigen::half, int64);
DECLARE_GPU_SPEC(float, int32);
DECLARE_GPU_SPEC(float, int64);
DECLARE_GPU_SPEC(double, int32);
DECLARE_GPU_SPEC(double, int64);
#undef DECLARE_GPU_SPEC
} // end of namespace functor

#define REGISTER_GPU_KERNEL(T)                                                     \
  REGISTER_KERNELS(GPU, T, int32);                                                 \
  REGISTER_KERNELS(GPU, T, int64);

TF_CALL_half(REGISTER_GPU_KERNEL);
TF_CALL_float(REGISTER_GPU_KERNEL);
TF_CALL_double(REGISTER_GPU_KERNEL);

#undef REGISTER_GPU_KERNEL
#endif // End of #if GOOGLE_CUDA || TENSORFLOW_USE_ROCM
#undef REGISTER_KERNELS

template <typename Device, typename T, typename Tindex,
          typename Tstep, bool indices_as_pointer, bool has_counts>
class KvSparseApplyAdamAsyncOp : public OpKernel {
 public:
  explicit KvSparseApplyAdamAsyncOp(OpKernelConstruction* ctx) : OpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("use_locking", &use_exclusive_lock_));
    OP_REQUIRES_OK(ctx, ctx->GetAttr("apply_sparse_rmsprop", &apply_sparse_rmsprop_));
  }

  void Compute(OpKernelContext* ctx) override NO_THREAD_SAFETY_ANALYSIS {
    auto locks = MaybeLockEmbeddingVariableInputMutexesInOrder<Tindex, T>(
      ctx, use_exclusive_lock_, {0, 1, 2, 3, 4});
    EmbeddingVar<Tindex, T>* var = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 0, &var));
    core::ScopedUnref unref_var(var);

    EmbeddingVar<Tindex, T>* m = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 1, &m));
    core::ScopedUnref unref_m(m);

    EmbeddingVar<Tindex, T>* v = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 2, &v));
    core::ScopedUnref unref_v(v);

    Tensor beta1_power;
    OP_REQUIRES_OK(ctx, GetInputTensorFromVariable<Device, T>(
                            ctx, 3, use_exclusive_lock_, true, &beta1_power));

    Tensor beta2_power;
    OP_REQUIRES_OK(ctx, GetInputTensorFromVariable<Device, T>(
                            ctx, 4, use_exclusive_lock_, true, &beta2_power));
    OP_REQUIRES(
        ctx, beta1_power.IsInitialized(),
        errors::FailedPrecondition(
            "Attempting to use uninitialized variables: ", requested_input(3)));
    OP_REQUIRES(
        ctx, beta2_power.IsInitialized(),
        errors::FailedPrecondition(
            "Attempting to use uninitialized variables: ", requested_input(4)));

    const Tensor& lr = ctx->input(5);
    const Tensor& beta1 = ctx->input(6);
    const Tensor& beta2 = ctx->input(7);
    const Tensor& epsilon = ctx->input(8);
    const Tensor& grad = ctx->input(9);
    const Tensor& indices = ctx->input(10);
    const Tensor& global_step = ctx->input(11);

    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(lr.shape()),
        errors::InvalidArgument("lr is not a scalar: ",
                                lr.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(beta1.shape()),
        errors::InvalidArgument("beta1 is not a scalar: ",
                                beta1.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(beta2.shape()),
        errors::InvalidArgument("beta2 is not a scalar: ",
                                beta2.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(epsilon.shape()),
        errors::InvalidArgument("epsilon is not a scalar: ",
                                epsilon.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsVector(indices.shape()),
        errors::InvalidArgument("indices must be one-dimensional"));

    int64 inner_dim = 1;
    TensorShape var_shape({var->ValueLen()});
    for (int d = 0; d < var_shape.dims(); d++) {
      OP_REQUIRES(
          ctx, var_shape.dim_size(d) == grad.dim_size(d + 1),
          errors::InvalidArgument(strings::StrCat(
                "var and grad must match in dimension ", d + 1)));
      inner_dim *= grad.dim_size(d + 1);
    }
    OP_REQUIRES(
        ctx, inner_dim > 0,
        errors::InvalidArgument(
            "Inner dimension should be greater than zero."));

    OP_REQUIRES(
        ctx, IsLegacyScalar(global_step.shape()),
        errors::InvalidArgument(
            "global_step is not a scalar: ", global_step.shape().DebugString()));

    const int64 N = indices.dim_size(0);
    OP_REQUIRES(
        ctx, grad.dim_size(0) == N,
        errors::InvalidArgument(
            "grad must be the same size as indices in the first dimension."));
    int64* indices_counts = nullptr;
    std::function<int64(int64*, int64)> get_count_fn = 0;
    if (has_counts) {
      const Tensor& counts_tensor = ctx->input(12);
      indices_counts = (int64*)counts_tensor.data();
      get_count_fn = [](int64* counts, int64 index) {return counts[index];};
    } else {
      get_count_fn = [](int64* counts, int64 index) {return 1;};
    }
    if (N > 0) {
      if (apply_sparse_rmsprop_) {
        auto indices_vec = indices.vec<Tindex>();

        auto grad_flat = grad.flat_outer_dims<T>();
        const T lr_scalar = lr.scalar<T>()();
        const T beta1_scalar = beta1.scalar<T>()();
        const T beta2_scalar = beta2.scalar<T>()();
        const T epsilon_scalar = epsilon.scalar<T>()();

        auto do_work = [this, ctx, &indices_vec, &var, v, m, &grad_flat,
            &beta2_scalar, &beta1_scalar, &epsilon_scalar, &lr_scalar, &global_step,
            get_count_fn, indices_counts]
            (int64 start_i, int64 limit_i) {
          Tstep gs = global_step.scalar<Tstep>()();
          for (int64 i = start_i; i < limit_i; i++) {
            const Tindex index = indices_vec(i);
            ValuePtr<T>* value_ptr = nullptr;
            bool is_filter = false;
            int64 count = get_count_fn(indices_counts, i);
            OP_REQUIRES_OK(ctx, var->LookupOrCreateKey(index, &value_ptr,
                           &is_filter, indices_as_pointer, count));
            var->UpdateVersion(value_ptr, gs);
            if (is_filter) {
              auto v_ = v->flat(value_ptr, index);
              auto m_ = m->flat(value_ptr, index);
              auto grad_ = grad_flat.template chip<0>(i);

              v_ = v_ * v_.constant(beta2_scalar) +
              grad_.square() * grad_.constant(T(1) - beta2_scalar);
              m_ = m_ * m_.constant(beta1_scalar) +
                     (v_ + v_.constant(epsilon_scalar)).rsqrt() *
                         v_.constant(lr_scalar) * grad_;

              auto v = var->flat(value_ptr, index);
              v -= m_;
            }
          }
        };
        const int64 cost = 1000;
        auto worker_threads = *(ctx->device()->tensorflow_cpu_worker_threads());
        Shard(worker_threads.num_threads, worker_threads.workers, N, cost, do_work);
      } else {
        auto beta1_power_scalar = beta1_power.scalar<T>();
        auto beta2_power_scalar = beta2_power.scalar<T>();
        T lr_scalar = lr.scalar<T>()();
        T beta1_scalar = beta1.scalar<T>()();
        T beta2_scalar = beta2.scalar<T>()();
        T epsilon_scalar = epsilon.scalar<T>()();
        const T alpha = lr_scalar *
            Eigen::numext::sqrt(static_cast<T>(1) - beta2_power_scalar()) /
            (static_cast<T>(1) - beta1_power_scalar());

        auto do_work = [this, ctx, inner_dim, &var, &m, &v, &grad, &indices,
             &lr_scalar, &beta1_scalar,
             &beta1_power, &beta2_power,
             &beta2_scalar, &epsilon_scalar, &alpha, &global_step,
             get_count_fn, indices_counts] (int64 start_i, int64 limit_i) {

          if (inner_dim > 0) {
            auto grad_flat = grad.flat_outer_dims<T>();
            auto indices_vec = indices.vec<Tindex>();
            Tstep gs = global_step.scalar<Tstep>()();

            for (int64 i = start_i; i < limit_i; i++) {
              const Tindex index = indices_vec(i);
              ValuePtr<T>* value_ptr = nullptr;
              bool is_filter = false;
              int64 count = get_count_fn(indices_counts, i);
              OP_REQUIRES_OK(ctx, var->LookupOrCreateKey(index, &value_ptr,
                             &is_filter, indices_as_pointer, count));
              var->UpdateVersion(value_ptr, gs);
              if (is_filter) {
                auto m_a = m->flat(value_ptr, index);
                auto v_a = v->flat(value_ptr, index);
                auto g = grad_flat.template chip<0>(i);
                auto var_i = var->flat(value_ptr, index);

                m_a = m_a * beta1_scalar + g * (static_cast<T>(1) - beta1_scalar);
                v_a = v_a * beta2_scalar + g.square() * (static_cast<T>(1) - beta2_scalar);
                var_i -= (m_a * alpha) / (v_a.sqrt() + epsilon_scalar);
              }
            }
          }
        };

        const int64 cost = 1000;
        auto worker_threads = *(ctx->device()->tensorflow_cpu_worker_threads());
        Shard(worker_threads.num_threads, worker_threads.workers, N, cost, do_work);

        beta1_power_scalar() *= beta1_scalar;
        beta2_power_scalar() *= beta2_scalar;
      }
      if (has_counts && !indices_as_pointer) {
        const Tensor& indices_counts = ctx->input(12);
        var->UpdateCache(indices, indices_counts);
      }
    }

    MaybeForwardRefInputToRefOutput(ctx, 0, 0);
  }

 private:
  bool use_exclusive_lock_;
  bool apply_sparse_rmsprop_;
};

#define REGISTER_KERNELS(D, T, Tindices, Tstep)                            \
  REGISTER_KERNEL_BUILDER(Name("KvResourceSparseApplyAdamAsync")           \
                              .Device(DEVICE_##D)                          \
                              .TypeConstraint<T>("T")                      \
                              .TypeConstraint<Tindices>("Tindices")        \
                              .TypeConstraint<Tstep>("Tstep"),             \
                          KvSparseApplyAdamAsyncOp<D##Device, T, Tindices, Tstep, false, false>);\
  REGISTER_KERNEL_BUILDER(Name("_OPT_KvResourceSparseApplyAdamAsync")      \
                              .Device(DEVICE_##D)                          \
                              .TypeConstraint<T>("T")                      \
                              .TypeConstraint<Tindices>("Tindices")        \
                              .TypeConstraint<Tstep>("Tstep"),             \
                          KvSparseApplyAdamAsyncOp<D##Device, T, Tindices, Tstep, true, false>);\
  REGISTER_KERNEL_BUILDER(Name("KvResourceSparseApplyAdamAsyncWithCounts")           \
                              .Device(DEVICE_##D)                          \
                              .TypeConstraint<T>("T")                      \
                              .TypeConstraint<Tindices>("Tindices")        \
                              .TypeConstraint<Tstep>("Tstep"),             \
                          KvSparseApplyAdamAsyncOp<D##Device, T, Tindices, Tstep, false, true>);\
  REGISTER_KERNEL_BUILDER(Name("_OPT_KvResourceSparseApplyAdamAsyncWithCounts")      \
                              .Device(DEVICE_##D)                          \
                              .TypeConstraint<T>("T")                      \
                              .TypeConstraint<Tindices>("Tindices")        \
                              .TypeConstraint<Tstep>("Tstep"),             \
                          KvSparseApplyAdamAsyncOp<D##Device, T, Tindices, Tstep, true, true>);

#define REGISTER_CPU_KERNELS(T)             \
  REGISTER_KERNELS(CPU, T, int32, int32);   \
  REGISTER_KERNELS(CPU, T, int64, int32);   \
  REGISTER_KERNELS(CPU, T, int32, int64);   \
  REGISTER_KERNELS(CPU, T, int64, int64);

TF_CALL_half(REGISTER_CPU_KERNELS);
TF_CALL_bfloat16(REGISTER_CPU_KERNELS);
TF_CALL_float(REGISTER_CPU_KERNELS);
TF_CALL_double(REGISTER_CPU_KERNELS);

#undef REGISTER_CPU_KERNELS
#undef REGISTER_KERNELS

#if GOOGLE_CUDA
template <typename Device, typename T, typename Tindex,
          typename Tstep, bool indices_as_pointer, bool has_counts>
class KvSparseApplyAdamAsyncGPUOp : public OpKernel {
 public:
  explicit KvSparseApplyAdamAsyncGPUOp(OpKernelConstruction* ctx) : OpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("use_locking", &use_exclusive_lock_));
    OP_REQUIRES_OK(ctx, ctx->GetAttr("apply_sparse_rmsprop", &apply_sparse_rmsprop_));

    int num_worker_threads = ctx->device()
                             ->tensorflow_cpu_worker_threads()
                             ->num_threads;
    thread_copy_id_alloc_.reset(
        new IntraThreadCopyIdAllocator(num_worker_threads));
  }

  void ApplyGradients(
      EmbeddingVar<Tindex, T>* var,
      EmbeddingVar<Tindex, T>* m,
      EmbeddingVar<Tindex, T>* v,
      T** var_ptr, T** m_ptr, T** v_ptr,
      T beta1, T beta2,
      T epsilon, T lr,
      typename TTypes<T>::Scalar beta1_power_scalar,
      typename TTypes<T>::Scalar beta2_power_scalar,
      const T* grad_base,
      const int64 task_size,
      se::Stream* stream,
      EventMgr* event_mgr,
      const Eigen::GpuDevice& gpu_device) {
    // Send pointers of embeddings to GPU
    T** dev_var_ptr = (T**)var->GetBuffer(task_size * 3);
    T** dev_m_ptr = dev_var_ptr + task_size;
    T** dev_v_ptr = dev_m_ptr + task_size;
    CHECK(dev_var_ptr);
    CHECK(dev_m_ptr);
    CHECK(dev_v_ptr);

    DeviceMemoryBase dst_ptr(dev_var_ptr, sizeof(T*) * task_size * 3);
    stream->ThenMemcpy(&dst_ptr, var_ptr, sizeof(T*) * task_size * 3);

    int block_size = 128;
    int embedding_dim = var->ValueLen();
    T* beta1_power_ptr = beta1_power_scalar.data();
    T* beta2_power_ptr = beta2_power_scalar.data();
    if (apply_sparse_rmsprop_) {
      functor::KvSparseApplyAdamAsyncSparseRmspropHbm<GPUDevice, Tindex, T>()(
          block_size, embedding_dim,
          dev_var_ptr, dev_m_ptr, dev_v_ptr,
          grad_base, lr, beta1,
          beta2, epsilon, task_size,
          gpu_device);
    } else {
     functor::KvSparseApplyAdamAsyncHbm<GPUDevice, Tindex, T>()(
          block_size, embedding_dim,
          dev_var_ptr, dev_m_ptr, dev_v_ptr,
          grad_base, lr, beta1,
          beta2, epsilon, beta1_power_ptr,
          beta2_power_ptr, task_size,
          gpu_device);
    }
    SyncWithEventMgr(stream, event_mgr);
  }

  void Compute(OpKernelContext* ctx) override NO_THREAD_SAFETY_ANALYSIS {
    auto locks = MaybeLockEmbeddingVariableInputMutexesInOrder<Tindex, T>(
      ctx, use_exclusive_lock_, {0, 1, 2, 3, 4});
    EmbeddingVar<Tindex, T>* var = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 0, &var));
    core::ScopedUnref unref_var(var);

    EmbeddingVar<Tindex, T>* m = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 1, &m));
    core::ScopedUnref unref_m(m);

    EmbeddingVar<Tindex, T>* v = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 2, &v));
    core::ScopedUnref unref_v(v);

    Tensor beta1_power;
    OP_REQUIRES_OK(ctx, GetInputTensorFromVariable<Device, T>(
                            ctx, 3, use_exclusive_lock_, true, &beta1_power));

    Tensor beta2_power;
    OP_REQUIRES_OK(ctx, GetInputTensorFromVariable<Device, T>(
                            ctx, 4, use_exclusive_lock_, true, &beta2_power));
    OP_REQUIRES(
        ctx, beta1_power.IsInitialized(),
        errors::FailedPrecondition(
            "Attempting to use uninitialized variables: ", requested_input(3)));
    OP_REQUIRES(
        ctx, beta2_power.IsInitialized(),
        errors::FailedPrecondition(
            "Attempting to use uninitialized variables: ", requested_input(4)));

    const Tensor& lr = ctx->input(5);
    const Tensor& beta1 = ctx->input(6);
    const Tensor& beta2 = ctx->input(7);
    const Tensor& epsilon = ctx->input(8);
    const Tensor& grad = ctx->input(9);
    const Tensor& indices = ctx->input(10);
    const Tensor& global_step = ctx->input(11);

    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(lr.shape()),
        errors::InvalidArgument("lr is not a scalar: ",
                                lr.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(beta1.shape()),
        errors::InvalidArgument("beta1 is not a scalar: ",
                                beta1.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(beta2.shape()),
        errors::InvalidArgument("beta2 is not a scalar: ",
                                beta2.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(epsilon.shape()),
        errors::InvalidArgument("epsilon is not a scalar: ",
                                epsilon.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsVector(indices.shape()),
        errors::InvalidArgument("indices must be one-dimensional"));

    int64 inner_dim = 1;
    TensorShape var_shape({var->ValueLen()});
    for (int d = 0; d < var_shape.dims(); d++) {
      OP_REQUIRES(
          ctx, var_shape.dim_size(d) == grad.dim_size(d + 1),
          errors::InvalidArgument(strings::StrCat(
                "var and grad must match in dimension ", d + 1)));
      inner_dim *= grad.dim_size(d + 1);
    }
    OP_REQUIRES(
        ctx, inner_dim > 0,
        errors::InvalidArgument(
            "Inner dimension should be greater than zero."));

    OP_REQUIRES(
        ctx, IsLegacyScalar(global_step.shape()),
        errors::InvalidArgument(
            "global_step is not a scalar: ", global_step.shape().DebugString()));

    const int64 N = indices.dim_size(0);
    OP_REQUIRES(
        ctx, grad.dim_size(0) == N,
        errors::InvalidArgument(
            "grad must be the same size as indices in the first dimension."));

    if (N > 0) {
      if (var->IsSingleHbm()) {
        const Device& device = ctx->eigen_device<Device>();
        OP_REQUIRES_OK(ctx,
        functor::KvSparseApplyAdamAsync<Device, T, Tindex, Tstep>()(
          device, var, m, v, beta1_power.scalar<T>(), beta2_power.scalar<T>(),
          indices.vec<Tindex>(), grad.flat_outer_dims<T>(), lr.scalar<T>(),
          beta1.scalar<T>(), beta2.scalar<T>(), epsilon.scalar<T>(),
          global_step.scalar<Tstep>(), apply_sparse_rmsprop_, inner_dim,
          ctx->get_allocator(AllocatorAttributes())));
      } else {
        auto indices_vec = indices.vec<Tindex>();
        auto grad_flat = grad.flat_outer_dims<T>();
        Tstep gs = global_step.scalar<int64>()();
        const T lr_scalar = lr.scalar<T>()();
        const T beta1_scalar = beta1.scalar<T>()();
        const T beta2_scalar = beta2.scalar<T>()();
        const T epsilon_scalar = epsilon.scalar<T>()();
        auto beta1_power_scalar = beta1_power.scalar<T>();
        auto beta2_power_scalar = beta2_power.scalar<T>();

        Tensor indices_temp_host(indices.dtype(), indices.shape());
        const Tensor* indices_host_ptr = nullptr;
        //Copy ids from GPU to CPU for CPU Lookup.
        auto stream = ctx->op_device_context()->stream();
        auto event_mgr = ctx->device()->tensorflow_gpu_device_info()->event_mgr;
        if (!indices_as_pointer) {
          indices_host_ptr = &indices_temp_host;
          se::DeviceMemoryBase gpu_src(
              const_cast<Tindex*>(&indices_vec(0)), N * sizeof(Tindex));
          stream->ThenMemcpy(indices_host_ptr->data(), gpu_src, N * sizeof(Tindex));
          SyncWithEventMgr(stream, event_mgr);
        } else {
          indices_host_ptr = &indices;
        }

        int counts_index = has_counts ? 12 : -1;
        T** var_ptr = new T*[N * 3];
        T** m_ptr = var_ptr + N;
        T** v_ptr = m_ptr + N;
        std::vector<std::pair<EmbeddingVar<Tindex, T>*, T**>> vars(3);
        vars[0] = std::pair<EmbeddingVar<Tindex, T>*, T**>(var, var_ptr);
        vars[1] = std::pair<EmbeddingVar<Tindex, T>*, T**>(m, m_ptr);
        vars[2] = std::pair<EmbeddingVar<Tindex, T>*, T**>(v, v_ptr);
        GetEmbeddingPointers(ctx, vars, (Tindex*)indices_host_ptr->data(),
                             gs, indices_as_pointer,
                             counts_index, N, thread_copy_id_alloc_.get());

        ApplyGradients(
            var, m, v, var_ptr,
            m_ptr, v_ptr,
            beta1_scalar, beta2_scalar,
            epsilon_scalar, lr_scalar,
            beta1_power_scalar,
            beta2_power_scalar,
            &grad_flat(0), N,
            stream, event_mgr,
            ctx->eigen_device<GPUDevice>());

        if (has_counts && !indices_as_pointer) {
          const Tensor& counts_tensor = ctx->input(counts_index);
          var->UpdateCache(*indices_host_ptr, counts_tensor);
        }

        delete[] var_ptr;
      }
    }
    MaybeForwardRefInputToRefOutput(ctx, 0, 0);
  }

 private:
  bool use_exclusive_lock_;
  bool apply_sparse_rmsprop_;
  std::unique_ptr<IntraThreadCopyIdAllocator> thread_copy_id_alloc_;
};

#define REGISTER_KERNELS(D, T, Tindices, Tstep)                             \
  REGISTER_KERNEL_BUILDER(Name("KvResourceSparseApplyAdamAsync")           \
                              .Device(DEVICE_##D)                          \
                              .HostMemory("lr")                      \
                              .HostMemory("beta1")                   \
                              .HostMemory("beta2")                   \
                              .HostMemory("epsilon")                 \
                              .HostMemory("global_step")             \
                              .TypeConstraint<T>("T")                      \
                              .TypeConstraint<Tindices>("Tindices")        \
                              .TypeConstraint<Tstep>("Tstep"),             \
                          KvSparseApplyAdamAsyncGPUOp<D##Device, T, Tindices, Tstep, false, false>); \
  REGISTER_KERNEL_BUILDER(Name("_OPT_KvResourceSparseApplyAdamAsync")           \
                              .Device(DEVICE_##D)                          \
                              .HostMemory("indices")                 \
                              .HostMemory("lr")                      \
                              .HostMemory("beta1")                   \
                              .HostMemory("beta2")                   \
                              .HostMemory("epsilon")                 \
                              .HostMemory("global_step")             \
                              .TypeConstraint<T>("T")                      \
                              .TypeConstraint<Tindices>("Tindices")        \
                              .TypeConstraint<Tstep>("Tstep"),             \
                          KvSparseApplyAdamAsyncGPUOp<D##Device, T, Tindices, Tstep, true, false>); \
  REGISTER_KERNEL_BUILDER(Name("KvResourceSparseApplyAdamAsyncWithCounts")           \
                              .Device(DEVICE_##D)                          \
                              .HostMemory("lr")                      \
                              .HostMemory("beta1")                   \
                              .HostMemory("beta2")                   \
                              .HostMemory("epsilon")                 \
                              .HostMemory("global_step")             \
                              .HostMemory("indices_counts")          \
                              .TypeConstraint<T>("T")                      \
                              .TypeConstraint<Tindices>("Tindices")        \
                              .TypeConstraint<Tstep>("Tstep"),             \
                          KvSparseApplyAdamAsyncGPUOp<D##Device, T, Tindices, Tstep, false, true>); \
  REGISTER_KERNEL_BUILDER(Name("_OPT_KvResourceSparseApplyAdamAsyncWithCounts")           \
                              .Device(DEVICE_##D)                          \
                              .HostMemory("indices")                 \
                              .HostMemory("lr")                      \
                              .HostMemory("beta1")                   \
                              .HostMemory("beta2")                   \
                              .HostMemory("epsilon")                 \
                              .HostMemory("global_step")             \
                              .HostMemory("indices_counts")          \
                              .TypeConstraint<T>("T")                      \
                              .TypeConstraint<Tindices>("Tindices")        \
                              .TypeConstraint<Tstep>("Tstep"),             \
                          KvSparseApplyAdamAsyncGPUOp<D##Device, T, Tindices, Tstep, true, true>);
#define REGISTER_GPU_KERNELS(T)        \
  REGISTER_KERNELS(GPU, T, int32, int32);   \
  REGISTER_KERNELS(GPU, T, int64, int32);   \
  REGISTER_KERNELS(GPU, T, int32, int64);   \
  REGISTER_KERNELS(GPU, T, int64, int64);

TF_CALL_float(REGISTER_GPU_KERNELS);
TF_CALL_double(REGISTER_GPU_KERNELS);

#undef REGISTER_GPU_KERNELS
#undef REGISTER_KERNELS
#endif  // GOOGLE_CUDA


#if GOOGLE_CUDA || TENSORFLOW_USE_ROCM
// Forward declarations of the functor specializations for GPU.
namespace functor {
#define DECLARE_GPU_SPEC(T, Tindex, Tstep)                                 \
  template <>                                                              \
  Status KvSparseApplyAdamAsync<GPUDevice, T, Tindex, Tstep>::operator()(  \
                    const GPUDevice &d,                                    \
                    EmbeddingVar<Tindex, T> *var,                          \
                    EmbeddingVar<Tindex, T> *m,                            \
                    EmbeddingVar<Tindex, T> *v,                            \
                    typename TTypes<T>::Scalar beta1_power_scalar,         \
                    typename TTypes<T>::Scalar beta2_power_scalar,         \
                    typename TTypes<Tindex>::ConstVec indices_vec,         \
                    typename TTypes<T>::ConstMatrix grad,                  \
                    typename TTypes<T>::ConstScalar lr_scalar,             \
                    typename TTypes<T>::ConstScalar beta1_scalar,          \
                    typename TTypes<T>::ConstScalar beta2_scalar,          \
                    typename TTypes<T>::ConstScalar epsilon_scalar,        \
                    typename TTypes<Tstep>::ConstScalar global_step_scalar,\
                    bool apply_sparse_rmsprop, const int64 inner_dim,      \
                    Allocator *alloc);                                     \
  extern template struct KvSparseApplyAdamAsync<GPUDevice, T, Tindex, Tstep>;

#define DECLARE_GPU_SPEC_TYPE(T)                                           \
  DECLARE_GPU_SPEC(T, int32, int32);                                       \
  DECLARE_GPU_SPEC(T, int32, int64);                                       \
  DECLARE_GPU_SPEC(T, int64, int32);                                       \
  DECLARE_GPU_SPEC(T, int64, int64);

DECLARE_GPU_SPEC_TYPE(float);
DECLARE_GPU_SPEC_TYPE(double);

#undef DECLARE_GPU_SPEC_TYPE
#undef DECLARE_GPU_SPEC
} // end of namespace functor

#endif // End of GOOGLE_CUDA || TENSORFLOW_USE_ROCM

template <typename T, typename Tindex, typename Tstep,
          bool indices_as_pointer, bool has_counts>
class KvResourceSparseApplyGradientDescentOp : public OpKernel {
 public:
  explicit KvResourceSparseApplyGradientDescentOp(OpKernelConstruction* ctx) : OpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("use_locking", &use_exclusive_lock_));
  }

  void Compute(OpKernelContext* ctx) override NO_THREAD_SAFETY_ANALYSIS {
    auto locks = MaybeLockEmbeddingVariableInputMutexesInOrder<Tindex, T>(
      ctx, use_exclusive_lock_, {0});

    EmbeddingVar<Tindex, T>* var = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 0, &var));
    core::ScopedUnref unref_var(var);

    const Tensor& lr = ctx->input(1);
    OP_REQUIRES(
      ctx, IsLegacyScalar(lr.shape()),
      errors::InvalidArgument(
        "lr is not a scalar: ", lr.shape().DebugString()));

    const Tensor& grad = ctx->input(2);
    const Tensor& indices = ctx->input(3);
    OP_REQUIRES(ctx, TensorShapeUtils::IsVector(indices.shape()),
                errors::InvalidArgument("indices must be one-dimensional"));

    const Tensor& global_step = ctx->input(4);
    OP_REQUIRES(
      ctx, IsLegacyScalar(global_step.shape()),
      errors::InvalidArgument(
        "global_step is not a scalar: ", global_step.shape().DebugString()));

    int64 inner_dim = 1;
    TensorShape var_shape({var->ValueLen()});
    for (int d = 0; d < var_shape.dims(); d++) {
      OP_REQUIRES(ctx, var_shape.dim_size(d) == grad.dim_size(d + 1),
                  errors::InvalidArgument(strings::StrCat(
                      "var and grad must match in dimension ", d + 1)));
      inner_dim *= grad.dim_size(d + 1);
    }
    OP_REQUIRES(
      ctx, inner_dim > 0,
      errors::InvalidArgument("Inner dimension should be greater than zero."));

    const int64 N = indices.dim_size(0);
    OP_REQUIRES(
      ctx, grad.dim_size(0) == N,
      errors::InvalidArgument(
        "grad must be the same size as indices in the first dimension."));
    int64* indices_counts = nullptr;
    std::function<int64(int64*, int64)> get_count_fn = 0;
    if (has_counts) {
      const Tensor& counts_tensor = ctx->input(5);
      indices_counts = (int64*)counts_tensor.data();
      get_count_fn = [](int64* counts, int64 index) {return counts[index];};
    } else {
      get_count_fn = [](int64* counts, int64 index) {return 1;};
    }

    if (N > 0) {
      auto indices_vec = indices.vec<Tindex>();
      T lr_scalar = lr.scalar<T>()();
      Tstep gs = global_step.scalar<Tstep>()();

      if (inner_dim > 0) {
        auto grad_flat = grad.flat_outer_dims<T>();
        auto do_work = [this, ctx, &indices_vec, var, &grad_flat, &gs,
            &lr_scalar, indices_counts, get_count_fn]
            (int64 start_i, int64 limit_i) {
          for (int64 i = start_i; i < limit_i; i++) {
            const Tindex index = indices_vec(i);
            ValuePtr<T>* value_ptr = nullptr;
            bool is_filter = false;
            int64 count = get_count_fn(indices_counts, i);
            OP_REQUIRES_OK(ctx, var->LookupOrCreateKey(index, &value_ptr,
                           &is_filter, indices_as_pointer, count));
            var->UpdateVersion(value_ptr, gs);
            if (is_filter) {
              auto g = grad_flat.template chip<0>(i);
              auto v = var->flat(value_ptr, index);
              v -= g.constant(lr_scalar) * g;
            }
          }
        };
        const int64 cost = 1000;
        auto worker_threads = *(ctx->device()->tensorflow_cpu_worker_threads());
        Shard(worker_threads.num_threads, worker_threads.workers, N, cost, do_work);
        if (has_counts && !indices_as_pointer) {
          const Tensor& indices = ctx->input(5);
          var->UpdateCache(indices, indices_counts);
        } else {
          var->UpdateCache(indices);
        }
      }
    }

    MaybeForwardRefInputToRefOutput(ctx, 0, 0);
  }

 private:
  bool use_exclusive_lock_;
};

#define REGISTER_KERNELS(T, Tindices, Tstep)                               \
  REGISTER_KERNEL_BUILDER(Name("KvResourceSparseApplyGradientDescent")     \
                              .Device(DEVICE_CPU)                          \
                              .HostMemory("var")                           \
                              .TypeConstraint<T>("T")                      \
                              .TypeConstraint<Tindices>("Tindices")        \
                              .TypeConstraint<Tstep>("Tstep"),             \
                          KvResourceSparseApplyGradientDescentOp<T, Tindices, Tstep, false, false>);\
  REGISTER_KERNEL_BUILDER(Name("_OPT_KvResourceSparseApplyGradientDescent")\
                              .Device(DEVICE_CPU)                          \
                              .HostMemory("var")                           \
                              .TypeConstraint<T>("T")                      \
                              .TypeConstraint<Tindices>("Tindices")        \
                              .TypeConstraint<Tstep>("Tstep"),             \
                          KvResourceSparseApplyGradientDescentOp<T, Tindices, Tstep, true, false>);\
  REGISTER_KERNEL_BUILDER(Name("KvResourceSparseApplyGradientDescentWithCounts")     \
                              .Device(DEVICE_CPU)                          \
                              .HostMemory("var")                           \
                              .TypeConstraint<T>("T")                      \
                              .TypeConstraint<Tindices>("Tindices")        \
                              .TypeConstraint<Tstep>("Tstep"),             \
                          KvResourceSparseApplyGradientDescentOp<T, Tindices, Tstep, false, true>);\
  REGISTER_KERNEL_BUILDER(Name("_OPT_KvResourceSparseApplyGradientDescentWithCounts")\
                              .Device(DEVICE_CPU)                          \
                              .HostMemory("var")                           \
                              .TypeConstraint<T>("T")                      \
                              .TypeConstraint<Tindices>("Tindices")        \
                              .TypeConstraint<Tstep>("Tstep"),             \
                          KvResourceSparseApplyGradientDescentOp<T, Tindices, Tstep, true, true>);

#define REGISTER_CPU_KERNELS(T)        \
  REGISTER_KERNELS(T, int64, int32);   \
  REGISTER_KERNELS(T, int64, int64);   \
  REGISTER_KERNELS(T, int32, int32);   \
  REGISTER_KERNELS(T, int32, int64);   \

TF_CALL_float(REGISTER_CPU_KERNELS);

#undef REGISTER_CPU_KERNELS
#undef REGISTER_KERNELS

template <typename Device, typename T, typename Tindex,
          bool indices_as_pointer, bool has_counts>
class KvSparseApplyAdamWOp : public OpKernel {
 public:
  explicit KvSparseApplyAdamWOp(OpKernelConstruction* ctx) : OpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("use_locking", &use_exclusive_lock_));
  }

  void Compute(OpKernelContext* ctx) override NO_THREAD_SAFETY_ANALYSIS {
    auto locks = MaybeLockEmbeddingVariableInputMutexesInOrder<Tindex, T>(ctx, use_exclusive_lock_,
                                                      {0, 1, 2});
    EmbeddingVar<Tindex, T>* var = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 0, &var));
    core::ScopedUnref unref_var(var);

    EmbeddingVar<Tindex, T>* m = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 1, &m));
    core::ScopedUnref unref_m(m);

    EmbeddingVar<Tindex, T>* v = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 2, &v));
    core::ScopedUnref unref_v(v);

    const Tensor& beta1_power = ctx->input(3);
    const Tensor& beta2_power = ctx->input(4);
    const Tensor& lr = ctx->input(5);
    const Tensor& beta1 = ctx->input(6);
    const Tensor& beta2 = ctx->input(7);
    const Tensor& epsilon = ctx->input(8);
    const Tensor& grad = ctx->input(9);
    const Tensor& indices = ctx->input(10);
    const Tensor& global_step = ctx->input(11);
    const Tensor& weight_decay = ctx->input(12);

    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(beta1_power.shape()),
        errors::InvalidArgument("beta1_power is not a scalar: ",
                                beta1_power.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(beta2_power.shape()),
        errors::InvalidArgument("beta2_power is not a scalar: ",
                                beta2_power.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(lr.shape()),
        errors::InvalidArgument("lr is not a scalar: ",
                                lr.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(beta1.shape()),
        errors::InvalidArgument("beta1 is not a scalar: ",
                                beta1.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(beta2.shape()),
        errors::InvalidArgument("beta2 is not a scalar: ",
                                beta2.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(epsilon.shape()),
        errors::InvalidArgument("epsilon is not a scalar: ",
                                epsilon.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsVector(indices.shape()),
        errors::InvalidArgument("indices must be one-dimensional"));
      
    OP_REQUIRES(ctx, TensorShapeUtils::IsScalar(weight_decay.shape()),
                errors::InvalidArgument("weight_decay is not a scalar: ",
                                        weight_decay.shape().DebugString()));

    int64 inner_dim = 1;
    TensorShape var_shape({var->ValueLen()});
    for (int d = 0; d < var_shape.dims(); d++) {
      OP_REQUIRES(ctx, var_shape.dim_size(d) == grad.dim_size(d + 1),
                  errors::InvalidArgument(strings::StrCat(
                      "var and grad must match in dimension ", d + 1)));
      inner_dim *= grad.dim_size(d + 1);
    }
    OP_REQUIRES(
        ctx, inner_dim > 0,
        errors::InvalidArgument(
            "Inner dimension should be greater than zero."));

    OP_REQUIRES(
      ctx, IsLegacyScalar(global_step.shape()),
      errors::InvalidArgument(
        "global_step is not a scalar: ", global_step.shape().DebugString()));

    const int64 N = indices.dim_size(0);
    OP_REQUIRES(
        ctx, grad.dim_size(0) == N,
        errors::InvalidArgument(
            "grad must be the same size as indices in the first dimension."));
    int64* indices_counts = nullptr;
    std::function<int64(int64*, int64)> get_count_fn = 0;
    if (has_counts) {
      const Tensor& counts_tensor = ctx->input(13);
      indices_counts = (int64*)counts_tensor.data();
      get_count_fn = [](int64* counts, int64 index) {return counts[index];};
    } else {
      get_count_fn = [](int64* counts, int64 index) {return 1;};
    }

    if (N > 0) {
      T beta1_power_scalar = beta1_power.scalar<T>()();
      T beta2_power_scalar = beta2_power.scalar<T>()();
      T lr_scalar = lr.scalar<T>()();
      T beta1_scalar = beta1.scalar<T>()();
      T beta2_scalar = beta2.scalar<T>()();
      T epsilon_scalar = epsilon.scalar<T>()();
      T weight_decay_scalar = weight_decay.scalar<T>()();
      const T alpha = lr_scalar *
          Eigen::numext::sqrt(static_cast<T>(1) - beta2_power_scalar) /
          (static_cast<T>(1) - beta1_power_scalar);

      auto DoWork = [this, ctx, inner_dim, &var, &m, &v, &grad, &indices,
          &beta1_power_scalar, &beta2_power_scalar, &lr_scalar, &beta1_scalar,
          &beta2_scalar, &epsilon_scalar, &alpha, &global_step, 
          &weight_decay_scalar, get_count_fn, indices_counts]
          (int64 start_i, int64 limit_i) {
        if (inner_dim > 0) {
          auto grad_flat = grad.flat_outer_dims<T>();
          auto indices_vec = indices.vec<Tindex>();

          int64 gs = global_step.scalar<int64>()();

          for (int64 i = start_i; i < limit_i; i++) {
            const Tindex index = indices_vec(i);
            ValuePtr<T>* value_ptr = nullptr;
            bool is_filter =false;
            int64 count = get_count_fn(indices_counts, i);
            OP_REQUIRES_OK(ctx, var->LookupOrCreateKey(index, &value_ptr,
                           &is_filter, indices_as_pointer, count));
            var->UpdateVersion(value_ptr, gs);
            if (is_filter) {
              auto var_i = var->flat(value_ptr, index);
              auto m_a = m->flat(value_ptr, index);
              auto v_a = v->flat(value_ptr, index);
              auto g = grad_flat.template chip<0>(i);
              // m_a = beta1 * m + (1 - beta1) * g
              m_a += (g - m_a) * (static_cast<T>(1) - beta1_scalar);
              // v_a = beta2 * v + (1 - beta2) * (g * g)
              v_a += (g.square() - v_a) * (static_cast<T>(1) - beta2_scalar);
              var_i -= (m_a * alpha) / (v_a.sqrt() + epsilon_scalar) + weight_decay_scalar * var_i;
            }
          }
        }
      };

      const int64 cost = 1000;
      auto worker_threads = *(ctx->device()->tensorflow_cpu_worker_threads());
      Shard(worker_threads.num_threads, worker_threads.workers, N, cost, DoWork);
      if (has_counts && !indices_as_pointer) {
        const Tensor& indices_counts = ctx->input(13);
        var->UpdateCache(indices, indices_counts);
      }
    }
  }

 private:
  bool use_exclusive_lock_;
};

#define REGISTER_KERNELS(T, Tindices)                                 \
  REGISTER_KERNEL_BUILDER(Name("KvResourceSparseApplyAdamW")          \
                              .Device(DEVICE_CPU)                     \
                              .TypeConstraint<T>("T")                 \
                              .TypeConstraint<Tindices>("Tindices"),  \
                          KvSparseApplyAdamWOp<CPUDevice, T, Tindices, false, false>);\
  REGISTER_KERNEL_BUILDER(Name("_OPT_KvResourceSparseApplyAdamW")     \
                              .Device(DEVICE_CPU)                     \
                              .TypeConstraint<T>("T")                 \
                              .TypeConstraint<Tindices>("Tindices"),  \
                          KvSparseApplyAdamWOp<CPUDevice, T, Tindices, true, false>);\
  REGISTER_KERNEL_BUILDER(Name("KvResourceSparseApplyAdamWWithCounts")          \
                              .Device(DEVICE_CPU)                     \
                              .TypeConstraint<T>("T")                 \
                              .TypeConstraint<Tindices>("Tindices"),  \
                          KvSparseApplyAdamWOp<CPUDevice, T, Tindices, false, true>);\
  REGISTER_KERNEL_BUILDER(Name("_OPT_KvResourceSparseApplyAdamWWithCounts")     \
                              .Device(DEVICE_CPU)                     \
                              .TypeConstraint<T>("T")                 \
                              .TypeConstraint<Tindices>("Tindices"),  \
                          KvSparseApplyAdamWOp<CPUDevice, T, Tindices, true, true>);

#define REGISTER_CPU_KERNELS(T) \
  REGISTER_KERNELS(T, int32);   \
  REGISTER_KERNELS(T, int64);

TF_CALL_float(REGISTER_CPU_KERNELS);

#undef REGISTER_CPU_KERNELS
#undef REGISTER_KERNELS

#if GOOGLE_CUDA
template <typename Device, typename T, typename Tindex,
          bool indices_as_pointer, bool has_counts>
class KvSparseApplyAdamWGPUOp : public OpKernel {
 public:
  explicit KvSparseApplyAdamWGPUOp(OpKernelConstruction* ctx) : OpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("use_locking", &use_exclusive_lock_));

    int num_worker_threads = ctx->device()
                             ->tensorflow_cpu_worker_threads()
                             ->num_threads;
    thread_copy_id_alloc_.reset(
        new IntraThreadCopyIdAllocator(num_worker_threads));
  }

  void ApplyGradients(
      EmbeddingVar<Tindex, T>* var,
      EmbeddingVar<Tindex, T>* m,
      EmbeddingVar<Tindex, T>* v,
      T** var_ptr, T** m_ptr, T** v_ptr,
      T alpha, T beta1, T beta2,
      T epsilon, T weight_decay,
      const T* grad_base,
      const int64 task_size,
      se::Stream* stream,
      EventMgr* event_mgr,
      const Eigen::GpuDevice& gpu_device) {
    // Send pointers of embeddings to GPU
    T** dev_var_ptr = (T**)var->GetBuffer(task_size * 3);
    T** dev_m_ptr = dev_var_ptr + task_size;
    T** dev_v_ptr = dev_m_ptr + task_size;
    CHECK(dev_var_ptr);
    CHECK(dev_m_ptr);
    CHECK(dev_v_ptr);

    DeviceMemoryBase dst_ptr(dev_var_ptr, sizeof(T*) * task_size * 3);
    stream->ThenMemcpy(&dst_ptr, var_ptr, sizeof(T*) * task_size * 3);

    int block_size = 128;
    int embedding_dim = var->ValueLen();
    functor::KvSparseApplyAdamWHbm<GPUDevice, Tindex, T>()(
            block_size, embedding_dim,
            dev_var_ptr, dev_m_ptr, dev_v_ptr, grad_base,
            alpha, beta1, beta2, epsilon, weight_decay,
            task_size, gpu_device);
    SyncWithEventMgr(stream, event_mgr);
  }

  void Compute(OpKernelContext* ctx) override NO_THREAD_SAFETY_ANALYSIS {
    auto locks = MaybeLockEmbeddingVariableInputMutexesInOrder<Tindex, T>(ctx, use_exclusive_lock_,
                                                      {0, 1, 2});
    EmbeddingVar<Tindex, T>* var = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 0, &var));
    core::ScopedUnref unref_var(var);

    EmbeddingVar<Tindex, T>* m = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 1, &m));
    core::ScopedUnref unref_m(m);

    EmbeddingVar<Tindex, T>* v = nullptr;
    OP_REQUIRES_OK(ctx, GetInputEmbeddingVar(ctx, 2, &v));
    core::ScopedUnref unref_v(v);

    const Tensor& beta1_power = ctx->input(3);
    const Tensor& beta2_power = ctx->input(4);
    const Tensor& lr = ctx->input(5);
    const Tensor& beta1 = ctx->input(6);
    const Tensor& beta2 = ctx->input(7);
    const Tensor& epsilon = ctx->input(8);
    const Tensor& grad = ctx->input(9);
    const Tensor& indices = ctx->input(10);
    const Tensor& global_step = ctx->input(11);
    const Tensor& weight_decay = ctx->input(12);

    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(beta1_power.shape()),
        errors::InvalidArgument("beta1_power is not a scalar: ",
                                beta1_power.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(beta2_power.shape()),
        errors::InvalidArgument("beta2_power is not a scalar: ",
                                beta2_power.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(lr.shape()),
        errors::InvalidArgument("lr is not a scalar: ",
                                lr.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(beta1.shape()),
        errors::InvalidArgument("beta1 is not a scalar: ",
                                beta1.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(beta2.shape()),
        errors::InvalidArgument("beta2 is not a scalar: ",
                                beta2.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsScalar(epsilon.shape()),
        errors::InvalidArgument("epsilon is not a scalar: ",
                                epsilon.shape().DebugString()));
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsVector(indices.shape()),
        errors::InvalidArgument("indices must be one-dimensional"));

    int64 inner_dim = 1;
    TensorShape var_shape({var->ValueLen()});
    for (int d = 0; d < var_shape.dims(); d++) {
      OP_REQUIRES(ctx, var_shape.dim_size(d) == grad.dim_size(d + 1),
                  errors::InvalidArgument(strings::StrCat(
                      "var and grad must match in dimension ", d + 1)));
      inner_dim *= grad.dim_size(d + 1);
    }
    OP_REQUIRES(
        ctx, inner_dim > 0,
        errors::InvalidArgument(
            "Inner dimension should be greater than zero."));

    OP_REQUIRES(
      ctx, IsLegacyScalar(global_step.shape()),
      errors::InvalidArgument(
        "global_step is not a scalar: ", global_step.shape().DebugString()));

    const int64 N = indices.dim_size(0);
    OP_REQUIRES(
        ctx, grad.dim_size(0) == N,
        errors::InvalidArgument(
            "grad must be the same size as indices in the first dimension."));
    
    OP_REQUIRES(ctx, !var->IsSingleHbm(), errors::InvalidArgument(
        "AdamW optimizer doesn't support EV with single-level HBM storage."));

    if (N > 0) {
      if (inner_dim > 0) {
        auto indices_flat = indices.flat<Tindex>();
        auto grad_flat = grad.flat_outer_dims<T>();
        int64 gs = global_step.scalar<int64>()();
        T beta1_power_scalar = beta1_power.scalar<T>()();
        T beta2_power_scalar = beta2_power.scalar<T>()();
        T lr_scalar = lr.scalar<T>()();
        T beta1_scalar = beta1.scalar<T>()();
        T beta2_scalar = beta2.scalar<T>()();
        T epsilon_scalar = epsilon.scalar<T>()();
        T weight_decay_scalar = weight_decay.scalar<T>()();
        const T alpha = lr_scalar *
            Eigen::numext::sqrt(static_cast<T>(1) - beta2_power_scalar) /
            (static_cast<T>(1) - beta1_power_scalar);

        // Lookup ValuePtrs of ids and set version of each id  in parallel
        int counts_index = has_counts ? 13 : -1;
        T** var_ptr = new T*[N * 3];
        T** m_ptr = var_ptr + N;
        T** v_ptr = m_ptr + N;
        std::vector<std::pair<EmbeddingVar<Tindex, T>*, T**>> vars(3);
        vars[0] = std::pair<EmbeddingVar<Tindex, T>*, T**>(var, var_ptr);
        vars[1] = std::pair<EmbeddingVar<Tindex, T>*, T**>(m, m_ptr);
        vars[2] = std::pair<EmbeddingVar<Tindex, T>*, T**>(v, v_ptr);
        GetEmbeddingPointers(ctx, vars, indices_flat.data(),
                             gs, indices_as_pointer,
                             counts_index, N, thread_copy_id_alloc_.get());

        auto stream = ctx->op_device_context()->stream();
        auto event_mgr = ctx->device()->tensorflow_gpu_device_info()->event_mgr;
        ApplyGradients(
            var, m, v, var_ptr,
            m_ptr, v_ptr, alpha,
            beta1_scalar, beta2_scalar,
            epsilon_scalar, weight_decay_scalar,
            &grad_flat(0), N,
            stream, event_mgr,
            ctx->eigen_gpu_device());

        if (has_counts && !indices_as_pointer) {
          const Tensor& counts_tensor = ctx->input(counts_index);
          var->UpdateCache(indices, counts_tensor);
        }

        delete[] var_ptr;
      }
    }
  }

 private:
  bool use_exclusive_lock_;
  std::unique_ptr<IntraThreadCopyIdAllocator> thread_copy_id_alloc_;
};

#define REGISTER_KERNELS(T, Tindices)                                 \
  REGISTER_KERNEL_BUILDER(Name("KvResourceSparseApplyAdamW")             \
                              .Device(DEVICE_GPU)                     \
                              .HostMemory("indices")                 \
                              .HostMemory("lr")                      \
                              .HostMemory("beta1_power")             \
                              .HostMemory("beta2_power")             \
                              .HostMemory("beta1")                   \
                              .HostMemory("beta2")                   \
                              .HostMemory("epsilon")                 \
                              .HostMemory("global_step")             \
                              .HostMemory("weight_decay")            \
                              .TypeConstraint<T>("T")                 \
                              .TypeConstraint<Tindices>("Tindices"),  \
                          KvSparseApplyAdamWGPUOp<GPUDevice, T, Tindices, false, false>); \
  REGISTER_KERNEL_BUILDER(Name("_OPT_KvResourceSparseApplyAdamW")             \
                              .Device(DEVICE_GPU)                     \
                              .HostMemory("indices")                 \
                              .HostMemory("lr")                      \
                              .HostMemory("beta1_power")             \
                              .HostMemory("beta2_power")             \
                              .HostMemory("beta1")                   \
                              .HostMemory("beta2")                   \
                              .HostMemory("epsilon")                 \
                              .HostMemory("global_step")             \
                              .HostMemory("weight_decay")            \
                              .TypeConstraint<T>("T")                 \
                              .TypeConstraint<Tindices>("Tindices"),  \
                          KvSparseApplyAdamWGPUOp<GPUDevice, T, Tindices, true, false>);\
  REGISTER_KERNEL_BUILDER(Name("KvResourceSparseApplyAdamWWithCounts")             \
                              .Device(DEVICE_GPU)                     \
                              .HostMemory("indices")                 \
                              .HostMemory("lr")                      \
                              .HostMemory("beta1_power")             \
                              .HostMemory("beta2_power")             \
                              .HostMemory("beta1")                   \
                              .HostMemory("beta2")                   \
                              .HostMemory("epsilon")                 \
                              .HostMemory("global_step")             \
                              .HostMemory("weight_decay")            \
                              .HostMemory("indices_counts")           \
                              .TypeConstraint<T>("T")                 \
                              .TypeConstraint<Tindices>("Tindices"),  \
                          KvSparseApplyAdamWGPUOp<GPUDevice, T, Tindices, false, true>); \
  REGISTER_KERNEL_BUILDER(Name("_OPT_KvResourceSparseApplyAdamWWithCounts")             \
                              .Device(DEVICE_GPU)                     \
                              .HostMemory("indices")                 \
                              .HostMemory("lr")                      \
                              .HostMemory("beta1_power")             \
                              .HostMemory("beta2_power")             \
                              .HostMemory("beta1")                   \
                              .HostMemory("beta2")                   \
                              .HostMemory("epsilon")                 \
                              .HostMemory("global_step")             \
                              .HostMemory("weight_decay")            \
                              .HostMemory("indices_counts")           \
                              .TypeConstraint<T>("T")                 \
                              .TypeConstraint<Tindices>("Tindices"),  \
                          KvSparseApplyAdamWGPUOp<GPUDevice, T, Tindices, true, true>);
#define REGISTER_GPU_KERNELS(T) \
  REGISTER_KERNELS(T, int32);   \
  REGISTER_KERNELS(T, int64);

TF_CALL_float(REGISTER_GPU_KERNELS);

#undef REGISTER_GPU_KERNELS
#undef REGISTER_KERNELS
#endif  // GOOGLE_CUDA

}  // namespace tensorflow
