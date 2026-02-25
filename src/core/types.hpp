#pragma once
#include <cstdint>

//ID is 32-bit for performance/memory efficiency.

#include "../../third_party/roaring_bitmap/roaring.hh"
#include "../utils/settings.hpp"

namespace ndd {

    struct FilterParams {
        size_t prefilter_threshold = settings::PREFILTER_CARDINALITY_THRESHOLD;
        size_t boost_percentage = settings::FILTER_BOOST_PERCENTAGE;
    };

    using idInt = uint32_t;   // External ID (stored in DB, exposed to user)
    using idhInt = uint32_t;  // Internal HNSW ID (used inside HNSW structures)
    using RoaringBitmap = roaring::Roaring;

}  //namespace ndd
