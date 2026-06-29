#pragma once

#include <stdexcept>
#include <string>

namespace ndd {

class ApiError : public std::runtime_error {
private:
    int status_code_;

public:
    ApiError(int status_code, const std::string& message)
        : std::runtime_error(message), status_code_(status_code) {}

    int status_code() const {
        return status_code_;
    }
};

} // namespace ndd
