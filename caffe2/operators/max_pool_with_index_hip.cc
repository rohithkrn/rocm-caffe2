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

#include "caffe2/operators/max_pool_with_index_hip.h"
#include "caffe2/utils/conversions.h"
#include "hip/hip_runtime.h"

namespace caffe2 {

namespace {
template <typename Dtype>
__global__ void MaxPoolForward(const int nthreads,
                               const Dtype* const bottom_data,
                               const int num,
                               const int channels,
                               const int height,
                               const int width,
                               const int pooled_height,
                               const int pooled_width,
                               const int kernel_h,
                               const int kernel_w,
                               const int stride_h,
                               const int stride_w,
                               const int pad_h,
                               const int pad_w,
                               Dtype* const top_data,
                               int* mask)
{
    HIP_1D_KERNEL_LOOP(index, nthreads)
    {
        const int pw                    = index % pooled_width;
        const int ph                    = (index / pooled_width) % pooled_height;
        const int c                     = (index / pooled_width / pooled_height) % channels;
        const int n                     = index / pooled_width / pooled_height / channels;
        int hstart                      = ph * stride_h - pad_h;
        int wstart                      = pw * stride_w - pad_w;
        const int hend                  = min(hstart + kernel_h, height);
        const int wend                  = min(wstart + kernel_w, width);
        hstart                          = max(hstart, 0);
        wstart                          = max(wstart, 0);
        float maxval                    = -FLT_MAX;
        int maxidx                      = -1;
        const Dtype* const bottom_slice = bottom_data + (n * channels + c) * height * width;
        for(int h = hstart; h < hend; ++h)
        {
            for(int w = wstart; w < wend; ++w)
            {
                if(convert::To<Dtype, float>(bottom_slice[h * width + w]) > maxval)
                {
                    maxidx = h * width + w;
                    maxval = convert::To<Dtype, float>(bottom_slice[maxidx]);
                }
            }
        }
        top_data[index] = convert::To<float, Dtype>(maxval);
        mask[index]     = maxidx;
    }
}

template <typename Dtype>
__global__ void MaxPoolBackward(const int nthreads,
                                const Dtype* const top_diff,
                                const int* const mask,
                                const int num,
                                const int channels,
                                const int height,
                                const int width,
                                const int pooled_height,
                                const int pooled_width,
                                const int kernel_h,
                                const int kernel_w,
                                const int stride_h,
                                const int stride_w,
                                const int pad_h,
                                const int pad_w,
                                Dtype* const bottom_diff)
{
    HIP_1D_KERNEL_LOOP(index, nthreads)
    {
        // find out the local index
        // find out the local offset
        const int w       = index % width;
        const int h       = (index / width) % height;
        const int c       = (index / width / height) % channels;
        const int n       = index / width / height / channels;
        const int phstart = (h + pad_h < kernel_h) ? 0 : (h + pad_h - kernel_h) / stride_h + 1;
        const int phend   = min((h + pad_h) / stride_h + 1, pooled_height);
        const int pwstart = (w + pad_w < kernel_w) ? 0 : (w + pad_w - kernel_w) / stride_w + 1;
        const int pwend   = min((w + pad_w) / stride_w + 1, pooled_width);
        float gradient    = 0;
        const int offset  = (n * channels + c) * pooled_height * pooled_width;
        const Dtype* const top_diff_slice = top_diff + offset;
        const int* const mask_slice       = mask + offset;

        for(int ph = phstart; ph < phend; ++ph)
        {
            for(int pw = pwstart; pw < pwend; ++pw)
            {
                if(mask_slice[ph * pooled_width + pw] == h * width + w)
                {
                    gradient += convert::To<Dtype, float>(top_diff_slice[ph * pooled_width + pw]);
                }
            }
        }
        bottom_diff[index] = convert::To<float, Dtype>(gradient);
    }
}
};

template <typename T>
bool MaxPoolWithIndexOp::DoRunWithType()
{
    auto& X    = Input(0);
    auto* Y    = Output(0);
    auto* mask = Output(1);

    ConvPoolOpBase<HIPContext>::SetOutputSize(X, Y, X.dim32(1));
    int output_size = Y->size();
    mask->Resize(output_size);

    hipLaunchKernelGGL((MaxPoolForward<T>),
                       dim3(CAFFE_GET_BLOCKS(output_size)),
                       dim3(CAFFE_HIP_NUM_THREADS),
                       0,
                       context_.hip_stream(),
                       output_size,
                       X.data<T>(),
                       X.dim32(0),
                       X.dim32(1),
                       X.dim32(2),
                       X.dim32(3),
                       Y->dim32(2),
                       Y->dim32(3),
                       kernel_h(),
                       kernel_w(),
                       stride_h(),
                       stride_w(),
                       pad_t(),
                       pad_l(),
                       Y->mutable_data<T>(),
                       mask->mutable_data<int>());
    return true;
}

bool MaxPoolWithIndexOp::RunOnDevice()
{
    auto& X = Input(0);

    CAFFE_ENFORCE(X.ndim() == 4, "Operator only supports 4D tensors");

    if(X.IsType<float>())
    {
        return DoRunWithType<float>();
    }
    else if(X.IsType<float16>())
    {
        return DoRunWithType<float16>();
    }
    else
    {
        CAFFE_THROW("Unsupported input type");
    }
}

template <typename T>
bool MaxPoolWithIndexGradientOp::DoRunWithType()
{
    auto& X    = Input(0);
    auto& dY   = Input(1);
    auto& mask = Input(2);
    auto* dX   = Output(0);

    CAFFE_ENFORCE(X.ndim() == 4, "Operator only supports 4D tensors");

    dX->ResizeLike(X);
    ConvPoolOpBase<HIPContext>::ComputePads(vector<int>{X.dim32(2), X.dim32(3)});

    hipLaunchKernelGGL((MaxPoolBackward<T>),
                       dim3(CAFFE_GET_BLOCKS(X.size())),
                       dim3(CAFFE_HIP_NUM_THREADS),
                       0,
                       context_.hip_stream(),
                       X.size(),
                       dY.data<T>(),
                       mask.data<int>(),
                       X.dim32(0),
                       X.dim32(1),
                       X.dim32(2),
                       X.dim32(3),
                       dY.dim32(2),
                       dY.dim32(3),
                       kernel_h(),
                       kernel_w(),
                       stride_h(),
                       stride_w(),
                       pad_t(),
                       pad_l(),
                       dX->template mutable_data<T>());
    return true;
}

bool MaxPoolWithIndexGradientOp::RunOnDevice()
{
    auto& X = Input(0);

    if(X.IsType<float>())
    {
        return DoRunWithType<float>();
    }
    else if(X.IsType<float16>())
    {
        return DoRunWithType<float16>();
    }
    else
    {
        CAFFE_THROW("Unsupported input type");
    }
}

namespace {

REGISTER_HIP_OPERATOR(MaxPoolWithIndex, MaxPoolWithIndexOp);
REGISTER_HIP_OPERATOR(MaxPoolWithIndexGradient, MaxPoolWithIndexGradientOp);

class GetMaxPoolWithIndexGradient : public GradientMakerBase
{
    using GradientMakerBase::GradientMakerBase;
    vector<OperatorDef> GetGradientDefs() override
    {
        return SingleGradientDef("MaxPoolWithIndexGradient",
                                 "",
                                 vector<string>{I(0), GO(0), O(1)},
                                 vector<string>{GI(0)});
    }
};

REGISTER_GRADIENT(MaxPoolWithIndex, GetMaxPoolWithIndexGradient);

OPERATOR_SCHEMA(MaxPoolWithIndexGradient);

OPERATOR_SCHEMA(MaxPoolWithIndex)
    .NumInputs(1)
    .NumOutputs(2)
    .TensorInferenceFunction(ConvPoolOpBase<CPUContext>::TensorInferenceForPool)
    .SetDoc(R"DOC(
    MaxPoolWithIndex consumes an input blob X and applies max pooling across the
    blob according to kernel sizes, stride sizes and pad lengths defined by the
    ConvPoolOpBase operator. It also produces an explicit mask that defines the
    location that all maximum values were found, which is re-used in the
    gradient pass. This op is deterministic.
  )DOC")
    .Input(0,
           "X",
           "Input data tensor from the previous operator; dimensions "
           "depend on whether the NCHW or NHWC operators are being used. For "
           "example, in the former, the input has size (N x C x H x W), where N is"
           " the batch size, C is the number of channels, and H and W are the "
           "height and the width of the data. The corresponding permutation of "
           "dimensions is used in the latter case. ")
    .Output(0,
            "Y",
            "Output data tensor from average pooling across the input "
            "tensor. Dimensions will vary based on various kernel, stride, and pad "
            "sizes.")
    .Output(1,
            "Index",
            "Mask of location indices of the found maximum values, "
            " used in the gradient operator to accumulate dY values to the "
            "appropriate locations in Y");
};

}; // namespace caffe2
