#include "rebuild.hpp"

#include <filesystem>
#include <iomanip>
#include <sstream>
#include <unordered_set>

#include "settings.hpp"
#include "log.hpp"
#include "utils/types.hpp"
#include "ndd.hpp"          // CacheEntry, IndexManager (friend access)

std::string Rebuild::statusToString(RebuildStatus s) {
    switch (s) {
        case RebuildStatus::IN_PROGRESS: return "in_progress";
        case RebuildStatus::COMPLETED:   return "completed";
        case RebuildStatus::FAILED:      return "failed";
    }
    __builtin_unreachable();
}

std::string Rebuild::timeToISO8601(std::chrono::system_clock::time_point tp) {
    auto time_t_val = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_val{};
    gmtime_r(&time_t_val, &tm_val);
    std::ostringstream oss;
    oss << std::put_time(&tm_val, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

void Rebuild::cleanupTempFiles(const std::string& data_dir) {
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
    } catch (const std::filesystem::filesystem_error& e) {
        if (e.code() != std::errc::no_such_file_or_directory)
            LOG_WARN(1803, "rebuild", "Error during temp cleanup: " << e.what());
    } catch (const std::exception& e) {
        LOG_WARN(1803, "rebuild", "Error during temp cleanup: " << e.what());
    }
}

void Rebuild::setActiveRebuild(const std::string& username, const std::string& index_id,
                                size_t total_vectors) {
    std::lock_guard<std::mutex> lock(rebuild_state_mutex_);
    auto state = std::make_shared<ActiveRebuild>();
    state->index_id = index_id;
    state->status = RebuildStatus::IN_PROGRESS;
    state->total_vectors = total_vectors;
    state->vectors_processed = 0;
    state->started_at = std::chrono::system_clock::now();
    active_rebuilds_[username] = state;
}

void Rebuild::completeActiveRebuild(const std::string& username) {
    std::lock_guard<std::mutex> lock(rebuild_state_mutex_);
    auto it = active_rebuilds_.find(username);
    if (it != active_rebuilds_.end()) {
        // Called from within the thread — detach so the jthread dtor doesn't join us
        if (it->second->thread.joinable()) {
            it->second->thread.detach();
        }
        it->second->status = RebuildStatus::COMPLETED;
        it->second->completed_at = std::chrono::system_clock::now();
    }
}

void Rebuild::failActiveRebuild(const std::string& username, const std::string& error) {
    std::lock_guard<std::mutex> lock(rebuild_state_mutex_);
    auto it = active_rebuilds_.find(username);
    if (it != active_rebuilds_.end()) {
        // Called from within the thread — detach so the jthread dtor doesn't join us
        if (it->second->thread.joinable()) {
            it->second->thread.detach();
        }
        it->second->status = RebuildStatus::FAILED;
        it->second->error_message = error;
        it->second->completed_at = std::chrono::system_clock::now();
    }
}

bool Rebuild::hasActiveRebuild(const std::string& username) const {
    std::lock_guard<std::mutex> lock(rebuild_state_mutex_);
    auto it = active_rebuilds_.find(username);
    // Only IN_PROGRESS blocks a new rebuild
    return it != active_rebuilds_.end() && it->second->status == RebuildStatus::IN_PROGRESS;
}

void Rebuild::joinAllThreads() {
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

void Rebuild::attachRebuildThread(const std::string& username, std::jthread&& thread) {
    std::lock_guard<std::mutex> lock(rebuild_state_mutex_);
    auto it = active_rebuilds_.find(username);
    if (it != active_rebuilds_.end()) {
        it->second->thread = std::move(thread);
    }
}

void Rebuild::updateProgress(const std::string& username, size_t processed) {
    std::lock_guard<std::mutex> lock(rebuild_state_mutex_);
    auto it = active_rebuilds_.find(username);
    if (it != active_rebuilds_.end()) {
        it->second->vectors_processed = processed;
    }
}

nlohmann::json Rebuild::getProgress(const std::string& username, const std::string& index_id) const {
    std::lock_guard<std::mutex> lock(rebuild_state_mutex_);
    auto it = active_rebuilds_.find(username);
    if (it != active_rebuilds_.end() && it->second->index_id == index_id) {
        const auto& state = *it->second;
        size_t processed = state.vectors_processed;
        size_t total = state.total_vectors;
        double percent = total > 0 ? (100.0 * processed / total) : 0.0;
        nlohmann::json result = {
            {"status", statusToString(state.status)},
            {"vectors_processed", processed},
            {"total_vectors", total},
            {"percent_complete", percent},
            {"started_at", timeToISO8601(state.started_at)}
        };
        if (state.status == RebuildStatus::COMPLETED || state.status == RebuildStatus::FAILED) {
            result["completed_at"] = timeToISO8601(state.completed_at);
        }
        if (state.status == RebuildStatus::FAILED && !state.error_message.empty()) {
            result["error"] = state.error_message;
        }
        return result;
    }
    return {{"status", "idle"}};
}

std::string Rebuild::getTempPath(const std::string& index_dir) {
    return index_dir + "/vectors/" + settings::DEFAULT_SUBINDEX + ".idx.temp";
}

std::string Rebuild::getTimestampedPath(const std::string& index_dir) {
    auto ts = std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    return index_dir + "/vectors/" + settings::DEFAULT_SUBINDEX + ".idx." + ts;
}

void Rebuild::executeJob(const RebuildJobParams& p, std::stop_token st) {
    auto& entry = p.entry;     // shared_ptr<CacheEntry>
    auto* manager = p.manager;
    try {
        std::unique_lock<std::shared_mutex> op_lock(entry->operation_mutex);

        // Phase 1 — save current state before rebuilding
        manager->saveIndexInternal(*entry);

        // Phase 2 — build new HNSW with updated M/ef_con
        auto* old_alg = entry->alg.get();
        // Size new graph based on live vector count + buffer, not allocated capacity.
        // This ensures default.idx shrinks proportionally after deletions.
        size_t live_count = old_alg->getElementsCount();
        size_t new_max    = live_count + settings::MAX_ELEMENTS_INCREMENT;
        auto new_alg = std::make_unique<hnswlib::HierarchicalNSW<float>>(
            new_max, old_alg->getSpaceType(), old_alg->getDimension(),
            p.new_M, p.new_ef_con,
            settings::RANDOM_SEED, old_alg->getQuantLevel(), old_alg->getChecksum());

        // MUST wire fetchers before addPoint — searchBaseLayer needs this for base-layer-only nodes
        IndexManager::wireVectorFetchers(new_alg.get(), entry->vector_storage);

        auto deleted_ids = entry->id_mapper->getDeletedIdsSet();
        auto cursor = entry->vector_storage->getCursor();
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
                if (!vec_bytes.empty() && deleted_ids.count(label) == 0)
                    batch.emplace_back(label, std::move(vec_bytes));
            }
            if (batch.empty()) break;

            IndexManager::parallelAddPoints(batch.size(), p.num_parallel_inserts,
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

        if (st.stop_requested()) {
            if (std::filesystem::exists(p.temp_path))
                std::filesystem::remove(p.temp_path);
            failActiveRebuild(p.username, "Rebuild interrupted by server shutdown");
            return;
        }

        // Phase 3 — persist to timestamped path, atomically rename to canonical path
        new_alg->saveIndex(p.timestamped_path);
        std::filesystem::rename(p.timestamped_path, p.index_path);

        if (std::filesystem::exists(p.temp_path)) std::filesystem::remove(p.temp_path);

        // new_alg is fully built and fetchers are already wired (line 194) — use directly
        entry->alg = std::move(new_alg);

        // Update metadata (uses friend access to manager->metadata_manager_)
        auto m = manager->metadata_manager_->getMetadata(entry->index_id);
        if (m) {
            m->M = p.new_M;
            m->ef_con = p.new_ef_con;
            m->total_elements = entry->alg->getElementsCount();
            manager->metadata_manager_->storeMetadata(entry->index_id, *m);
        }

        entry->is_dirty = false;

        LOG_INFO(1801, entry->index_id, "Rebuild completed: " << total_processed << " vectors rebuilt");
        completeActiveRebuild(p.username);

    } catch (const std::exception& e) {
        LOG_ERROR(1802, entry->index_id, "Rebuild failed: " << e.what());
        if (std::filesystem::exists(p.temp_path)) std::filesystem::remove(p.temp_path);
        failActiveRebuild(p.username, e.what());
    }
}
