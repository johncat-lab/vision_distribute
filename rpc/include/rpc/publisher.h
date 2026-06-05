#pragma once
#include <string>
#include <vector>
#include <functional>

template<typename T>
class IPublisher {
public:
    virtual ~IPublisher() = default;
    virtual bool publish(const T& msg) = 0;
    virtual std::string getTopic() const = 0;
};