#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>
#include <vector>
#include <stop_token>

#include "json/nlohmann_json.hpp"

// Forward declarations — full definitions live in ndd.hpp, included by rebuild.cpp.
struct CacheEntry;
class IndexManager;

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

// Parameters passed to Rebuild::executeJob. `entry` and `manager` give executeJob
// direct access to graph config, vector storage, mutexes, save/metadata operations.
struct RebuildJobParams {
    std::string username;
    size_t new_M;
    size_t new_ef_con;

    std::shared_ptr<CacheEntry> entry;  // shared_ptr keeps CacheEntry alive for the rebuild duration
    IndexManager* manager;              // saveIndexInternal, metadata_manager_ (via friend)

    std::string temp_path;
    std::string timestamped_path;
    std::string index_path;
    size_t      num_parallel_inserts;
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

    static std::string getTempPath(const std::string& index_dir);
    static std::string getTimestampedPath(const std::string& index_dir);

    void executeJob(const RebuildJobParams& p, std::stop_token st);
};
