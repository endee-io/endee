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
#include <vector>
#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include "common.hpp"

// Include all quantizer implementations to ensure they are registered
#include "float16.hpp"
#include "float32.hpp"
#include "int8.hpp"
#include "int16.hpp"
#include "binary.hpp"

namespace ndd {
    namespace quant {

        // The "One Function" to get the behavior
        inline QuantizerDispatch get_quantizer_dispatch(QuantizationLevel level) {
            auto quantizer = QuantizationRegistry::instance().getQuantizer(level);
            if(!quantizer) {
                throw std::runtime_error("Quantization level not registered: "
                                         + quantLevelToString(level));
            }
            return quantizer->getDispatch();
        }

    }  // namespace quant
}  // namespace ndd
