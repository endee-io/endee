#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <iostream>

namespace ndd {
    inline constexpr bool SEARCH_TIMING_ENABLED = false;

    struct SearchTimingCounter {
        std::atomic<uint64_t> calls{0};
        std::atomic<uint64_t> total_ns{0};
    };

    struct SearchTimingStats {
        SearchTimingCounter search_total;
        SearchTimingCounter filter_bitmap_compute;
        SearchTimingCounter filtered_hnsw_search;
        SearchTimingCounter prefilter_total;
        SearchTimingCounter prefilter_bitmap_to_ids;
        SearchTimingCounter prefilter_direct_mdbx_score;
        SearchTimingCounter prefilter_mdbx_get;
        SearchTimingCounter prefilter_distance_compute;
        std::atomic<uint64_t> prefilter_cardinality_total{0};
        std::atomic<uint64_t> prefilter_cardinality_max{0};
    };

    inline SearchTimingStats& searchTimingStats() {
        static SearchTimingStats stats;
        return stats;
    }

    inline timespec searchTimingNow() {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts;
    }

    inline uint64_t searchTimingElapsedNs(const timespec& start, const timespec& end) {
        const uint64_t start_ns =
                static_cast<uint64_t>(start.tv_sec) * 1'000'000'000ULL
                + static_cast<uint64_t>(start.tv_nsec);
        const uint64_t end_ns =
                static_cast<uint64_t>(end.tv_sec) * 1'000'000'000ULL
                + static_cast<uint64_t>(end.tv_nsec);
        return end_ns >= start_ns ? end_ns - start_ns : 0;
    }

    inline void addSearchTiming(SearchTimingCounter& counter, uint64_t elapsed_ns) {
        if constexpr(SEARCH_TIMING_ENABLED) {
            counter.calls.fetch_add(1, std::memory_order_relaxed);
            counter.total_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
        }
    }

    class ScopedSearchTiming {
    public:
        explicit ScopedSearchTiming(SearchTimingCounter& counter) :
            counter_(SEARCH_TIMING_ENABLED ? &counter : nullptr) {
            if constexpr(SEARCH_TIMING_ENABLED) {
                start_ = searchTimingNow();
            }
        }

        ~ScopedSearchTiming() {
            if constexpr(SEARCH_TIMING_ENABLED) {
                addSearchTiming(*counter_,
                                searchTimingElapsedNs(start_, searchTimingNow()));
            }
        }

    private:
        SearchTimingCounter* counter_{nullptr};
        timespec start_{};
    };

    inline void recordPrefilterCardinality(size_t cardinality) {
        if constexpr(!SEARCH_TIMING_ENABLED) {
            return;
        }
        SearchTimingStats& stats = searchTimingStats();
        stats.prefilter_cardinality_total.fetch_add(static_cast<uint64_t>(cardinality),
                                                    std::memory_order_relaxed);

        uint64_t current_max =
                stats.prefilter_cardinality_max.load(std::memory_order_relaxed);
        const uint64_t card = static_cast<uint64_t>(cardinality);
        while(card > current_max
              && !stats.prefilter_cardinality_max.compare_exchange_weak(
                      current_max, card, std::memory_order_relaxed)) {
        }
    }

    inline void printSearchTimingStats() {
        if constexpr(!SEARCH_TIMING_ENABLED) {
            return;
        }
        SearchTimingStats& stats = searchTimingStats();

        auto print_counter = [](const char* name, SearchTimingCounter& counter) -> uint64_t {
            const uint64_t calls = counter.calls.exchange(0, std::memory_order_relaxed);
            const uint64_t total_ns = counter.total_ns.exchange(0, std::memory_order_relaxed);
            const double total_ms = static_cast<double>(total_ns) / 1'000'000.0;
            const double avg_ms = calls ? total_ms / static_cast<double>(calls) : 0.0;
            std::cerr << name << " count: " << calls << '\n';
            std::cerr << name << " total(ms): "
                      << std::fixed << std::setprecision(3) << total_ms << '\n';
            std::cerr << name << " avg(ms): "
                      << std::fixed << std::setprecision(3) << avg_ms << '\n';
            return calls;
        };

        std::cerr << "Search timing stats since last healthcheck\n";
        print_counter("search_total", stats.search_total);
        print_counter("filter_bitmap_compute", stats.filter_bitmap_compute);
        print_counter("filtered_hnsw_search", stats.filtered_hnsw_search);
        const uint64_t prefilter_calls = print_counter("prefilter_total", stats.prefilter_total);
        print_counter("prefilter_bitmap_to_ids", stats.prefilter_bitmap_to_ids);
        print_counter("prefilter_direct_mdbx_score", stats.prefilter_direct_mdbx_score);
        print_counter("prefilter_mdbx_get", stats.prefilter_mdbx_get);
        print_counter("prefilter_distance_compute", stats.prefilter_distance_compute);

        const uint64_t cardinality_total =
                stats.prefilter_cardinality_total.exchange(0, std::memory_order_relaxed);
        const uint64_t cardinality_max =
                stats.prefilter_cardinality_max.exchange(0, std::memory_order_relaxed);
        std::cerr << "prefilter_cardinality total: " << cardinality_total << '\n';
        std::cerr << "prefilter_cardinality max: " << cardinality_max << '\n';
        std::cerr << "prefilter_cardinality avg: "
                  << std::fixed << std::setprecision(3)
                  << (prefilter_calls
                              ? static_cast<double>(cardinality_total)
                                        / static_cast<double>(prefilter_calls)
                              : 0.0)
                  << '\n';
        std::cerr << "=================================\n";
    }

}  // namespace ndd
