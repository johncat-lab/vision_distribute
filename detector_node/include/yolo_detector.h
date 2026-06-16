#ifndef YOLO_DETECTOR_H
#define YOLO_DETECTOR_H

#include <string>
#include <vector>
#include <atomic>

#include "object_info.h"
#include "frame_queue.h"
#include "detector.h"

// YOLOv11-OBB 检测器
// 支持 ONNX Runtime 和 NCNN 两种推理后端
class YoloDetector : public Detector {
public:
    enum class Backend { ONNX, NCNN };

    YoloDetector(const std::string& model_path,
                 float conf_threshold = 0.5f,
                 float nms_threshold = 0.3f,
                 int input_width = 640,
                 int input_height = 640,
                 Backend backend = Backend::ONNX);
    ~YoloDetector();

    // 禁止拷贝
    YoloDetector(const YoloDetector&) = delete;
    YoloDetector& operator=(const YoloDetector&) = delete;

    // 初始化模型 (加载 ONNX 文件，创建 session)
    bool init() override;

    // 对一帧图像执行完整推理流水线
    // 预处理 → ONNX推理 → 后处理 → 返回检测结果
    ObjectInfoList detect(const Frame& frame) override;

    // 模型是否已加载就绪
    bool isReady() const override;

    bool saveAnnotated(const Frame& frame, const std::string& path) override;

    void drawAnnotations(cv::Mat& image) override;

private:
    // ===== 预处理 =====
    // 将原始帧数据转换为模型输入 tensor
    // Mono8 → RGB → letterbox resize → normalize → CHW
    std::vector<float> preprocess(const Frame& frame,
                                  float& scale, int& pad_x, int& pad_y);

    // ===== 后处理 =====
    // 解析模型输出，应用NMS，返回检测结果
    ObjectInfoList postprocess(const float* output_data,
                               const std::vector<int64_t>& output_shape,
                               float scale, int pad_x, int pad_y,
                               int orig_width, int orig_height);

    // 旋转矩形 NMS
    struct Detection {
        float x, y, w, h, angle;  // OBB 参数
        float confidence;
        int class_id;
    };
    std::vector<Detection> rotatedNMS(std::vector<Detection>& detections);
    float rotatedIoU(const Detection& a, const Detection& b);

    // 最近一次 NMS 后检测结果 (已反算回原图坐标系)
    std::vector<Detection> last_detections_;

    // ===== 图像处理辅助 =====
    // 双线性插值 resize
    void bilinearResize(const unsigned char* src, int src_w, int src_h, int src_channels,
                        unsigned char* dst, int dst_w, int dst_h);

    // ===== NCNN 后端 =====
    bool initNcnn();
    ObjectInfoList detectWithNcnn(const std::vector<float>& input_tensor,
                                  float scale, int pad_x, int pad_y,
                                  int orig_width, int orig_height);

    std::string model_path_;
    float conf_threshold_;
    float nms_threshold_;
    int input_width_;
    int input_height_;

    // ONNX Runtime 内部状态 (使用 pImpl 避免头文件暴露 ORT)
    struct OrtSession;
    OrtSession* session_ = nullptr;

    // NCNN 内部状态 (使用 pImpl 避免头文件暴露 NCNN)
    struct NcnnSession;
    NcnnSession* ncnn_session_ = nullptr;

    Backend backend_;
    std::atomic<bool> ready_{false};
};

#endif // YOLO_DETECTOR_H
