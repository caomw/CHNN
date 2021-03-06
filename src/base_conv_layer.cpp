#include <algorithm>
#include <vector>

#include "base_conv_layer.hpp"
#include "math_functions.hpp"
#include "logging.hpp"

namespace caffe {

template <typename Dtype>
void BaseConvolutionLayer<Dtype>::LayerSetUp(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  // Configure the kernel size, padding, stride, and inputs.
  ConvolutionParameter conv_param = this->layer_param_.convParam;
  channel_axis_ = 1;  // bottom[0]->CanonicalAxisIndex(1);//ԭ����conv_param.axis()
  const int first_spatial_axis = channel_axis_ + 1;
  const int num_axes = 4; // bottom[0]->num_axes();
  num_spatial_axes_ = num_axes - first_spatial_axis;
  CHECK_GE(num_spatial_axes_, 0);
  vector<int> bottom_dim_blob_shape(1, num_spatial_axes_ + 1);
  vector<int> spatial_dim_blob_shape(1, std::max(num_spatial_axes_, 1));
  // Setup filter kernel dimensions (kernel_shape_).
  kernel_shape_.Reshape(spatial_dim_blob_shape);
  int* kernel_shape_data = kernel_shape_.mutable_cpu_data();

    const int num_kernel_dims = 1;
      for (int i = 0; i < num_spatial_axes_; ++i) {
        kernel_shape_data[i] =
            conv_param.kernel_size();
      }
  for (int i = 0; i < num_spatial_axes_; ++i) {
    CHECK_GT(kernel_shape_data[i], 0) << "Filter dimensions must be nonzero.";
  }
  // Setup stride dimensions (stride_).
  stride_.Reshape(spatial_dim_blob_shape);
  int* stride_data = stride_.mutable_cpu_data();
    const int num_stride_dims = 1;
    const int kDefaultStride = 1;
    for (int i = 0; i < num_spatial_axes_; ++i) {
      stride_data[i] = (num_stride_dims == 0) ? kDefaultStride : conv_param.stride();
      CHECK_GT(stride_data[i], 0) << "Stride dimensions must be nonzero.";
    }
  // Setup pad dimensions (pad_).
  pad_.Reshape(spatial_dim_blob_shape);
  int* pad_data = pad_.mutable_cpu_data();

    const int num_pad_dims = 1;
    const int kDefaultPad = 0;
    for (int i = 0; i < num_spatial_axes_; ++i) {
      pad_data[i] = (num_pad_dims == 0) ? kDefaultPad : conv_param.pad();
    }

	// Setup dilation dimensions (dilation_).
	dilation_.Reshape(spatial_dim_blob_shape);
	int* dilation_data = dilation_.mutable_cpu_data();
	const int num_dilation_dims = 1;//vertex and horizontal have the same dillation
	const int kDefaultDilation = 1;
	for (int i = 0; i < num_spatial_axes_; ++i) {
		dilation_data[i] = (num_dilation_dims == 0) ? kDefaultDilation :
			conv_param.dilation();
	}
	// Special case: im2col is the identity for 1x1 convolution with stride 1
	// and no padding, so flag for skipping the buffer and transformation.
	is_1x1_ = true;
	for (int i = 0; i < num_spatial_axes_; ++i) {
		is_1x1_ &=
			kernel_shape_data[i] == 1 && stride_data[i] == 1 && pad_data[i] == 0;
		if (!is_1x1_) { break; }
	}
	// Configure output channels and groups.
	channels_ = bottom[0]->shape(channel_axis_);
	num_output_ = conv_param.output();
	CHECK_GT(num_output_, 0);

	conv_out_channels_ = num_output_;
	conv_in_channels_ = channels_;

	// Handle the parameters: weights and biases.
	// - blobs_[0] holds the filter weights
	// - blobs_[1] holds the biases (optional)
	vector<int> weight_shape(2);
	weight_shape[0] = conv_out_channels_;
	weight_shape[1] = conv_in_channels_;
	for (int i = 0; i < num_spatial_axes_; ++i) {
		weight_shape.push_back(kernel_shape_data[i]);
	}
	kernel_dim_ = this->blobs_[0]->count(1);
	weight_offset_ = conv_out_channels_ * kernel_dim_;
}

template <typename Dtype>
void BaseConvolutionLayer<Dtype>::Reshape(const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top)
{
	const int first_spatial_axis = channel_axis_ + 1;
	CHECK_EQ(bottom[0]->num_axes(), first_spatial_axis + num_spatial_axes_)
		<< "bottom num_axes may not change.";
	num_ = bottom[0]->count(0, channel_axis_);
	CHECK_EQ(bottom[0]->shape(channel_axis_), channels_)
		<< "Input size incompatible with convolution kernel.";
	// TODO: generalize to handle inputs of different shapes.
	for (int bottom_id = 1; bottom_id < bottom.size(); ++bottom_id) {
		CHECK(bottom[0]->shape() == bottom[bottom_id]->shape())
			<< "All inputs must have the same shape.";
	}
	// Shape the tops.
	bottom_shape_ = &bottom[0]->shape();
	compute_output_shape();
	vector<int> top_shape(bottom[0]->shape().begin(),
		bottom[0]->shape().begin() + channel_axis_);
	top_shape.push_back(num_output_);
	for (int i = 0; i < num_spatial_axes_; ++i) {
		top_shape.push_back(output_shape_[i]);
	}
	for (int top_id = 0; top_id < top.size(); ++top_id) {
		top[top_id]->Reshape(top_shape);
	}
	conv_out_spatial_dim_ = top[0]->count(first_spatial_axis);
	col_offset_ = kernel_dim_ * conv_out_spatial_dim_;
	output_offset_ = conv_out_channels_ * conv_out_spatial_dim_;
	// Setup input dimensions (conv_input_shape_).
	vector<int> bottom_dim_blob_shape(1, num_spatial_axes_ + 1);
	conv_input_shape_.Reshape(bottom_dim_blob_shape);
	int* conv_input_shape_data = conv_input_shape_.mutable_cpu_data();
	for (int i = 0; i < num_spatial_axes_ + 1; ++i) {
		conv_input_shape_data[i] = bottom[0]->shape(channel_axis_ + i);
	}
	// The im2col result buffer will only hold one image at a time to avoid
	// overly large memory usage. In the special case of 1x1 convolution
	// it goes lazily unused to save memory.
	col_buffer_shape_.clear();
	col_buffer_shape_.push_back(kernel_dim_);
	for (int i = 0; i < num_spatial_axes_; ++i) {
		col_buffer_shape_.push_back(output_shape_[i]);
	}
	col_buffer_.Reshape(col_buffer_shape_);
	bottom_dim_ = bottom[0]->count(channel_axis_);
	top_dim_ = top[0]->count(channel_axis_);
	num_kernels_im2col_ = conv_in_channels_ * conv_out_spatial_dim_;
	num_kernels_col2im_ = bottom_dim_;
	// Set up the all ones "bias multiplier" for adding biases by BLAS
	out_spatial_dim_ = top[0]->count(first_spatial_axis);
	if (bias_term_) {
		vector<int> bias_multiplier_shape(1, out_spatial_dim_);
		bias_multiplier_.Reshape(bias_multiplier_shape);
		caffe_set(bias_multiplier_.count(), Dtype(1),
			bias_multiplier_.mutable_cpu_data());
	}
}

template <typename Dtype>
void BaseConvolutionLayer<Dtype>::forward_cpu_gemm(const Dtype* input,
    const Dtype* weights, Dtype* output, bool skip_im2col) {
  const Dtype* col_buff = input;
  if (!is_1x1_) {
    if (!skip_im2col) {
      conv_im2col_cpu(input, col_buffer_.mutable_cpu_data());
    }
    col_buff = col_buffer_.cpu_data();
  }
    caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, conv_out_channels_, conv_out_spatial_dim_, kernel_dim_,
        (Dtype)1., weights, col_buff,
        (Dtype)0., output);
  }

template <typename Dtype>
void BaseConvolutionLayer<Dtype>::forward_cpu_bias(Dtype* output,
    const Dtype* bias) {
  caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, num_output_,
      out_spatial_dim_, 1, (Dtype)1., bias, bias_multiplier_.cpu_data(),
      (Dtype)1., output);
}

#ifndef CPU_ONLY

template <typename Dtype>
void BaseConvolutionLayer<Dtype>::forward_gpu_gemm(const Dtype* input,
    const Dtype* weights, Dtype* output, bool skip_im2col) {
  const Dtype* col_buff = input;
  if (!is_1x1_) {
    if (!skip_im2col) {
      conv_im2col_gpu(input, col_buffer_.mutable_gpu_data());
    }
    col_buff = col_buffer_.gpu_data();
  }
    caffe_gpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, conv_out_channels_, conv_out_spatial_dim_, kernel_dim_,
        (Dtype)1., weights, col_buff,
        (Dtype)0., output);
}

template <typename Dtype>
void BaseConvolutionLayer<Dtype>::forward_gpu_bias(Dtype* output,
    const Dtype* bias) {
  caffe_gpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, num_output_,
      out_spatial_dim_, 1, (Dtype)1., bias, bias_multiplier_.gpu_data(),
      (Dtype)1., output);
}
#endif  // !CPU_ONLY

INSTANTIATE_CLASS(BaseConvolutionLayer);

}  // namespace caffe
