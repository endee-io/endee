#pragma once

#include <ctime>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "json/nlohmann_json.hpp"

#include "../utils/types.hpp"

/**
 * Forward declaration only. The Rebuild worker needs IndexManager internals, so
 * Rebuild is declared a `friend` of IndexManager and the two methods that touch it
 * (start, run) are defined in rebuild.cpp - which includes ndd.hpp. This header is
 * deliberately free of that dependency so tests can use the inline state machine
 * without pulling in the server stack.
 */
class IndexManager;

enum class RebuildStatus { Idle, InProgress, Completed, Failed };

inline const char* rebuildStatusToString(RebuildStatus status) {
    switch(status) {
        case RebuildStatus::Idle:
            return "idle";
        case RebuildStatus::InProgress:
            return "in_progress";
        case RebuildStatus::Completed:
            return "completed";
        case RebuildStatus::Failed:
            return "failed";
    }
    return "idle";
}

/**
 * One rebuild record per user. Retained after completion/failure so the status
 * endpoint can report the final outcome; overwritten when the next rebuild starts.
 *
 * `processed` is a plain size_t updated by the worker thread without taking a lock for
 * the value itself - progress reporting has a loose SLO, so a momentarily stale read is
 * acceptable and we avoid the cost of atomics on the per-batch update.
 */
struct RebuildJob {
    std::string index_id;
    size_t prev_M = 0;
    size_t prev_ef = 0;
    size_t target_M = 0;
    size_t target_ef = 0;
    RebuildStatus status = RebuildStatus::Idle;
    size_t processed = 0;
    size_t total = 0;
    std::time_t started_at = 0;
    std::time_t completed_at = 0;
    std::string error;
    std::jthread thread;  // jthread: built-in stop_token + auto-join on destruction
};

/**
 * Returned by Rebuild::start on success so the HTTP layer can build the 202 body without
 * a second getIndexInfo call - which matters because getIndexInfo takes the per-index
 * shared lock that an in-progress rebuild holds exclusively, so calling it from the route
 * before the exclusion check would block a competing request instead of rejecting it.
 */
struct RebuildInfo {
    size_t prev_M = 0;
    size_t prev_ef = 0;
    size_t new_M = 0;
    size_t new_ef = 0;
    size_t total_vectors = 0;
};

/**
 * Rebuild reconstructs an index's HNSW graph in place with new M / ef_construction,
 * reading the vectors already in storage - the user does not re-upload anything. Only
 * the graph layout depends on these parameters, so the stored vector bytes, id mapping,
 * filters and sparse data are all reused unchanged.
 *
 * It is asynchronous (one jthread per job), allows at most one rebuild per user, and is
 * mutually exclusive with backup. Owned by IndexManager as a member; IndexManager
 * forwards createRebuildAsync()/getRebuildStatus() here.
 */
class Rebuild {
public:
    Rebuild(IndexManager* mgr, std::string data_dir) :
        mgr_(mgr),
        data_dir_(std::move(data_dir)) {
        cleanupOrphanTempFiles();
    }

    Rebuild(const Rebuild&) = delete;
    Rebuild& operator=(const Rebuild&) = delete;

    /**
     * Validate and launch a rebuild for `index_id`. Omitted M/ef keep their current
     * value. Runs on the request thread and stays cheap; the rebuild runs on a spawned
     * jthread. The exclusion checks run before any lock-taking call, so a competing
     * request is rejected immediately rather than blocking. Defined in rebuild.cpp.
     *
     * Return codes:
     * 0     = accepted; value holds the previous/new config + vector count for the 202 body
     * 1     = index not found -> HTTP 404
     * 2-99  = caller-fixable rejection (no change requested; or a backup/rebuild is already
     *         running for the user) -> HTTP 400
     * 100+  = reserved for internal failures -> HTTP 500
     *
     * May propagate the std::runtime_error that getIndexInfo throws for a
     * layout/migration-blocked index; the HTTP layer maps that to 409.
     */
    ndd::OperationResult<RebuildInfo> start(const std::string& index_id,
                                            std::optional<size_t> new_M,
                                            std::optional<size_t> new_ef);

    /** Per-user rebuild status for GET /api/v1/index/<name>/rebuild/status. */
    nlohmann::json status(const std::string& username) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = jobs_.find(username);
        if(it == jobs_.end()) {
            return nlohmann::json{{"status", "idle"}};
        }
        const RebuildJob& job = it->second;
        nlohmann::json out;
        out["status"] = rebuildStatusToString(job.status);
        out["index_id"] = job.index_id;
        out["vectors_processed"] = job.processed;
        out["total_vectors"] = job.total;
        out["percent_complete"] =
                job.total ? (100.0 * static_cast<double>(job.processed)
                             / static_cast<double>(job.total))
                          : 0.0;
        out["previous_config"] = {{"M", job.prev_M}, {"ef_con", job.prev_ef}};
        out["new_config"] = {{"M", job.target_M}, {"ef_con", job.target_ef}};
        out["started_at"] = static_cast<int64_t>(job.started_at);
        if(job.status == RebuildStatus::Completed || job.status == RebuildStatus::Failed) {
            out["completed_at"] = static_cast<int64_t>(job.completed_at);
        }
        if(job.status == RebuildStatus::Failed) {
            out["error"] = job.error;
        }
        return out;
    }

    /** True iff a rebuild is currently running for `username`. */
    bool isActive(const std::string& username) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = jobs_.find(username);
        return it != jobs_.end() && it->second.status == RebuildStatus::InProgress;
    }

    /**
     * Request stop + join all worker threads before IndexManager members are destroyed.
     * Mirrors BackupStore::joinAllThreads: move the threads out under the lock, then
     * request_stop()/join() outside it so a worker finishing via markCompleted() (which
     * also locks mutex_) cannot deadlock against the join.
     */
    void joinAll() {
        std::vector<std::jthread> threads;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for(auto& [username, job] : jobs_) {
                if(job.thread.joinable()) {
                    threads.push_back(std::move(job.thread));
                }
            }
        }
        for(auto& t : threads) {
            t.request_stop();
            if(t.joinable()) {
                t.join();
            }
        }
    }

private:
    /**
     * The rebuild worker (runs on the jthread). Defined in rebuild.cpp.
     *
     * Return codes:
     * 0    = success; the new graph was built, swapped in, and persisted
     * 1    = cancelled via the stop_token (shutdown)
     * 100+ = build/persist failure; the old graph is left intact
     *
     * The caller (the spawning lambda) records the outcome as the job's
     * completed/failed status.
     */
    ndd::OperationResult<> run(const std::string& index_id, size_t new_M, size_t new_ef,
                               std::stop_token st);

    void markCompleted(const std::string& username) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = jobs_.find(username);
        if(it != jobs_.end()) {
            it->second.status = RebuildStatus::Completed;
            it->second.completed_at = std::time(nullptr);
        }
    }

    void markFailed(const std::string& username, const std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = jobs_.find(username);
        if(it != jobs_.end()) {
            it->second.status = RebuildStatus::Failed;
            it->second.completed_at = std::time(nullptr);
            it->second.error = error;
        }
    }

    void setProgress(const std::string& username, size_t processed) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = jobs_.find(username);
        if(it != jobs_.end()) {
            it->second.processed = processed;
        }
    }

    /**
     * Best-effort removal of `<...>/vectors/default.idx.rebuild` temp files left behind
     * by a rebuild that crashed before its atomic rename. Run once at construction.
     */
    void cleanupOrphanTempFiles() {
        std::error_code ec;
        std::filesystem::path root(data_dir_);
        if(!std::filesystem::exists(root, ec)) {
            return;
        }
        for(auto it = std::filesystem::recursive_directory_iterator(
                    root, std::filesystem::directory_options::skip_permission_denied, ec);
            !ec && it != std::filesystem::recursive_directory_iterator();
            it.increment(ec)) {
            if(it->is_regular_file(ec)
               && it->path().filename().string().ends_with(".idx.rebuild")) {
                std::error_code rm_ec;
                std::filesystem::remove(it->path(), rm_ec);
            }
        }
    }

    static std::string usernameOf(const std::string& index_id) {
        auto pos = index_id.find('/');
        return pos == std::string::npos ? index_id : index_id.substr(0, pos);
    }

    IndexManager* mgr_;
    std::string data_dir_;
    std::unordered_map<std::string, RebuildJob> jobs_;
    mutable std::mutex mutex_;
};
