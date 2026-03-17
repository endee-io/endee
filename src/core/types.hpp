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
