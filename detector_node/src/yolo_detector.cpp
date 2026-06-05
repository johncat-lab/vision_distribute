#include "yolo_detector.h"
#include <opencv2/imgproc.hpp>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <sstream>
#include <iomanip>

#ifdef HAS_ONNXRUNTIME
#include "onnxruntime_cxx_api.h"
#endif

#ifdef HAS_NCNN
#include "ncnn/net.h"
#include "ncnn/cpu.h"
#endif

#ifdef HAS_ONNXRUNTIME
struct YoloDetector::OrtSession {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "yolo_detector"};
    Ort::SessionOptions session_options;
    std::unique_ptr<Ort::Session> session;

    std::vector<std::string> input_names_str;
    std::vector<std::string> output_names_str;
    std::vector<const char*> input_names;
    std::vector<const char*> output_names;
};
#endif

// ========== NCNN Net 封装 ==========

#ifdef HAS_NCNN
struct YoloDetector::NcnnSession {
    ncnn::Net net;
    std::string input_name;
    std::string output_name;
};
#endif

// ========== 构造 / 析构 ==========

YoloDetector::YoloDetector(const std::string& model_path,
                           float conf_threshold,
                           float nms_threshold,
                           int input_width,
                           int input_height,
                           Backend backend)
    : model_path_(model_path)
    , conf_threshold_(conf_threshold)
    , nms_threshold_(nms_threshold)
    , input_width_(input_width)
    , input_height_(input_height)
    , backend_(backend) {}

YoloDetector::~YoloDetector() {
#ifdef HAS_ONNXRUNTIME
    delete session_;
    session_ = nullptr;
#endif
#ifdef HAS_NCNN
    delete ncnn_session_;
    ncnn_session_ = nullptr;
#endif
}

// ========== 初始化 ==========

bool YoloDetector::init() {
    if (backend_ == Backend::NCNN) {
#ifdef HAS_NCNN
        return initNcnn();
#else
        std::cerr << "[YOLO] NCNN 后端未编译，请使用 cmake -DUSE_NCNN=ON 重新编译" << std::endl;
        return false;
#endif
    }

#ifdef HAS_ONNXRUNTIME
    try {
        session_ = new OrtSession();

        session_->session_options.SetIntraOpNumThreads(2);
        session_->session_options.SetGraphOptimizationLevel(
            GraphOptimizationLevel::ORT_ENABLE_ALL);

        session_->session.reset(new Ort::Session(
            session_->env, model_path_.c_str(), session_->session_options));

        Ort::AllocatorWithDefaultOptions allocator;

        size_t num_inputs = session_->session->GetInputCount();
        for (size_t i = 0; i < num_inputs; ++i) {
            auto name = session_->session->GetInputNameAllocated(i, allocator);
            session_->input_names_str.push_back(name.get());
        }
        for (auto& s : session_->input_names_str) {
            session_->input_names.push_back(s.c_str());
        }

        size_t num_outputs = session_->session->GetOutputCount();
        for (size_t i = 0; i < num_outputs; ++i) {
            auto name = session_->session->GetOutputNameAllocated(i, allocator);
            session_->output_names_str.push_back(name.get());
        }
        for (auto& s : session_->output_names_str) {
            session_->output_names.push_back(s.c_str());
        }

        ready_.store(true);
        std::cout << "[YOLO] ONNX 模型加载成功: " << model_path_ << std::endl;
        std::cout << "[YOLO] 输入: " << session_->input_names_str[0]
                  << ", 输出: " << session_->output_names_str[0] << std::endl;
        return true;

    } catch (const Ort::Exception& e) {
        std::cerr << "[YOLO] ONNX Runtime 错误: " << e.what() << std::endl;
        delete session_;
        session_ = nullptr;
        return false;
    } catch (const std::exception& e) {
        std::cerr << "[YOLO] 初始化错误: " << e.what() << std::endl;
        delete session_;
        session_ = nullptr;
        return false;
    }
#else
    std::cerr << "[YOLO] ONNX Runtime 后端未编译，请使用 cmake -DUSE_ONNXRUNTIME=ON 重新编译" << std::endl;
    return false;
#endif
}

bool YoloDetector::isReady() const {
    return ready_.load();
}

// ========== NCNN 初始化 ==========

#ifdef HAS_NCNN
bool YoloDetector::initNcnn() {
    ncnn_session_ = new NcnnSession();

    ncnn_session_->net.opt.num_threads = 2;
    ncnn_session_->net.opt.use_vulkan_compute = false;

    std::string param_path = model_path_;
    std::string bin_path = model_path_;

    size_t pos = model_path_.rfind(".param");
    if (pos != std::string::npos) {
        bin_path = model_path_.substr(0, pos) + ".bin";
    } else {
        bin_path = model_path_ + ".bin";
    }

    int ret = ncnn_session_->net.load_param(param_path.c_str());
    if (ret != 0) {
        std::cerr << "[YOLO] NCNN 加载 param 失败: " << param_path << std::endl;
        delete ncnn_session_;
        ncnn_session_ = nullptr;
        return false;
    }

    ret = ncnn_session_->net.load_model(bin_path.c_str());
    if (ret != 0) {
        std::cerr << "[YOLO] NCNN 加载 bin 失败: " << bin_path << std::endl;
        delete ncnn_session_;
        ncnn_session_ = nullptr;
        return false;
    }

    ncnn_session_->input_name = "in0";
    ncnn_session_->output_name = "out0";

    ready_.store(true);
    std::cout << "[YOLO] NCNN 模型加载成功: " << param_path << " + " << bin_path << std::endl;
    return true;
}

ObjectInfoList YoloDetector::detectWithNcnn(const std::vector<float>& input_tensor,
                                            float scale, int pad_x, int pad_y,
                                            int orig_width, int orig_height) {
    ObjectInfoList result;

    if (ncnn_session_ == nullptr) {
        return result;
    }

    ncnn::Mat input_mat(input_width_, input_height_, 3);
    memcpy(input_mat.data, input_tensor.data(), input_tensor.size() * sizeof(float));

    ncnn::Extractor ex = ncnn_session_->net.create_extractor();

    ex.input(ncnn_session_->input_name.c_str(), input_mat);

    ncnn::Mat output_mat;
    int ret = ex.extract(ncnn_session_->output_name.c_str(), output_mat);
    if (ret != 0) {
        std::cerr << "[YOLO] NCNN 推理失败" << std::endl;
        return result;
    }

    std::vector<int64_t> output_shape;
    output_shape.push_back(1);

    std::cout << "[YOLO] NCNN 输出 Mat 形状: c=" << output_mat.c
              << " h=" << output_mat.h << " w=" << output_mat.w << std::endl;

    if (output_mat.c > 1) {
        output_shape.push_back(output_mat.c);
        output_shape.push_back(output_mat.h * output_mat.w);
    } else {
        output_shape.push_back(output_mat.h);
        output_shape.push_back(output_mat.w);
    }

    std::cout << "[YOLO] NCNN output_shape: [" << output_shape[0]
              << ", " << output_shape[1] << ", " << output_shape[2]
              << "]" << std::endl;

    // NCNN 的 ncnn::Mat 数据是连续内存 (无 padding)，
    // 对 c==1 的 2D Mat: data 指向 [h*w] 连续 buffer，行优先排列
    // 对 c>1 的 3D Mat: data 指向 [c*h*w] 连续 buffer，CHW 排列
    // 两种情况都可直接通过 data 指针 + 偏移访问，无需逐通道拷贝
    // 注意: c==1 时 channel(ch) 对 ch>0 会越界，不能逐通道拷贝
    const float* output_data = static_cast<const float*>(output_mat.data);

    std::cout << "[YOLO] NCNN 输出前5个预测的原始值:" << std::endl;
    for (int ch = 0; ch < static_cast<int>(output_shape[1]); ++ch) {
        std::cout << "  ch" << ch << ":";
        for (int j = 0; j < 5 && j < static_cast<int>(output_shape[2]); ++j) {
            std::cout << " " << output_data[ch * output_shape[2] + j];
        }
        std::cout << std::endl;
    }

    int stride = static_cast<int>(output_shape[2]);
    float ch4_max = -999, ch4_min = 999, ch5_max = -999, ch5_min = 999;
    for (int j = 0; j < stride; ++j) {
        float v4 = output_data[4 * stride + j];
        float v5 = output_data[5 * stride + j];
        if (v4 > ch4_max) ch4_max = v4;
        if (v4 < ch4_min) ch4_min = v4;
        if (v5 > ch5_max) ch5_max = v5;
        if (v5 < ch5_min) ch5_min = v5;
    }
    std::cout << "[YOLO] ch4 range: [" << ch4_min << ", " << ch4_max << "]" << std::endl;
    std::cout << "[YOLO] ch5 range: [" << ch5_min << ", " << ch5_max << "]" << std::endl;

    result = postprocess(output_data, output_shape, scale, pad_x, pad_y,
                         orig_width, orig_height);

    return result;
}
#endif

// ========== 标注绘制 (OpenCV) ==========

void YoloDetector::drawAnnotations(cv::Mat& image) {
    for (size_t i = 0; i < last_detections_.size(); ++i) {
        const Detection& d = last_detections_[i];

        double angle_deg = d.angle * 180.0 / CV_PI;
        cv::RotatedRect rrect(
            cv::Point2f(d.x, d.y),
            cv::Size2f(d.w, d.h),
            static_cast<float>(angle_deg)
        );

        cv::Point2f vertices[4];
        rrect.points(vertices);
        for (int j = 0; j < 4; ++j) {
            cv::line(image, vertices[j], vertices[(j + 1) % 4],
                     cv::Scalar(0, 255, 0), 2);
        }

        int cs = 15;
        int cx = static_cast<int>(std::round(d.x));
        int cy = static_cast<int>(std::round(d.y));
        cv::line(image, cv::Point(cx - cs, cy),
                 cv::Point(cx + cs, cy), cv::Scalar(0, 0, 255), 2);
        cv::line(image, cv::Point(cx, cy - cs),
                 cv::Point(cx, cy + cs), cv::Scalar(0, 0, 255), 2);

        char idx_label[32];
        std::snprintf(idx_label, sizeof(idx_label), "#%zu", i);
        cv::putText(image, idx_label, cv::Point(cx + 10, cy - 10),
                    cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 255), 2);

        char coord_label[64];
        std::snprintf(coord_label, sizeof(coord_label),
                      "(%.4f, %.4f, a=%.4f)", d.x, d.y, angle_deg);
        cv::putText(image, coord_label, cv::Point(cx + 10, cy + 15),
                    cv::FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(0, 0, 255), 1);
    }
}

// ========== 完整推理流水线 ==========

ObjectInfoList YoloDetector::detect(const Frame& frame) {
    ObjectInfoList result;

    if (!ready_.load()) {
        return result;
    }

    // 1. 预处理
    float scale = 1.0f;
    int pad_x = 0, pad_y = 0;
    std::vector<float> input_tensor = preprocess(frame, scale, pad_x, pad_y);

    if (input_tensor.empty()) {
        return result;
    }

    // 2. 推理
    if (backend_ == Backend::NCNN) {
#ifdef HAS_NCNN
        result = detectWithNcnn(input_tensor, scale, pad_x, pad_y,
                                frame.width, frame.height);
#endif
        return result;
    }

    // ONNX 推理
#ifdef HAS_ONNXRUNTIME
    if (session_ == nullptr) {
        return result;
    }

    try {
        std::vector<int64_t> input_shape = {1, 3, input_height_, input_width_};

        Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(
            OrtArenaAllocator, OrtMemTypeDefault);

        Ort::Value input_ort = Ort::Value::CreateTensor<float>(
            mem_info, input_tensor.data(), input_tensor.size(),
            input_shape.data(), input_shape.size());

        auto output_tensors = session_->session->Run(
            Ort::RunOptions{nullptr},
            session_->input_names.data(), &input_ort, 1,
            session_->output_names.data(), session_->output_names.size());

        if (!output_tensors.empty() && output_tensors[0].IsTensor()) {
            auto type_info = output_tensors[0].GetTensorTypeAndShapeInfo();
            auto output_shape = type_info.GetShape();
            const float* output_data = output_tensors[0].GetTensorData<float>();

            result = postprocess(output_data, output_shape, scale, pad_x, pad_y,
                                 frame.width, frame.height);
        }

    } catch (const Ort::Exception& e) {
        std::cerr << "[YOLO] 推理错误: " << e.what() << std::endl;
    }
#endif

    return result;
}

// ========== 预处理 ==========

std::vector<float> YoloDetector::preprocess(const Frame& frame,
                                            float& scale, int& pad_x, int& pad_y) {
    if (frame.data.empty() || frame.width == 0 || frame.height == 0) {
        return {};
    }

    int src_w = frame.width;
    int src_h = frame.height;

    std::cout << "[YOLO] 预处理: " << src_w << "x" << src_h
              << " pixelType=0x" << std::hex << frame.pixelType << std::dec
              << " dataSize=" << frame.data.size() << std::endl;

    // 1. 转换为 RGB (3通道)
    std::vector<unsigned char> rgb_data;
    int channels = 3;

    // 根据像素格式处理
    // Mono8: 灰度图复制到3通道
    // 其他格式尽量按灰度处理
    if (frame.pixelType == 0x01080001 || // PixelType_Gvsp_Mono8
        frame.data.size() == static_cast<size_t>(src_w * src_h)) {
        // Mono8 → RGB (R=G=B=gray)
        rgb_data.resize(src_w * src_h * 3);
        for (int i = 0; i < src_w * src_h; ++i) {
            rgb_data[i * 3 + 0] = frame.data[i];
            rgb_data[i * 3 + 1] = frame.data[i];
            rgb_data[i * 3 + 2] = frame.data[i];
        }
    } else if (frame.data.size() == static_cast<size_t>(src_w * src_h * 3)) {
        if (frame.pixelType == 0x02180015) {
            rgb_data.resize(src_w * src_h * 3);
            for (int i = 0; i < src_w * src_h; ++i) {
                rgb_data[i * 3 + 0] = frame.data[i * 3 + 2];
                rgb_data[i * 3 + 1] = frame.data[i * 3 + 1];
                rgb_data[i * 3 + 2] = frame.data[i * 3 + 0];
            }
        } else {
            rgb_data.assign(frame.data.begin(), frame.data.end());
        }
    } else {
        // 未知格式，尝试按 Mono8 处理
        size_t expected_mono = static_cast<size_t>(src_w * src_h);
        if (frame.data.size() >= expected_mono) {
            rgb_data.resize(src_w * src_h * 3);
            for (int i = 0; i < src_w * src_h; ++i) {
                rgb_data[i * 3 + 0] = frame.data[i];
                rgb_data[i * 3 + 1] = frame.data[i];
                rgb_data[i * 3 + 2] = frame.data[i];
            }
        } else {
            std::cerr << "[YOLO] 不支持的像素格式: 0x" << std::hex
                      << frame.pixelType << std::dec << std::endl;
            return {};
        }
    }

    // 2. Letterbox resize 到 input_width_ x input_height_
    scale = std::min(static_cast<float>(input_width_) / src_w,
                     static_cast<float>(input_height_) / src_h);
    int new_w = static_cast<int>(src_w * scale);
    int new_h = static_cast<int>(src_h * scale);
    pad_x = (input_width_ - new_w) / 2;
    pad_y = (input_height_ - new_h) / 2;

    // resize
    std::vector<unsigned char> resized(new_w * new_h * channels);
    bilinearResize(rgb_data.data(), src_w, src_h, channels,
                   resized.data(), new_w, new_h);

    // letterbox (填充灰色 114)
    std::vector<unsigned char> letterbox(input_width_ * input_height_ * channels, 114);
    for (int y = 0; y < new_h; ++y) {
        int dst_y = y + pad_y;
        if (dst_y >= input_height_) break;
        for (int x = 0; x < new_w; ++x) {
            int dst_x = x + pad_x;
            if (dst_x >= input_width_) break;
            int src_idx = (y * new_w + x) * channels;
            int dst_idx = (dst_y * input_width_ + dst_x) * channels;
            letterbox[dst_idx + 0] = resized[src_idx + 0];
            letterbox[dst_idx + 1] = resized[src_idx + 1];
            letterbox[dst_idx + 2] = resized[src_idx + 2];
        }
    }

    // 3. 归一化 + HWC → CHW
    int total_pixels = input_width_ * input_height_;
    std::vector<float> tensor(3 * total_pixels);

    for (int i = 0; i < total_pixels; ++i) {
        tensor[0 * total_pixels + i] = letterbox[i * 3 + 0] / 255.0f;
        tensor[1 * total_pixels + i] = letterbox[i * 3 + 1] / 255.0f;
        tensor[2 * total_pixels + i] = letterbox[i * 3 + 2] / 255.0f;
    }

    return tensor;
}

// ========== 后处理 ==========

ObjectInfoList YoloDetector::postprocess(const float* output_data,
                                         const std::vector<int64_t>& output_shape,
                                         float scale, int pad_x, int pad_y,
                                         int orig_width, int orig_height) {
    ObjectInfoList result;

    if (output_shape.size() < 3) {
        std::cerr << "[YOLO] 输出维度异常: " << output_shape.size() << std::endl;
        return result;
    }

    // YOLOv11-OBB 输出格式: [1, 5+num_classes, num_predictions]
    // 或 [1, num_predictions, 5+num_classes]
    // 我们需要判断哪个维度是 predictions

    int64_t dim1 = output_shape[1];
    int64_t dim2 = output_shape[2];

    int num_predictions;
    int num_channels;
    bool transposed;

    // 通常 num_predictions >> num_channels (如 8400 vs 6)
    if (dim1 > dim2) {
        // [1, num_predictions, num_channels] - 已按行排列
        num_predictions = static_cast<int>(dim1);
        num_channels = static_cast<int>(dim2);
        transposed = false;
    } else {
        // [1, num_channels, num_predictions] - 需要转置访问
        num_predictions = static_cast<int>(dim2);
        num_channels = static_cast<int>(dim1);
        transposed = true;
    }

    // OBB: YOLOv11-OBB 输出格式 [x, y, w, h, class_scores..., angle]
    // angle 在最后一个通道，类别置信度在 4~4+num_classes 通道
    int num_classes = num_channels - 5;
    if (num_classes <= 0) {
        std::cerr << "[YOLO] 输出通道数异常: " << num_channels
                  << " (dim1=" << dim1 << " dim2=" << dim2
                  << " num_predictions=" << num_predictions
                  << " transposed=" << transposed << ")" << std::endl;
        return result;
    }

    std::cout << "[YOLO] 后处理: num_predictions=" << num_predictions
              << " num_channels=" << num_channels
              << " num_classes=" << num_classes
              << " transposed=" << transposed
              << " conf_threshold=" << conf_threshold_ << std::endl;

    std::vector<Detection> detections;

    int score_buckets[5] = {0};
    float max_conf_all = 0;

    for (int i = 0; i < num_predictions; ++i) {
        float x, y, w, h, angle;
        float max_score = 0;
        int max_class = 0;

        if (transposed) {
            x     = output_data[0 * num_predictions + i];
            y     = output_data[1 * num_predictions + i];
            w     = output_data[2 * num_predictions + i];
            h     = output_data[3 * num_predictions + i];

            for (int c = 0; c < num_classes; ++c) {
                float score = output_data[(4 + c) * num_predictions + i];
                if (score > max_score) {
                    max_score = score;
                    max_class = c;
                }
            }

            angle = output_data[(4 + num_classes) * num_predictions + i];
        } else {
            const float* row = output_data + i * num_channels;
            x     = row[0];
            y     = row[1];
            w     = row[2];
            h     = row[3];

            for (int c = 0; c < num_classes; ++c) {
                float score = row[4 + c];
                if (score > max_score) {
                    max_score = score;
                    max_class = c;
                }
            }

            angle = row[4 + num_classes];
        }

        if (max_score > max_conf_all) max_conf_all = max_score;
        if (max_score < 0.1f) score_buckets[0]++;
        else if (max_score < 0.3f) score_buckets[1]++;
        else if (max_score < 0.5f) score_buckets[2]++;
        else if (max_score < 0.7f) score_buckets[3]++;
        else score_buckets[4]++;

        // 置信度过滤
        if (max_score < conf_threshold_) {
            continue;
        }

        Detection det;
        det.x = x;
        det.y = y;
        det.w = w;
        det.h = h;
        det.angle = angle;
        det.confidence = max_score;
        det.class_id = max_class;
        detections.push_back(det);
    }

    // NMS
    std::vector<Detection> nms_result = rotatedNMS(detections);

    std::cout << "[YOLO] 检测结果: 置信度过滤后=" << detections.size()
              << " NMS后=" << nms_result.size() << std::endl;
    std::cout << "[YOLO] 置信度分布: [<0.1]=" << score_buckets[0]
              << " [0.1-0.3]=" << score_buckets[1]
              << " [0.3-0.5]=" << score_buckets[2]
              << " [0.5-0.7]=" << score_buckets[3]
              << " [0.7-1.0]=" << score_buckets[4]
              << " max_conf=" << max_conf_all << std::endl;

    // 清空上一次缓存
    last_detections_.clear();

    // 坐标反算回原图
    for (auto& det : nms_result) {
        // letterbox 坐标 → 原图坐标
        float orig_x = (det.x - pad_x) / scale;
        float orig_y = (det.y - pad_y) / scale;
        float orig_w = det.w / scale;
        float orig_h = det.h / scale;

        // 限制在原图范围内
        orig_x = std::max(0.0f, std::min(orig_x, static_cast<float>(orig_width)));
        orig_y = std::max(0.0f, std::min(orig_y, static_cast<float>(orig_height)));

        // 角度: 弧度 → 角度
        float angle_deg = det.angle * 180.0f / static_cast<float>(M_PI);

        ObjectInfo obj = ObjectInfo::Builder()
                             .setX(static_cast<double>(orig_x))
                             .setY(static_cast<double>(orig_y))
                             .setAngle(static_cast<double>(-angle_deg))
                             .setType(det.class_id)
                             .build();
        result.add(obj);

        // 缓存原图坐标系下的检测框，供 saveAnnotated() 绘制
        Detection raw = det;
        raw.x = orig_x;
        raw.y = orig_y;
        raw.w = orig_w;
        raw.h = orig_h;
        // raw.angle 保留弧度，绘制时直接使用
        last_detections_.push_back(raw);
    }

    return result;
}

// ========== Rotated NMS ==========

std::vector<YoloDetector::Detection> YoloDetector::rotatedNMS(
    std::vector<Detection>& detections) {

    // 按置信度降序排列
    std::sort(detections.begin(), detections.end(),
              [](const Detection& a, const Detection& b) {
                  return a.confidence > b.confidence;
              });

    std::vector<bool> suppressed(detections.size(), false);
    std::vector<Detection> result;

    for (size_t i = 0; i < detections.size(); ++i) {
        if (suppressed[i]) continue;
        result.push_back(detections[i]);

        for (size_t j = i + 1; j < detections.size(); ++j) {
            if (suppressed[j]) continue;
            if (detections[i].class_id != detections[j].class_id) continue;

            float iou = rotatedIoU(detections[i], detections[j]);
            if (iou > nms_threshold_) {
                suppressed[j] = true;
                continue;
            }

            // 旋转框 IoU 对角度差异敏感，两个预测同一物体的框
            // 可能因角度微小差异导致 IoU 很低而逃逸 NMS。
            // 补充中心距离检查：中心距 < 较短边的 15% → 同一物体
            float dx = detections[i].x - detections[j].x;
            float dy = detections[i].y - detections[j].y;
            float dist = std::sqrt(dx * dx + dy * dy);
            float min_side_i = std::min(detections[i].w, detections[i].h);
            float min_side_j = std::min(detections[j].w, detections[j].h);
            float min_side = std::min(min_side_i, min_side_j);
            if (dist < min_side * 0.15f) {
                suppressed[j] = true;
            }
        }
    }

    return result;
}

float YoloDetector::rotatedIoU(const Detection& a, const Detection& b) {
    // 使用 Sutherland-Hodgman 多边形裁剪计算精确旋转矩形 IoU

    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dist = std::sqrt(dx * dx + dy * dy);

    // 快速排除: 中心距离大于两者对角线之和的一半，一定不重叠
    float diag_a = std::sqrt(a.w * a.w + a.h * a.h);
    float diag_b = std::sqrt(b.w * b.w + b.h * b.h);
    if (dist > (diag_a + diag_b) * 0.5f) {
        return 0.0f;
    }

    // 计算两个旋转矩形的4个顶点
    auto getVertices = [](const Detection& d) -> std::vector<std::pair<float,float>> {
        float ca = std::cos(d.angle), sa = std::sin(d.angle);
        float hw = d.w * 0.5f, hh = d.h * 0.5f;
        float lx[4] = { -hw,  hw,  hw, -hw };
        float ly[4] = { -hh, -hh,  hh,  hh };
        std::vector<std::pair<float,float>> pts(4);
        for (int i = 0; i < 4; ++i) {
            pts[i].first  = d.x + lx[i] * ca - ly[i] * sa;
            pts[i].second = d.y + lx[i] * sa + ly[i] * ca;
        }
        return pts;
    };

    auto polyA = getVertices(a);
    auto polyB = getVertices(b);

    // Sutherland-Hodgman 多边形裁剪: 用 polyA 裁剪 polyB
    auto clipPolygon = [](const std::vector<std::pair<float,float>>& subject,
                          const std::vector<std::pair<float,float>>& clip) -> std::vector<std::pair<float,float>> {
        auto output = subject;
        for (size_t i = 0; i < clip.size() && !output.empty(); ++i) {
            auto input = output;
            output.clear();
            size_t j = (i + 1) % clip.size();
            float x1 = clip[i].first,  y1 = clip[i].second;
            float x2 = clip[j].first,  y2 = clip[j].second;
            float ex = x2 - x1, ey = y2 - y1;

            auto inside = [&](const std::pair<float,float>& p) -> bool {
                return ex * (p.second - y1) - ey * (p.first - x1) >= 0;
            };

            auto intersect = [&](const std::pair<float,float>& p1,
                                 const std::pair<float,float>& p2) -> std::pair<float,float> {
                float dx1 = p2.first - p1.first, dy1 = p2.second - p1.second;
                float denom = ex * dy1 - ey * dx1;
                if (std::abs(denom) < 1e-10f) return p1;
                float t = ((x1 - p1.first) * dy1 - (y1 - p1.second) * dx1) / denom;
                return {p1.first + t * dx1, p1.second + t * dy1};
            };

            for (size_t k = 0; k < input.size(); ++k) {
                size_t k2 = (k + 1) % input.size();
                bool in_k  = inside(input[k]);
                bool in_k2 = inside(input[k2]);
                if (in_k && in_k2) {
                    output.push_back(input[k2]);
                } else if (in_k && !in_k2) {
                    output.push_back(intersect(input[k], input[k2]));
                } else if (!in_k && in_k2) {
                    output.push_back(intersect(input[k], input[k2]));
                    output.push_back(input[k2]);
                }
            }
        }
        return output;
    };

    auto intersection = clipPolygon(polyB, polyA);

    // Shoelace 公式计算多边形面积
    auto polyArea = [](const std::vector<std::pair<float,float>>& poly) -> float {
        if (poly.size() < 3) return 0.0f;
        float area = 0.0f;
        for (size_t i = 0; i < poly.size(); ++i) {
            size_t j = (i + 1) % poly.size();
            area += poly[i].first * poly[j].second;
            area -= poly[j].first * poly[i].second;
        }
        return std::abs(area) * 0.5f;
    };

    float inter_area = polyArea(intersection);
    float area_a = a.w * a.h;
    float area_b = b.w * b.h;
    float union_area = area_a + area_b - inter_area;

    if (union_area <= 0) return 0.0f;
    return inter_area / union_area;
}

// ========== 双线性插值 resize ==========

void YoloDetector::bilinearResize(const unsigned char* src, int src_w, int src_h,
                                  int channels,
                                  unsigned char* dst, int dst_w, int dst_h) {
    float x_ratio = static_cast<float>(src_w) / dst_w;
    float y_ratio = static_cast<float>(src_h) / dst_h;

    for (int y = 0; y < dst_h; ++y) {
        float src_y = y * y_ratio;
        int y0 = static_cast<int>(src_y);
        int y1 = std::min(y0 + 1, src_h - 1);
        float fy = src_y - y0;

        for (int x = 0; x < dst_w; ++x) {
            float src_x = x * x_ratio;
            int x0 = static_cast<int>(src_x);
            int x1 = std::min(x0 + 1, src_w - 1);
            float fx = src_x - x0;

            for (int c = 0; c < channels; ++c) {
                float val =
                    src[(y0 * src_w + x0) * channels + c] * (1 - fx) * (1 - fy) +
                    src[(y0 * src_w + x1) * channels + c] * fx * (1 - fy) +
                    src[(y1 * src_w + x0) * channels + c] * (1 - fx) * fy +
                    src[(y1 * src_w + x1) * channels + c] * fx * fy;

                dst[(y * dst_w + x) * channels + c] =
                    static_cast<unsigned char>(std::max(0.0f, std::min(255.0f, val)));
            }
        }
    }
}

// ========== 带标注图像保存 ==========
// 以下为自实现的最小绘制 / 5x7 位图字体 / BMP 落盘
// 不依赖 OpenCV，输出 24bit BGR BMP 文件

namespace {

struct RgbColor { unsigned char r, g, b; };

static inline void putPixel(unsigned char* buf, int W, int H,
                            int x, int y, RgbColor c) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    int idx = (y * W + x) * 3;
    buf[idx + 0] = c.r;
    buf[idx + 1] = c.g;
    buf[idx + 2] = c.b;
}

static void drawLine(unsigned char* buf, int W, int H,
                     int x0, int y0, int x1, int y1,
                     RgbColor c, int thickness = 2) {
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int half = thickness / 2;
    while (true) {
        for (int iy = -half; iy <= half; ++iy) {
            for (int ix = -half; ix <= half; ++ix) {
                putPixel(buf, W, H, x0 + ix, y0 + iy, c);
            }
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// 5x7 位图字体 (每列用 1 个 byte，bit0 = 顶，bit6 = 底)
struct Glyph { char c; unsigned char cols[5]; };
static const Glyph kGlyphs[] = {
    {' ', {0x00,0x00,0x00,0x00,0x00}},
    {'#', {0x14,0x7F,0x14,0x7F,0x14}},
    {'.', {0x00,0x60,0x60,0x00,0x00}},
    {'-', {0x08,0x08,0x08,0x08,0x08}},
    {'=', {0x14,0x14,0x14,0x14,0x14}},
    {'0', {0x3E,0x51,0x49,0x45,0x3E}},
    {'1', {0x00,0x42,0x7F,0x40,0x00}},
    {'2', {0x42,0x61,0x51,0x49,0x46}},
    {'3', {0x21,0x41,0x45,0x4B,0x31}},
    {'4', {0x18,0x14,0x12,0x7F,0x10}},
    {'5', {0x27,0x45,0x45,0x45,0x39}},
    {'6', {0x3C,0x4A,0x49,0x49,0x30}},
    {'7', {0x01,0x71,0x09,0x05,0x03}},
    {'8', {0x36,0x49,0x49,0x49,0x36}},
    {'9', {0x06,0x49,0x49,0x29,0x1E}},
    {'a', {0x20,0x54,0x54,0x54,0x78}},
    {'t', {0x04,0x3F,0x44,0x40,0x20}},
    {'x', {0x44,0x28,0x10,0x28,0x44}},
    {'y', {0x0C,0x50,0x50,0x50,0x3C}},
};

static const unsigned char* findGlyph(char c) {
    for (const auto& g : kGlyphs) {
        if (g.c == c) return g.cols;
    }
    return nullptr;
}

static void drawChar(unsigned char* buf, int W, int H,
                     int x, int y, char c, int scale, RgbColor color) {
    const unsigned char* cols = findGlyph(c);
    if (!cols) cols = findGlyph(' ');
    if (!cols) return;
    for (int cx = 0; cx < 5; ++cx) {
        unsigned char col = cols[cx];
        for (int cy = 0; cy < 7; ++cy) {
            if ((col >> cy) & 1) {
                // 放大 scale 倍
                for (int sy = 0; sy < scale; ++sy) {
                    for (int sx = 0; sx < scale; ++sx) {
                        putPixel(buf, W, H,
                                 x + cx * scale + sx,
                                 y + cy * scale + sy, color);
                    }
                }
            }
        }
    }
}

static void drawText(unsigned char* buf, int W, int H,
                     int x, int y, const std::string& s,
                     int scale, RgbColor color) {
    int cur_x = x;
    int char_w = 6 * scale; // 5 像素字宽 + 1 像素间隔
    for (char c : s) {
        drawChar(buf, W, H, cur_x, y, c, scale, color);
        cur_x += char_w;
    }
}

// 绘制文字背景矩形（实心矩形，提升可读性）
static void fillRect(unsigned char* buf, int W, int H,
                     int x, int y, int w, int h, RgbColor color) {
    for (int iy = y; iy < y + h; ++iy) {
        for (int ix = x; ix < x + w; ++ix) {
            putPixel(buf, W, H, ix, iy, color);
        }
    }
}

static void drawRotatedBox(unsigned char* buf, int W, int H,
                           float cx, float cy, float w, float h, float angle_rad,
                           RgbColor color, int thickness) {
    float ca = std::cos(angle_rad), sa = std::sin(angle_rad);
    float hw = w * 0.5f, hh = h * 0.5f;
    // 4 个角点局部坐标: (±hw, ±hh)
    float lx[4] = { -hw,  hw, hw, -hw };
    float ly[4] = { -hh, -hh, hh,  hh };
    int px[4], py[4];
    for (int i = 0; i < 4; ++i) {
        px[i] = static_cast<int>(cx + lx[i] * ca - ly[i] * sa);
        py[i] = static_cast<int>(cy + lx[i] * sa + ly[i] * ca);
    }
    for (int i = 0; i < 4; ++i) {
        int j = (i + 1) % 4;
        drawLine(buf, W, H, px[i], py[i], px[j], py[j], color, thickness);
    }
}

// 写 24bit BGR BMP 文件（行对齐 4 字节，自底向上）
static bool saveBMP(const std::string& path, const unsigned char* rgb,
                    int W, int H) {
    int row_bytes = W * 3;
    int pad = (4 - (row_bytes % 4)) % 4;
    int row_stride = row_bytes + pad;
    uint32_t pixel_size = static_cast<uint32_t>(row_stride) * H;
    uint32_t file_size = 54 + pixel_size;

    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open()) {
        std::cerr << "[YOLO] 无法打开保存文件: " << path << std::endl;
        return false;
    }

    unsigned char hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2]  = (unsigned char)(file_size);
    hdr[3]  = (unsigned char)(file_size >> 8);
    hdr[4]  = (unsigned char)(file_size >> 16);
    hdr[5]  = (unsigned char)(file_size >> 24);
    hdr[10] = 54;                       // 像素数据偏移
    hdr[14] = 40;                       // DIB header size
    hdr[18] = (unsigned char)(W);
    hdr[19] = (unsigned char)(W >> 8);
    hdr[20] = (unsigned char)(W >> 16);
    hdr[21] = (unsigned char)(W >> 24);
    hdr[22] = (unsigned char)(H);
    hdr[23] = (unsigned char)(H >> 8);
    hdr[24] = (unsigned char)(H >> 16);
    hdr[25] = (unsigned char)(H >> 24);
    hdr[26] = 1;                        // planes
    hdr[28] = 24;                       // bits per pixel
    hdr[34] = (unsigned char)(pixel_size);
    hdr[35] = (unsigned char)(pixel_size >> 8);
    hdr[36] = (unsigned char)(pixel_size >> 16);
    hdr[37] = (unsigned char)(pixel_size >> 24);
    ofs.write(reinterpret_cast<char*>(hdr), 54);

    // BMP 底 → 顶 写入，RGB → BGR
    std::vector<unsigned char> row(row_stride, 0);
    for (int y = H - 1; y >= 0; --y) {
        for (int x = 0; x < W; ++x) {
            int src = (y * W + x) * 3;
            int dst = x * 3;
            row[dst + 0] = rgb[src + 2]; // B
            row[dst + 1] = rgb[src + 1]; // G
            row[dst + 2] = rgb[src + 0]; // R
        }
        ofs.write(reinterpret_cast<char*>(row.data()), row_stride);
    }
    ofs.close();
    return true;
}

// Frame (Mono8/RGB/BGR) → RGB 缓冲区
static bool frameToRgb(const Frame& frame, std::vector<unsigned char>& rgb) {
    if (frame.data.empty() || frame.width == 0 || frame.height == 0) {
        return false;
    }
    int W = frame.width;
    int H = frame.height;
    size_t expected_mono = static_cast<size_t>(W) * H;
    size_t expected_rgb  = expected_mono * 3;

    if (frame.pixelType == 0x01080001 || frame.data.size() == expected_mono) {
        rgb.resize(expected_rgb);
        for (size_t i = 0; i < expected_mono; ++i) {
            rgb[i * 3 + 0] = frame.data[i];
            rgb[i * 3 + 1] = frame.data[i];
            rgb[i * 3 + 2] = frame.data[i];
        }
        return true;
    }
    if (frame.data.size() == expected_rgb) {
        rgb.assign(frame.data.begin(), frame.data.end());
        return true;
    }
    if (frame.data.size() >= expected_mono) {
        rgb.resize(expected_rgb);
        for (size_t i = 0; i < expected_mono; ++i) {
            rgb[i * 3 + 0] = frame.data[i];
            rgb[i * 3 + 1] = frame.data[i];
            rgb[i * 3 + 2] = frame.data[i];
        }
        return true;
    }
    return false;
}

static std::string formatFloat(double v, int precision = 1) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << v;
    return oss.str();
}

} // namespace

bool YoloDetector::saveAnnotated(const Frame& frame, const std::string& path) {
    std::vector<unsigned char> rgb;
    if (!frameToRgb(frame, rgb)) {
        std::cerr << "[YOLO] saveAnnotated: 不支持的帧格式" << std::endl;
        return false;
    }

    int W = frame.width;
    int H = frame.height;

    const RgbColor red   = {255, 0,   0};
    const RgbColor black = {0,   0,   0};
    const RgbColor yellow= {255, 255, 0};

    int thickness = std::max(2, (std::min(W, H) / 400));
    int text_scale = std::max(2, (std::min(W, H) / 600));
    int text_h = 7 * text_scale;

    // 绘制每个检测目标
    for (size_t i = 0; i < last_detections_.size(); ++i) {
        const Detection& d = last_detections_[i];

        // 旋转框
        drawRotatedBox(rgb.data(), W, H,
                       d.x, d.y, d.w, d.h, d.angle,
                       red, thickness);

        // 文本: #i x=.. y=.. a=.. t=..
        double angle_deg = d.angle * 180.0 / M_PI;
        std::string label = "#" + std::to_string(static_cast<int>(i))
                          + " x=" + formatFloat(d.x, 4)
                          + " y=" + formatFloat(d.y, 4)
                          + " a=" + formatFloat(angle_deg, 1)
                          + " t=" + std::to_string(d.class_id);

        int text_w = static_cast<int>(label.size()) * 6 * text_scale;
        int tx = static_cast<int>(d.x - d.w * 0.5f);
        int ty = static_cast<int>(d.y - d.h * 0.5f) - text_h - 4;
        if (tx < 0) tx = 0;
        if (ty < 0) ty = static_cast<int>(d.y + d.h * 0.5f) + 4;
        if (tx + text_w > W) tx = W - text_w;
        if (tx < 0) tx = 0;

        // 背景块 + 文字
        fillRect(rgb.data(), W, H, tx, ty, text_w + 2, text_h + 2, black);
        drawText(rgb.data(), W, H, tx + 1, ty + 1, label, text_scale, yellow);
    }

    if (!saveBMP(path, rgb.data(), W, H)) {
        return false;
    }
    std::cout << "[YOLO] 已保存标注图: " << path
              << " (目标数: " << last_detections_.size() << ")" << std::endl;
    return true;
}
