#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <chrono>
#include <vector>
#include <stop_token>
#include <functional>

#include "json/nlohmann_json.hpp"
#include "hnsw/hnswlib.h"
#include "vector_storage.hpp"
#include "../quant/common.hpp"

enum class RebuildStatus : unsigned char {
    IN_PROGRESS = 0,
    COMPLETED   = 1,
    FAILED      = 2
};

struct ActiveRebuild {
    std::string index_id;
    RebuildStatus status{RebuildStatus::IN_PROGRESS};
    std::string error_message;
    size_t vectors_processed{0};
    size_t total_vectors{0};
    std::chrono::system_clock::time_point started_at;
    std::chrono::system_clock::time_point completed_at;
    std::jthread thread;  // jthread: built-in stop_token + auto-join on destruction
};

// Parameters passed to Rebuild::executeJob. IndexManager-specific operations are
// provided as callbacks so rebuild.hpp does not need to include ndd.hpp.
struct RebuildJobParams {
    // Identity
    std::string index_id;
    std::string username;
    size_t new_M;
    size_t new_ef_con;

    // Current graph config (read from entry->alg by IndexManager before thread spawn)
    hnswlib::SpaceType space_type;
    size_t dim;
    ndd::quant::QuantizationLevel quant_level;
    int32_t checksum;
    size_t max_elements;

    // Storage for vector iteration
    std::shared_ptr<VectorStorage> vector_storage;

    // File paths
    std::string temp_path;
    std::string timestamped_path;
    std::string index_path;

    // Threading
    size_t num_parallel_inserts;

    // Mutex pointer — executeJob acquires this for the whole job duration
    std::shared_mutex* operation_mutex;

    // Callbacks for IndexManager-specific actions (avoids circular ndd.hpp include)
    std::function<void()> save_current_index;
    std::function<void(std::unique_ptr<hnswlib::HierarchicalNSW<float>>)> swap_alg;
    std::function<void(size_t new_M, size_t new_ef_con)> update_metadata;
    std::function<void()> clear_dirty;
    std::function<void(hnswlib::HierarchicalNSW<float>*, std::shared_ptr<VectorStorage>)> wire_fetchers;
    std::function<void(size_t, size_t, std::function<void(size_t)>)> parallel_add;
};

class Rebuild {
private:
    std::unordered_map<std::string, std::shared_ptr<ActiveRebuild>> active_rebuilds_;
    mutable std::mutex rebuild_state_mutex_;

    static std::string statusToString(RebuildStatus s);
    static std::string timeToISO8601(std::chrono::system_clock::time_point tp);

public:
    Rebuild() = default;

    void cleanupTempFiles(const std::string& data_dir);

    void setActiveRebuild(const std::string& username, const std::string& index_id,
                          size_t total_vectors);
    void completeActiveRebuild(const std::string& username);
    void failActiveRebuild(const std::string& username, const std::string& error);
    bool hasActiveRebuild(const std::string& username) const;
    void joinAllThreads();
    void attachRebuildThread(const std::string& username, std::jthread&& thread);
    void updateProgress(const std::string& username, size_t processed);
    nlohmann::json getProgress(const std::string& username, const std::string& index_id) const;

    static std::string formatTime(std::chrono::system_clock::time_point tp);
    static std::string getTempPath(const std::string& index_dir);
    static std::string getTimestampedPath(const std::string& index_dir);

    void executeJob(const RebuildJobParams& p, std::stop_token st);
};
