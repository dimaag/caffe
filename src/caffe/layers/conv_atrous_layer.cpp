#include <vector>
#include "caffe/layer.hpp"
#include "caffe/filler.hpp"
#include "caffe/layers/conv_atrous_layer.hpp"
//#include "caffe/layers/base_conv_layer.hpp"
#include "caffe/util/im2col_atrous.hpp"
#include "caffe/util/math_functions.hpp"

namespace caffe {

template <typename Dtype>
void ConvolutionAtrousLayer<Dtype>::LayerSetUp(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  // Configure the kernel size, padding, stride, and inputs.
  ConvolutionAtrousParameter conv_param = this->layer_param_.convolution_atrous_param();
  CHECK(!conv_param.has_kernel_size() !=
      !(conv_param.has_kernel_h() && conv_param.has_kernel_w()))
      << "Filter size is kernel_size OR kernel_h and kernel_w; not both";
  CHECK(conv_param.has_kernel_size() ||
      (conv_param.has_kernel_h() && conv_param.has_kernel_w()))
      << "For non-square filters both kernel_h and kernel_w are required.";
  CHECK((!conv_param.has_pad() && conv_param.has_pad_h()
      && conv_param.has_pad_w())
      || (!conv_param.has_pad_h() && !conv_param.has_pad_w()))
      << "pad is pad OR pad_h and pad_w are required.";
  CHECK((!conv_param.has_stride() && conv_param.has_stride_h()
      && conv_param.has_stride_w())
      || (!conv_param.has_stride_h() && !conv_param.has_stride_w()))
      << "Stride is stride OR stride_h and stride_w are required.";
  CHECK((!conv_param.has_hole() && conv_param.has_hole_h()
      && conv_param.has_hole_w())
      || (!conv_param.has_hole_h() && !conv_param.has_hole_w()))
      << "hole is hole OR hole_h and hole_w are required.";
  if (conv_param.has_kernel_size()) {
    kernel_h_ = kernel_w_ = conv_param.kernel_size();
  } else {
    kernel_h_ = conv_param.kernel_h();
    kernel_w_ = conv_param.kernel_w();
  }
  CHECK_GT(kernel_h_, 0) << "Filter dimensions cannot be zero.";
  CHECK_GT(kernel_w_, 0) << "Filter dimensions cannot be zero.";
  if (!conv_param.has_pad_h()) {
    pad_h_ = pad_w_ = conv_param.pad();
  } else {
    pad_h_ = conv_param.pad_h();
    pad_w_ = conv_param.pad_w();
  }
  if (!conv_param.has_hole_h()) {
    hole_h_ = hole_w_ = conv_param.hole();
  } else {
    hole_h_ = conv_param.hole_h();
    hole_w_ = conv_param.hole_w();
  }
  if (!conv_param.has_stride_h()) {
    stride_h_ = stride_w_ = conv_param.stride();
  } else {
    stride_h_ = conv_param.stride_h();
    stride_w_ = conv_param.stride_w();
  }
  // Special case: im2col_atrous is the identity for 1x1 convolution with stride 1
  // and no padding or holes, so flag for skipping the buffer and transformation.
  is_1x1_ = kernel_w_ == 1 && kernel_h_ == 1  && stride_h_ == 1 && stride_w_ == 1
    && pad_h_ == 0 && pad_w_ == 0 && hole_h_ == 1 && hole_w_ == 1;
  // Configure output channels and groups.
  channels_ = bottom[0]->channels();
  num_output_ = this->layer_param_.convolution_atrous_param().num_output();
  CHECK_GT(num_output_, 0);
  group_ = this->layer_param_.convolution_atrous_param().group();
  CHECK_EQ(channels_ % group_, 0);
  CHECK_EQ(num_output_ % group_, 0)
      << "Number of output should be multiples of group.";
  // Handle the parameters: weights and biases.
  // - blobs_[0] holds the filter weights
  // - blobs_[1] holds the biases (optional)
  bias_term_ = this->layer_param_.convolution_atrous_param().bias_term();
  if (this->blobs_.size() > 0) {
    LOG(INFO) << "Skipping parameter initialization";
  } else {
    if (bias_term_) {
      this->blobs_.resize(2);
    } else {
      this->blobs_.resize(1);
    }
    // Initialize and fill the weights:
    // output channels x input channels per-group x kernel height x kernel width
    this->blobs_[0].reset(new Blob<Dtype>(
        num_output_, channels_ / group_, kernel_h_, kernel_w_));
    shared_ptr<Filler<Dtype> > weight_filler(GetFiller<Dtype>(
        this->layer_param_.convolution_atrous_param().weight_filler()));
    weight_filler->Fill(this->blobs_[0].get());
    // If necessary, initialize and fill the biases:
    // 1 x 1 x 1 x output channels
    if (bias_term_) {
      this->blobs_[1].reset(new Blob<Dtype>(1, 1, 1, num_output_));
      shared_ptr<Filler<Dtype> > bias_filler(GetFiller<Dtype>(
          this->layer_param_.convolution_atrous_param().bias_filler()));
      bias_filler->Fill(this->blobs_[1].get());
    }
  }
  // Propagate gradients to the parameters (as directed by backward pass).
  this->param_propagate_down_.resize(this->blobs_.size(), true);
}

template <typename Dtype>
void ConvolutionAtrousLayer<Dtype>::Reshape(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  num_ = bottom[0]->num();
  height_ = bottom[0]->height();
  width_ = bottom[0]->width();
  CHECK_EQ(bottom[0]->channels(), channels_) << "Input size incompatible with"
    " convolution kernel.";
  // TODO: generalize to handle inputs of different shapes.
  for (int bottom_id = 1; bottom_id < bottom.size(); ++bottom_id) {
    CHECK_EQ(num_, bottom[bottom_id]->num()) << "Inputs must have same num.";
    CHECK_EQ(channels_, bottom[bottom_id]->channels())
        << "Inputs must have same channels.";
    CHECK_EQ(height_, bottom[bottom_id]->height())
        << "Inputs must have same height.";
    CHECK_EQ(width_, bottom[bottom_id]->width())
        << "Inputs must have same width.";
  }
  // Shape the tops.
  const int kernel_h_eff = kernel_h_ + (kernel_h_ - 1) * (hole_h_ - 1);
  const int kernel_w_eff = kernel_w_ + (kernel_w_ - 1) * (hole_w_ - 1);
  height_out_ = (height_ + 2 * pad_h_ - kernel_h_eff) / stride_h_ + 1;
  width_out_ = (width_ + 2 * pad_w_ - kernel_w_eff) / stride_w_ + 1;
  for (int top_id = 0; top_id < top.size(); ++top_id) {
    top[top_id]->Reshape(num_, num_output_, height_out_, width_out_);
  }
  // Prepare the matrix multiplication computation.
  // Each input will be convolved as a single GEMM.
  M_ = num_output_ / group_;
  K_ = channels_ * kernel_h_ * kernel_w_ / group_;
  N_ = height_out_ * width_out_;
  // The im2col_atrous result buffer will only hold one image at a time to avoid
  // overly large memory usage. In the special case of 1x1 convolution
  // it goes lazily unused to save memory.
  col_buffer_.Reshape(
      1, channels_ * kernel_h_ * kernel_w_, height_out_, width_out_);
  for (int top_id = 0; top_id < top.size(); ++top_id) {
    top[top_id]->Reshape(num_, num_output_, height_out_, width_out_);
  }
  // Set up the all ones "bias multiplier" for adding biases by BLAS
  if (bias_term_) {
    bias_multiplier_.Reshape(1, 1, 1, N_);
    caffe_set(N_, Dtype(1), bias_multiplier_.mutable_cpu_data());
  }
}

template <typename Dtype>
void ConvolutionAtrousLayer<Dtype>::Forward_cpu(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  for (int i = 0; i < bottom.size(); ++i) {
    const Dtype* bottom_data = bottom[i]->cpu_data();
    Dtype* top_data = top[i]->mutable_cpu_data();
    Dtype* col_buff = NULL;
    if (!is_1x1_) {
      col_buff = col_buffer_.mutable_cpu_data();
    }
    const Dtype* weight = this->blobs_[0]->cpu_data();
    int weight_offset = M_ * K_;  // number of filter parameters in a group
    int col_offset = K_ * N_;  // number of values in an input region / column
    int top_offset = M_ * N_;  // number of values in an output region / column
    for (int n = 0; n < num_; ++n) {
      // im2col_atrous transformation: unroll input regions for filtering
      // into column matrix for multplication.
      if (!is_1x1_) {
        im2col_atrous_cpu(bottom_data + bottom[i]->offset(n),
            1, channels_, height_, width_,
            kernel_h_, kernel_w_, pad_h_, pad_w_,
	    stride_h_, stride_w_, hole_h_, hole_w_,
            col_buff);
      } else {  // special case for 1x1 convolution
        col_buff = bottom[i]->mutable_cpu_data() + bottom[i]->offset(n);
      }
      // Take inner products for groups.
      for (int g = 0; g < group_; ++g) {
        caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, M_, N_, K_,
          (Dtype)1., weight + weight_offset * g, col_buff + col_offset * g,
          (Dtype)0., top_data + top[i]->offset(n) + top_offset * g);
      }
      // Add bias.
      if (bias_term_) {
        caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, num_output_,
            N_, 1, (Dtype)1., this->blobs_[1]->cpu_data(),
            bias_multiplier_.cpu_data(),
            (Dtype)1., top_data + top[i]->offset(n));
      }
    }
  }
}

template <typename Dtype>
void ConvolutionAtrousLayer<Dtype>::Backward_cpu(const vector<Blob<Dtype>*>& top,
      const vector<bool>& propagate_down, const vector<Blob<Dtype>*>& bottom) {
  const Dtype* weight = NULL;
  Dtype* weight_diff = NULL;
  if (this->param_propagate_down_[0]) {
    weight = this->blobs_[0]->cpu_data();
    weight_diff = this->blobs_[0]->mutable_cpu_diff();
    caffe_set(this->blobs_[0]->count(), Dtype(0), weight_diff);
  }
  Dtype* bias_diff = NULL;
  if (bias_term_ && this->param_propagate_down_[1]) {
    bias_diff = this->blobs_[1]->mutable_cpu_diff();
    caffe_set(this->blobs_[1]->count(), Dtype(0), bias_diff);
  }
  const int weight_offset = M_ * K_;
  const int col_offset = K_ * N_;
  const int top_offset = M_ * N_;
  for (int i = 0; i < top.size(); ++i) {
    const Dtype* top_diff = NULL;
    // Bias gradient, if necessary.
    if (bias_term_ && this->param_propagate_down_[1]) {
      top_diff = top[i]->cpu_diff();
      for (int n = 0; n < num_; ++n) {
        caffe_cpu_gemv<Dtype>(CblasNoTrans, num_output_, N_,
            1., top_diff + top[0]->offset(n),
            bias_multiplier_.cpu_data(), 1.,
            bias_diff);
      }
    }
    if (this->param_propagate_down_[0] || propagate_down[i]) {
      if (!top_diff) {
        top_diff = top[i]->cpu_diff();
      }
      Dtype* col_buff = NULL;
      if (!is_1x1_) {
        col_buff = col_buffer_.mutable_cpu_data();
      }
      const Dtype* bottom_data = bottom[i]->cpu_data();
      Dtype* bottom_diff = bottom[i]->mutable_cpu_diff();
      for (int n = 0; n < num_; ++n) {
        // Since we saved memory in the forward pass by not storing all col
        // data, we will need to recompute them.
        if (!is_1x1_) {
          im2col_atrous_cpu(bottom_data + bottom[i]->offset(n),
		     1, channels_, height_, width_,
		     kernel_h_, kernel_w_, pad_h_, pad_w_,
		     stride_h_, stride_w_, hole_h_, hole_w_,
		     col_buff);
        } else {
          col_buff = bottom[i]->mutable_cpu_data() + bottom[i]->offset(n);
        }
        // gradient w.r.t. weight. Note that we will accumulate diffs.
        if (this->param_propagate_down_[0]) {
          for (int g = 0; g < group_; ++g) {
            caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasTrans, M_, K_, N_,
                (Dtype)1., top_diff + top[i]->offset(n) + top_offset * g,
                col_buff + col_offset * g, (Dtype)1.,
                weight_diff + weight_offset * g);
          }
        }
        // gradient w.r.t. bottom data, if necessary.
        if (propagate_down[i]) {
          if (weight == NULL) {
            weight = this->blobs_[0]->cpu_data();
          }
          if (is_1x1_) {
            col_buff = bottom[i]->mutable_cpu_diff() + bottom[i]->offset(n);
          }
          for (int g = 0; g < group_; ++g) {
            caffe_cpu_gemm<Dtype>(CblasTrans, CblasNoTrans, K_, N_, M_,
                (Dtype)1., weight + weight_offset * g,
                top_diff + top[i]->offset(n) + top_offset * g,
                (Dtype)0., col_buff + col_offset * g);
          }
          // col2im back to the data
          if (!is_1x1_) {
            col2im_atrous_cpu(col_buff,
		1, channels_, height_, width_,
                kernel_h_, kernel_w_, pad_h_, pad_w_,
		stride_h_, stride_w_, hole_h_, hole_w_, 
		bottom_diff + bottom[i]->offset(n));
          }
        }
      }
    }
  }
}

#ifdef CPU_ONLY
STUB_GPU(ConvolutionAtrousLayer);
#endif

INSTANTIATE_CLASS(ConvolutionAtrousLayer);
}  // namespace caffe
