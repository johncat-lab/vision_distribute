#pragma once
#include <string>
#include <functional>

template<typename Request, typename Response>
class IService {
public:
    virtual ~IService() = default;
    using Handler = std::function<Response(const Request&)>;
    virtual bool serve(const std::string& endpoint, Handler handler) = 0;
    virtual Response call(const std::string& endpoint, const Request& req) = 0;
};