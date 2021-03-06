/**
 * Copyright (c) 2016-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "hip/hip_runtime.h"
#include "caffe2/core/context_hip.h"
#include "caffe2/operators/one_hot_ops.h"

namespace caffe2 {

__global__ void OneHotOpKernel(const TIndex batch_size,
                               const TIndex index_size,
                               const TIndex* indices,
                               float* output)
{
    HIP_1D_KERNEL_LOOP(i, batch_size) { output[i * index_size + indices[i]] = 1.; }
}

template <>
void OneHotOp<HIPContext>::DoOneHotOp(TIndex batch_size,
                                      TIndex index_size,
                                      const Tensor<HIPContext>& indices,
                                      Tensor<HIPContext>* output)
{
    float* output_ptr = output->mutable_data<float>();
    math::Set<float, HIPContext>(output->size(), 0., output_ptr, &context_);
    hipLaunchKernelGGL((OneHotOpKernel),
                       dim3(CAFFE_GET_BLOCKS(batch_size)),
                       dim3(CAFFE_HIP_NUM_THREADS),
                       0,
                       context_.hip_stream(),
                       batch_size,
                       index_size,
                       indices.data<TIndex>(),
                       output_ptr);
}

REGISTER_HIP_OPERATOR(OneHot, OneHotOp<HIPContext>);
} // namespace
