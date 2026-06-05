#pragma once
#include <string>
#include <functional>

template<typename T>
class ISubscriber {
public:
    virtual ~ISubscriber() = default;
    using Callback = std::function<void(const T&)>;
    virtual bool subscribe(Callback cb) = 0;
    virtual std::string getTopic() const = 0;
};