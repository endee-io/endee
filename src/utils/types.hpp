#pragma once

#include <optional>
#include <string>
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
};

}  // namespace ndd
