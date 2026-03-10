#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// Debug logging macro
#ifdef ND_DEBUG
#    define LOG_DEBUG(msg)                                                                         \
        do {                                                                                       \
            std::stringstream ss;                                                                  \
            ss << msg;                                                                             \
            std::cerr << "[DEBUG] " << ss.str() << std::endl;                                      \
        } while(0)
#else
#    define LOG_DEBUG(msg)                                                                         \
        do {                                                                                       \
        } while(0)
#endif

// Forward declare the timing macros
#ifdef ND_DEBUG
#    define LOG_TIME(name) FunctionTimer timer##__LINE__(name)
#    define PRINT_LOG_TIME() FunctionTimer::printAndReset()
#else
#    define LOG_TIME(name)                                                                         \
        if constexpr(false) {                                                                      \
        }
#    define PRINT_LOG_TIME()                                                                       \
        if constexpr(false) {                                                                      \
        }
#endif

#ifdef ND_DEBUG
// Only define the class in debug builds
class FunctionTimer {
private:
    struct TimingStats {
        uint64_t total_time{0};  // Total time in microseconds
        uint64_t count{0};       // Number of calls
    };

    static std::unordered_map<std::string, TimingStats> stats;
    static std::mutex mutex;

public:
    const std::string name;
    std::chrono::high_resolution_clock::time_point start;

    FunctionTimer(const std::string& func_name) :
        name(func_name) {
        start = std::chrono::high_resolution_clock::now();
    }

    ~FunctionTimer() {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

        std::lock_guard<std::mutex> lock(mutex);
        auto& stat = stats[name];
        stat.total_time += duration;
        stat.count++;
    }

    static void printAndReset() {
        std::lock_guard<std::mutex> lock(mutex);

        std::vector<std::pair<std::string, TimingStats>> sorted_stats;
        sorted_stats.reserve(stats.size());
        for(const auto& pair : stats) {
            sorted_stats.push_back(pair);
        }

        std::sort(sorted_stats.begin(), sorted_stats.end(), [](const auto& a, const auto& b) {
            return a.second.total_time > b.second.total_time;
        });

        std::cerr << "\n=== Function Timings ===\n";
        std::cerr << std::fixed << std::setprecision(3);
        std::cerr << std::setw(30) << std::left << "Function" << std::setw(15) << "Count"
                  << std::setw(15) << "Total(ms)" << std::setw(15) << "Avg(ms)\n";
        std::cerr << std::string(75, '-') << "\n";

        for(const auto& [name, stat] : sorted_stats) {
            double total_ms = stat.total_time / 1000.0;
            double avg_ms = stat.count > 0 ? total_ms / stat.count : 0;

            std::cerr << std::setw(30) << std::left << name << std::setw(15) << stat.count
                      << std::setw(15) << total_ms << std::setw(15) << avg_ms << "\n";
        }
        std::cerr << "=====================\n";
        stats.clear();
    }
};

// Define static members only in debug builds
inline std::unordered_map<std::string, FunctionTimer::TimingStats> FunctionTimer::stats;
inline std::mutex FunctionTimer::mutex;
#endif

// Production logs share one formatter so every call site emits stable operational output.
namespace ndd::log {
constexpr int kNoCode = -1;

struct Context {
    std::string username{"-"};
    std::string index_name{"-"};
};

// Logs always render username/index_name, using "-" placeholders when scope is missing.
inline std::string normalizeContextPart(std::string value) {
    if(value.empty()) {
        return "-";
    }
    return value;
}

inline Context makeContext(const std::string& username, const std::string& index_name) {
    return {normalizeContextPart(username), normalizeContextPart(index_name)};
}

inline Context makeUserContext(const std::string& username) { return makeContext(username, "-"); }

inline Context makeGlobalContext() { return makeContext("-", "-"); }

inline Context contextFromIndexId(const std::string& index_id) {
    const size_t slash_pos = index_id.find('/');
    if(slash_pos == std::string::npos) {
        return makeGlobalContext();
    }

    return makeContext(index_id.substr(0, slash_pos), index_id.substr(slash_pos + 1));
}

inline Context contextFromString(const std::string& context) {
    if(context.empty() || context == "-" || context == "-/-") {
        return makeGlobalContext();
    }
    if(context.find('/') != std::string::npos) {
        return contextFromIndexId(context);
    }
    return makeUserContext(context);
}

inline std::string formatContext(const Context& context) {
    return normalizeContextPart(context.username) + "/"
           + normalizeContextPart(context.index_name);
}

// Prefixes are either LEVEL_code for explicit codes or LEVEL for intentional code-less logs.
inline void emit(const char* level, int code, const Context& context, const std::string& message) {
    std::cerr << level;
    if(code != kNoCode) {
        std::cerr << "_" << code;
    }
    std::cerr << ": " << formatContext(context) << ": " << message << std::endl;
}
}  // namespace ndd::log

#define NDD_LOG_EMIT(level, code, context, msg)                                                    \
    do {                                                                                           \
        std::stringstream __log_ss__;                                                              \
        __log_ss__ << msg;                                                                         \
        ndd::log::emit(level, code, context, __log_ss__.str());                                    \
    } while(0)

// Arity dispatch keeps the public macros simple while selecting global, user, index, or explicit context.
#define NDD_LOG_1(level, msg)                                                                      \
    NDD_LOG_EMIT(level, ndd::log::kNoCode, ndd::log::makeGlobalContext(), msg)

#define NDD_LOG_2(level, code, msg)                                                                \
    NDD_LOG_EMIT(level, code, ndd::log::makeGlobalContext(), msg)

#define NDD_LOG_3(level, code, context, msg)                                                       \
    NDD_LOG_EMIT(level, code, ndd::log::contextFromString(context), msg)

#define NDD_LOG_4(level, code, username, index_name, msg)                                          \
    NDD_LOG_EMIT(level, code, ndd::log::makeContext(username, index_name), msg)

#define NDD_LOG_PICK(_1, _2, _3, _4, NAME, ...) NAME

#define LOG_INFO(...)                                                                              \
    NDD_LOG_PICK(__VA_ARGS__, NDD_LOG_4, NDD_LOG_3, NDD_LOG_2, NDD_LOG_1)("INFO", __VA_ARGS__)
#define LOG_WARN(...)                                                                              \
    NDD_LOG_PICK(__VA_ARGS__, NDD_LOG_4, NDD_LOG_3, NDD_LOG_2, NDD_LOG_1)("WARN", __VA_ARGS__)
#define LOG_ERROR(...)                                                                             \
    NDD_LOG_PICK(__VA_ARGS__, NDD_LOG_4, NDD_LOG_3, NDD_LOG_2, NDD_LOG_1)("ERROR", __VA_ARGS__)
