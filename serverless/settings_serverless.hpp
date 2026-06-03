#pragma once
#include "../src/quant/common.hpp"
#include <vector>

namespace settings {
namespace serverless {
    // Vector limits per tier (from original pre-OSS code)
    constexpr size_t MAX_VECTORS_STARTER    = 1'000'000;        // 1M vectors
    constexpr size_t MAX_VECTORS_PRO        = 10'000'000;       // 10M vectors
    constexpr size_t MAX_VECTORS_SCALE      = 100'000'000;      // 100M vectors
    constexpr size_t MAX_VECTORS_ADMIN      = 1'000'000'000;    // 1B vectors (same as OSS)

    // Index limits per tier
    constexpr size_t STARTER_MAX_INDICES    = 3;
    constexpr size_t PRO_MAX_INDICES        = 10;
    // Scale = unlimited (-1), Admin = unlimited (-1)

    // Max vector dimension per tier
    constexpr size_t MAX_DIM_STARTER = 2'000;
    constexpr size_t MAX_DIM_PRO     = 4'000;
    constexpr size_t MAX_DIM_SCALE   = 8'000;
    constexpr size_t MAX_DIM_ADMIN   = 16'384;  // matches global settings::MAX_DIMENSION

    // top_k limits per tier
    constexpr size_t MAX_K_STARTER = 128;
    constexpr size_t MAX_K_PRO     = 1'024;
    constexpr size_t MAX_K_SCALE   = 4'096;
    constexpr size_t MAX_K_ADMIN   = 4'096;

    // Filter boost percentage cap per tier (0 = not allowed)
    constexpr size_t MAX_BOOST_STARTER = 0;
    constexpr size_t MAX_BOOST_PRO     = 100;
    constexpr size_t MAX_BOOST_SCALE   = 400;
    constexpr size_t MAX_BOOST_ADMIN   = 400;

    // Bloom filter bits per tier
    constexpr size_t BLOOM_FILTER_BITS_STARTER    = 20;  // 1M elements
    constexpr size_t BLOOM_FILTER_BITS_PRO        = 23;  // 8M elements
    constexpr size_t BLOOM_FILTER_BITS_SCALE      = 24;  // 16M elements

    // Allowed precisions per tier (edit these lists to change tier access)
    const std::vector<ndd::quant::QuantizationLevel> ALLOWED_PRECISIONS_STARTER = {
        ndd::quant::QuantizationLevel::INT8
    };
    const std::vector<ndd::quant::QuantizationLevel> ALLOWED_PRECISIONS_PRO = {
        ndd::quant::QuantizationLevel::INT8,
        ndd::quant::QuantizationLevel::INT16,
        ndd::quant::QuantizationLevel::FP16,
        ndd::quant::QuantizationLevel::FP32,
        ndd::quant::QuantizationLevel::BINARY
    };
    const std::vector<ndd::quant::QuantizationLevel> ALLOWED_PRECISIONS_SCALE = {
        ndd::quant::QuantizationLevel::INT8,
        ndd::quant::QuantizationLevel::INT16,
        ndd::quant::QuantizationLevel::FP16,
        ndd::quant::QuantizationLevel::FP32,
        ndd::quant::QuantizationLevel::BINARY
    };
    // Admin: all precisions (handled in isPrecisionAllowed — always returns true)

    // Auth MDBX database sizing
    constexpr size_t AUTH_MAP_SIZE_BITS     = 24;   // 16 MiB initial
    constexpr size_t AUTH_MAP_SIZE_MAX_BITS = 30;   // 1 GiB max

    // Token cache for hot path
    constexpr size_t MAX_TOKENS_IN_CACHE = 10'000;

    // Token generation
    constexpr size_t TOKEN_LENGTH = 32;  // bytes (64 hex chars)

    // Usage stats endpoint
    constexpr const char* DEFAULT_USAGE_STATS_URL = "https://login.endee.io/user/query-count/";
    inline std::string USAGE_STATS_URL = [] {
        const char* env = std::getenv("NDD_USAGE_STATS_URL");
        return std::string(env ? env : DEFAULT_USAGE_STATS_URL);
    }();
}
}
