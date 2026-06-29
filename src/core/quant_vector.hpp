// Endee — high-performance vector database
// Copyright (C) 2026 Endee Labs
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once
#include "msgpack_ndd.hpp"        // For VectorObject (comprehensive version)
#include "../quant/dispatch.hpp"  // For QuantizerDispatch
#include "../quant/common.hpp"    // For QuantizationLevel
#include <vector>
#include <string>
#include <cstring>

// Lightweight quantized vector object for internal processing
// Does not include msgpack serialization to keep it lean and efficient
struct QuantVectorObject {
    std::string id;                     // String identifier
    std::vector<uint8_t> meta;          // Binary metadata (zipped)
    std::string filter;                 // Filter as JSON string
    float norm;                         // Vector norm (only for cosine distance)
    std::vector<uint8_t> quant_vector;  // Quantized vector data as uint8_t buffer

    // Default constructor
    QuantVectorObject() = default;

    // Efficient move constructor with quantization
    QuantVectorObject(ndd::VectorObject&& vec_obj,
                      ndd::quant::QuantizationLevel quant_level,
                      const void* params = nullptr) :
        id(std::move(vec_obj.id)),
        meta(std::move(vec_obj.meta)),
        filter(std::move(vec_obj.filter)),
        norm(vec_obj.norm),
        quant_vector(quant_vector_buffer(vec_obj.vector, quant_level, params)) {
        // vec_obj.vector will be destroyed automatically after this constructor
        // All quantization logic handled by our internal quant_vector_buffer function
    }

    // Efficient move constructor for HybridVectorObject (ignores sparse data)
    QuantVectorObject(ndd::HybridVectorObject&& vec_obj,
                      ndd::quant::QuantizationLevel quant_level,
                      const void* params = nullptr) :
        id(std::move(vec_obj.id)),
        meta(std::move(vec_obj.meta)),
        filter(std::move(vec_obj.filter)),
        norm(vec_obj.norm),
        quant_vector(quant_vector_buffer(vec_obj.vector, quant_level, params)) {
        // vec_obj.vector will be destroyed automatically after this constructor
    }

private:
    // Self-contained quantization function that uses optimized implementations
    // Convert vector<float> to quantized uint8_t buffer based on quantization level
    static std::vector<uint8_t> quant_vector_buffer(const std::vector<float>& input,
                                                    ndd::quant::QuantizationLevel quant_level,
                                                    const void* params = nullptr) {
        return ndd::quant::get_quantizer_dispatch(quant_level).quantize(input);
    }
};