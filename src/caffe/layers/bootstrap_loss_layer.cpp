#include <algorithm>
#include <cfloat>
#include <vector>

#include "caffe/layer.hpp"
#include "caffe/layer_factory.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/vision_layers.hpp"

namespace caffe {

template <typename Dtype>
void BootstrapLossLayer<Dtype>::LayerSetUp(
    const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  LossLayer<Dtype>::LayerSetUp(bottom, top);

  LayerParameter softmax_param(this->layer_param_);
  softmax_param.set_type("Softmax");
  softmax_layer_ = LayerRegistry<Dtype>::CreateLayer(softmax_param);
  softmax_bottom_vec_.clear();
  softmax_bottom_vec_.push_back(bottom[0]);
  softmax_top_vec_.clear();
  softmax_top_vec_.push_back(&prob_);
  softmax_layer_->SetUp(softmax_bottom_vec_, softmax_top_vec_);

  LayerParameter layer_param;
  layer_param.set_type("ArgMax");
  ArgMaxParameter* argmax_param = layer_param.mutable_argmax_param();
  argmax_param->set_top_k(1);
  argmax_layer_ = LayerRegistry<Dtype>::CreateLayer(layer_param);
  argmax_top_vec_.clear();
  argmax_top_vec_.push_back(&p_label_);
  argmax_layer_->SetUp(softmax_top_vec_, argmax_top_vec_);

  has_ignore_label_ =
    this->layer_param_.loss_param().has_ignore_label();
  if (has_ignore_label_) {
    ignore_label_ = this->layer_param_.loss_param().ignore_label();
  }
  normalize_ = this->layer_param_.loss_param().normalize();
  is_hard_mode_ = this->layer_param_.bootstrap_param().is_hard_mode();
  beta_ = this->layer_param_.bootstrap_param().beta();
}

template <typename Dtype>
void BootstrapLossLayer<Dtype>::Reshape(
    const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  LossLayer<Dtype>::Reshape(bottom, top);
  softmax_layer_->Reshape(softmax_bottom_vec_, softmax_top_vec_);
  argmax_layer_->Reshape(softmax_top_vec_, argmax_top_vec_);
  softmax_axis_ =
      bottom[0]->CanonicalAxisIndex(this->layer_param_.softmax_param().axis());
  outer_num_ = bottom[0]->count(0, softmax_axis_);
  inner_num_ = bottom[0]->count(softmax_axis_ + 1);
  CHECK_EQ(outer_num_ * inner_num_, bottom[1]->count())
      << "Number of labels must match number of predictions; "
      << "e.g., if softmax axis == 1 and prediction shape is (N, C, H, W), "
      << "label count (number of labels) must be N*H*W, "
      << "with integer values in {0, 1, ..., C-1}.";
  if (top.size() >= 2) {
    // softmax output
    top[1]->ReshapeLike(*bottom[0]);
  }
}

template <typename Dtype>
void BootstrapLossLayer<Dtype>::Forward_cpu(
    const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  // The forward pass computes the softmax prob values.
  softmax_layer_->Forward(softmax_bottom_vec_, softmax_top_vec_);
  argmax_layer_->Forward(softmax_top_vec_, argmax_top_vec_);
  const Dtype* prob_data = prob_.cpu_data();
  const Dtype* p_label = p_label_.cpu_data();  // predicted label
  const Dtype* n_label = bottom[1]->cpu_data();  // noisy label
  int dim = prob_.count() / outer_num_;
  int count = 0;
  Dtype loss = 0;
  for (int i = 0; i < outer_num_; ++i) {
    for (int j = 0; j < inner_num_; j++) {
      const int n_label_value = static_cast<int>(n_label[i * inner_num_ + j]);
      const int p_label_value = static_cast<int>(p_label[i * inner_num_ + j]);
      if (has_ignore_label_ && n_label_value == ignore_label_) {
        continue;
      }
      DCHECK_GE(n_label_value, 0);
      DCHECK_LT(n_label_value, prob_.shape(softmax_axis_));
      for (int k = 0; k < bottom[0]->channels(); ++k) {
        Dtype c = is_hard_mode_
                  ? (beta_ * (k == n_label_value) +
                    (1 - beta_) * (k == p_label_value))
                  : (beta_ * (k == n_label_value) +
                    (1 - beta_) * prob_data[i * dim + k * inner_num_ + j]);
        loss -= c * log(std::max(prob_data[i * dim + k * inner_num_ + j],
                                 Dtype(FLT_MIN)));
      }
      ++count;
    }
  }
  if (normalize_) {
    top[0]->mutable_cpu_data()[0] = loss / count;
  } else {
    top[0]->mutable_cpu_data()[0] = loss / outer_num_;
  }
  if (top.size() == 2) {
    top[1]->ShareData(prob_);
  }
}

template <typename Dtype>
void BootstrapLossLayer<Dtype>::Backward_cpu(const vector<Blob<Dtype>*>& top,
    const vector<bool>& propagate_down, const vector<Blob<Dtype>*>& bottom) {
  if (propagate_down[1]) {
    LOG(FATAL) << this->type()
               << " Layer cannot backpropagate to label inputs.";
  }
  if (propagate_down[0]) {
    Dtype* bottom_diff = bottom[0]->mutable_cpu_diff();
    const Dtype* prob_data = prob_.cpu_data();
    caffe_copy(prob_.count(), prob_data, bottom_diff);
    const Dtype* p_label = p_label_.cpu_data();  // predicted label
    const Dtype* n_label = bottom[1]->cpu_data();  // noisy label
    int dim = prob_.count() / outer_num_;
    int count = 0;
    for (int i = 0; i < outer_num_; ++i) {
      for (int j = 0; j < inner_num_; ++j) {
        const int n_label_value = static_cast<int>(n_label[i * inner_num_ + j]);
        const int p_label_value = static_cast<int>(p_label[i * inner_num_ + j]);
        if (has_ignore_label_ && n_label_value == ignore_label_) {
          for (int c = 0; c < bottom[0]->shape(softmax_axis_); ++c) {
            bottom_diff[i * dim + c * inner_num_ + j] = 0;
          }
        } else {
          for (int k = 0; k < bottom[0]->channels(); ++k) {
            Dtype c = is_hard_mode_
                      ? (beta_ * (k == n_label_value) +
                        (1 - beta_) * (k == p_label_value))
                      : (beta_ * (k == n_label_value) +
                        (1 - beta_) * prob_data[i * dim + k * inner_num_ + j]);
            bottom_diff[i * dim + k * inner_num_ + j] -= c;
          }
          ++count;
        }
      }
    }
    // Scale gradient
    const Dtype loss_weight = top[0]->cpu_diff()[0];
    if (normalize_) {
      caffe_scal(prob_.count(), loss_weight / count, bottom_diff);
    } else {
      caffe_scal(prob_.count(), loss_weight / outer_num_, bottom_diff);
    }
  }
}

// #ifdef CPU_ONLY
// STUB_GPU(BootstrapLossLayer);
// #endif

INSTANTIATE_CLASS(BootstrapLossLayer);
REGISTER_LAYER_CLASS(BootstrapLoss);

}  // namespace caffe
