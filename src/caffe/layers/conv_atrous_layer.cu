#include <vector>

#include "caffe/filler.hpp"
#include "caffe/layer.hpp"
#include "caffe/util/im2col_atrous.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/layers/conv_atrous_layer.hpp"
#include "caffe/layer.hpp"

namespace caffe {

/// @brief refer to CPU forward -- the BLAS implementation is the same.
template <typename Dtype>
void ConvolutionAtrousLayer<Dtype>::Forward_gpu(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  for (int i = 0; i < bottom.size(); ++i) {
    const Dtype* bottom_data = bottom[i]->gpu_data();
    Dtype* top_data = top[i]->mutable_gpu_data();
    Dtype* col_buff = NULL;
    if (!is_1x1_) {
      col_buff = col_buffer_.mutable_gpu_data();
    }
    const Dtype* weight = this->blobs_[0]->gpu_data();
    int weight_offset = M_ * K_;
    int col_offset = K_ * N_;
    int top_offset = M_ * N_;
    for (int n = 0; n < num_; ++n) {
      // im2col_atrous transformation: unroll input regions for filtering
      // into column matrix for multplication.
      if (!is_1x1_) {
        im2col_atrous_gpu(bottom_data + bottom[i]->offset(n),
	    1, channels_, height_, width_,
	    kernel_h_, kernel_w_, pad_h_, pad_w_,
	    stride_h_, stride_w_, hole_h_, hole_w_,
            col_buff);
      } else {
        col_buff = bottom[i]->mutable_gpu_data() + bottom[i]->offset(n);
      }
      // Take inner products for groups.
      for (int g = 0; g < group_; ++g) {
        caffe_gpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, M_, N_, K_,
          (Dtype)1., weight + weight_offset * g, col_buff + col_offset * g,
          (Dtype)0., top_data + top[i]->offset(n) + top_offset * g);
      }
      // Add bias.
      if (bias_term_) {
        caffe_gpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, num_output_,
            N_, 1, (Dtype)1., this->blobs_[1]->gpu_data(),
            bias_multiplier_.gpu_data(),
            (Dtype)1., top_data + top[i]->offset(n));
      }
    }
  }
}

/// @brief refer to CPU backward -- the BLAS implementation is the same.
template <typename Dtype>
void ConvolutionAtrousLayer<Dtype>::Backward_gpu(const vector<Blob<Dtype>*>& top,
      const vector<bool>& propagate_down, const vector<Blob<Dtype>*>& bottom) {
  const Dtype* weight = NULL;
  Dtype* weight_diff = NULL;
  if (this->param_propagate_down_[0]) {
    weight = this->blobs_[0]->gpu_data();
    weight_diff = this->blobs_[0]->mutable_gpu_diff();
    caffe_gpu_set(this->blobs_[0]->count(), Dtype(0), weight_diff);
  }
  Dtype* bias_diff = NULL;
  if (bias_term_ && this->param_propagate_down_[1]) {
    bias_diff = this->blobs_[1]->mutable_gpu_diff();
    caffe_gpu_set(this->blobs_[1]->count(), Dtype(0), bias_diff);
  }
  const int weight_offset = M_ * K_;
  const int col_offset = K_ * N_;
  const int top_offset = M_ * N_;
  for (int i = 0; i < top.size(); ++i) {
    const Dtype* top_diff = NULL;
    // Bias gradient, if necessary.
    if (bias_term_ && this->param_propagate_down_[1]) {
      top_diff = top[i]->gpu_diff();
      for (int n = 0; n < num_; ++n) {
        caffe_gpu_gemv<Dtype>(CblasNoTrans, num_output_, N_,
            1., top_diff + top[0]->offset(n),
            bias_multiplier_.gpu_data(), 1.,
            bias_diff);
      }
    }
    if (this->param_propagate_down_[0] || propagate_down[i]) {
      if (!top_diff) {
        top_diff = top[i]->gpu_diff();
      }
      Dtype* col_buff = NULL;
      if (!is_1x1_) {
        col_buff = col_buffer_.mutable_gpu_data();
      }
      const Dtype* bottom_data = bottom[i]->gpu_data();
      Dtype* bottom_diff = bottom[i]->mutable_gpu_diff();
      for (int n = 0; n < num_; ++n) {
        // Since we saved memory in the forward pass by not storing all col
        // data, we will need to recompute them.
        if (!is_1x1_) {
          im2col_atrous_gpu(bottom_data + bottom[i]->offset(n),
		    1, channels_, height_, width_,
		    kernel_h_, kernel_w_, pad_h_, pad_w_,
		    stride_h_, stride_w_, hole_h_, hole_w_,
		    col_buff);
        } else {
          col_buff = bottom[i]->mutable_gpu_data() + bottom[i]->offset(n);
        }
        // gradient w.r.t. weight. Note that we will accumulate diffs.
        if (this->param_propagate_down_[0]) {
          for (int g = 0; g < group_; ++g) {
            caffe_gpu_gemm<Dtype>(CblasNoTrans, CblasTrans, M_, K_, N_,
                (Dtype)1., top_diff + top[i]->offset(n) + top_offset * g,
                col_buff + col_offset * g, (Dtype)1.,
                weight_diff + weight_offset * g);
          }
        }
        // gradient w.r.t. bottom data, if necessary
        if (propagate_down[i]) {
          if (weight == NULL) {
            weight = this->blobs_[0]->gpu_data();
          }
          if (is_1x1_) {
            col_buff = bottom[i]->mutable_gpu_diff() + bottom[i]->offset(n);
          }
          for (int g = 0; g < group_; ++g) {
            caffe_gpu_gemm<Dtype>(CblasTrans, CblasNoTrans, K_, N_, M_,
                (Dtype)1., weight + weight_offset * g,
                top_diff + top[i]->offset(n) + top_offset * g,
                (Dtype)0., col_buff + col_offset * g);
          }
          // col2im back to the data
          if (!is_1x1_) {
            col2im_atrous_gpu(col_buff,
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


INSTANTIATE_LAYER_GPU_FUNCS(ConvolutionAtrousLayer);

}  // namespace caffe
