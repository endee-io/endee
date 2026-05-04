#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#ifndef SUCCESS
#    define SUCCESS 0
#endif

namespace ndd {

template <typename T = std::monostate>
struct OperationResult {
    unsigned int code = SUCCESS;
    std::string message;
    std::optional<T> value;

    bool ok() const { return code == SUCCESS; }

    T& value_or_throw() & {
        if(!ok() || !value.has_value()) {
            throw std::logic_error("OperationResult success value is not available: " + message);
        }
        return *value;
    }

    const T& value_or_throw() const& {
        if(!ok() || !value.has_value()) {
            throw std::logic_error("OperationResult success value is not available: " + message);
        }
        return *value;
    }

    T&& value_or_throw() && {
        if(!ok() || !value.has_value()) {
            throw std::logic_error("OperationResult success value is not available: " + message);
        }
        return std::move(*value);
    }
};

}  // namespace ndd
