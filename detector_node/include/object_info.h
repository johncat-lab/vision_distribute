#ifndef OBJECT_INFO_H
#define OBJECT_INFO_H

#include <string>
#include <vector>
#include <sstream>
#include <iomanip>

// ========== 单个物体信息 ==========
// 描述一个被检测到的物体:
//   x: 横坐标
//   y: 纵坐标
//   a: 角度
//   t: 类型
class ObjectInfo {
public:
    ObjectInfo() = default;
    ObjectInfo(double x, double y, double a, int t)
        : x_(x), y_(y), a_(a), t_(t) {}

    // ===== Builder 模式 =====
    class Builder {
    public:
        Builder() = default;

        Builder& setX(double x) { x_ = x; return *this; }
        Builder& setY(double y) { y_ = y; return *this; }
        Builder& setAngle(double a) { a_ = a; return *this; }
        Builder& setType(int t) { t_ = t; return *this; }

        ObjectInfo build() const {
            return ObjectInfo(x_, y_, a_, t_);
        }

    private:
        double x_ = 0.0;
        double y_ = 0.0;
        double a_ = 0.0;
        int    t_ = 0;
    };

    // ===== 访问器 =====
    double getX() const { return x_; }
    double getY() const { return y_; }
    double getAngle() const { return a_; }
    int    getType() const { return t_; }

    void setX(double x) { x_ = x; }
    void setY(double y) { y_ = y; }
    void setAngle(double a) { a_ = a; }
    void setType(int t) { t_ = t; }

private:
    double x_ = 0.0;
    double y_ = 0.0;
    double a_ = 0.0;
    int    t_ = 0;
};

// ========== 物体列表 / 协议打包器 ==========
// 负责打包成协议字符串，格式:
//   多个物体: "TA,x1,y1,a1,t1,TA,x2,y2,a2,t2;"
//   无物体:   "NG"
class ObjectInfoList {
public:
    ObjectInfoList() = default;

    // 添加物体
    void add(const ObjectInfo& obj) {
        objects_.push_back(obj);
    }

    void add(ObjectInfo&& obj) {
        objects_.push_back(std::move(obj));
    }

    // 清空
    void clear() {
        objects_.clear();
    }

    // 数量
    size_t size() const {
        return objects_.size();
    }

    bool empty() const {
        return objects_.empty();
    }

    // 访问物体
    const std::vector<ObjectInfo>& getObjects() const {
        return objects_;
    }

    // 打包成协议字符串
    // precision: 浮点数保留的小数位数，默认 3
    std::string toProtocolString(int precision = 3) const {
        if (objects_.empty()) {
            return "NG";
        }

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(precision);

        for (size_t i = 0; i < objects_.size(); ++i) {
            const ObjectInfo& obj = objects_[i];
            oss << "TA,"
                << obj.getX() << ","
                << obj.getY() << ","
                << obj.getAngle() << ","
                << obj.getType();
            if (i + 1 < objects_.size()) {
                oss << ",";
            }
        }
        oss << ";";
        return oss.str();
    }

private:
    std::vector<ObjectInfo> objects_;
};

#endif // OBJECT_INFO_H
