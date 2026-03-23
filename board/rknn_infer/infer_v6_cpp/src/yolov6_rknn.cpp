#include "yolov6_rknn.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <utility>

#include <opencv2/imgproc.hpp>

#include "project_logging.hpp"

namespace infer_v6_cpp {
namespace {

constexpr float kDefaultObjThreshold = 0.25f;
constexpr float kDefaultNmsThreshold = 0.45f;
constexpr int kColorThickness = 2;
constexpr int kLabelThickness = 2;
constexpr double kLabelScale = 0.6;
constexpr std::array<const char*, 1> kClassNames = {"Head"};

struct TensorNCHW {
    int n = 1;
    int c = 0;
    int h = 0;
    int w = 0;
    std::vector<float> data;

    float at(int n_index, int c_index, int h_index, int w_index) const {
        const size_t index =
            static_cast<size_t>((((n_index * c) + c_index) * h + h_index) * w + w_index);
        return data[index];
    }
};

std::vector<char> ReadBinaryFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }

    const std::streamsize size = file.tellg();
    if (size <= 0) {
        return {};
    }

    file.seekg(0, std::ios::beg);
    std::vector<char> buffer(static_cast<size_t>(size));
    if (!file.read(buffer.data(), size)) {
        return {};
    }
    return buffer;
}

std::string ShapeToString(const rknn_tensor_attr& attr) {
    std::ostringstream oss;
    oss << "[";
    for (uint32_t i = 0; i < attr.n_dims; ++i) {
        if (i != 0) {
            oss << ", ";
        }
        oss << attr.dims[i];
    }
    oss << "]";
    return oss.str();
}

void DumpTensorAttr(const char* tensor_kind, const rknn_tensor_attr& attr) {
    project_logging::Info(
        "{}张量: 索引={} 名称={} 维度={} 元素数={} 字节数={} 格式={} 类型={} 量化={} 零点={} 缩放={}",
        tensor_kind,
        attr.index,
        attr.name,
        ShapeToString(attr),
        attr.n_elems,
        attr.size,
        get_format_string(attr.fmt),
        get_type_string(attr.type),
        get_qnt_type_string(attr.qnt_type),
        attr.zp,
        attr.scale);
}

std::vector<int> GetShape(const rknn_tensor_attr& attr) {
    std::vector<int> shape;
    shape.reserve(attr.n_dims);
    for (uint32_t i = 0; i < attr.n_dims; ++i) {
        shape.push_back(static_cast<int>(attr.dims[i]));
    }
    return shape;
}

std::vector<int> SqueezeLeadingOnes(std::vector<int> shape) {
    while (shape.size() > 2 && !shape.empty() && shape.front() == 1) {
        shape.erase(shape.begin());
    }
    return shape;
}

cv::Mat LetterBox(const cv::Mat& image, const cv::Size& new_shape, YoloV6RknnInfer::LetterBoxInfo* info) {
    const float ratio = std::min(
        static_cast<float>(new_shape.width) / static_cast<float>(image.cols),
        static_cast<float>(new_shape.height) / static_cast<float>(image.rows));
    const int resized_width = static_cast<int>(std::round(image.cols * ratio));
    const int resized_height = static_cast<int>(std::round(image.rows * ratio));
    const float dw = static_cast<float>(new_shape.width - resized_width) / 2.0f;
    const float dh = static_cast<float>(new_shape.height - resized_height) / 2.0f;

    cv::Mat resized;
    if (resized_width != image.cols || resized_height != image.rows) {
        cv::resize(image, resized, cv::Size(resized_width, resized_height), 0.0, 0.0, cv::INTER_LINEAR);
    } else {
        resized = image.clone();
    }

    const int top = static_cast<int>(std::round(dh - 0.1f));
    const int bottom = static_cast<int>(std::round(dh + 0.1f));
    const int left = static_cast<int>(std::round(dw - 0.1f));
    const int right = static_cast<int>(std::round(dw + 0.1f));

    cv::Mat bordered;
    cv::copyMakeBorder(
        resized,
        bordered,
        top,
        bottom,
        left,
        right,
        cv::BORDER_CONSTANT,
        cv::Scalar(0, 0, 0));

    if (info != nullptr) {
        info->ratio = ratio;
        info->dw = dw;
        info->dh = dh;
    }
    return bordered;
}

float ClipValue(float value, float min_value, float max_value) {
    return std::max(min_value, std::min(value, max_value));
}

void ScaleDetection(Detection* detection,
                    const YoloV6RknnInfer::LetterBoxInfo& letter_box,
                    const cv::Size& original_size) {
    detection->x1 = ClipValue((detection->x1 - letter_box.dw) / letter_box.ratio, 0.0f, original_size.width);
    detection->x2 = ClipValue((detection->x2 - letter_box.dw) / letter_box.ratio, 0.0f, original_size.width);
    detection->y1 = ClipValue((detection->y1 - letter_box.dh) / letter_box.ratio, 0.0f, original_size.height);
    detection->y2 = ClipValue((detection->y2 - letter_box.dh) / letter_box.ratio, 0.0f, original_size.height);
}

float IoU(const Detection& lhs, const Detection& rhs) {
    const float xx1 = std::max(lhs.x1, rhs.x1);
    const float yy1 = std::max(lhs.y1, rhs.y1);
    const float xx2 = std::min(lhs.x2, rhs.x2);
    const float yy2 = std::min(lhs.y2, rhs.y2);

    const float inter_w = std::max(0.0f, xx2 - xx1);
    const float inter_h = std::max(0.0f, yy2 - yy1);
    const float inter = inter_w * inter_h;
    const float lhs_area = std::max(0.0f, lhs.x2 - lhs.x1) * std::max(0.0f, lhs.y2 - lhs.y1);
    const float rhs_area = std::max(0.0f, rhs.x2 - rhs.x1) * std::max(0.0f, rhs.y2 - rhs.y1);
    return inter / (lhs_area + rhs_area - inter + 1e-6f);
}

std::vector<Detection> ApplyNms(std::vector<Detection> detections, float nms_threshold) {
    std::sort(
        detections.begin(),
        detections.end(),
        [](const Detection& lhs, const Detection& rhs) { return lhs.score > rhs.score; });

    std::vector<Detection> kept;
    std::vector<bool> suppressed(detections.size(), false);
    for (size_t i = 0; i < detections.size(); ++i) {
        if (suppressed[i]) {
            continue;
        }
        kept.push_back(detections[i]);
        for (size_t j = i + 1; j < detections.size(); ++j) {
            if (suppressed[j] || detections[i].class_id != detections[j].class_id) {
                continue;
            }
            if (IoU(detections[i], detections[j]) > nms_threshold) {
                suppressed[j] = true;
            }
        }
    }
    return kept;
}

bool DecodeSingleOutput(const rknn_tensor_attr& attr,
                        const float* data,
                        float obj_threshold,
                        std::vector<Detection>* detections,
                        float* raw_score_max) {
    std::vector<int> shape = SqueezeLeadingOnes(GetShape(attr));
    if (shape.size() == 1 && attr.n_elems % 5 == 0) {
        shape = {static_cast<int>(attr.n_elems / 5), 5};
    }

    int rows = 0;
    bool channel_first = false;
    if (shape.size() == 2 && shape[0] == 5) {
        rows = shape[1];
        channel_first = true;
    } else if (shape.size() == 2 && shape[1] == 5) {
        rows = shape[0];
    } else {
        return false;
    }

    float max_score = std::numeric_limits<float>::lowest();
    for (int row = 0; row < rows; ++row) {
        const auto read_value = [&](int column) -> float {
            if (channel_first) {
                return data[static_cast<size_t>(column) * rows + row];
            }
            return data[static_cast<size_t>(row) * 5 + column];
        };

        const float cx = read_value(0);
        const float cy = read_value(1);
        const float width = read_value(2);
        const float height = read_value(3);
        const float score = read_value(4);

        max_score = std::max(max_score, score);
        if (score < obj_threshold) {
            continue;
        }

        Detection detection;
        detection.x1 = cx - width / 2.0f;
        detection.y1 = cy - height / 2.0f;
        detection.x2 = cx + width / 2.0f;
        detection.y2 = cy + height / 2.0f;
        detection.score = score;
        detection.class_id = 0;
        detections->push_back(detection);
    }

    if (raw_score_max != nullptr) {
        *raw_score_max = std::max(*raw_score_max, max_score);
    }
    return true;
}

bool ShouldTreatAsNhwc(const rknn_tensor_attr& attr, const std::vector<int>& shape) {
    if (attr.fmt == RKNN_TENSOR_NHWC) {
        return true;
    }
    if (attr.fmt == RKNN_TENSOR_NCHW) {
        return false;
    }
    return shape.size() == 4 && shape[1] == shape[2] && shape[2] != shape[3];
}

bool ToNchwTensor(const rknn_tensor_attr& attr, const float* data, TensorNCHW* tensor) {
    const std::vector<int> shape = GetShape(attr);
    if (shape.size() != 3 && shape.size() != 4) {
        return false;
    }

    const bool is_nhwc = ShouldTreatAsNhwc(attr, shape);
    int n = 1;
    int c = 0;
    int h = 0;
    int w = 0;
    if (shape.size() == 4) {
        if (is_nhwc) {
            n = shape[0];
            h = shape[1];
            w = shape[2];
            c = shape[3];
        } else {
            n = shape[0];
            c = shape[1];
            h = shape[2];
            w = shape[3];
        }
    } else {
        if (is_nhwc) {
            h = shape[0];
            w = shape[1];
            c = shape[2];
        } else {
            c = shape[0];
            h = shape[1];
            w = shape[2];
        }
    }

    if (n <= 0 || c <= 0 || h <= 0 || w <= 0) {
        return false;
    }

    tensor->n = n;
    tensor->c = c;
    tensor->h = h;
    tensor->w = w;
    tensor->data.resize(static_cast<size_t>(n) * c * h * w);

    if (!is_nhwc) {
        const size_t element_count = static_cast<size_t>(n) * c * h * w;
        std::copy(data, data + element_count, tensor->data.begin());
        return true;
    }

    for (int n_index = 0; n_index < n; ++n_index) {
        for (int h_index = 0; h_index < h; ++h_index) {
            for (int w_index = 0; w_index < w; ++w_index) {
                for (int c_index = 0; c_index < c; ++c_index) {
                    const size_t src_index = static_cast<size_t>((((n_index * h) + h_index) * w + w_index) * c + c_index);
                    const size_t dst_index = static_cast<size_t>((((n_index * c) + c_index) * h + h_index) * w + w_index);
                    tensor->data[dst_index] = data[src_index];
                }
            }
        }
    }
    return true;
}

float DecodeDfl(const TensorNCHW& tensor, int start_channel, int y, int x, int dfl_len) {
    float max_value = std::numeric_limits<float>::lowest();
    for (int i = 0; i < dfl_len; ++i) {
        max_value = std::max(max_value, tensor.at(0, start_channel + i, y, x));
    }

    float exp_sum = 0.0f;
    float acc_sum = 0.0f;
    for (int i = 0; i < dfl_len; ++i) {
        const float exp_value = std::exp(tensor.at(0, start_channel + i, y, x) - max_value);
        exp_sum += exp_value;
        acc_sum += exp_value * static_cast<float>(i);
    }
    return acc_sum / exp_sum;
}

bool DecodeMultiOutput(const std::vector<rknn_tensor_attr>& attrs,
                       const rknn_output outputs[],
                       int model_width,
                       int model_height,
                       float obj_threshold,
                       std::vector<Detection>* detections,
                       float* raw_score_max) {
    if (attrs.empty() || attrs.size() % 3 != 0) {
        return false;
    }

    const int branch_count = 3;
    const int pair_per_branch = static_cast<int>(attrs.size()) / branch_count;
    if (pair_per_branch * branch_count != static_cast<int>(attrs.size())) {
        return false;
    }

    for (int branch = 0; branch < branch_count; ++branch) {
        const int box_index = branch * pair_per_branch;
        const int cls_index = box_index + 1;
        if (cls_index >= static_cast<int>(attrs.size())) {
            return false;
        }

        TensorNCHW box_tensor;
        TensorNCHW cls_tensor;
        if (!ToNchwTensor(attrs[box_index], static_cast<const float*>(outputs[box_index].buf), &box_tensor) ||
            !ToNchwTensor(attrs[cls_index], static_cast<const float*>(outputs[cls_index].buf), &cls_tensor)) {
            return false;
        }

        if (box_tensor.n != 1 || cls_tensor.n != 1 || box_tensor.h != cls_tensor.h || box_tensor.w != cls_tensor.w) {
            return false;
        }

        const int dfl_len = box_tensor.c / 4;
        if (box_tensor.c != 4 && (box_tensor.c % 4 != 0 || dfl_len <= 0)) {
            return false;
        }

        const float stride_x = static_cast<float>(model_width) / static_cast<float>(box_tensor.w);
        const float stride_y = static_cast<float>(model_height) / static_cast<float>(box_tensor.h);

        for (int y = 0; y < cls_tensor.h; ++y) {
            for (int x = 0; x < cls_tensor.w; ++x) {
                int best_class = 0;
                float best_score = std::numeric_limits<float>::lowest();
                for (int c = 0; c < cls_tensor.c; ++c) {
                    const float score = cls_tensor.at(0, c, y, x);
                    if (score > best_score) {
                        best_score = score;
                        best_class = c;
                    }
                }
                if (raw_score_max != nullptr) {
                    *raw_score_max = std::max(*raw_score_max, best_score);
                }
                if (best_score < obj_threshold) {
                    continue;
                }

                float left = 0.0f;
                float top = 0.0f;
                float right = 0.0f;
                float bottom = 0.0f;
                if (box_tensor.c == 4) {
                    left = box_tensor.at(0, 0, y, x);
                    top = box_tensor.at(0, 1, y, x);
                    right = box_tensor.at(0, 2, y, x);
                    bottom = box_tensor.at(0, 3, y, x);
                } else {
                    left = DecodeDfl(box_tensor, 0 * dfl_len, y, x, dfl_len);
                    top = DecodeDfl(box_tensor, 1 * dfl_len, y, x, dfl_len);
                    right = DecodeDfl(box_tensor, 2 * dfl_len, y, x, dfl_len);
                    bottom = DecodeDfl(box_tensor, 3 * dfl_len, y, x, dfl_len);
                }

                Detection detection;
                detection.x1 = (static_cast<float>(x) + 0.5f - left) * stride_x;
                detection.y1 = (static_cast<float>(y) + 0.5f - top) * stride_y;
                detection.x2 = (static_cast<float>(x) + 0.5f + right) * stride_x;
                detection.y2 = (static_cast<float>(y) + 0.5f + bottom) * stride_y;
                detection.score = best_score;
                detection.class_id = best_class;
                detections->push_back(detection);
            }
        }
    }

    return true;
}

}  // namespace

YoloV6RknnInfer::YoloV6RknnInfer()
    : class_names_(kClassNames.begin(), kClassNames.end()) {
    SetThresholds(kDefaultObjThreshold, kDefaultNmsThreshold);
}

YoloV6RknnInfer::~YoloV6RknnInfer() {
    Unload();
}

bool YoloV6RknnInfer::Load(const std::string& model_path, bool verbose) {
    Unload();

    const std::vector<char> model = ReadBinaryFile(model_path);
    if (model.empty()) {
        SetError("读取模型文件失败: " + model_path);
        return false;
    }

    const int init_ret = rknn_init(&ctx_, const_cast<char*>(model.data()), static_cast<uint32_t>(model.size()), 0, nullptr);
    if (init_ret != RKNN_SUCC) {
        SetError("rknn_init 失败，返回值=" + std::to_string(init_ret));
        ctx_ = 0;
        return false;
    }

    loaded_ = QueryTensorInfo(verbose);
    if (!loaded_) {
        Unload();
        return false;
    }

    if (verbose) {
        rknn_sdk_version sdk_version{};
        if (rknn_query(ctx_, RKNN_QUERY_SDK_VERSION, &sdk_version, sizeof(sdk_version)) == RKNN_SUCC) {
            project_logging::Info("RKNN API 版本: {}", sdk_version.api_version);
            project_logging::Info("RKNN 驱动版本: {}", sdk_version.drv_version);
        }
    }

    last_error_.clear();
    return true;
}

void YoloV6RknnInfer::Unload() {
    input_attrs_.clear();
    output_attrs_.clear();
    input_width_ = 0;
    input_height_ = 0;
    input_channel_ = 0;
    loaded_ = false;
    if (ctx_ != 0) {
        rknn_destroy(ctx_);
        ctx_ = 0;
    }
}

bool YoloV6RknnInfer::IsLoaded() const {
    return loaded_;
}

bool YoloV6RknnInfer::QueryTensorInfo(bool verbose) {
    rknn_input_output_num io_num{};
    const int io_ret = rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (io_ret != RKNN_SUCC) {
        SetError("查询 RKNN 输入输出数量失败，返回值=" + std::to_string(io_ret));
        return false;
    }

    input_attrs_.assign(io_num.n_input, rknn_tensor_attr{});
    output_attrs_.assign(io_num.n_output, rknn_tensor_attr{});

    if (verbose) {
        project_logging::Info("模型输入张量数量: {}，输出张量数量: {}", io_num.n_input, io_num.n_output);
    }

    for (uint32_t i = 0; i < io_num.n_input; ++i) {
        input_attrs_[i].index = i;
        const int ret = rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &input_attrs_[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            SetError("查询输入张量属性失败，返回值=" + std::to_string(ret));
            return false;
        }
        if (verbose) {
            DumpTensorAttr("输入", input_attrs_[i]);
        }
    }

    for (uint32_t i = 0; i < io_num.n_output; ++i) {
        output_attrs_[i].index = i;
        const int ret = rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &output_attrs_[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            SetError("查询输出张量属性失败，返回值=" + std::to_string(ret));
            return false;
        }
        if (verbose) {
            DumpTensorAttr("输出", output_attrs_[i]);
        }
    }

    if (input_attrs_.empty()) {
        SetError("模型没有输入张量。");
        return false;
    }

    const rknn_tensor_attr& input = input_attrs_.front();
    if (input.fmt == RKNN_TENSOR_NCHW) {
        input_channel_ = static_cast<int>(input.dims[1]);
        input_height_ = static_cast<int>(input.dims[2]);
        input_width_ = static_cast<int>(input.dims[3]);
    } else {
        input_height_ = static_cast<int>(input.dims[1]);
        input_width_ = static_cast<int>(input.dims[2]);
        input_channel_ = static_cast<int>(input.dims[3]);
    }

    if (input_channel_ != 3 || input_width_ <= 0 || input_height_ <= 0) {
        SetError("当前模型输入形状不受支持。");
        return false;
    }
    return true;
}

bool YoloV6RknnInfer::Infer(const cv::Mat& bgr_image, std::vector<Detection>& detections, InferenceStats* stats) {
    detections.clear();
    if (stats != nullptr) {
        *stats = InferenceStats{};
    }

    if (!loaded_) {
        SetError("模型尚未加载。");
        return false;
    }
    if (bgr_image.empty()) {
        SetError("输入图像为空。");
        return false;
    }
    if (bgr_image.channels() != 3) {
        SetError("当前只支持 3 通道 BGR 图像。");
        return false;
    }

    LetterBoxInfo letter_box;
    const auto preprocess_begin = std::chrono::steady_clock::now();
    cv::Mat letterboxed = LetterBox(bgr_image, cv::Size(input_width_, input_height_), &letter_box);
    cv::Mat input_rgb;
    cv::cvtColor(letterboxed, input_rgb, cv::COLOR_BGR2RGB);
    if (!input_rgb.isContinuous()) {
        input_rgb = input_rgb.clone();
    }
    const auto preprocess_end = std::chrono::steady_clock::now();

    rknn_input input{};
    input.index = 0;
    input.type = RKNN_TENSOR_UINT8;
    input.fmt = RKNN_TENSOR_NHWC;
    input.buf = input_rgb.data;
    input.size = static_cast<uint32_t>(input_rgb.total() * input_rgb.elemSize());

    const int input_ret = rknn_inputs_set(ctx_, 1, &input);
    if (input_ret != RKNN_SUCC) {
        SetError("设置 RKNN 输入失败，返回值=" + std::to_string(input_ret));
        return false;
    }

    const auto inference_begin = std::chrono::steady_clock::now();
    const int run_ret = rknn_run(ctx_, nullptr);
    if (run_ret != RKNN_SUCC) {
        SetError("执行 RKNN 推理失败，返回值=" + std::to_string(run_ret));
        return false;
    }

    std::vector<rknn_output> outputs(output_attrs_.size());
    for (size_t i = 0; i < outputs.size(); ++i) {
        outputs[i].index = static_cast<uint32_t>(i);
        outputs[i].want_float = 1;
        outputs[i].is_prealloc = 0;
    }

    const int output_ret = rknn_outputs_get(ctx_, static_cast<uint32_t>(outputs.size()), outputs.data(), nullptr);
    if (output_ret != RKNN_SUCC) {
        SetError("获取 RKNN 输出失败，返回值=" + std::to_string(output_ret));
        return false;
    }
    const auto inference_end = std::chrono::steady_clock::now();

    float raw_score_max = 0.0f;
    const auto postprocess_begin = std::chrono::steady_clock::now();
    const bool postprocess_ok = PostProcess(outputs.data(), detections, bgr_image.size(), letter_box, &raw_score_max);
    const auto postprocess_end = std::chrono::steady_clock::now();

    rknn_outputs_release(ctx_, static_cast<uint32_t>(outputs.size()), outputs.data());
    if (!postprocess_ok) {
        return false;
    }

    if (stats != nullptr) {
        stats->preprocess_ms =
            std::chrono::duration<double, std::milli>(preprocess_end - preprocess_begin).count();
        stats->inference_ms =
            std::chrono::duration<double, std::milli>(inference_end - inference_begin).count();
        stats->postprocess_ms =
            std::chrono::duration<double, std::milli>(postprocess_end - postprocess_begin).count();
        stats->output_count = static_cast<int>(output_attrs_.size());
        stats->raw_score_max = raw_score_max;

        rknn_perf_run perf_run{};
        if (rknn_query(ctx_, RKNN_QUERY_PERF_RUN, &perf_run, sizeof(perf_run)) == RKNN_SUCC) {
            stats->inference_ms = static_cast<double>(perf_run.run_duration) / 1000.0;
        }
    }

    last_error_.clear();
    return true;
}

cv::Mat YoloV6RknnInfer::DrawDetections(const cv::Mat& bgr_image, const std::vector<Detection>& detections) const {
    cv::Mat visualized = bgr_image.clone();
    for (const Detection& detection : detections) {
        const int x1 = static_cast<int>(std::round(detection.x1));
        const int y1 = static_cast<int>(std::round(detection.y1));
        const int x2 = static_cast<int>(std::round(detection.x2));
        const int y2 = static_cast<int>(std::round(detection.y2));
        const std::string class_name =
            (detection.class_id >= 0 && detection.class_id < static_cast<int>(class_names_.size()))
                ? class_names_[detection.class_id]
                : "cls_" + std::to_string(detection.class_id);
        const std::string label = class_name + " " + cv::format("%.3f", detection.score);

        cv::rectangle(visualized, cv::Point(x1, y1), cv::Point(x2, y2), cv::Scalar(0, 255, 0), kColorThickness);
        cv::putText(
            visualized,
            label,
            cv::Point(x1, std::max(0, y1 - 8)),
            cv::FONT_HERSHEY_SIMPLEX,
            kLabelScale,
            cv::Scalar(0, 0, 255),
            kLabelThickness,
            cv::LINE_AA);
    }
    return visualized;
}

void YoloV6RknnInfer::SetThresholds(float obj_threshold, float nms_threshold) {
    obj_threshold_ = obj_threshold;
    nms_threshold_ = nms_threshold;
}

cv::Size YoloV6RknnInfer::InputSize() const {
    return cv::Size(input_width_, input_height_);
}

const std::string& YoloV6RknnInfer::LastError() const {
    return last_error_;
}

bool YoloV6RknnInfer::PostProcess(const rknn_output outputs[],
                                  std::vector<Detection>& detections,
                                  const cv::Size& original_size,
                                  const LetterBoxInfo& letter_box,
                                  float* raw_score_max) {
    std::vector<Detection> model_space_detections;
    bool decoded = false;

    if (output_attrs_.size() == 1) {
        decoded = DecodeSingleOutput(
            output_attrs_.front(),
            static_cast<const float*>(outputs[0].buf),
            obj_threshold_,
            &model_space_detections,
            raw_score_max);
    } else {
        decoded = DecodeMultiOutput(
            output_attrs_,
            outputs,
            input_width_,
            input_height_,
            obj_threshold_,
            &model_space_detections,
            raw_score_max);
    }

    if (!decoded) {
        SetError("当前模型的输出张量布局不受支持。");
        return false;
    }

    std::vector<Detection> filtered = ApplyNms(std::move(model_space_detections), nms_threshold_);
    for (Detection& detection : filtered) {
        ScaleDetection(&detection, letter_box, original_size);
    }

    detections = std::move(filtered);
    return true;
}

void YoloV6RknnInfer::SetError(const std::string& message) {
    last_error_ = message;
}

}  // namespace infer_v6_cpp
