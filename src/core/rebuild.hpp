#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <sstream>
#include <stop_token>
#include <vector>

#include "settings.hpp"
#include "log.hpp"
#include "json/nlohmann_json.hpp"
#include "hnsw/hnswlib.h"
#include "vector_storage.hpp"
#include "../quant/common.hpp"
#include "utils/types.hpp"

struct ActiveRebuild {
    std::string index_id;
    std::string status{"in_progress"};  // "in_progress", "completed", "failed"
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
    // Keyed by username — one rebuild per user at a time
    std::unordered_map<std::string, std::shared_ptr<ActiveRebuild>> active_rebuilds_;
    mutable std::mutex rebuild_state_mutex_;

    static std::string timeToISO8601(std::chrono::system_clock::time_point tp) {
        auto time_t_val = std::chrono::system_clock::to_time_t(tp);
        std::tm tm_val{};
        gmtime_r(&time_t_val, &tm_val);
        std::ostringstream oss;
        oss << std::put_time(&tm_val, "%Y-%m-%dT%H:%M:%SZ");
        return oss.str();
    }

public:
    Rebuild() = default;

    // Lifecycle — cleanup temp files from interrupted rebuilds on startup
    void cleanupTempFiles(const std::string& data_dir) {
        if (!std::filesystem::exists(data_dir)) {
            return;
        }
        try {
            std::string temp_filename = std::string(settings::DEFAULT_SUBINDEX) + ".idx.temp";
            std::string ts_prefix     = std::string(settings::DEFAULT_SUBINDEX) + ".idx.";
            for (const auto& entry : std::filesystem::recursive_directory_iterator(data_dir)) {
                if (!entry.is_regular_file()) continue;
                const std::string fname = entry.path().filename().string();
                bool is_temp = (fname == temp_filename);
                bool is_ts   = fname.size() > ts_prefix.size()
                               && fname.substr(0, ts_prefix.size()) == ts_prefix
                               && std::all_of(fname.begin() + ts_prefix.size(), fname.end(), ::isdigit);
                if (is_temp || is_ts) {
                    std::filesystem::remove(entry.path());
                }
            }
        } catch (const std::exception& e) {
            LOG_WARN(2053, "rebuild", "Failed to cleanup temp files on startup: " << e.what());
        }
    }

    // State tracking — per user

    void setActiveRebuild(const std::string& username, const std::string& index_id,
                          size_t total_vectors, std::jthread&& thread) {
        std::lock_guard<std::mutex> lock(rebuild_state_mutex_);
        auto state = std::make_shared<ActiveRebuild>();
        state->index_id = index_id;
        state->status = "in_progress";
        state->total_vectors = total_vectors;
        state->vectors_processed = 0;
        state->started_at = std::chrono::system_clock::now();
        state->thread = std::move(thread);
        active_rebuilds_[username] = state;
    }

    void completeActiveRebuild(const std::string& username) {
        std::lock_guard<std::mutex> lock(rebuild_state_mutex_);
        auto it = active_rebuilds_.find(username);
        if (it != active_rebuilds_.end()) {
            // Called from within the thread — detach so the jthread dtor doesn't join us
            if (it->second->thread.joinable()) {
                it->second->thread.detach();
            }
            it->second->status = "completed";
            it->second->completed_at = std::chrono::system_clock::now();
        }
    }

    void failActiveRebuild(const std::string& username, const std::string& error) {
        std::lock_guard<std::mutex> lock(rebuild_state_mutex_);
        auto it = active_rebuilds_.find(username);
        if (it != active_rebuilds_.end()) {
            // Called from within the thread — detach so the jthread dtor doesn't join us
            if (it->second->thread.joinable()) {
                it->second->thread.detach();
            }
            it->second->status = "failed";
            it->second->error_message = error;
            it->second->completed_at = std::chrono::system_clock::now();
        }
    }

    bool hasActiveRebuild(const std::string& username) const {
        std::lock_guard<std::mutex> lock(rebuild_state_mutex_);
        auto it = active_rebuilds_.find(username);
        // Only "in_progress" blocks a new rebuild
        return it != active_rebuilds_.end() && it->second->status == "in_progress";
    }

    // Join all in-progress rebuild threads on shutdown. Mirrors BackupStore::joinAllThreads:
    // move threads out under lock, request_stop + join outside lock to avoid deadlock
    // (finishing threads call completeActiveRebuild which also locks rebuild_state_mutex_).
    void joinAllThreads() {
        std::vector<std::jthread> threads_to_join;
        {
            std::lock_guard<std::mutex> lock(rebuild_state_mutex_);
            for (auto& [username, state] : active_rebuilds_) {
                if (state->thread.joinable()) {
                    threads_to_join.push_back(std::move(state->thread));
                }
            }
            active_rebuilds_.clear();
        }
        for (auto& t : threads_to_join) {
            t.request_stop();
            if (t.joinable()) {
                t.join();
            }
        }
    }

    void attachRebuildThread(const std::string& username, std::jthread&& thread) {
        std::lock_guard<std::mutex> lock(rebuild_state_mutex_);
        auto it = active_rebuilds_.find(username);
        if (it != active_rebuilds_.end()) {
            it->second->thread = std::move(thread);
        }
    }

    void updateProgress(const std::string& username, size_t processed) {
        std::lock_guard<std::mutex> lock(rebuild_state_mutex_);
        auto it = active_rebuilds_.find(username);
        if (it != active_rebuilds_.end()) {
            it->second->vectors_processed = processed;
        }
    }

    nlohmann::json getProgress(const std::string& username, const std::string& index_id) const {
        std::lock_guard<std::mutex> lock(rebuild_state_mutex_);
        auto it = active_rebuilds_.find(username);
        if (it != active_rebuilds_.end() && it->second->index_id == index_id) {
            const auto& state = *it->second;
            size_t processed = state.vectors_processed;
            size_t total = state.total_vectors;
            double percent = total > 0 ? (100.0 * processed / total) : 0.0;
            nlohmann::json result = {
                {"status", state.status},
                {"vectors_processed", processed},
                {"total_vectors", total},
                {"percent_complete", percent},
                {"started_at", formatTime(state.started_at)}
            };
            if (state.status == "completed" || state.status == "failed") {
                result["completed_at"] = formatTime(state.completed_at);
            }
            if (state.status == "failed" && !state.error_message.empty()) {
                result["error"] = state.error_message;
            }
            return result;
        }
        return {{"status", "idle"}};
    }

    // Format state as JSON fields
    static std::string formatTime(std::chrono::system_clock::time_point tp) {
        return timeToISO8601(tp);
    }

    // Path helpers

    static std::string getTempPath(const std::string& index_dir) {
        return index_dir + "/vectors/" + settings::DEFAULT_SUBINDEX + ".idx.temp";
    }

    static std::string getTimestampedPath(const std::string& index_dir) {
        auto ts = std::to_string(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count()
        );
        return index_dir + "/vectors/" + settings::DEFAULT_SUBINDEX + ".idx." + ts;
    }

    // Owns all rebuild execution. Called directly from the jthread lambda spawned in
    // rebuildIndexAsync. IndexManager-specific operations come in via p callbacks.
    void executeJob(const RebuildJobParams& p, std::stop_token st) {
        try {
            std::unique_lock<std::shared_mutex> op_lock(*p.operation_mutex);

            // Phase 1 — save current state before rebuilding
            p.save_current_index();

            // Phase 2 — build new HNSW with updated M/ef_con
            auto new_alg = std::make_unique<hnswlib::HierarchicalNSW<float>>(
                p.max_elements, p.space_type, p.dim, p.new_M, p.new_ef_con,
                settings::RANDOM_SEED, p.quant_level, p.checksum);

            // MUST wire fetchers before addPoint — searchBaseLayer needs this for base-layer-only nodes
            p.wire_fetchers(new_alg.get(), p.vector_storage);

            auto cursor = p.vector_storage->getCursor();
            const size_t batch_size = settings::RECOVERY_BATCH_SIZE;
            size_t total_processed = 0;
            size_t batches_since_checkpoint = 0;
            constexpr size_t CHECKPOINT_INTERVAL = 5;

            while (cursor.hasNext()) {
                if (st.stop_requested()) {
                    if (std::filesystem::exists(p.temp_path))
                        std::filesystem::remove(p.temp_path);
                    failActiveRebuild(p.username, "Rebuild interrupted by server shutdown");
                    return;
                }

                std::vector<std::pair<ndd::idInt, std::vector<uint8_t>>> batch;
                batch.reserve(batch_size);
                while (cursor.hasNext() && batch.size() < batch_size) {
                    auto [label, vec_bytes] = cursor.next();
                    if (!vec_bytes.empty())
                        batch.emplace_back(label, std::move(vec_bytes));
                }
                if (batch.empty()) break;

                p.parallel_add(batch.size(), p.num_parallel_inserts,
                    [&](size_t i) {
                        const auto& [label, vec_bytes] = batch[i];
                        new_alg->addPoint<true>(vec_bytes.data(), label);
                    });

                total_processed += batch.size();
                updateProgress(p.username, total_processed);

                if (++batches_since_checkpoint >= CHECKPOINT_INTERVAL) {
                    new_alg->saveIndex(p.temp_path);
                    batches_since_checkpoint = 0;
                }
            }

            // Phase 3 — save final, copy to canonical path, load fresh from disk
            new_alg->saveIndex(p.timestamped_path);
            std::filesystem::copy_file(p.timestamped_path, p.index_path,
                std::filesystem::copy_options::overwrite_existing);

            // Cannot call reloadIndex() here — we hold operation_mutex and reloadIndex acquires
            // indices_mutex_, while deleteIndex holds indices_mutex_ then acquires operation_mutex.
            // Calling reloadIndex here would deadlock with a concurrent delete on the same index.
            auto fresh_alg = std::make_unique<hnswlib::HierarchicalNSW<float>>(p.index_path, 0);
            p.wire_fetchers(fresh_alg.get(), p.vector_storage);

            // Both files are deleted here on success. If the server crashes before reaching this
            // point, the timestamped file (default.idx.<ts>) will be removed on next startup
            // by cleanupTempFiles — it does not affect index correctness.
            if (std::filesystem::exists(p.temp_path)) std::filesystem::remove(p.temp_path);
            if (std::filesystem::exists(p.timestamped_path)) std::filesystem::remove(p.timestamped_path);

            p.swap_alg(std::move(fresh_alg));
            p.update_metadata(p.new_M, p.new_ef_con);
            p.clear_dirty();

            LOG_INFO(2051, p.index_id, "Rebuild completed: " << total_processed << " vectors rebuilt");
            completeActiveRebuild(p.username);

        } catch (const std::exception& e) {
            LOG_ERROR(2052, p.index_id, "Rebuild failed: " << e.what());
            if (std::filesystem::exists(p.temp_path)) std::filesystem::remove(p.temp_path);
            failActiveRebuild(p.username, e.what());
        }
    }
};
