#include "rebuild.hpp"

#include "ndd.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <thread>
#include <vector>

/**
 * Validate the request and launch the worker. Runs on the request thread, so it stays
 * cheap: the heavy work happens in run() on the spawned jthread. May propagate the
 * runtime_error that getIndexInfo throws for a layout/migration-blocked index - the HTTP
 * layer maps that to 409.
 */
ndd::OperationResult<RebuildInfo> Rebuild::start(const std::string& index_id,
                                                 std::optional<size_t> new_M,
                                                 std::optional<size_t> new_ef) {
    const std::string username = usernameOf(index_id);

    if(mgr_->backupActive(username)) {
        return {3, "Backup in progress for user: " + username};
    }
    if(isActive(username)) {
        return {4, "Rebuild already in progress for user: " + username};
    }

    auto info = mgr_->getIndexInfo(index_id);
    if(!info) {
        return {1, "Index not found"};
    }

    const size_t target_M = new_M.value_or(info->M);
    const size_t target_ef = new_ef.value_or(info->ef_con);
    if(target_M == info->M && target_ef == info->ef_con) {
        return {2, "Rebuild requires a change to M or ef_con"};
    }

    {
        /**
         * Authoritative check-and-reserve under backup_rebuild_exclusion_mutex_ (the same
         * mutex createBackupAsync holds), so a backup cannot start between this check and the
         * reservation below. The top-of-function backupActive/isActive checks are only a cheap
         * pre-filter that rejects a competing request without first blocking on getIndexInfo's
         * per-index lock. Lock order is exclusion -> mutex_ (never the reverse).
         */
        std::lock_guard<std::mutex> exclusion(mgr_->backup_rebuild_exclusion_mutex_);
        if(mgr_->backupActive(username)) {
            return {3, "Backup in progress for user: " + username};
        }

        std::lock_guard<std::mutex> lock(mutex_);
        /**
         * Re-check under the lock: two concurrent start() calls for the same user must
         * not both launch. A prior Completed/Failed record is simply overwritten.
         */
        auto existing = jobs_.find(username);
        if(existing != jobs_.end() && existing->second.status == RebuildStatus::InProgress) {
            return {4, "Rebuild already in progress for user: " + username};
        }

        RebuildJob& job = jobs_[username];
        job.index_id = index_id;
        job.prev_M = info->M;
        job.prev_ef = info->ef_con;
        job.target_M = target_M;
        job.target_ef = target_ef;
        job.status = RebuildStatus::InProgress;
        job.processed = 0;
        job.total = info->total_elements;
        job.started_at = std::time(nullptr);
        job.completed_at = 0;
        job.error.clear();
        /**
         * Assigning a fresh jthread move-assigns over any prior (already-finished) one,
         * which request_stop()+join()s it - instant, since a retained record's worker has
         * returned.
         */
        job.thread = std::jthread([this, index_id, target_M, target_ef](std::stop_token st) {
            const std::string worker_user = usernameOf(index_id);
            ndd::OperationResult<> result = run(index_id, target_M, target_ef, st);
            if(result.ok()) {
                markCompleted(worker_user);
            } else {
                markFailed(worker_user, result.message);
            }
        });
    }

    LOG_INFO(2060,
             index_id,
             "Rebuild started: M " << info->M << " -> " << target_M << ", ef_con "
                                   << info->ef_con << " -> " << target_ef);
    return {SUCCESS, "",
            RebuildInfo{info->M, info->ef_con, target_M, target_ef, info->total_elements}};
}

/**
 * The rebuild worker. Holds the per-index write lock for the whole build (this is what
 * blocks upserts), reconstructs the HNSW graph from stored vectors with the new
 * parameters, persists it with an atomic rename, then hot-swaps it in. Crash-safe: the
 * old default.idx and the in-memory graph are untouched until the rename, so any failure
 * before it leaves the index exactly as it was.
 */
ndd::OperationResult<> Rebuild::run(const std::string& index_id,
                                    size_t new_M,
                                    size_t new_ef,
                                    std::stop_token st) {
    const std::string username = usernameOf(index_id);
    try {
        auto entry_ptr = mgr_->getIndexEntry(index_id);
        auto& entry = *entry_ptr;

        // Held until after the in-memory swap; serializes against all writers.
        std::unique_lock<std::shared_mutex> op_lock(entry.operation_mutex);

        if(st.stop_requested() || !entry.cache_valid) {
            LOG_INFO(2062, index_id, "Rebuild cancelled before it started");
            return {1, "cancelled before it started"};
        }

        LOG_INFO(2061,
                 index_id,
                 "Rebuilding graph: M " << entry.alg->getM() << " -> " << new_M << ", ef_con "
                                        << entry.alg->getEfConstruction() << " -> " << new_ef);

        // 1. Flush the current graph and clear the op_log -> clean on-disk baseline.
        mgr_->saveIndexInternal(entry);

        // 2. Build a fresh graph with the new M/ef; every other parameter is copied as-is.
        const size_t max_elements = entry.alg->getMaxElements();
        const hnswlib::SpaceType space = hnswlib::getSpaceType(entry.alg->getSpaceTypeStr());
        const size_t dim = entry.alg->getDimension();
        const ndd::quant::QuantizationLevel quant = entry.alg->getQuantLevel();
        const int32_t checksum = entry.alg->getChecksum();

        auto new_alg = std::make_unique<hnswlib::HierarchicalNSW<float>>(
                max_elements, space, dim, new_M, new_ef, settings::RANDOM_SEED, quant, checksum);

        auto vs = entry.vector_storage;
        new_alg->setVectorFetcher([vs](MDBX_txn* txn, ndd::idInt label, uint8_t* buffer) {
            return vs->get_vector(txn, label, buffer);
        });
        new_alg->setVectorFetcherBatch([vs](MDBX_txn* txn,
                                            const ndd::idInt* labels,
                                            uint8_t* buffers,
                                            bool* success,
                                            size_t count) -> size_t {
            return vs->get_vectors_batch_into(txn, labels, buffers, success, count);
        });

        // 3. Re-insert every live vector (deletes are hard, so the cursor yields only live
        //    rows). Drain a batch on this thread, then addPoint in parallel on workers -
        //    the same pattern recoverIndex uses; this keeps the cursor's read txn off the
        //    insert threads (which open their own).
        auto cursor = vs->getCursor();
        size_t processed = 0;
        size_t skipped_empty = 0;
        const size_t batch_size = settings::RECOVERY_BATCH_SIZE;
        while(cursor.hasNext()) {
            if(st.stop_requested()) {
                LOG_INFO(2067,
                         index_id,
                         "Rebuild cancelled after " << processed << " vectors");
                return {1, "cancelled after " + std::to_string(processed) + " vectors"};
            }

            std::vector<std::pair<ndd::idInt, std::vector<uint8_t>>> batch;
            batch.reserve(batch_size);
            while(cursor.hasNext() && batch.size() < batch_size) {
                auto [label, bytes] = cursor.next();
                if(bytes.empty()) {
                    ++skipped_empty;
                    continue;
                }
                batch.emplace_back(label, std::move(bytes));
            }
            if(batch.empty()) {
                break;
            }

            const size_t num_threads = std::min(settings::NUM_PARALLEL_INSERTS, batch.size());
            std::atomic<size_t> next{0};
            std::vector<std::thread> workers;
            workers.reserve(num_threads);
            for(size_t t = 0; t < num_threads; ++t) {
                workers.emplace_back([&]() {
                    size_t i;
                    while((i = next.fetch_add(1)) < batch.size()) {
                        new_alg->addPoint<true>(batch[i].second.data(), batch[i].first);
                    }
                });
            }
            for(auto& w : workers) {
                w.join();
            }

            processed += batch.size();
            setProgress(username, processed);
        }
        if(skipped_empty > 0) {
            LOG_WARN(2063,
                     index_id,
                     "Skipped " << skipped_empty << " empty vectors during rebuild");
        }

        // 4. Persist atomically: write to a temp file, then rename - the commit point.
        const std::string index_path = mgr_->data_dir_ + "/" + index_id + "/vectors/"
                                       + settings::DEFAULT_SUBINDEX + ".idx";
        const std::string rebuild_path = index_path + ".rebuild";
        new_alg->saveIndex(rebuild_path);
        std::filesystem::rename(rebuild_path, index_path);

        // 5. Keep the denormalized metadata copy in sync (the .idx is authoritative).
        if(!mgr_->metadata_manager_->updateHnswParams(index_id, new_M, new_ef).ok()) {
            LOG_WARN(2066, index_id, "Failed to update HNSW params in metadata after rebuild");
        }
        mgr_->metadata_manager_->updateElementCount(index_id, new_alg->getElementsCount());

        /**
         * 6. Hot-swap the in-memory graph. Take alg_swap_mutex exclusively so a lock-free
         *    reader (search/getVector) never observes a torn pointer; an in-flight reader that
         *    already captured the old graph keeps it alive via its shared_ptr until it
         *    finishes (same in-place swap as reloadIndex).
         */
        {
            std::lock_guard<std::shared_mutex> swap_lock(entry.alg_swap_mutex);
            entry.alg = std::move(new_alg);
        }
        entry.is_dirty = false;
        op_lock.unlock();

        LOG_INFO(2064,
                 index_id,
                 "Rebuild completed: " << processed << " vectors, M=" << new_M
                                       << ", ef_con=" << new_ef);
        return {SUCCESS, ""};
    } catch(const std::exception& e) {
        /**
         * The old default.idx and entry.alg are untouched before the rename in step 4, so
         * the index remains intact. Clean up any partial temp file.
         */
        std::error_code ec;
        const std::string rebuild_path = mgr_->data_dir_ + "/" + index_id + "/vectors/"
                                         + settings::DEFAULT_SUBINDEX + ".idx.rebuild";
        std::filesystem::remove(rebuild_path, ec);
        LOG_ERROR(2065, index_id, "Rebuild failed: " << e.what());
        return {100, std::string("Rebuild failed: ") + e.what()};
    }
}
