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
#include <cstdint>
#include <optional>
#include <string>

//ID is 32-bit for performance/memory efficiency.

#include "../../third_party/roaring_bitmap/roaring.hh"
#include "../utils/settings.hpp"

namespace ndd {

    enum class SparseScoringModel : uint8_t {
        NONE = 0,
        DEFAULT = 1,
        ENDEE_BM25 = 2,
    };

    inline const char* sparseScoringModelToString(SparseScoringModel model) {
        switch(model) {
            case SparseScoringModel::NONE:
                return "None";
            case SparseScoringModel::DEFAULT:
                return "default";
            case SparseScoringModel::ENDEE_BM25:
                return "endee_bm25";
        }
        return "None";
    }

    inline std::optional<SparseScoringModel> sparseScoringModelFromString(
        const std::string& value)
    {
        if(value == "None") {
            return SparseScoringModel::NONE;
        }
        if(value == "default") {
            return SparseScoringModel::DEFAULT;
        }
        if(value == "endee_bm25") {
            return SparseScoringModel::ENDEE_BM25;
        }
        return std::nullopt;
    }

    inline bool sparseModelEnabled(SparseScoringModel model) {
        return model != SparseScoringModel::NONE;
    }

    struct FilterParams {
        size_t prefilter_threshold = settings::PREFILTER_CARDINALITY_THRESHOLD;
        size_t boost_percentage = settings::FILTER_BOOST_PERCENTAGE;
    };

    using idInt = uint32_t;   // External ID (stored in DB, exposed to user)
    using idhInt = uint32_t;  // Internal HNSW ID (used inside HNSW structures)
    using RoaringBitmap = roaring::Roaring;

}  //namespace ndd
