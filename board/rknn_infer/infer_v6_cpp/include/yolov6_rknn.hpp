#pragma once

#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "rknn_api.h"

namespace infer_v6_cpp {

struct Detection {
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
    float score = 0.0f;
    int class_id = 0;
};

struct InferenceStats {
    double preprocess_ms = 0.0;
    double inference_ms = 0.0;
    double postprocess_ms = 0.0;
    int output_count = 0;
    float raw_score_max = 0.0f;
};

class YoloV6RknnInfer {
public:
    struct LetterBoxInfo {
        float ratio = 1.0f;
        float dw = 0.0f;
        float dh = 0.0f;
    };

    YoloV6RknnInfer();
    ~YoloV6RknnInfer();

    YoloV6RknnInfer(const YoloV6RknnInfer&) = delete;
    YoloV6RknnInfer& operator=(const YoloV6RknnInfer&) = delete;

    bool Load(const std::string& model_path, bool verbose = true);
    void Unload();
    bool IsLoaded() const;

    bool Infer(const cv::Mat& bgr_image, std::vector<Detection>& detections, InferenceStats* stats = nullptr);
    cv::Mat DrawDetections(const cv::Mat& bgr_image, const std::vector<Detection>& detections) const;

    void SetThresholds(float obj_threshold, float nms_threshold);
    cv::Size InputSize() const;
    const std::string& LastError() const;

private:
    bool QueryTensorInfo(bool verbose);
    bool PostProcess(const rknn_output outputs[],
                     std::vector<Detection>& detections,
                     const cv::Size& original_size,
                     const LetterBoxInfo& letter_box,
                     float* raw_score_max);
    void SetError(const std::string& message);

    rknn_context ctx_ = 0;
    bool loaded_ = false;
    std::string last_error_;
    int input_width_ = 0;
    int input_height_ = 0;
    int input_channel_ = 0;
    float obj_threshold_ = 0.25f;
    float nms_threshold_ = 0.45f;
    std::vector<rknn_tensor_attr> input_attrs_;
    std::vector<rknn_tensor_attr> output_attrs_;
    std::vector<std::string> class_names_;
};

}  // namespace infer_v6_cpp
