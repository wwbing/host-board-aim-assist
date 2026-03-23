#include <cmath>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "yolov6_rknn.hpp"

namespace {

std::filesystem::path DefaultModelPath() {
    return std::filesystem::path(INFER_V6_CPP_SOURCE_DIR) / ".." / "model" /
           "v6n_cs2_head_rk3588_i8_normal_layer_channel.rknn";
}

std::filesystem::path DefaultImageDir() {
    return std::filesystem::path(INFER_V6_CPP_SOURCE_DIR) / ".." / "images";
}

std::filesystem::path DefaultOutputDir() {
    return std::filesystem::path(INFER_V6_CPP_SOURCE_DIR) / "result";
}

bool IsImageFile(const std::filesystem::path& path) {
    static const std::set<std::string> kSuffixes = {".jpg", ".jpeg", ".png", ".bmp"};
    std::string suffix = path.extension().string();
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return kSuffixes.count(suffix) != 0;
}

void PrintUsage(const char* program_name) {
    std::cout
        << "Usage: " << program_name
        << " [--model MODEL] [--image IMAGE] [--image_dir DIR] [--output_dir DIR] [--show]\n"
        << "Defaults:\n"
        << "  model : " << DefaultModelPath() << "\n"
        << "  image_dir : " << DefaultImageDir() << "\n"
        << "  output_dir: " << DefaultOutputDir() << "\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    std::filesystem::path model_path = DefaultModelPath();
    std::filesystem::path image_path;
    std::filesystem::path image_dir = DefaultImageDir();
    std::filesystem::path output_dir = DefaultOutputDir();
    bool show = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        }
        if (arg == "--show") {
            show = true;
            continue;
        }
        if (arg == "--model" && i + 1 < argc) {
            model_path = argv[++i];
            continue;
        }
        if (arg == "--image" && i + 1 < argc) {
            image_path = argv[++i];
            continue;
        }
        if (arg == "--image_dir" && i + 1 < argc) {
            image_dir = argv[++i];
            continue;
        }
        if (arg == "--output_dir" && i + 1 < argc) {
            output_dir = argv[++i];
            continue;
        }

        std::cerr << "Unknown argument: " << arg << std::endl;
        PrintUsage(argv[0]);
        return 1;
    }

    std::vector<std::filesystem::path> image_paths;
    if (!image_path.empty()) {
        image_paths.push_back(image_path);
    } else {
        if (!std::filesystem::exists(image_dir)) {
            std::cerr << "Image directory not found: " << image_dir << std::endl;
            return 1;
        }
        for (const auto& entry : std::filesystem::directory_iterator(image_dir)) {
            if (entry.is_regular_file() && IsImageFile(entry.path())) {
                image_paths.push_back(entry.path());
            }
        }
        std::sort(image_paths.begin(), image_paths.end());
    }

    if (image_paths.empty()) {
        std::cerr << "No input images found." << std::endl;
        return 1;
    }

    infer_v6_cpp::YoloV6RknnInfer infer;
    if (!infer.Load(model_path.string())) {
        std::cerr << "Failed to load model: " << infer.LastError() << std::endl;
        return 1;
    }

    std::filesystem::create_directories(output_dir);

    double total_preprocess_ms = 0.0;
    double total_inference_ms = 0.0;
    double total_postprocess_ms = 0.0;

    for (size_t index = 0; index < image_paths.size(); ++index) {
        const std::filesystem::path& current_image = image_paths[index];
        const cv::Mat image = cv::imread(current_image.string(), cv::IMREAD_COLOR);
        if (image.empty()) {
            std::cerr << "Failed to read image: " << current_image << std::endl;
            return 1;
        }

        std::vector<infer_v6_cpp::Detection> detections;
        infer_v6_cpp::InferenceStats stats;
        if (!infer.Infer(image, detections, &stats)) {
            std::cerr << "Inference failed on " << current_image << ": " << infer.LastError() << std::endl;
            return 1;
        }

        cv::Mat visualized = infer.DrawDetections(image, detections);
        const std::filesystem::path output_path = output_dir / current_image.filename();
        if (!cv::imwrite(output_path.string(), visualized)) {
            std::cerr << "Failed to write output image: " << output_path << std::endl;
            return 1;
        }

        total_preprocess_ms += stats.preprocess_ms;
        total_inference_ms += stats.inference_ms;
        total_postprocess_ms += stats.postprocess_ms;

        std::cout
            << "[" << (index + 1) << "/" << image_paths.size() << "] " << current_image.filename().string()
            << " detections=" << detections.size()
            << " preprocess_ms=" << stats.preprocess_ms
            << " inference_ms=" << stats.inference_ms
            << " postprocess_ms=" << stats.postprocess_ms
            << " raw_score_max=" << stats.raw_score_max
            << " saved=" << output_path
            << std::endl;

        for (const auto& detection : detections) {
            std::cout
                << "  Head @ ("
                << static_cast<int>(std::round(detection.x1)) << ", "
                << static_cast<int>(std::round(detection.y1)) << ", "
                << static_cast<int>(std::round(detection.x2)) << ", "
                << static_cast<int>(std::round(detection.y2)) << ") "
                << detection.score
                << std::endl;
        }

        if (show) {
            cv::imshow("infer_v6_cpp_demo", visualized);
            cv::waitKey(0);
        }
    }

    const double image_count = static_cast<double>(image_paths.size());
    std::cout
        << "SUMMARY images=" << image_paths.size()
        << " avg_preprocess_ms=" << (total_preprocess_ms / image_count)
        << " avg_inference_ms=" << (total_inference_ms / image_count)
        << " avg_postprocess_ms=" << (total_postprocess_ms / image_count)
        << std::endl;
    return 0;
}
