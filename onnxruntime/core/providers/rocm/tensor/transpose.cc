// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/providers/cpu/tensor/utils.h"
#include "core/providers/rocm/shared_inc/fpgeneric.h"
#include "core/providers/rocm/tensor/transpose.h"
#include "core/providers/rocm/tensor/transpose_impl.h"

namespace onnxruntime {
namespace rocm {

ONNX_OPERATOR_VERSIONED_KERNEL_EX(Transpose,
                        kOnnxDomain,
                        1, 12,
                        kRocmExecutionProvider,
                        KernelDefBuilder()
                            .TypeConstraint("T", DataTypeImpl::AllFixedSizeTensorTypes()),
                        Transpose);

ONNX_OPERATOR_KERNEL_EX(Transpose,
                        kOnnxDomain,
                        13,
                        kRocmExecutionProvider,
                        KernelDefBuilder()
                            .TypeConstraint("T", DataTypeImpl::AllFixedSizeTensorTypes()),
                        Transpose);

// special case acceleration using rocblas matrix transpose
static std::tuple<int, int> TryTransposeWithRocblas(const std::vector<size_t>& perm, const TensorShape& input_shape) {
  int M = 0;
  int N = 0;

  if (perm.size() == 4 && input_shape[0] == 1 && perm[0] == 0) {
    // NCHW <-> NHWC when N == 1
    if ((perm[1] == 2 && perm[2] == 3 && perm[3] == 1) ||
        (perm[1] == 3 && perm[2] == 1 && perm[3] == 2)) {
      if (perm[1] == 2) {
        M = gsl::narrow<int>(input_shape[1]);
        N = gsl::narrow<int>(input_shape[2] * input_shape[3]);
      } else {
        M = gsl::narrow<int>(input_shape[1] * input_shape[2]);
        N = gsl::narrow<int>(input_shape[3]);
      }
    }
  } else if (perm.size() == 2 && perm[1] == 0 && perm[0] == 1) {
    // 2D matrix transpose
    M = gsl::narrow<int>(input_shape[0]);
    N = gsl::narrow<int>(input_shape[1]);
  }

  return std::make_tuple(M, N);
}

template <typename T>
Status TransposeWithRocblas(rocblas_handle handle, const Tensor& input, Tensor& output, int M, int N) {
  typedef typename ToHipType<T>::MappedType HipT;
  HipT one = ToHipType<T>::FromFloat(1.0f);
  HipT zero = ToHipType<T>::FromFloat(0.0f);
  const HipT* input_data = reinterpret_cast<const HipT*>(input.Data<T>());
  HipT* output_data = reinterpret_cast<HipT*>(output.MutableData<T>());
  ROCBLAS_RETURN_IF_ERROR(
      rocblasTransposeHelper(handle,
                            rocblas_operation_transpose, rocblas_operation_transpose, M, N,
                            &one,
                            input_data,
                            N,
                            &zero,
                            input_data,
                            N,
                            output_data,
                            M));
  return Status::OK();
}

Status Transpose::DoTranspose(const Transpose& kernel,
                              const std::vector<size_t>& permutations, const Tensor& input, Tensor& output) {
  // special case when there is a dim value of 0 in the shape.
  if (output.Shape().Size() == 0)
    return Status::OK();

  auto element_type = input.GetElementType();
  if (element_type == utils::GetONNXTensorElementDataType<float>() ||
      element_type == utils::GetONNXTensorElementDataType<double>() ||
      element_type == utils::GetONNXTensorElementDataType<MLFloat16>()) {
    auto mn = TryTransposeWithRocblas(permutations, input.Shape());
    int M = std::get<0>(mn);
    int N = std::get<1>(mn);
    if (M != 0 && N != 0) {
      if (element_type == utils::GetONNXTensorElementDataType<float>()) {
        return TransposeWithRocblas<float>(kernel.RocblasHandle(), input, output, M, N);
      } else if (element_type == utils::GetONNXTensorElementDataType<double>()) {
        return TransposeWithRocblas<double>(kernel.RocblasHandle(), input, output, M, N);
      } else {
        return TransposeWithRocblas<MLFloat16>(kernel.RocblasHandle(), input, output, M, N);
      }
    }
  }

  const std::vector<int64_t>& input_dims = input.Shape().GetDims();
  const std::vector<int64_t>& output_dims = output.Shape().GetDims();

  auto rank = static_cast<int32_t>(input_dims.size());
  TensorPitches original_input_strides(input_dims);
  TensorPitches original_output_strides(output_dims);

  // flatten the adjacent dimensions which are contiguous
  // for example: permutations[0, 2, 3, 1] -> [0, 2, 1], permutations[0, 3, 1, 2] -> [0, 2, 1]
  auto new_rank = rank;
  std::vector<size_t> new_permutations(permutations);
  std::vector<int64_t> new_input_dims(input_dims);
  std::vector<int64_t> new_output_dims(output_dims);

  for (auto i = rank - 1; i > 0; i--) {
    auto curr = new_permutations[i];
    auto prev = new_permutations[i - 1];
    if (prev + 1 == curr) {
      // all dims bigger than curr need to be reduced by 1 due to the merging.
      for (auto j = 0; j < new_rank; j++) {
        if (new_permutations[j] > curr) {
          new_permutations[j] -= 1;
        }
      }
      for (auto j = i+1; j < new_rank; j++) {
        new_permutations[j-1] = new_permutations[j];
      }

      // update input dims
      new_input_dims[prev] *= new_input_dims[curr];
      new_input_dims[curr] = 1;
      for (auto j = static_cast<int32_t>(curr+1); j < new_rank; j++) {
        new_input_dims[j-1] = new_input_dims[j];
      }
      new_input_dims[new_rank-1] = 1;

      // update output dims
      new_output_dims[i-1] *= new_output_dims[i];
      new_output_dims[i] = 1;
      for (auto j = i+1; j < new_rank; j++) {
        new_output_dims[j-1] = new_output_dims[j];
      }
      new_output_dims[new_rank-1] = 1;

      new_rank--;
    }
  }
  new_permutations.resize(new_rank);
  new_input_dims.resize(new_rank);
  new_output_dims.resize(new_rank);

  TensorPitches new_input_strides(new_input_dims);
  TensorPitches new_output_strides(new_output_dims);

  // TArray<int64_t> input_strides(rank);
  // for (auto i = 0; i < rank; i++) {
  //   input_strides[i] = original_input_strides[permutations[i]];
  // }

  // TArray<fast_divmod> output_strides(rank);
  // for (auto i = 0; i < rank; i++) {
  //   output_strides[i] = fast_divmod(gsl::narrow_cast<int>(original_output_strides[i]));
  // }

  size_t element_size = input.DataType()->Size();
  std::vector<int64_t> input_shape(new_rank);
  std::vector<int64_t> tmp_input_strides(new_rank);
  std::vector<int64_t> tmp_output_strides(new_rank);  
  for (auto i = 0; i < new_rank; i++) {
    input_shape[i] = new_input_dims[i];    
    tmp_input_strides[i] = new_input_strides[i];
    tmp_output_strides[i] = new_output_strides[new_permutations[i]];    
  }

  if (CanDoTranspose3D(new_rank, new_input_dims, new_permutations)) {
    return Transpose3DImpl(kernel, element_size, input_shape, tmp_input_strides,
                           input.DataRaw(), output.MutableDataRaw(), output.Shape().Size());

  } else if (CanDoTranspose4D(kernel.GetDeviceProp(), element_size, new_rank, new_input_dims, new_permutations)) {
    return Transpose4DImpl(kernel, element_size, input_shape, tmp_input_strides, input.DataRaw(),
                           tmp_output_strides, output.MutableDataRaw(), output.Shape().Size());
  } 

  RocmAsyncBuffer<int64_t> input_strides(&kernel, new_rank);
  for (auto i = 0; i < new_rank; i++) {
    input_strides.CpuPtr()[i] = new_input_strides[new_permutations[i]];
  }

  RocmAsyncBuffer<fast_divmod> output_strides(&kernel, new_rank);
  ORT_ENFORCE(CalculateFdmStrides(output_strides.CpuSpan(), new_output_dims));

  // TODO: use output shape in reverse order for uint24 math
  // for (auto i = 0; i < rank; i++) {
  //   output_strides.CpuPtr()[i] = output_dims[rank - 1 - i];
  //   if (output_dims[rank-1-i] > 0x7FFFFF) {
  //     printf("shape size is: %lx\n", output_dims[rank-1-i]);
  //   }
  // }
  ORT_RETURN_IF_ERROR(input_strides.CopyToGpu());
  ORT_RETURN_IF_ERROR(output_strides.CopyToGpu());

  auto status = TransposeImpl(element_size, new_rank, input_strides.GpuPtr(), input.DataRaw(),
                              output_strides.GpuPtr(), output.MutableDataRaw(), output.Shape().Size());

  return status;
}

Status Transpose::ComputeInternal(OpKernelContext* ctx) const {
  const Tensor* X_ptr = ctx->Input<Tensor>(0);
  if (X_ptr == nullptr) return Status(common::ONNXRUNTIME, common::FAIL, "input count mismatch");
  const Tensor& X = *X_ptr;
  const TensorShape& input_shape = X.Shape();
  const std::vector<int64_t>& input_dims = input_shape.GetDims();
  int32_t rank = gsl::narrow_cast<int32_t>(input_dims.size());

  std::vector<int64_t> output_dims(rank);
  std::vector<size_t> default_perm(rank);
  const std::vector<size_t>* p_perm = nullptr;
  const auto& status = ComputeOutputShape(X, output_dims, default_perm, p_perm);
  if (!status.IsOK())
    return status;

  TensorShape output_shape{output_dims};
  Tensor* Y = ctx->Output(0, output_shape);

  return DoTranspose(*this, *p_perm, X, *Y);
}

}  // namespace rocm
}  // namespace onnxruntime