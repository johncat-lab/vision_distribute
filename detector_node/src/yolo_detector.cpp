#include "yolo_detector.h"
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

#include "onnxruntime_cxx_api.h"

// ========== ONNX Runtime Session 封装 ==========

struct YoloDetector::OrtSession {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "yolo_detector"};
    Ort::SessionOptions session_options;
    std::unique_ptr<Ort::Session> session;

    std::vector<std::string> input_names_str;
    std::vector<std::string> output_names_str;
    std::vector<const char*> input_names;
    std::vector<const char*> output_names;
};

// ========== 构造 / 析构 ==========

YoloDetector::YoloDetector(const std::string& model_path,
                           float conf_threshold,
                           float nms_threshold,
                           int input_width,
                           int input_height)
    : model_path_(model_path)
    , conf_threshold_(conf_threshold)
    , nms_threshold_(nms_threshold)
    , input_width_(input_width)
    , input_height_(input_height) {}

YoloDetector::~YoloDetector() {
    delete session_;
    session_ = nullptr;
}

// ========== 初始化 ==========

bool YoloDetector::init() {
    try {
        session_ = new OrtSession();

        // 配置 session options
        session_->session_options.SetIntraOpNumThreads(2);
        session_->session_options.SetGraphOptimizationLevel(
            GraphOptimizationLevel::ORT_ENABLE_ALL);

        // 加载模型
        session_->session.reset(new Ort::Session(
            session_->env, model_path_.c_str(), session_->session_options));

        Ort::AllocatorWithDefaultOptions allocator;

        // 获取输入名称
        size_t num_inputs = session_->session->GetInputCount();
        for (size_t i = 0; i < num_inputs; ++i) {
            auto name = session_->session->GetInputNameAllocated(i, allocator);
            session_->input_names_str.push_back(name.get());
        }
        for (auto& s : session_->input_names_str) {
            session_->input_names.push_back(s.c_str());
        }

        // 获取输出名称
        size_t num_outputs = session_->session->GetOutputCount();
        for (size_t i = 0; i < num_outputs; ++i) {
            auto name = session_->session->GetOutputNameAllocated(i, allocator);
            session_->output_names_str.push_back(name.get());
        }
        for (auto& s : session_->output_names_str) {
            session_->output_names.push_back(s.c_str());
        }

        ready_.store(true);
        std::cout << "[YOLO] 模型加载成功: " << model_path_ << std::endl;
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
}

bool YoloDetector::isReady() const {
    return ready_.load();
}

// ========== 完整推理流水线 ==========

ObjectInfoList YoloDetector::detect(const Frame& frame) {
    ObjectInfoList result;

    if (!ready_.load() || session_ == nullptr) {
        return result;
    }

    // 1. 预处理
    float scale = 1.0f;
    int pad_x = 0, pad_y = 0;
    std::vector<float> input_tensor = preprocess(frame, scale, pad_x, pad_y);

    if (input_tensor.empty()) {
        return result;
    }

    // 2. ONNX 推理
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

        // 3. 后处理
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
        // RGB8 或 BGR8 直接使用
        rgb_data.assign(frame.data.begin(), frame.data.end());
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
        tensor[0 * total_pixels + i] = letterbox[i * 3 + 0] / 255.0f; // R
        tensor[1 * total_pixels + i] = letterbox[i * 3 + 1] / 255.0f; // G
        tensor[2 * total_pixels + i] = letterbox[i * 3 + 2] / 255.0f; // B
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

    // OBB: 前5个通道 = x, y, w, h, angle; 后面是类别置信度
    int num_classes = num_channels - 5;
    if (num_classes <= 0) {
        std::cerr << "[YOLO] 输出通道数异常: " << num_channels << std::endl;
        return result;
    }

    // 解析检测结果
    std::vector<Detection> detections;

    for (int i = 0; i < num_predictions; ++i) {
        // 获取每个预测的数据
        float x, y, w, h, angle;
        float max_score = 0;
        int max_class = 0;

        if (transposed) {
            // [1, num_channels, num_predictions]
            x     = output_data[0 * num_predictions + i];
            y     = output_data[1 * num_predictions + i];
            w     = output_data[2 * num_predictions + i];
            h     = output_data[3 * num_predictions + i];
            angle = output_data[4 * num_predictions + i];

            for (int c = 0; c < num_classes; ++c) {
                float score = output_data[(5 + c) * num_predictions + i];
                if (score > max_score) {
                    max_score = score;
                    max_class = c;
                }
            }
        } else {
            // [1, num_predictions, num_channels]
            const float* row = output_data + i * num_channels;
            x     = row[0];
            y     = row[1];
            w     = row[2];
            h     = row[3];
            angle = row[4];

            for (int c = 0; c < num_classes; ++c) {
                float score = row[5 + c];
                if (score > max_score) {
                    max_score = score;
                    max_class = c;
                }
            }
        }

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
                             .setAngle(static_cast<double>(angle_deg))
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
            }
        }
    }

    return result;
}

float YoloDetector::rotatedIoU(const Detection& a, const Detection& b) {
    // 简化版旋转矩形 IoU:
    // 使用中心距离 + 面积比 + 角度差作为近似判断
    // 对于工业场景（物体间距较大），这种近似足够

    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dist = std::sqrt(dx * dx + dy * dy);

    // 如果中心距离大于两者对角线之和的一半，一定不重叠
    float diag_a = std::sqrt(a.w * a.w + a.h * a.h);
    float diag_b = std::sqrt(b.w * b.w + b.h * b.h);
    if (dist > (diag_a + diag_b) * 0.5f) {
        return 0.0f;
    }

    // 使用外接正矩形计算近似 IoU
    // 计算每个旋转矩形的 AABB
    float cos_a = std::cos(a.angle), sin_a = std::sin(a.angle);
    float cos_b = std::cos(b.angle), sin_b = std::sin(b.angle);

    // AABB 半宽半高
    float hw_a = std::abs(a.w * cos_a) * 0.5f + std::abs(a.h * sin_a) * 0.5f;
    float hh_a = std::abs(a.w * sin_a) * 0.5f + std::abs(a.h * cos_a) * 0.5f;
    float hw_b = std::abs(b.w * cos_b) * 0.5f + std::abs(b.h * sin_b) * 0.5f;
    float hh_b = std::abs(b.w * sin_b) * 0.5f + std::abs(b.h * cos_b) * 0.5f;

    // AABB 交集
    float x_overlap = std::max(0.0f, std::min(a.x + hw_a, b.x + hw_b) -
                                     std::max(a.x - hw_a, b.x - hw_b));
    float y_overlap = std::max(0.0f, std::min(a.y + hh_a, b.y + hh_b) -
                                     std::max(a.y - hh_a, b.y - hw_b));

    float intersection = x_overlap * y_overlap;
    float area_a = a.w * a.h;
    float area_b = b.w * b.h;
    float union_area = area_a + area_b - intersection;

    if (union_area <= 0) return 0.0f;
    return intersection / union_area;
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
                          + " x=" + formatFloat(d.x, 1)
                          + " y=" + formatFloat(d.y, 1)
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
