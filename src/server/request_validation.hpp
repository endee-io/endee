#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

#include "crow/json.h"
#include "utils/types.hpp"

namespace ndd::server {

inline std::string bounded_size_error(const std::string& field_name, size_t min_value, size_t max_value) {
    return field_name + " must be between " + std::to_string(min_value) + " and "
            + std::to_string(max_value);
}

inline ndd::OperationResult<size_t>
parse_bounded_size(const crow::json::rvalue& value,
                   const std::string& field_name,
                   size_t min_value,
                   size_t max_value) {
    try {
        if(value.t() != crow::json::type::Number) {
            return {1, field_name + " must be an integer"};
        }

        const auto number_type = value.nt();
        if(number_type == crow::json::num_type::Floating_point
           || number_type == crow::json::num_type::Double_precision_floating_point) {
            return {1, field_name + " must be an integer"};
        }

        size_t parsed_value = 0;
        if(number_type == crow::json::num_type::Unsigned_integer) {
            const uint64_t unsigned_value = value.u();
            if(unsigned_value > static_cast<uint64_t>(max_value)) {
                return {1, bounded_size_error(field_name, min_value, max_value)};
            }
            parsed_value = static_cast<size_t>(unsigned_value);
        } else {
            const int64_t signed_value = value.i();
            if(signed_value < 0) {
                return {1, bounded_size_error(field_name, min_value, max_value)};
            }
            parsed_value = static_cast<size_t>(signed_value);
        }

        if(parsed_value < min_value || parsed_value > max_value) {
            return {1, bounded_size_error(field_name, min_value, max_value)};
        }

        return {SUCCESS, "", parsed_value};
    } catch(const std::exception&) {
        return {1, field_name + " must be an integer"};
    }
}

}  // namespace ndd::server
