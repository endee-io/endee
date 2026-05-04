#pragma once

#include <optional>
#include <string>
#include <variant>

namespace ndd {

template <typename T = std::monostate>
struct OperationResult {
    unsigned int code = 0;
    std::string message;
    std::optional<T> value;

    bool ok() const { return code == 0; }
};

}  // namespace ndd
