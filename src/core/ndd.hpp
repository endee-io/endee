#pragma once
#include <curl/curl.h>
#include <regex>

#include "hnsw/hnswlib.h"
#include "settings.hpp"
#include "types.hpp"
#include "id_mapper.hpp"
#include "vector_storage.hpp"
#include "../sparse/sparse_storage.hpp"
#include "rand_utils.hpp"
#include "index_meta.hpp"
#include "shared_mdbx.hpp"
#include "msgpack_ndd.hpp"
#include "quant_vector.hpp"
#include "wal.hpp"
#include "../quant/dispatch.hpp"
#include <memory>
#include <deque>
#include <unordered_map>
#include <list>
#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <filesystem>
#include <thread>
#include <atomic>
#include <optional>
#include <queue>
#include <random>
#include <type_traits>
#include <future>

struct IndexConfig {
    size_t dim;
    ndd::SparseScoringModel sparse_model = ndd::SparseScoringModel::NONE;
    size_t max_elements = settings::MAX_ELEMENTS;
    std::string space_type_str;
    size_t M = settings::DEFAULT_M;
    size_t ef_construction = settings::DEFAULT_EF_CONSTRUCT;
    ndd::quant::QuantizationLevel quant_level =
            ndd::quant::QuantizationLevel::INT8;  // Default to INT8 quantization
    const int32_t checksum;
};

struct IndexInfo {
    size_t total_elements;
    size_t dimension;
    ndd::SparseScoringModel sparse_model = ndd::SparseScoringModel::NONE;
    std::string space_type_str;
    ndd::quant::QuantizationLevel
            quant_level;  // Selected quantization level
    int32_t checksum;
    size_t M;
    size_t ef_con;
};

struct CacheEntry {
    std::string index_id;
    ndd::SparseScoringModel sparse_model = ndd::SparseScoringModel::NONE;
    std::unique_ptr<hnswlib::HierarchicalNSW<float>> alg;
    std::shared_ptr<IDMapper> id_mapper;
    std::shared_ptr<VectorStorage> vector_storage;
    std::unique_ptr<ndd::SparseVectorStorage> sparse_storage;
    std::unique_ptr<WriteAheadLog> wal;
    // UNUSED as of 2026-06-01: written by both constructors but never read.
    std::chrono::system_clock::time_point last_access;
    // UNUSED as of 2026-06-01: set once in the full constructor, never read.
    std::chrono::system_clock::time_point last_saved_at;
    std::chrono::system_clock::time_point last_dirtied_at;

    /**
     * Indicates if the index has unsaved in-memory changes
     * is_dirty = true -> the index has not been persisted in storage
     */
    bool is_dirty{false};

    /**
     * cache_valid marks whether this CacheEntry is still the active entry
     * for its index_id.
     *
     * This is needed because the per-thread cache stores weak_ptr<CacheEntry>.
     * A weak_ptr only tells us whether the old CacheEntry object is still alive;
     * it does not tell us whether that object is still the current entry in
     * IndexManager::indices_.
     *
     * After delete/evict/reload, the old CacheEntry may still remain alive
     * because in-progress readers still hold shared_ptr references to it. In that
     * window, weak_ptr.lock() can still succeed. cache_valid prevents new lookups
     * from reusing that stale entry while allowing existing users of the entry to
     * finish safely.
     *
     * NOTE: This need not be an atomic bool because its a huge performance penalty
     * and negligible correctness improvement.
     */
    bool cache_valid{true};

    /**
     * UNUSED as of 2026-06-01: incremented in search() but never read by
     * any caller or telemetry path. Left in place pending a future metrics hookup.
     *
     * Number of searches performed on this index. For a search with top_k=10
     * it will be 10
     *
     * NOTE: Since there can be multiple readers hitting the same index,
     * there is no guarantee that this number will capture the true searchCount.
     * using std::atomics will levy (~10%) performance penalty.
     */
    size_t searchCount{0};

    /**
     * Per-index reader's - writer's operation lock
     *
     * writers: addVectors, saveIndexInternal, saveIndex, deleteVectors,
     * evictIfNeeded, recoverIndex, deleteVectorsByFilter, updateFilters,
     * deleteIndex, executeBackupJob
     *
     * readers: search, getVector, getIndexInfo (loaded-index path only)
     *
     * NOTE: std::shared_mutex dont guarantee fairness between
     * readers and writers. ie. currently it could be the case that either
     * reads or writes can starve if other is being flooded.
     *
     * TODO: If that is required, we will have to implement a custom reader-writer
     * locking mechanism with a ticketing system to guarantee fairness.
     *
     * XXX: We want readers to work even when writers are happening on an index
     * If we use a reader's lock in read path, long running writes will starve 
     * them. So for now, we are not using readers lock. This doesnt affect
     * correctness for now. Access-after-delete errors are handled by making
     * CacheEntry a shared_ptr in IndexManager.
     * TODO: Revisit the locking mechanism to make it finegrained for performance.
     */
    std::shared_mutex operation_mutex;

    // Default constructor required for map
    CacheEntry() :
        last_access(std::chrono::system_clock::now()) {}

    CacheEntry(std::string index_id_,
               ndd::SparseScoringModel sparse_model_,
               std::unique_ptr<hnswlib::HierarchicalNSW<float>> alg_,
               std::shared_ptr<IDMapper> mapper_,
               std::shared_ptr<VectorStorage> storage_,
               std::unique_ptr<ndd::SparseVectorStorage> sparse_storage_,
               std::unique_ptr<WriteAheadLog> wal_,
               std::chrono::system_clock::time_point access_time_) {
        LOG_INFO(2001, index_id_, "Creating cache entry");

        // Validate all components
        if(!alg_) {
            LOG_ERROR(2002, index_id_, "Algorithm is null");
            throw std::runtime_error("Algorithm is null");
        }
        if(!mapper_) {
            LOG_ERROR(2003, index_id_, "ID mapper is null");
            throw std::runtime_error("ID Mapper is null");
        }
        if(!storage_) {
            LOG_ERROR(2004, index_id_, "Vector storage is null");
            throw std::runtime_error("Vector Storage is null");
        }

        LOG_INFO(2005, index_id_, "Assigning index id");
        index_id = std::move(index_id_);
        sparse_model = sparse_model_;

        id_mapper = std::move(mapper_);

        vector_storage = std::move(storage_);

        sparse_storage = std::move(sparse_storage_);
        wal = std::move(wal_);

        last_access = access_time_;

        LOG_INFO(2006, index_id, "Moving algorithm instance");
        alg = std::move(alg_);

        last_saved_at = std::chrono::system_clock::now();

        LOG_INFO(2007, index_id, "Cache entry construction completed");
    }

    void markDirty() {
        is_dirty = true;
        last_dirtied_at = std::chrono::system_clock::now();
    }
    // UNUSED as of 2026-06-01: no callers (paired with searchCount above).
    void resetSearchCount() { searchCount = 0; }
    // Delete copy constructor and assignment
    CacheEntry(const CacheEntry&) = delete;
    CacheEntry& operator=(const CacheEntry&) = delete;

    // Disable move operations since mutex is not movable
    CacheEntry(CacheEntry&&) = delete;
    CacheEntry& operator=(CacheEntry&&) = delete;
};

#ifdef NDD_SERVERLESS
    #include "../../serverless/usage_stats_severless.hpp"
#endif

struct PersistenceConfig {
    size_t save_every_n_updates{settings::SAVE_EVERY_N_UPDATES};
    std::chrono::minutes save_interval{settings::SAVE_EVERY_N_MINUTES};
    bool save_on_shutdown{true};
};

#include "../storage/backup_store.hpp"

class IndexManager {
private:
    std::deque<std::string> indices_list_;
    std::unordered_map<std::string, std::shared_ptr<CacheEntry>> indices_;

    /**
     * This is a thread local store(TLS) for the indices_. ie. hot indices
     * need not look at indices_ and take a global lock repeatedly.
     * look at getIndexEntry() for more.
     */
    inline static thread_local std::unordered_map<std::string, std::weak_ptr<CacheEntry>>
            per_thread_indices_;
    std::shared_mutex indices_mutex_;
    std::string data_dir_;
    PersistenceConfig persistence_config_;
    std::atomic<bool> shutdown_requested_{false};
    std::condition_variable persistence_cv_;
    std::unique_ptr<MetadataManager> metadata_manager_;
    // Autosave methods
    std::thread autosave_thread_;
    std::atomic<bool> running_{true};
    BackupStore backup_store_;
    void executeBackupJob(const std::string& index_id, const std::string& backup_name,
                          std::stop_token st);

    std::unique_ptr<WriteAheadLog> createWAL(const std::string& index_id, MDBX_env* env) {
        return std::make_unique<WriteAheadLog>(env, index_id);
    }

    WriteAheadLog* getOrCreateWAL(CacheEntry& entry) {
        if(!entry.wal) {
            entry.wal = createWAL(entry.index_id, entry.vector_storage->shared_env());
        }
        return entry.wal.get();
    }

    /**
     * Clears the WAL by opening a one-shot WRITE txn and calling `clear`.
     * WAL clear is not safety-critical - stale entries replay idempotently
     * on next recovery - so failures are logged and swallowed instead of
     * thrown.
     */
    void clearWAL(CacheEntry& entry) {
        WriteAheadLog* wal = getOrCreateWAL(entry);
        MDBX_txn* txn = nullptr;
        int rc = mdbx_txn_begin(wal->env(), nullptr, MDBX_TXN_READWRITE, &txn);
        if(rc != MDBX_SUCCESS) {
            LOG_ERROR(1404,
                      entry.index_id,
                      "Failed to begin WAL clear txn: " << mdbx_strerror(rc));
            return;
        }
        try {
            wal->clear(txn);
        } catch(const std::exception& e) {
            mdbx_txn_abort(txn);
            LOG_ERROR(1405, entry.index_id, "WAL clear failed: " << e.what());
            return;
        }
        rc = mdbx_txn_commit(txn);
        if(rc != MDBX_SUCCESS) {
            LOG_ERROR(1406,
                      entry.index_id,
                      "Failed to commit WAL clear: " << mdbx_strerror(rc));
        }
    }

    // Helper method for WAL recovery
    void recoverFromWAL(CacheEntry& entry) {
        const std::string& index_id = entry.index_id;
        WriteAheadLog* wal = getOrCreateWAL(entry);

        // Check if WAL has entries needing recovery
        if(wal->hasEntries()) {
            LOG_INFO(2008, index_id, "WAL recovery needed");

            auto wal_entries = wal->readEntries();
            LOG_INFO(2009, index_id, "Read " << wal_entries.size() << " entries from WAL");

            // Process all entries in the exact order they were recorded
            std::vector<idInt> failed_vector_add_ids;

            for(const auto& wal_entry : wal_entries) {
                try {
                    if(wal_entry.op_type == WALOperationType::VECTOR_ADD) {
                        // Check if vector exists in storage before recovering
                        auto vec_result = entry.vector_storage->get_vector(wal_entry.numeric_id);
                        if(vec_result.ok() && !vec_result.value->empty()) {
                            const auto& vector_bytes = *vec_result.value;
                            if(entry.alg->hasLabel(wal_entry.numeric_id)) {
                                entry.alg->addPoint<false>(vector_bytes.data(), wal_entry.numeric_id);
                            } else {
                                entry.alg->addPoint<true>(vector_bytes.data(), wal_entry.numeric_id);
                            }
                        } else {
                            // Vector doesn't exist - this VECTOR_ADD failed
                            failed_vector_add_ids.push_back(wal_entry.numeric_id);
                            LOG_DEBUG("VECTOR_ADD failed for ID " << wal_entry.numeric_id
                                                                  << " - adding to deleted_ids");
                        }
                    } else if(wal_entry.op_type == WALOperationType::VECTOR_UPDATE) {
                        // Recover vector update
                        auto vec_result = entry.vector_storage->get_vector(wal_entry.numeric_id);
                        if(vec_result.ok() && !vec_result.value->empty()) {
                            const auto& vector_bytes = *vec_result.value;
                            if(entry.alg->hasLabel(wal_entry.numeric_id)) {
                                entry.alg->addPoint<false>(vector_bytes.data(), wal_entry.numeric_id);
                            } else {
                                entry.alg->addPoint<true>(vector_bytes.data(), wal_entry.numeric_id);
                            }
                        }
                    } else if(wal_entry.op_type == WALOperationType::VECTOR_DELETE) {
                        // For deletions, just mark the vector as deleted
                        if(entry.alg->hasLabel(wal_entry.numeric_id)
                           && !entry.alg->isLabelDeleted(wal_entry.numeric_id)) {
                            entry.alg->markDelete(wal_entry.numeric_id);
                        }
                    }
                } catch(const std::exception& e) {
                    if(wal_entry.op_type == WALOperationType::VECTOR_ADD) {
                        // If VECTOR_ADD recovery failed, add ID to failed list
                        failed_vector_add_ids.push_back(wal_entry.numeric_id);
                        LOG_DEBUG("VECTOR_ADD recovery failed for ID " << wal_entry.numeric_id
                                                                       << ": " << e.what());
                    } else {
                        LOG_DEBUG("Failed to recover operation for vector " << wal_entry.numeric_id
                                                                            << ": " << e.what());
                    }
                }
            }

            // Add failed VECTOR_ADD IDs back to deleted_ids for reuse
            if(!failed_vector_add_ids.empty()) {
                entry.id_mapper->reclaim_failed_ids(failed_vector_add_ids);
                LOG_INFO(2010,
                               index_id,
                               "Reclaimed " << failed_vector_add_ids.size()
                                            << " failed VECTOR_ADD ids for reuse");
            }

            // Mark as dirty to trigger a save
            entry.markDirty();
            // Explicitly save the index after recovery
            LOG_DEBUG("Saving index after WAL recovery: " << index_id);
            // Save index will also clear the WAL and save bloom filter
            // FIX: Call saveIndexInternal instead of saveIndex to avoid circular lock
            saveIndexInternal(entry);
        }
    }

    // The thread will call this method
    void autosaveLoop() {
        LOG_INFO(2011, "Autosave thread started");
        while(running_) {
            // Sleep for AUTOSAVE_SLEEP_MINUTES
            std::this_thread::sleep_for(std::chrono::minutes(settings::AUTOSAVE_SLEEP_MINUTES));

            // Check if we're still running
            if(!running_) {
                break;
            }
            LOG_INFO(2012, "Autosave check running");
            checkAndSaveIndices();
        }
        LOG_INFO(2013, "Autosave thread stopped");
    }

    // Check and save indices based on when they were last dirtied
    void checkAndSaveIndices() {
        std::vector<std::string> indices_to_save;
        auto now = std::chrono::system_clock::now();

        /**
         * Identify the dirty indices without holding indices_mutex_ for too long
         */
        {
            std::shared_lock<std::shared_mutex> read_lock(indices_mutex_);
            for(const auto& [index_id, entry] : indices_) {
                if(entry && entry->is_dirty) {
                    auto time_since_dirtied = now - entry->last_dirtied_at;
                    // Save if more than SAVE_EVERY_N_MINUTES minutes since the last mutation
                    if(time_since_dirtied
                       > std::chrono::minutes(settings::SAVE_EVERY_N_MINUTES)) {
                        indices_to_save.push_back(index_id);
                    }
                }
            }
        }

        /* Write each dirty index back */
        for(const auto& index_id : indices_to_save) {
            bool should_save = false;
            {
                std::shared_lock<std::shared_mutex> read_lock(indices_mutex_);
                auto it = indices_.find(index_id);
                should_save = (it != indices_.end() && it->second && it->second->is_dirty);
            }

            if(should_save) {
                LOG_DEBUG("Auto-saving index (60-minute threshold): " << index_id);
                saveIndex(index_id);
            }
        }

        #ifdef NDD_SERVERLESS
        // Collect and send usage stats (serverless only)
        serverless::collectAndSendUsageStats(indices_);
        #endif
    }

    /**
     * Returns the CacheEntry if it is resident in memory.
     */
    std::shared_ptr<CacheEntry> findInMemoryIndexEntry(const std::string& index_id) {

        /**
         * First check the entry in thread local storage (TLS)
         * indices_ will be referred only if:
         * 1. index_id is not found
         * 2. entry is not pointing to a valid shared_ptr
         * 3. entry is pointing to a valid shared_ptr but cache_valid == false
         */
        auto cached_it = per_thread_indices_.find(index_id);
        if(cached_it != per_thread_indices_.end()) {
            auto entry = cached_it->second.lock();
            if(entry && entry->cache_valid) {
                return entry;
            }
            per_thread_indices_.erase(cached_it);
        }

        /**
         * Second check if this index is in global indices_.
         * A read lock on indices_mutex_ is enough for this.
         */
        std::shared_lock<std::shared_mutex> read_lock(indices_mutex_);
        auto it = indices_.find(index_id);
        if(it == indices_.end() || !it->second || !it->second->cache_valid) {
            return nullptr;
        }

        //update TLS indices_
        auto entry = it->second;
        per_thread_indices_[index_id] = entry;

        return entry;
    }

    /**
     * Returns the shared_ptr to CacheEntry
     * 1. If Index is active (in-memory), return from there
     * 2. Else, fetch from disk, make active and then return.
     */
    std::shared_ptr<CacheEntry> getIndexEntry(const std::string& index_id) {
        if(auto entry = findInMemoryIndexEntry(index_id)) {
            return entry;
        }

        /**
         * Index not found in memory.
         * Hold a write lock on indices_mutex_ and fetch it from disk
         */
        {
            std::unique_lock<std::shared_mutex> write_lock(indices_mutex_);
            auto it = indices_.find(index_id);
            if(it == indices_.end()) {
                ensureLiveIndexCapacity(index_id, "load index");
                loadIndex(index_id);  // modifies indices_
            }
            it = indices_.find(index_id);
            if(it == indices_.end()) {
                throw std::runtime_error("[ERROR] Index " + index_id + " doesnt exist.");
            }
            auto entry = it->second;
            per_thread_indices_[index_id] = entry;
            return entry;
        }
    }

    void saveIndex(const std::string& index_id) {
        LOG_DEBUG("saveIndex called for index=" + index_id);

        // Get the index entry (thread-safe)
        auto entry = getIndexEntry(index_id);

        // Use per-index operation mutex to prevent concurrent operations
        std::unique_lock<std::shared_mutex> operation_lock(entry->operation_mutex);

        // Call internal implementation
        saveIndexInternal(*entry);
    }

private:
    // Internal saveIndex implementation that doesn't call getIndexEntry
    // Used by functions that already have the entry and mutex
    void saveIndexInternal(CacheEntry& entry) {
        // Double check if the index is still dirty
        if(!entry.is_dirty) {
            return;
        }
        LOG_DEBUG("Saving index " << entry.index_id);
        // Auto-resize check
        size_t remainingCapacity = entry.alg->getRemainingCapacity();
        LOG_DEBUG("Remaining capacity for index " << entry.index_id << ": " << remainingCapacity);
        size_t maxElements = entry.alg->getMaxElements();
        LOG_DEBUG("Max elements for index " << entry.index_id << ": " << maxElements);

        // If remaining capacity is less than 50k, resize by adding 100k
        if(remainingCapacity < settings::MAX_ELEMENTS_INCREMENT_TRIGGER) {
            size_t newMaxElements = maxElements + settings::MAX_ELEMENTS_INCREMENT;
            LOG_DEBUG("Auto-resizing index " << entry.index_id << " from " << maxElements << " to "
                                            << newMaxElements << " elements");

            try {
                entry.alg->resizeIndex(newMaxElements);
            } catch(const std::exception& e) {
                LOG_DEBUG("Failed to auto-resize index: " << e.what());
                // Continue with saving even if resize fails
            }
        }

        std::string index_dir = data_dir_ + "/" + entry.index_id;
        std::string vector_storage_dir = index_dir + "/vectors";
        std::string index_path = vector_storage_dir + "/" + settings::DEFAULT_SUBINDEX + ".idx";
        std::string temp_path = index_path + ".tmp";

        entry.alg->saveIndex(temp_path);
        std::filesystem::rename(temp_path, index_path);

        // Clear the WAL
        clearWAL(entry);

        // Update element count in metadata
        if(!metadata_manager_->updateElementCount(entry.index_id, entry.alg->getElementsCount())) {
            LOG_WARN(
                    2014, entry.index_id, "Failed to update element count in metadata");
        }
        entry.is_dirty = false;
    }

public:

    /**
     * TODO:
     * This function is currently triggered by:
     * 1.
     * 2.
     *
     * For more look at docs/memory_management.md
     */
    void evictIfNeeded() {
        size_t max_attempts = std::max(indices_list_.size(), indices_.size());
        while(indices_.size() >= settings::MAX_LIVE_INDICES && max_attempts > 0) {
            if(indices_list_.empty()) {
                LOG_ERROR(2048, "Cannot evict index: indices_list_ is empty while cache is full");
                return;
            }

            std::string to_evict = indices_list_.back();
            auto it = indices_.find(to_evict);
            if(it == indices_.end()) {
                LOG_WARN(2049, to_evict, "Dropping stale eviction candidate from indices_list_");
                indices_list_.pop_back();
                --max_attempts;
                continue;
            }

            try {
                auto entry = it->second;
                std::unique_lock<std::shared_mutex> operation_lock(entry->operation_mutex);
                if(entry->is_dirty) {
                    LOG_INFO(2050, to_evict, "Saving dirty index before eviction");
                    saveIndexInternal(*entry);
                }
            } catch(const std::exception& e) {
                LOG_ERROR(2051, to_evict, "Failed to save dirty index during eviction: " << e.what());
                return;
            }

            if(it->second->is_dirty) {
                LOG_WARN(2054, to_evict, "Index remained dirty after forced save; aborting eviction");
                return;
            }

            LOG_INFO(2016, to_evict, "Evicting clean index from cache");
            it->second->cache_valid = false;
            indices_.erase(it);
            indices_list_.pop_back();
            --max_attempts;
        }

        if(indices_.size() >= settings::MAX_LIVE_INDICES) {
            LOG_ERROR(2052,
                      "Eviction attempts exhausted while live index cache remains at limit");
        }
    }

    void ensureLiveIndexCapacity(const std::string& index_id, const char* action) {
        if(indices_.size() < settings::MAX_LIVE_INDICES) {
            return;
        }

        evictIfNeeded();
        if(indices_.size() >= settings::MAX_LIVE_INDICES) {
            LOG_ERROR(2047,
                      index_id,
                      "Unable to " << action << ": live index cache remains at limit "
                                    << settings::MAX_LIVE_INDICES);
            throw std::runtime_error("Unable to " + std::string(action)
                                     + ": live index cache is full");
        }
    }

    std::string getUserPath(const std::string& username) { return data_dir_ + "/" + username; }

    std::string getIndexPath(const std::string& username, const std::string& index_name) {
        return getUserPath(username) + "/" + index_name;
    }

public:
    IndexManager(const std::string& data_dir,
                const PersistenceConfig& persistence_config = PersistenceConfig{}) :
        data_dir_(data_dir),
        persistence_config_(persistence_config),
        backup_store_(data_dir) {
        std::filesystem::create_directories(data_dir);
        metadata_manager_ = std::make_unique<MetadataManager>(data_dir);
        // Start the autosave thread
        autosave_thread_ = std::thread(&IndexManager::autosaveLoop, this);
    }

    ~IndexManager() {
        // Signal all threads to stop (running_ is checked by autosave and backup threads)
        running_ = false;

        // Join background backup threads before destroying members
        // (prevents use-after-free when detached threads outlive IndexManager)
        backup_store_.joinAllThreads();

        /**
         * Don't wait for autosave thread to exit.
         * Since the thread might be sleeping, waiting for join
         * would be time consuming.
         *
         * TODO: This is a stop-gap solution.
         * Fix it with conditional variables.
         */
        if(autosave_thread_.joinable()) {
            autosave_thread_.detach();
        }

        /**
         * Persist all the dirty indices to disk.
         */
        if(persistence_config_.save_on_shutdown) {
            shutdown_requested_ = true;
            persistence_cv_.notify_all();
            LOG_INFO("Saving indices during shutdown");
            std::vector<std::string> indices_to_save;
            {
                std::shared_lock<std::shared_mutex> read_lock(indices_mutex_);
                for(const auto& pair : indices_) {
                    if(pair.second && pair.second->is_dirty) {
                        indices_to_save.push_back(pair.first);
                    }
                }
            }
            for(const auto& index_id : indices_to_save) {
                try {
                    LOG_INFO(2017, index_id, "Saving dirty index during shutdown");
                    saveIndex(index_id);
                } catch(const std::exception& e) {
                    LOG_ERROR(2015,
                                    index_id,
                                    "Failed to save index during shutdown: " << e.what());
                }
            }
            LOG_INFO("Shutdown complete");
        }
    }

    // Reset the index file. It does not affect the LMDB or metadata.
    // This is used when the index is corrupted or needs to be reset.
    bool resetIndex(const std::string& index_id, const IndexConfig& config) {
        std::string base_path = data_dir_ + "/" + index_id;
        std::string vector_storage_dir = base_path + "/vectors";
        std::string index_path = vector_storage_dir + "/" + settings::DEFAULT_SUBINDEX + ".idx";
        LOG_DEBUG(index_path);
        std::string recover_file = base_path + "/recover.txt";
        LOG_DEBUG(recover_file);

        // 1. Fail if directory doesn't exist
        if(!std::filesystem::exists(base_path)) {
            LOG_ERROR(2018, index_id, "Index directory does not exist: " << base_path);
            return false;
        }

        // 2. Fail if index file already exists
        if(std::filesystem::exists(index_path)) {
            LOG_ERROR(2019, index_id, "Index file already exists: " << index_path);
            return false;
        }

        // 3. Create and save empty HNSW index
        auto space_type = hnswlib::getSpaceType(config.space_type_str);
        ndd::quant::QuantizationLevel quant_level = config.quant_level;

        hnswlib::HierarchicalNSW<float> hnsw(config.max_elements,
                                             space_type,
                                             config.dim,
                                             config.M,
                                             config.ef_construction,
                                             settings::RANDOM_SEED,
                                             quant_level,
                                             config.checksum);
        hnsw.saveIndex(index_path);

        // 4. Write recover.txt with "0:0"
        std::ofstream fout(recover_file);
        fout << "0:0\n";
        fout.close();

        LOG_INFO(2020, index_id, "Index reset complete and saved");
        return true;
    }



    bool createIndex(const std::string& index_id,
                     const IndexConfig& config,
                     UserType user_type = UserType::Admin,
                     size_t size_in_millions = 0) {
        // Get username and index name from index_id
        auto pos = index_id.find('/');
        if(pos == std::string::npos) {
            throw std::runtime_error("Invalid index ID");
        }
        std::string index_dir = data_dir_ + "/" + index_id;
        std::string username = index_id.substr(0, pos);
        std::string index_name = index_id.substr(pos + 1);
        // Check if index already exists in metadata
        auto existing_indices = metadata_manager_->listUserIndexes(username);
        for(const auto& existing : existing_indices) {
            if(existing.first == index_name) {
                throw std::runtime_error("Index with this name already exists for this user");
            }
        }

        // Validate max_elements against user limits (unless admin with custom size)
        size_t max_vectors_allowed = getMaxVectorsPerIndex(user_type);
        if(user_type != UserType::Admin || size_in_millions == 0) {
            if(config.max_elements > max_vectors_allowed) {
                throw std::runtime_error("Index size " + std::to_string(config.max_elements)
                                         + " exceeds limit of "
                                         + std::to_string(max_vectors_allowed) + " vectors for "
                                         + userTypeToString(user_type) + " users");
            }
        }

        // Check file system without lock
        std::string vector_storage_dir = index_dir + "/vectors";
        std::string index_path = vector_storage_dir + "/" + settings::DEFAULT_SUBINDEX + ".idx";
        if(std::filesystem::exists(index_path)) {
            throw std::runtime_error("Index already exists");
        }

        // Evict if needed (clean indices only)
        {
            std::unique_lock<std::shared_mutex> temp_lock(indices_mutex_);
            ensureLiveIndexCapacity(index_id, "create index");
        }

        hnswlib::SpaceType space_type = hnswlib::getSpaceType(config.space_type_str);

        ndd::quant::QuantizationLevel quant_level = config.quant_level;
        auto vector_storage = std::make_shared<VectorStorage>(
                index_dir, index_id, config.dim, config.quant_level);
        MDBX_env* shared_env = vector_storage->shared_env();
        ndd::storage::SharedIndexEnv::write_layout_version(
                shared_env, settings::INDEX_LAYOUT_VERSION);

        //create the DBI and initialize sequence for IDMapper
        auto id_mapper = std::make_shared<IDMapper>(shared_env, "id_map");
        id_mapper->init_sequence();

        // Initialize Sparse Storage if needed
        std::unique_ptr<ndd::SparseVectorStorage> sparse_storage = nullptr;
        if(ndd::sparseModelEnabled(config.sparse_model)) {
            sparse_storage = std::make_unique<ndd::SparseVectorStorage>(
                shared_env, index_id, config.sparse_model);
            if(!sparse_storage->initialize()) {
                throw std::runtime_error("Failed to initialize sparse storage");
            }
        }

        auto alg = std::make_unique<hnswlib::HierarchicalNSW<float>>(config.max_elements,
                                                                     space_type,
                                                                     config.dim,
                                                                     config.M,
                                                                     config.ef_construction,
                                                                     settings::RANDOM_SEED,
                                                                     quant_level,
                                                                     config.checksum);

        /**
         * Hot fetcher path called by HNSW's graph traversal. When the
         * caller passes a non-null txn (the request-scoped MDBX
         * snapshot from search), the read uses that snapshot so the
         * whole traversal sees one view and MDBX sticky-thread mode is
         * not violated by a nested mdbx_txn_begin. A null txn means
         * the caller did not open a snapshot (e.g. the write path
         * during addPoint) - the storage opens its own RDONLY.
         */
        alg->setVectorFetcher([vs = vector_storage](MDBX_txn* txn,
                                                    ndd::idInt label,
                                                    uint8_t* buffer) {
            return vs->get_vector(txn, label, buffer);
        });

        alg->setVectorFetcherBatch([vs = vector_storage](MDBX_txn* txn,
                                                         const ndd::idInt* labels,
                                                         uint8_t* buffers,
                                                         bool* success,
                                                         size_t count) -> size_t {
            return vs->get_vectors_batch_into(txn, labels, buffers, success, count);
        });

        auto wal = createWAL(index_id, shared_env);

        // Add to indices with minimal lock scope
        {
            std::unique_lock<std::shared_mutex> lock(indices_mutex_);
            auto entry = std::make_shared<CacheEntry>(index_id,
                                                      config.sparse_model,
                                                      std::move(alg),
                                                      id_mapper,
                                                      vector_storage,
                                                      std::move(sparse_storage),
                                                      std::move(wal),
                                                      std::chrono::system_clock::now());
            auto [it, inserted] = indices_.emplace(index_id, entry);
            it->second->markDirty();
            indices_list_.push_front(index_id);
        }

        // Create and store index metadata
        IndexMetadata metadata_entry;
        metadata_entry.name = index_name;
        metadata_entry.dimension = config.dim;
        metadata_entry.sparse_model = config.sparse_model;
        metadata_entry.space_type_str = config.space_type_str;
        metadata_entry.quant_level = config.quant_level;
        metadata_entry.checksum = config.checksum;
        metadata_entry.total_elements = 0;
        metadata_entry.M = config.M;
        metadata_entry.ef_con = config.ef_construction;
        // SharedIndexEnv also persists this in the layout_meta DBI for storage-level gating.
        metadata_entry.layout_version = settings::INDEX_LAYOUT_VERSION;
        metadata_entry.created_at = std::chrono::system_clock::now();

        if(!metadata_manager_->storeMetadata(index_id, metadata_entry)) {
            throw std::runtime_error("Failed to store index metadata");
        }

        LOG_INFO(2022, index_id, "Saving newly created index");
        // Index is marked dirty so it needs to be saved immediately for crash recovery
        saveIndex(index_id);
        return true;
    }

    std::vector<std::pair<std::string, IndexMetadata>>
    listUserIndexes(const std::string& username) {
        // Use the metadata manager directly to get the list of indexes
        return metadata_manager_->listUserIndexes(username);
    }
    std::vector<std::pair<std::string, IndexMetadata>> listAllIndexes() {
        // Use the metadata manager directly to get the list of indexes
        return metadata_manager_->listAllIndexes();
    }

    void loadIndex(const std::string& index_id) {
        std::string index_dir = data_dir_ + "/" + index_id;
        std::string vector_storage_dir = index_dir + "/vectors";
        std::string index_path = vector_storage_dir + "/" + settings::DEFAULT_SUBINDEX + ".idx";

        if(std::filesystem::exists(std::filesystem::path(index_dir)
                                   / settings::INDEX_MIGRATION_MARKER)) {
            throw std::runtime_error(settings::INCOMPLETE_INDEX_MIGRATION_ERROR);
        }

        // Load metadata to get sparse_model
        auto metadata = metadata_manager_->getMetadata(index_id);
        if(!metadata) {
            throw std::runtime_error("Missing or incompatible index metadata for index: "
                                     + index_id);
        }
        if(metadata->layout_version != settings::INDEX_LAYOUT_VERSION) {
            throw std::runtime_error(settings::indexLayoutError(metadata->layout_version));
        }
        const ndd::SparseScoringModel sparse_model = metadata->sparse_model;

        if(!std::filesystem::exists(index_path) || !std::filesystem::exists(vector_storage_dir)) {
            throw std::runtime_error("Required files missing for index: " + index_id);
        }

        // Step 1: Load HNSW index (automatically adjusts cache based on element count and cache
        // percentage)
        std::unique_ptr<hnswlib::HierarchicalNSW<float>> alg;
        try {
            alg = std::make_unique<hnswlib::HierarchicalNSW<float>>(index_path, 0);

        } catch(const std::exception& e) {
            throw std::runtime_error("Cannot load index '" + index_id + "': " + e.what());
        }

        // Step 2: Create shared-env stores. IDMapper handles sequence initialization.
        auto vector_storage = std::make_shared<VectorStorage>(
                index_dir, index_id, alg->getDimension(), alg->getQuantLevel());
        MDBX_env* shared_env = vector_storage->shared_env();
        auto id_mapper = std::make_shared<IDMapper>(shared_env, "id_map");

        // Initialize Sparse Storage if sparse_model is enabled
        std::unique_ptr<ndd::SparseVectorStorage> sparse_storage;
        if(ndd::sparseModelEnabled(sparse_model)) {
            sparse_storage = std::make_unique<ndd::SparseVectorStorage>(
                shared_env, index_id, sparse_model);
            if(!sparse_storage->initialize()) {
                throw std::runtime_error("Failed to initialize sparse storage for index: "
                                         + index_id);
            }
        }

        // Set up vector fetcher
        /**
         * Hot fetcher path called by HNSW's graph traversal. When the
         * caller passes a non-null txn (the request-scoped MDBX
         * snapshot from search), the read uses that snapshot so the
         * whole traversal sees one view and MDBX sticky-thread mode is
         * not violated by a nested mdbx_txn_begin. A null txn means
         * the caller did not open a snapshot (e.g. the write path
         * during addPoint) - the storage opens its own RDONLY.
         */
        alg->setVectorFetcher([vs = vector_storage](MDBX_txn* txn,
                                                    ndd::idInt label,
                                                    uint8_t* buffer) {
            return vs->get_vector(txn, label, buffer);
        });

        alg->setVectorFetcherBatch([vs = vector_storage](MDBX_txn* txn,
                                                         const ndd::idInt* labels,
                                                         uint8_t* buffers,
                                                         bool* success,
                                                         size_t count) -> size_t {
            return vs->get_vectors_batch_into(txn, labels, buffers, success, count);
        });

        auto wal = createWAL(index_id, shared_env);

        LOG_DEBUG("Loaded index: " << index_id);
        LOG_DEBUG("Created space for index: " << index_id);

        // Step 3: Update cache entry so that index becomes available to other threads
        auto entry = std::make_shared<CacheEntry>(index_id,
                                                  sparse_model,
                                                  std::move(alg),
                                                  id_mapper,
                                                  vector_storage,
                                                  std::move(sparse_storage),
                                                  std::move(wal),
                                                  std::chrono::system_clock::now());
        auto [it, inserted] = indices_.emplace(index_id, entry);
        indices_list_.push_front(index_id);

        // Handle WAL recovery using the IndexManager's method
        recoverFromWAL(*it->second);
    }

    // Reload index: save (if dirty), evict from memory, and reload
    // Cache size is automatically checked and adjusted if < 5% of element count during reload
    bool reload(const std::string& index_id) {
        LOG_INFO(2023, index_id, "Starting reload");

        try {
            // Phase 1: Save index if it is dirty
            {
                std::shared_lock<std::shared_mutex> lock(indices_mutex_);
                auto it = indices_.find(index_id);
                if(it != indices_.end() && it->second && it->second->is_dirty) {
                    LOG_INFO(2055, index_id, "Saving dirty index before reload");
                    saveIndex(index_id);
                }
            }

            // Phase 2: Evict from memory
            {
                std::unique_lock<std::shared_mutex> lock(indices_mutex_);
                auto it = indices_.find(index_id);
                if(it != indices_.end()) {
                    // Remove from LRU list
                    auto list_it = std::find(indices_list_.begin(), indices_list_.end(), index_id);
                    if(list_it != indices_list_.end()) {
                        indices_list_.erase(list_it);
                    }
                    it->second->cache_valid = false;
                    indices_.erase(it);
                    LOG_INFO(2024, index_id, "Evicted index from cache");
                }
            }

            // Phase 3: Reload (cache adjustment happens automatically in loadIndex)
            {
                std::unique_lock<std::shared_mutex> lock(indices_mutex_);
                loadIndex(index_id);
            }

            // Phase 4: Report final state
            {
                std::shared_lock<std::shared_mutex> lock(indices_mutex_);
                auto it = indices_.find(index_id);
                if(it != indices_.end()) {
                    // Cache removed
                    LOG_INFO(2025,
                                   index_id,
                                   "Reloaded index with "
                                           << it->second->alg->getElementsCount() << " elements");
                }
            }

            return true;
        } catch(const std::exception& e) {
            LOG_ERROR(2026, index_id, "Failed to reload index: " << e.what());
            return false;
        }
    }

    // Add this new function to reload just the algorithm part while preserving the CacheEntry
    void reloadIndex(const std::string& index_id) {
        std::shared_ptr<CacheEntry> entry;
        {
            std::shared_lock<std::shared_mutex> read_lock(indices_mutex_);
            auto it = indices_.find(index_id);
            if(it == indices_.end()) {
                return;  // Index not in cache
            }
            entry = it->second;
        }

        std::string index_dir = data_dir_ + "/" + entry->index_id;
        std::string vector_storage_dir = index_dir + "/vectors";
        std::string index_path = vector_storage_dir + "/" + settings::DEFAULT_SUBINDEX + ".idx";

        // Create a new HNSW algorithm object from the saved file
        auto new_alg = std::make_unique<hnswlib::HierarchicalNSW<float>>(index_path, 0);

        // Set the vector fetcher to use our storage
        new_alg->setVectorFetcher([vs = entry->vector_storage](MDBX_txn* txn,
                                                                ndd::idInt label,
                                                                uint8_t* buffer) {
            return vs->get_vector(txn, label, buffer);
        });

        new_alg->setVectorFetcherBatch([vs = entry->vector_storage](MDBX_txn* txn,
                                                                    const ndd::idInt* labels,
                                                                    uint8_t* buffers,
                                                                    bool* success,
                                                                    size_t count) -> size_t {
            return vs->get_vectors_batch_into(txn, labels, buffers, success, count);
        });

        // Replace the algorithm in the existing entry
        entry->alg = std::move(new_alg);
    }

    /**
     * Adds or updates a batch of vectors and their associated filter documents.
     *
     * Return codes:
     * 0 = success; value is true when vectors were inserted and false for an empty batch
     * 1-99 = propagated filter validation failure from vector storage
     * 100-199 = storage, sparse, or propagated filter storage failure; caller should return HTTP 500
     * 200-299 = propagated filter corruption/invariant failure; caller should return HTTP 500
     */
    template <typename VectorType>
    ndd::OperationResult<bool> addVectors(const std::string& index_id,
                                          std::vector<VectorType> vectors) {
        try {
            // Get the index entry (loads if needed, handles all locking)
            auto entry_ptr = getIndexEntry(index_id);
            auto& entry = *entry_ptr;

            // Use per-index operation mutex to prevent concurrent operations
            std::unique_lock<std::shared_mutex> operation_lock(entry.operation_mutex);

            // Extract string IDs first
            LOG_DEBUG("Adding " << vectors.size() << " vectors to index " << index_id);
            if(vectors.empty()) {
                LOG_DEBUG("No vectors to add");
                return {SUCCESS, "No vectors to add", false};
            }

            /**
             * Reject the batch before any quantization, WAL append, or MDBX
             * transaction if any dense vector's length does not match the index
             * dimension. Downstream quantize() infers dimension from input.size()
             * so a mismatch yields an OOB read in addPoint's distance routines.
             */
            const size_t configured_dim_insert = entry.alg->getDimension();
            for(size_t i = 0; i < vectors.size(); ++i) {
                if(vectors[i].vector.size() != configured_dim_insert) {
                    LOG_WARN(2056,
                             index_id,
                             "Insert rejected: vector at index "
                                     << i << " (id=" << vectors[i].id
                                     << ") has dimension " << vectors[i].vector.size()
                                     << ", expected " << configured_dim_insert);
                    return {2,
                            "vector at index " + std::to_string(i) + " (id="
                                    + vectors[i].id + ") has dimension "
                                    + std::to_string(vectors[i].vector.size())
                                    + ", expected " + std::to_string(configured_dim_insert),
                            false};
                }
            }

            WriteAheadLog* wal = getOrCreateWAL(entry);

            std::vector<std::string> str_ids;
            str_ids.reserve(vectors.size());
            for(const auto& vec : vectors) {
                str_ids.push_back(vec.id);
            }
            LOG_DEBUG("Extracted " << str_ids.size() << " string IDs from vectors");

            std::vector<ndd::SparseVector> sparse_payloads;
            if(entry.sparse_storage) {
                if constexpr(std::is_same_v<VectorType, ndd::HybridVectorObject>) {
                    sparse_payloads.reserve(vectors.size());

                    for(auto& vec : vectors) {
                        if(vec.sparse_ids.size() != vec.sparse_values.size()) {
                            return {1,
                                    "sparse_ids and sparse_values must have the same length"};
                        }

                        ndd::SparseVector sparse_vec;
                        if(!vec.sparse_ids.empty()) {
                            // Sort indices and values together so replacement writes preserve the
                            // inverted index ordering invariants.
                            if(std::is_sorted(vec.sparse_ids.begin(), vec.sparse_ids.end())) {
                                sparse_vec.indices = std::move(vec.sparse_ids);
                                sparse_vec.values = std::move(vec.sparse_values);
                            } else {
                                std::vector<std::pair<uint32_t, float>> pairs(
                                        vec.sparse_ids.size());
                                for(size_t i = 0; i < vec.sparse_ids.size(); ++i) {
                                    pairs[i] = {vec.sparse_ids[i], vec.sparse_values[i]};
                                }
                                std::sort(pairs.begin(), pairs.end());
                                sparse_vec.indices.reserve(pairs.size());
                                sparse_vec.values.reserve(pairs.size());
                                for(const auto& [idx, val] : pairs) {
                                    sparse_vec.indices.push_back(idx);
                                    sparse_vec.values.push_back(val);
                                }
                            }
                        }

                        sparse_payloads.emplace_back(std::move(sparse_vec));
                    }
                }
            }

            // Convert all vectors to QuantVectorObject ONCE using efficient move constructor
            std::vector<QuantVectorObject> quantized_vectors;
            quantized_vectors.reserve(vectors.size());
            ndd::quant::QuantizationLevel quant_level = entry.alg->getQuantLevel();
            auto space = entry.alg->getSpace();
            const void* dist_params = space ? space->get_dist_func_param() : nullptr;

            LOG_DEBUG("Converting " << vectors.size() << " vectors to QuantVectorObject with level "
                                    << (int)quant_level);

            // `vectors` was taken by value; move each element into QuantVectorObject directly,
            // avoiding the temp-copy that the old code paid for.
            for(auto& vec_obj : vectors) {
                quantized_vectors.emplace_back(std::move(vec_obj), quant_level, dist_params);
            }
            LOG_DEBUG("QuantVectorObject conversion completed with move semantics");

            MDBX_txn* txn = nullptr;
            int rc = mdbx_txn_begin(
                    entry.vector_storage->shared_env(), nullptr, MDBX_TXN_READWRITE, &txn);
            if(rc != MDBX_SUCCESS) {
                return {100, "Failed to begin shared index transaction: "
                                     + std::string(mdbx_strerror(rc))};
            }

            std::vector<std::pair<idInt, bool>> numeric_ids;
            int64_t sparse_vector_count_delta = 0;
            auto abort_txn = [&txn, &entry]() {
                if(txn) {
                    mdbx_txn_abort(txn);
                    txn = nullptr;
                    entry.vector_storage->reload_filter_schema_cache();
                }
            };

            try {
                // Get or create numeric IDs in the same write transaction as the data rows.
                if(entry.alg->getDeletedCount() > 0) {
                    numeric_ids = entry.id_mapper->create_ids_batch<true>(txn, str_ids);
                } else {
                    numeric_ids = entry.id_mapper->create_ids_batch<false>(txn, str_ids);
                }
                LOG_DEBUG("Created " << numeric_ids.size() << " numeric IDs for string IDs");
            } catch(const std::exception& e) {
                abort_txn();
                return {100, std::string("Failed to allocate vector IDs: ") + e.what()};
            }

            std::vector<std::pair<idInt, QuantVectorObject>> storage_vectors;
            storage_vectors.reserve(quantized_vectors.size());
            for(size_t i = 0; i < quantized_vectors.size(); i++) {
                // Copy QuantVectorObject for storage (we need to keep original for HNSW)
                storage_vectors.emplace_back(numeric_ids[i].first, quantized_vectors[i]);
            }

            /*
             * Staged term_info_ mutations from the sparse write. These are
             * applied to InvertedIndex::term_info_ only AFTER the shared MDBX
             * txn commits successfully; on any abort path below they go out
             * of scope and term_info_ stays in lockstep with the rolled-back
             * MDBX state. See tests/acid_regression_test.cpp.
             */
            std::vector<ndd::TermInfoChange> sparse_term_info_changes;
            if(entry.sparse_storage && !sparse_payloads.empty()) {
                std::vector<std::pair<ndd::idInt, ndd::SparseVector>> sparse_batch;
                sparse_batch.reserve(sparse_payloads.size());
                for(size_t i = 0; i < sparse_payloads.size(); ++i) {
                    sparse_batch.emplace_back(numeric_ids[i].first, std::move(sparse_payloads[i]));
                }

                auto sparse_result = entry.sparse_storage->store_vectors_batch(
                        txn, sparse_batch, &sparse_vector_count_delta);
                if(!sparse_result.ok()) {
                    abort_txn();
                    LOG_ERROR(2053,
                              index_id,
                              "Failed to update sparse storage for batch size "
                                      << sparse_batch.size() << ": "
                                      << sparse_result.message);
                    return {sparse_result.code, sparse_result.message};
                }
                sparse_term_info_changes = std::move(sparse_result.value_or_throw());
            }

            bool filter_schema_changed = false;
            auto storage_result = entry.vector_storage->store_vectors_batch(
                    txn, storage_vectors, &filter_schema_changed);
            if(!storage_result.ok()) {
                abort_txn();
                if(storage_result.code < 100) {
                    LOG_WARN(1212, index_id, "Insert filters rejected: " << storage_result.message);
                } else {
                    LOG_ERROR(1219, index_id, "Insert filters failed: " << storage_result.message);
                }
                return {storage_result.code, storage_result.message};
            }
            LOG_DEBUG("Stored " << storage_vectors.size()
                                << " pre-quantized vectors in vector storage");

            std::vector<WriteAheadLog::WALEntry> wal_entries;
            wal_entries.reserve(numeric_ids.size());
            for(const auto& [numeric_id, is_new] : numeric_ids) {
                wal_entries.push_back({
                        is_new ? WALOperationType::VECTOR_ADD : WALOperationType::VECTOR_UPDATE,
                        numeric_id,
                });
            }

            try {
                wal->log(txn, wal_entries);
            } catch(const std::exception& e) {
                abort_txn();
                return {100, std::string("Failed to append operation log: ") + e.what()};
            }

            rc = mdbx_txn_commit(txn);
            txn = nullptr;
            if(rc != MDBX_SUCCESS) {
                entry.vector_storage->reload_filter_schema_cache();
                return {100, "Failed to commit shared index transaction: "
                                     + std::string(mdbx_strerror(rc))};
            }
            if(filter_schema_changed) {
                auto schema_result = entry.vector_storage->reload_filter_schema_cache();
                if(!schema_result.ok()) {
                    return {schema_result.code, schema_result.message, false};
                }
            }

            if(entry.sparse_storage && sparse_vector_count_delta != 0) {
                entry.sparse_storage->apply_vector_count_delta(sparse_vector_count_delta);
            }

            /*
             * Apply the staged term_info_ mutations now that the shared MDBX
             * txn has committed. Done here (not inside the sparse `_txn`
             * call) so the in-memory cache always agrees with what MDBX
             * actually committed - see acid_regression_test.cpp for the
             * abort scenario this fixes.
             */
            if(entry.sparse_storage && !sparse_term_info_changes.empty()) {
                entry.sparse_storage->apply_term_info_changes(sparse_term_info_changes);
            }

            // Add to HNSW index in parallel using pre-quantized data from QuantVectorObject
            size_t available_threads = settings::NUM_PARALLEL_INSERTS;
            const size_t num_threads = (available_threads < quantized_vectors.size())
                                               ? available_threads
                                               : quantized_vectors.size();
            std::vector<std::thread> threads;
            const size_t chunk_size =
                    (quantized_vectors.size() + num_threads - 1) / num_threads;  // Ceiling division

            threads.reserve(num_threads);
            for(size_t t = 0; t < num_threads; t++) {
                threads.emplace_back([&, t]() {
                    // Calculate start and end indices for this thread
                    size_t start_idx = t * chunk_size;
                    size_t end_idx = (start_idx + chunk_size < quantized_vectors.size())
                                            ? (start_idx + chunk_size)
                                            : quantized_vectors.size();

                    // Process assigned chunk of vectors
                    for(size_t i = start_idx; i < end_idx; i++) {
                        const auto& quant_vec_obj = quantized_vectors[i];

                        // Use pre-quantized data directly from QuantVectorObject - no conversion
                        // needed!
                        const uint8_t* vector_data = quant_vec_obj.quant_vector.data();

                        // Add to HNSW index using pre-quantized raw bytes
                        if(numeric_ids[i].second) {
                            // If it's a new ID, add it to the index
                            entry.alg->addPoint<true>(vector_data, numeric_ids[i].first);
                        } else {
                            // If it's an update, add it to the index
                            entry.alg->addPoint<false>(vector_data, numeric_ids[i].first);
                        }
                    }
                });
            }

            // Wait for all threads to complete
            for(auto& thread : threads) {
                thread.join();
            }

            entry.markDirty();

            // Check if we need to save based on WAL entry count after logging
            if(wal->getEntryCount() >= persistence_config_.save_every_n_updates) {
                LOG_DEBUG("Saving index " << index_id << " after " << wal->getEntryCount()
                                          << " updates");
                saveIndexInternal(entry);
            }

            PRINT_LOG_TIME();
            return {SUCCESS, "", true};
        } catch(const std::runtime_error& e) {
            // Re-throw runtime_error (includes backup-in-progress check)
            // so it can be caught by API layer and returned as proper JSON error
            throw;
        } catch(const std::exception& e) {
            LOG_ERROR(2027, index_id, "Batch insertion failed: " << e.what());
            return {100, std::string("Batch insertion failed: ") + e.what(), false};
        }
    }

    // Recover a corrupted index from vectorstore and keep adding to the index in batches
    bool recoverIndex(const std::string& index_id) {
        const size_t batch_size = settings::RECOVERY_BATCH_SIZE;
        std::string base_path = data_dir_ + "/" + index_id;
        std::string recover_file = base_path + "/recover.txt";

        if(!std::filesystem::exists(recover_file)) {
            LOG_ERROR(2028, index_id, "Recover file not found: " << recover_file);
            return false;
        }

        // Step 1: Read offset and busy flag
        std::ifstream fin(recover_file);
        std::string line;
        std::getline(fin, line);
        fin.close();

        auto colon = line.find(':');
        if(colon == std::string::npos) {
            LOG_ERROR(2029, index_id, "Invalid recover.txt format");
            return false;
        }

        size_t offset = std::stoull(line.substr(0, colon));
        int flag = std::stoi(line.substr(colon + 1));
        if(flag == 1) {
            LOG_INFO(2030, index_id, "Recovery already in progress");
            return false;
        }

        // Step 2: Mark as busy
        {
            std::ofstream fout(recover_file);
            fout << offset << ":1\n";
        }

        // Step 3: Load entry and acquire operation mutex for thread safety
        auto entry_ptr = getIndexEntry(index_id);
        auto& entry = *entry_ptr;

        // FIX: Use per-index operation mutex to prevent concurrent operations
        std::unique_lock<std::shared_mutex> operation_lock(entry.operation_mutex);

        auto cursor = entry.vector_storage->getCursor();

        // Step 4: Collect next batch
        std::vector<std::pair<ndd::idInt, std::vector<uint8_t>>> batch;
        while(cursor.hasNext() && batch.size() < batch_size) {
            auto [label, vec_bytes] = cursor.next();
            if(label < offset) {
                continue;
            }
            batch.emplace_back(label, std::move(vec_bytes));
        }

        if(batch.empty()) {
            LOG_INFO(2031, index_id, "No more vectors to recover");
            std::ofstream fout(recover_file);
            fout << offset << ":0\n";  // just mark as not busy
            return true;
        }

        // Step 5: Insert in parallel like addVectors()
        size_t num_threads = std::min(settings::NUM_RECOVERY_THREADS, batch.size());
        std::atomic<size_t> next{0};
        std::atomic<size_t> empty_vector_count{0};
        std::vector<std::thread> threads;

        for(size_t t = 0; t < num_threads; ++t) {
            threads.emplace_back([&]() {
                size_t i;
                while((i = next.fetch_add(1)) < batch.size()) {
                    const auto& [label, vec_bytes] = batch[i];
                    if(!vec_bytes.empty()) {
                        entry.alg->addPoint<true>(vec_bytes.data(), label);
                    } else {
                        empty_vector_count.fetch_add(1);
                    }
                }
            });
        }

        for(auto& th : threads) {
            th.join();
        }

        if(empty_vector_count.load() > 0) {
            LOG_WARN(2032,
                           index_id,
                           "Skipped " << empty_vector_count.load() << " vectors during recovery because they were empty");
        }

        LOG_INFO(2033, index_id, "Recovered " << batch.size() << " vectors");

        // Step 6: Save index
        // Mark the index as dirty so that it will be saved
        entry.markDirty();
        // FIX: Use internal save to avoid circular lock
        saveIndexInternal(entry);

        // Step 7: Update recover.txt to next offset
        std::ofstream fout(recover_file);
        fout << (offset + batch.size()) << ":0\n";

        return true;
    }

    std::optional<ndd::HybridVectorObject> getVector(const std::string& index_id,
                                                     const std::string& str_id) {
        try {
            auto entry_ptr = getIndexEntry(index_id);
            auto& entry = *entry_ptr;

            /**
             * XXX: We aren't using reader's lock here to enable reads while
             * writing.
             * TODO: check correctness when stressing the system.
             */
            // std::shared_lock<std::shared_mutex> operation_lock(entry.operation_mutex);

            MDBX_txn* txn = nullptr;
            int rc = mdbx_txn_begin(
                    entry.vector_storage->shared_env(), nullptr, MDBX_TXN_RDONLY, &txn);
            if(rc != MDBX_SUCCESS) {
                throw std::runtime_error("Failed to begin shared getVector transaction: "
                                         + std::string(mdbx_strerror(rc)));
            }

            try {
                ndd::idInt numeric_id = entry.id_mapper->get_id(txn, str_id);
                if(numeric_id == 0) {
                    mdbx_txn_abort(txn);
                    return std::nullopt;
                }

                auto vec_result = entry.vector_storage->get_vector(txn, numeric_id);
                if(!vec_result.ok() || vec_result.value->empty()) {
                    mdbx_txn_abort(txn);
                    if(vec_result.ok() || vec_result.code == 101) {
                        return std::nullopt;
                    }
                    throw std::runtime_error(vec_result.message);
                }
                const auto& vec_bytes = *vec_result.value;
                auto meta_result = entry.vector_storage->get_meta(txn, numeric_id);
                if(!meta_result.ok()) {
                    mdbx_txn_abort(txn);
                    if(meta_result.code == 101) {
                        // Meta missing for an existing id-map row is a recoverable not-found.
                        return std::nullopt;
                    }
                    throw std::runtime_error(meta_result.message);
                }
                const auto& meta = *meta_result.value;

                ndd::HybridVectorObject obj;
                obj.id = meta.id;
                obj.meta = meta.meta;
                obj.filter = meta.filter;
                obj.norm = meta.norm;

                ndd::quant::QuantizationLevel quant_level = entry.alg->getQuantLevel();
                std::vector<float> float_data =
                        ndd::quant::get_quantizer_dispatch(quant_level)
                                .dequantize(vec_bytes.data(), entry.alg->getDimension());
                obj.vector = {float_data.begin(), float_data.end()};

                if(entry.sparse_storage) {
                    auto sparse_vec = entry.sparse_storage->get_vector(txn, numeric_id);
                    if(sparse_vec.has_value()) {
                        obj.sparse_ids = std::move(sparse_vec->indices);
                        obj.sparse_values = std::move(sparse_vec->values);
                    }
                }

                mdbx_txn_abort(txn);
                return obj;
            } catch(...) {
                mdbx_txn_abort(txn);
                throw;
            }
        } catch(const std::exception& e) {
            LOG_ERROR(2034, index_id, "Error retrieving vector: " << e.what());
            return std::nullopt;
        }
    }

    /**
     * Deletes vectors from id mapper, filter indexes, sparse storage, and HNSW live set.
     * XXX: Does not delete meta, vector data Meta and vector data will be overwritten when
     * the id is reused.
     *
     * Return codes:
     * 0 = success
     * 1-99 = propagated filter validation failure from vector storage
     * 100-199 = storage or propagated filter storage failure; caller should return HTTP 500
     * 200-299 = propagated filter corruption/invariant failure; caller should return HTTP 500
     */
    ndd::OperationResult<bool>
    deleteVectorsByIds(CacheEntry& entry, const std::vector<ndd::idInt>& numeric_ids) {
        try {
            if(numeric_ids.empty()) {
                return {SUCCESS, "", true};
            }

            MDBX_txn* txn = nullptr;
            int rc = mdbx_txn_begin(
                    entry.vector_storage->shared_env(), nullptr, MDBX_TXN_READWRITE, &txn);
            if(rc != MDBX_SUCCESS) {
                return {100, "Failed to begin shared delete transaction: "
                                     + std::string(mdbx_strerror(rc))};
            }

            auto abort_txn = [&txn, &entry]() {
                if(txn) {
                    mdbx_txn_abort(txn);
                    txn = nullptr;
                    entry.vector_storage->reload_filter_schema_cache();
                }
            };

            std::vector<ndd::VectorMeta> metas;
            std::vector<std::string> external_ids;
            metas.reserve(numeric_ids.size());
            external_ids.reserve(numeric_ids.size());

            try {
                for(ndd::idInt numeric_id : numeric_ids) {
                    auto meta_result = entry.vector_storage->get_meta(txn, numeric_id);
                    if(!meta_result.ok()) {
                        abort_txn();
                        LOG_ERROR(2058,
                                  entry.index_id,
                                  "Failed to read meta for numeric_id "
                                          << numeric_id << ": " << meta_result.message);
                        return {meta_result.code, meta_result.message};
                    }
                    auto& meta = *meta_result.value;
                    external_ids.push_back(meta.id);
                    metas.push_back(std::move(meta));
                }

                auto stored_ids = entry.id_mapper->deletePoints(txn, external_ids);
                for(size_t i = 0; i < stored_ids.size(); ++i) {
                    if(stored_ids[i] != numeric_ids[i]) {
                        abort_txn();
                        LOG_ERROR(2035,
                                  entry.index_id,
                                  "ID mapping mismatch while deleting "
                                          << external_ids[i] << ": expected "
                                          << numeric_ids[i] << " got " << stored_ids[i]);
                        return {100, "ID mapping mismatch while deleting vectors"};
                    }
                }

                int64_t sparse_vector_count_delta = 0;
                /*
                 * Staged term_info_ mutations across the whole delete batch.
                 * Applied to InvertedIndex::term_info_ only AFTER the shared
                 * MDBX txn commits. Dropped on any abort path so the cache
                 * stays in lockstep with the rolled-back MDBX state.
                 */
                std::vector<ndd::TermInfoChange> sparse_term_info_changes;
                for(size_t i = 0; i < numeric_ids.size(); ++i) {
                    auto filter_result =
                            entry.vector_storage->deletePoint(txn, numeric_ids[i], metas[i]);
                    if(!filter_result.ok()) {
                        abort_txn();
                        if(filter_result.code < 100) {
                            LOG_WARN(1216,
                                     entry.index_id,
                                     "Delete-vector filter removal rejected: "
                                             << filter_result.message);
                        } else {
                            LOG_ERROR(1217,
                                      entry.index_id,
                                      "Delete-vector filter removal failed: "
                                              << filter_result.message);
                        }
                        return {filter_result.code, filter_result.message};
                    }

                    if(entry.sparse_storage) {
                        auto sparse_result = entry.sparse_storage->delete_vector(
                                txn, numeric_ids[i], &sparse_vector_count_delta, true);
                        if(!sparse_result.ok()) {
                            abort_txn();
                            return {sparse_result.code, sparse_result.message};
                        }
                        auto& changes = sparse_result.value_or_throw();
                        sparse_term_info_changes.insert(
                                sparse_term_info_changes.end(),
                                std::make_move_iterator(changes.begin()),
                                std::make_move_iterator(changes.end()));
                    }
                }

                std::vector<WriteAheadLog::WALEntry> wal_entries;
                wal_entries.reserve(numeric_ids.size());
                for(ndd::idInt numeric_id : numeric_ids) {
                    wal_entries.push_back({WALOperationType::VECTOR_DELETE, numeric_id});
                }
                getOrCreateWAL(entry)->log(txn, wal_entries);

                rc = mdbx_txn_commit(txn);
                txn = nullptr;
                if(rc != MDBX_SUCCESS) {
                    return {100, "Failed to commit shared delete transaction: "
                                         + std::string(mdbx_strerror(rc))};
                }

                if(entry.sparse_storage && sparse_vector_count_delta != 0) {
                    entry.sparse_storage->apply_vector_count_delta(sparse_vector_count_delta);
                }

                /*
                 * Apply staged term_info_ mutations after the shared MDBX txn
                 * has committed. See acid_regression_test.cpp for the abort
                 * scenario this guards against.
                 */
                if(entry.sparse_storage && !sparse_term_info_changes.empty()) {
                    entry.sparse_storage->apply_term_info_changes(sparse_term_info_changes);
                }
            } catch(const std::exception& e) {
                abort_txn();
                throw;
            }

            for(ndd::idInt numeric_id : numeric_ids) {
                if(entry.alg->hasLabel(numeric_id) && !entry.alg->isLabelDeleted(numeric_id)) {
                    entry.alg->markDelete(numeric_id);
                }
            }

            // Mark the index as dirty
            entry.markDirty();

            return {SUCCESS, "", true};
        } catch(const std::exception& e) {
            LOG_ERROR(2035, entry.index_id, "Failed to delete vectors: " << e.what());
            return {100, std::string("Failed to delete vectors: ") + e.what()};
        }
    }

    /**
     * Deletes all vectors matching a filter query.
     *
     * Return codes:
     * 0 = success; value is the number of deleted vectors
     * 1-99 = propagated filter validation failure; caller should return HTTP 400
     * 100-199 = storage or propagated filter storage failure; caller should return HTTP 500
     * 200-299 = propagated filter corruption/invariant failure; caller should return HTTP 500
     */
    ndd::OperationResult<size_t>
    deleteVectorsByFilter(const std::string& index_id, const nlohmann::json& filter_array) {
        try {
            auto entry_ptr = getIndexEntry(index_id);
            auto& entry = *entry_ptr;

            // Use per-index operation mutex to prevent concurrent operations
            std::unique_lock<std::shared_mutex> operation_lock(entry.operation_mutex);

            auto numeric_ids_result =
                    entry.vector_storage->filter_store_->getIdsMatchingFilter(filter_array);
            if(!numeric_ids_result.ok()) {
                if(numeric_ids_result.code < 100) {
                    LOG_WARN(1213,
                             index_id,
                             "Delete-by-filter rejected: " << numeric_ids_result.message);
                } else {
                    LOG_ERROR(1214,
                              index_id,
                              "Delete-by-filter failed while computing filter: "
                                      << numeric_ids_result.message);
                }
                return {numeric_ids_result.code, numeric_ids_result.message};
            }

            auto& numeric_ids = numeric_ids_result.value_or_throw();
            LOG_DEBUG("Filter matched " << numeric_ids.size() << " vectors");

            auto delete_result = deleteVectorsByIds(entry, numeric_ids);
            if(delete_result.ok() && delete_result.value_or_throw()) {
                // Check if we need to save based on WAL entry count after logging
                WriteAheadLog* wal = getOrCreateWAL(entry);
                if(wal->getEntryCount() >= persistence_config_.save_every_n_updates) {
                    LOG_DEBUG("Saving index " << index_id << " after " << wal->getEntryCount()
                                              << " updates");
                    saveIndexInternal(entry);
                }
                return {SUCCESS, "", numeric_ids.size()};
            } else {
                if(!delete_result.ok()) {
                    return {delete_result.code, delete_result.message};
                }
                return {SUCCESS, "", static_cast<size_t>(0)};
            }
        } catch(const std::runtime_error& e) {
            // Re-throw runtime_error (includes backup-in-progress check)
            throw;
        } catch(const std::exception& e) {
            LOG_ERROR(2036, index_id, "Failed to delete vectors by filter: " << e.what());
            return {100, std::string("Failed to delete vectors by filter: ") + e.what()};
        }
    }

    /**
     * Replaces filter documents for a batch of vectors.
     *
     * Return codes:
     * 0 = success; value is the number of updated filters
     * 1-99 = propagated filter validation failure; caller should return HTTP 400
     * 100-199 = storage or propagated filter storage failure; caller should return HTTP 500
     * 200-299 = propagated filter corruption/invariant failure; caller should return HTTP 500
     */
    ndd::OperationResult<size_t>
    updateFilters(const std::string& index_id,
                  const std::vector<std::pair<std::string, std::string>>& updates) {
        try {
            auto entry_ptr = getIndexEntry(index_id);
            auto& entry = *entry_ptr;

            std::unique_lock<std::shared_mutex> operation_lock(entry.operation_mutex);

            size_t updated_count = 0;
            MDBX_txn* txn = nullptr;
            int rc = mdbx_txn_begin(
                    entry.vector_storage->shared_env(), nullptr, MDBX_TXN_READWRITE, &txn);
            if(rc != MDBX_SUCCESS) {
                return {100, "Failed to begin shared filter update transaction: "
                                     + std::string(mdbx_strerror(rc))};
            }

            auto abort_txn = [&txn, &entry]() {
                if(txn) {
                    mdbx_txn_abort(txn);
                    txn = nullptr;
                    entry.vector_storage->reload_filter_schema_cache();
                }
            };

            bool filter_schema_changed = false;
            try {
                for(const auto& [str_id, new_filter] : updates) {
                    ndd::idInt numeric_id = entry.id_mapper->get_id(txn, str_id);
                    if(numeric_id == 0) {
                        LOG_DEBUG("updateFilters: ID not found: " << str_id);
                        continue;
                    }

                    auto filter_result =
                            entry.vector_storage->updateFilter(
                                    txn, numeric_id, new_filter, &filter_schema_changed);
                    if(!filter_result.ok()) {
                        abort_txn();
                        if(filter_result.code < 100) {
                            LOG_WARN(1215,
                                     index_id,
                                     "Update-filters rejected: " << filter_result.message);
                        } else {
                            LOG_ERROR(1218,
                                      index_id,
                                      "Update-filters failed: " << filter_result.message);
                        }
                        return {filter_result.code, filter_result.message};
                    }
                    updated_count++;
                }
            } catch(const std::exception& e) {
                abort_txn();
                return {100, std::string("Failed to update filters: ") + e.what()};
            }

            rc = mdbx_txn_commit(txn);
            txn = nullptr;
            if(rc != MDBX_SUCCESS) {
                entry.vector_storage->reload_filter_schema_cache();
                return {100, "Failed to commit shared filter update transaction: "
                                     + std::string(mdbx_strerror(rc))};
            }
            if(filter_schema_changed) {
                auto schema_result = entry.vector_storage->reload_filter_schema_cache();
                if(!schema_result.ok()) {
                    return {schema_result.code, schema_result.message, updated_count};
                }
            }

            if(updated_count > 0) {
                entry.markDirty();
            }

            return {SUCCESS, "", updated_count};
        } catch(const std::runtime_error& e) {
            // Re-throw runtime_error (includes backup-in-progress check)
            throw;
        } catch(const std::exception& e) {
            LOG_ERROR(2037, index_id, "Failed to update filters: " << e.what());
            return {100, std::string("Failed to update filters: ") + e.what()};
        }
    }

    /**
     * Deletes one vector by string id and removes its filter index entries.
     * The meta and filter will be deleted and the vector will be marked as
     * deleted in HNSW. The id will be put in the deleted_ids in id mapper and will be reused for new vectors.
     *
     * Return codes:
     * 0 = success; value is false when the vector id does not exist
     * 1-99 = propagated filter validation failure; caller should return HTTP 400
     * 100-199 = storage or propagated filter storage failure; caller should return HTTP 500
     * 200-299 = propagated filter corruption/invariant failure; caller should return HTTP 500
     */
    ndd::OperationResult<bool> deleteVector(const std::string& index_id,
                                            const std::string& str_id) {
        try {
            auto entry_ptr = getIndexEntry(index_id);
            auto& entry = *entry_ptr;

            // Use per-index operation mutex to prevent concurrent operations
            std::unique_lock<std::shared_mutex> operation_lock(entry.operation_mutex);

            size_t numeric_id = 0;
            {
                MDBX_txn* id_txn = nullptr;
                int rc = mdbx_txn_begin(
                        entry.id_mapper->get_env(), nullptr, MDBX_TXN_RDONLY, &id_txn);
                if(rc != MDBX_SUCCESS) {
                    return {100,
                            std::string("Failed to begin id read txn: ") + mdbx_strerror(rc)};
                }
                numeric_id = entry.id_mapper->get_id(id_txn, str_id);
                mdbx_txn_abort(id_txn);
            }
            if(numeric_id == 0) {
                return {SUCCESS, "", false};
            }
            auto result = deleteVectorsByIds(entry, {static_cast<idInt>(numeric_id)});

            // Check if we need to save based on WAL entry count after logging
            if(result.ok() && result.value_or_throw()) {
                WriteAheadLog* wal = getOrCreateWAL(entry);
                if(wal->getEntryCount() >= persistence_config_.save_every_n_updates) {
                    LOG_DEBUG("Saving index " << index_id << " after " << wal->getEntryCount()
                                              << " updates");
                    saveIndexInternal(entry);
                }
            }

            return result;
        } catch(const std::runtime_error& e) {
            // Re-throw runtime_error (includes backup-in-progress check)
            throw;
        } catch(const std::exception& e) {
            LOG_ERROR(2038, index_id, "Failed to delete vector: " << e.what());
            return {100, std::string("Failed to delete vector: ") + e.what()};
        }
    }

    /**
     * Searches an index with optional filter bitmap computation.
     *
     * Return codes:
     * 0 = success
     * 1-99 = propagated filter validation failure; caller should return HTTP 400
     * 100-199 = search or propagated filter storage failure; caller should return HTTP 500
     * 200-299 = propagated filter corruption/invariant failure; caller should return HTTP 500
     */
    ndd::OperationResult<std::vector<ndd::VectorResult>>
    search(const std::string& index_id,
                const std::vector<float>& query,
                const std::vector<uint32_t>& sparse_indices,
                const std::vector<float>& sparse_values,
                size_t top_k,
                const nlohmann::json& filter_array,
                ndd::FilterParams params = {},
                bool include_vectors = false,
                size_t ef = settings::DEFAULT_EF_SEARCH ,
                float kDenseRrfWeight = settings::DEFAULT_DENSE_RRF_WEIGHT,
                float kRrfRankConstant = settings::DEFAULT_RRF_RANK_CONSTANT)
    {
        const float kSparseRrfWeight = 1.0f - kDenseRrfWeight;
        MDBX_txn* main_txn = nullptr;
        try {
            auto entry_ptr = getIndexEntry(index_id);
            auto& entry = *entry_ptr;

            /**
             * XXX: We aren't using reader's lock here to enable reads while
             * writing.
             * TODO: check correctness when stressing the system.
             */
            // std::shared_lock<std::shared_mutex> operation_lock(entry.operation_mutex);

            entry.searchCount += 1;

            // ===== Sanity checks & parameter normalization =====
            // top_k == 0 means the caller wants no results; skip all retrieval work.
            if(top_k == 0) {
                return {SUCCESS, "", std::vector<ndd::VectorResult>()};
            }
            /**
             * The sparse leg pairs indices[i] with values[i]; a mismatch would read
             * out of bounds at sparse query construction.
             */
            if(sparse_indices.size() != sparse_values.size()) {
                return {1, "sparse_indices and sparse_values must have the same length"};
            }

            /**
             * A dense query whose length does not match the index dimension would
             * quantize into a too-small/large buffer; downstream HNSW distance
             * routines still read `dim` elements from it and produce OOB reads
             * with meaningless scores. Empty dense query stays legal so sparse-only
             * search continues to work.
             */
            const size_t configured_dim_search = entry.alg->getDimension();
            if(!query.empty() && query.size() != configured_dim_search) {
                LOG_WARN(2055,
                         index_id,
                         "Search rejected: dense query dimension "
                                 << query.size() << " does not match index dimension "
                                 << configured_dim_search);
                return {2,
                        "query vector dimension " + std::to_string(query.size())
                                + " does not match index dimension "
                                + std::to_string(configured_dim_search)};
            }

            /**
             * ef == 0 is the sentinel for "use the default"; normalize once here so the
             * dense leg (filtered and unfiltered) sees the same value downstream.
             */
            if(ef == 0) {
                ef = settings::DEFAULT_EF_SEARCH;
            }

            const bool run_dense_search = kDenseRrfWeight > 0.0f && !query.empty();
            const bool run_sparse_search =
                    kSparseRrfWeight > 0.0f && entry.sparse_storage && !sparse_indices.empty();

            /**
             * Zero-weight sources cannot influence the final ranking; if both legs would
             * be skipped there is nothing to do.
             */
            if(!run_dense_search && !run_sparse_search) {
                return {SUCCESS, "", std::vector<ndd::VectorResult>()};
            }
            // ===== End sanity checks =====

            /**
             * For shared-layout indexes, open one MDBX read transaction up front and
             * thread it through filter bitmap, Strategy-A brute force, and result
             * population so every MDBX read in this request observes the same
             * committed snapshot. The async sparse leg cannot reuse this txn - MDBX
             * read transactions are sticky-threaded (see docs/mdbx_shared_env_acid_revamp.md
             * "Durability Flags") - so the sparse lambda opens its own txn on its
             * own thread.
             */
            MDBX_env* shared_env_handle = entry.vector_storage->shared_env();
            {
                int rc = mdbx_txn_begin(
                        shared_env_handle, nullptr, MDBX_TXN_RDONLY, &main_txn);
                if(rc != MDBX_SUCCESS) {
                    LOG_ERROR(2235,
                              index_id,
                              "Failed to begin shared search transaction: "
                                      << mdbx_strerror(rc));
                    return {100,
                            std::string("Failed to begin shared search transaction: ")
                                    + mdbx_strerror(rc)};
                }
            }

            // 0. Compute Filter Bitmap (Shared)
            ndd::RoaringBitmap active_filter_bitmap;
            const ndd::RoaringBitmap* filter_ptr = nullptr;
            if(!filter_array.empty()) {
                auto filter_result = entry.vector_storage->filter_store_->computeFilterBitmap(
                        main_txn, filter_array);
                if(!filter_result.ok()) {
                    mdbx_txn_abort(main_txn);
                    if(filter_result.code < 100) {
                        LOG_WARN(1220, index_id, "Search filter rejected: " << filter_result.message);
                    } else {
                        LOG_ERROR(1221,
                                  index_id,
                                  "Search filter computation failed: " << filter_result.message);
                    }
                    return {filter_result.code, filter_result.message};
                }
                active_filter_bitmap = std::move(filter_result.value_or_throw());
                filter_ptr = &active_filter_bitmap;
            }

            // 1. Sparse Search (Async)
            std::future<std::vector<std::pair<ndd::idInt, float>>> sparse_future;
            if(run_sparse_search) {
                sparse_future = std::async(std::launch::async, [&, filter_ptr, shared_env_handle]() {

                    ndd::SparseVector sparse_query;
                    if(std::is_sorted(sparse_indices.begin(), sparse_indices.end())) {
                        sparse_query.indices = sparse_indices;
                        sparse_query.values = sparse_values;
                    } else {
                        std::vector<std::pair<uint32_t, float>> pairs(sparse_indices.size());
                        for(size_t i = 0; i < sparse_indices.size(); ++i) {
                            pairs[i] = {sparse_indices[i], sparse_values[i]};
                        }
                        std::sort(pairs.begin(), pairs.end());
                        sparse_query.indices.reserve(pairs.size());
                        sparse_query.values.reserve(pairs.size());
                        for(const auto& [idx, val] : pairs) {
                            sparse_query.indices.push_back(idx);
                            sparse_query.values.push_back(val);
                        }
                    }

                    MDBX_txn* sparse_txn = nullptr;
                    int rc = mdbx_txn_begin(
                            shared_env_handle, nullptr, MDBX_TXN_RDONLY, &sparse_txn);
                    if(rc != MDBX_SUCCESS) {
                        LOG_ERROR(2236,
                                  index_id,
                                  "Failed to begin sparse search transaction: "
                                          << mdbx_strerror(rc));
                        return std::vector<std::pair<ndd::idInt, float>>();
                    }

                    std::vector<std::pair<ndd::idInt, float>> sparse_results;
                    try {
                        sparse_results = entry.sparse_storage->search(
                                sparse_txn, sparse_query, top_k, filter_ptr);
                    } catch(...) {
                        mdbx_txn_abort(sparse_txn);
                        throw;
                    }
                    mdbx_txn_abort(sparse_txn);
                    return sparse_results;
                });
            }

            // 2. Dense Search (Main Thread)
            std::vector<std::pair<float, ndd::idInt>> dense_results;
            if(run_dense_search) {
                std::vector<uint8_t> query_bytes =
                        ndd::quant::get_quantizer_dispatch(entry.alg->getQuantLevel())
                                .quantize(query);

                /**
                 * Dense dispatch:
                 *   - No filter, or filter cardinality is large enough that the postfilter cost
                 *     beats prefilter materialization → HNSW searchKnn (functor optional).
                 *   - Filter exists, cardinality in (0, threshold) → prefilter brute force.
                 *   - Filter exists, cardinality == 0 → skip the work since it is not going to
                 *     do any good. No vector can satisfy an empty filter, so dense_results stays
                 *     empty.
                 */
                if(!filter_ptr || filter_ptr->cardinality() >= params.prefilter_threshold) {
                    std::optional<BitMapFilterFunctor> functor;
                    if(filter_ptr) {
                        functor.emplace(*filter_ptr);
                    }
                    dense_results = entry.alg->searchKnn(
                            query_bytes.data(), top_k, ef,
                            functor ? &*functor : nullptr,
                            filter_ptr ? params.boost_percentage : settings::FILTER_BOOST_PERCENTAGE,
                            main_txn);
                } else if(filter_ptr->cardinality() > 0) {
                    // Prefilter brute force on the small filtered subset.
                    const auto& bitmap = *filter_ptr;
                    std::vector<ndd::idInt> valid_ids(bitmap.cardinality());
                    bitmap.toUint32Array(valid_ids.data());

                    auto* space = entry.alg->getSpace();
                    auto distance_func = space->get_dist_func();
                    void* dist_func_param = space->get_dist_func_param();
                    std::priority_queue<std::pair<float, ndd::idInt>> top_results;

                    entry.vector_storage->visit_vectors_by_ids(
                            main_txn, valid_ids,
                            [&](ndd::idInt numeric_id, const void* vector_data) {
                                float distance = distance_func(query_bytes.data(),
                                                               vector_data,
                                                               dist_func_param);
                                if(top_results.size() < top_k) {
                                    top_results.emplace(distance, numeric_id);
                                } else if(distance < top_results.top().first) {
                                    top_results.pop();
                                    top_results.emplace(distance, numeric_id);
                                }
                            });

                    dense_results.resize(top_results.size());
                    for(auto it = dense_results.rbegin(); it != dense_results.rend(); ++it) {
                        *it = top_results.top();
                        top_results.pop();
                    }
                }
            }

            // 3. Get Sparse Results (Join)
            std::vector<std::pair<ndd::idInt, float>> sparse_results;
            if(sparse_future.valid()) {
                sparse_results = sparse_future.get();
            }

            // 4. Combine Results
            std::vector<std::pair<float, ndd::idInt>> final_candidates;

            if(dense_results.empty() && sparse_results.empty()) {
                if(main_txn) {
                    mdbx_txn_abort(main_txn);
                }
                return {SUCCESS, "", std::vector<ndd::VectorResult>()};
            } else if(sparse_results.empty()) {
                // Only dense results
                final_candidates.reserve(dense_results.size());
                for(const auto& p : dense_results) {
                    final_candidates.emplace_back(p.first, p.second);
                }
            } else if(dense_results.empty()) {
                // Only sparse results
                final_candidates.reserve(sparse_results.size());
                for(const auto& p : sparse_results) {
                    final_candidates.emplace_back(p.second, p.first);
                }
            } else {
                // Hybrid results - weighted RRF.
                std::unordered_map<ndd::idInt, float> combined_scores;
                combined_scores.reserve(dense_results.size() + sparse_results.size());

                /**
                 * Reuse the dense and sparse result buffers directly so hybrid fusion does not
                 * build another copied view of the same ranked lists.
                 */
                auto add_weighted_rrf_scores = [&](const auto& ranked_results,
                                                    float weight,
                                                    auto extract_id){
                    if(weight <= 0.0f) {
                        return;
                    }

                    for(size_t i = 0; i < ranked_results.size(); ++i) {
                        const ndd::idInt id = extract_id(ranked_results[i]);
                        combined_scores[id] +=
                                weight / (kRrfRankConstant + static_cast<float>(i) + 1.0f);
                    }
                };

                add_weighted_rrf_scores(
                        dense_results, kDenseRrfWeight, [](const auto& result) { return result.second; });
                add_weighted_rrf_scores(
                        sparse_results, kSparseRrfWeight, [](const auto& result) { return result.first; });

                final_candidates.reserve(combined_scores.size());
                for(const auto& [id, score] : combined_scores) {
                    final_candidates.emplace_back(score, id);
                }

                std::sort(final_candidates.begin(),
                            final_candidates.end(),
                            [](const auto& a, const auto& b) { return a.first > b.first; });
            }

            std::vector<ndd::VectorResult> results;
            results.reserve(std::min(top_k, final_candidates.size()));
            LOG_DEBUG("Search results size: " << final_candidates.size());

            // Loop-invariants for the include_vectors dequantization path.
            const auto quant_level = entry.alg->getQuantLevel();
            const auto quant_dispatch = ndd::quant::get_quantizer_dispatch(quant_level);
            const size_t vec_dim = entry.alg->getDimension();

            /**
             * Postfilter strategy:
             *   Every code path that feeds final_candidates already enforces filter_ptr:
             *     - Filtered HNSW search drops ids via BitMapFilterFunctor (filter.hpp).
             *     - Prefilter brute-force only iterates ids drawn from the bitmap.
             *     - Sparse search drops non-matching ids inside its scoring phase
             *       (inverted_index.cpp).
             *   So on the dense-only path the per-result contains() check is dead and
             *   we skip it. On the hybrid path we keep it as a safety check in case
             *   sparse search ever stops honoring the filter; either way the check now
             *   runs before get_meta() so a (defensive) reject does not pay an MDBX read.
             */
            const bool postfilter_active = filter_ptr != nullptr && run_sparse_search;
            size_t postfilter_drops = 0;
            size_t filtered_count = 0;
            for(const auto& p : final_candidates) {
                if(postfilter_active && !filter_ptr->contains(p.second)) {
                    ++postfilter_drops;
                    continue;
                }

                auto meta_result = entry.vector_storage->get_meta(main_txn, p.second);
                if(!meta_result.ok()) {
                    if(meta_result.code == 101) {
                        // Meta missing for a candidate id: skip that row.
                        continue;
                    }
                    if(main_txn) {
                        mdbx_txn_abort(main_txn);
                        main_txn = nullptr;
                    }
                    LOG_ERROR(2059,
                              entry.index_id,
                              "Populating results: meta read failed for id "
                                      << p.second << ": " << meta_result.message);
                    return {meta_result.code, meta_result.message};
                }
                auto& meta = *meta_result.value;

                ndd::VectorResult result;
                result.id = std::move(meta.id);
                result.filter = std::move(meta.filter);
                result.meta = std::move(meta.meta);
                result.similarity = p.first;
                result.norm = meta.norm;

                if(include_vectors) {
                    auto vec_result = entry.vector_storage->get_vector(main_txn, p.second);
                    if(vec_result.ok() && !vec_result.value->empty()) {
                        result.vector = quant_dispatch.dequantize(
                                vec_result.value->data(), vec_dim);
                    }
                }

                results.push_back(std::move(result));
                filtered_count++;

                // Early exit when we have enough results
                if(filtered_count >= top_k) {
                    break;
                }
            }

            // Ensure we don't return more than top_k results
            if(results.size() > top_k) {
                results.resize(top_k);
            }

            /**
             * A drop here means an upstream filter step failed to honor filter_ptr.
             * Log once per request rather than per-result to respect the hot-loop rule.
             */
            if(postfilter_drops > 0) {
                LOG_WARN(1222,
                         index_id,
                         "Postfilter dropped " << postfilter_drops
                                               << " ids that bypassed upstream filter checks");
            }
            if(main_txn) {
                mdbx_txn_abort(main_txn);
            }
            return {SUCCESS, "", std::move(results)};
        } catch(const std::exception& e) {
            if(main_txn) {
                mdbx_txn_abort(main_txn);
            }
            LOG_ERROR(2039, index_id, "Search failed: " << e.what());
            return {100, std::string("Search failed: ") + e.what()};
        }
    }

    bool deleteIndex(const std::string& index_id) {
        std::unique_lock<std::shared_mutex> write_lock(indices_mutex_);
        // Remove from in-memory structures if loaded
        auto it = indices_.find(index_id);
        if(it != indices_.end()) {
            auto entry = it->second;
            entry->cache_valid = false;
            std::unique_lock<std::shared_mutex> operation_lock(entry->operation_mutex);

            auto indx_it = std::find(indices_list_.begin(), indices_list_.end(), index_id);
            if(indx_it != indices_list_.end()) {
                indices_list_.erase(indx_it);
            }
            indices_.erase(it);
        }

        // Delete metadata
        metadata_manager_->deleteMetadata(index_id);

        // Move to deleted directory instead of removing
        std::string index_dir = data_dir_ + "/" + index_id;
        std::string deleted_dir = data_dir_ + "/deleted";

        try {
            LOG_DEBUG("Deleting index: " << index_dir);
            if(std::filesystem::exists(index_dir)) {
                // Create deleted directory if it doesn't exist
                std::filesystem::create_directories(deleted_dir);

                // Parse username and index_name from index_id (format: username/index_name)
                size_t slash_pos = index_id.find('/');
                std::string username = index_id.substr(0, slash_pos);
                std::string index_name = index_id.substr(slash_pos + 1);

                // Generate backup name with random suffix
                std::string rand_suffix = random_generator::rand_alphanum(4);
                std::string backup_path =
                        deleted_dir + "/" + username + "_" + index_name + "_" + rand_suffix;

                // Move the directory
                std::filesystem::rename(index_dir, backup_path);
                return true;
            }
        } catch(const std::filesystem::filesystem_error& e) {
            LOG_ERROR(
                    2040, index_id, "Failed to move index to deleted directory: " << e.what());
            return false;
        }

        return false;
    }

    /**
     * This function returns the information about a given index.
     * Currently the implementation is as follows:
     * 1. If the index is live (listed in IndexManager.indices_), populate
     * information from there.
     * 2. Else read the metadata and populate the information from there.
     *
     * NOTE: This is a stop-gap solution to make sure that elements_count
     * is never stale. This should be fixed later with metadata overhaul.  
     */
    std::optional<IndexInfo> getIndexInfo(const std::string& index_id) {

        if(auto entry_ptr = findInMemoryIndexEntry(index_id)) {
            /**
             * XXX: We aren't using reader's lock here to enable reads while
             * writing.
             * TODO: check correctness when stressing the system.
             * check other instances of shared_lock on operation_mutex.
             */

            std::shared_lock<std::shared_mutex> operation_lock(entry_ptr->operation_mutex);

            return std::optional<IndexInfo>{std::in_place,
                                            entry_ptr->alg->getElementsCount(),
                                            entry_ptr->alg->getDimension(),
                                            entry_ptr->sparse_model,
                                            entry_ptr->alg->getSpaceTypeStr(),
                                            entry_ptr->alg->getQuantLevel(),
                                            entry_ptr->alg->getChecksum(),
                                            entry_ptr->alg->getM(),
                                            entry_ptr->alg->getEfConstruction()};
        }

        if(std::filesystem::exists(std::filesystem::path(data_dir_) / index_id
                                   / settings::INDEX_MIGRATION_MARKER)) {
            throw std::runtime_error(settings::INCOMPLETE_INDEX_MIGRATION_ERROR);
        }

        auto metadata = metadata_manager_->getMetadata(index_id);
        if(!metadata) {
            return std::nullopt;
        }
        if(metadata->layout_version != settings::INDEX_LAYOUT_VERSION) {
            throw std::runtime_error(settings::indexLayoutError(metadata->layout_version));
        }

        return std::optional<IndexInfo>{std::in_place,
                                        metadata->total_elements,
                                        metadata->dimension,
                                        metadata->sparse_model,
                                        std::move(metadata->space_type_str),
                                        metadata->quant_level,
                                        metadata->checksum,
                                        metadata->M,
                                        metadata->ef_con};
    }

    // ========== Backup operations ==========

    // Orchestration methods (defined below after class)
    std::pair<bool, std::string> createBackupAsync(const std::string& index_id,
                                                    const std::string& backup_name);

    std::pair<bool, std::string> restoreBackup(const std::string& backup_name,
                                                const std::string& target_index_name,
                                                const std::string& username);

    // Forwarding methods (no IndexManager internals needed)
    nlohmann::json listBackups(const std::string& username) {
        return backup_store_.listBackups(username);
    }

    std::pair<bool, std::string> deleteBackup(const std::string& backup_name,
                                               const std::string& username) {
        return backup_store_.deleteBackup(backup_name, username);
    }

    std::optional<std::pair<std::string, std::string>> getActiveBackup(const std::string& username) {
        return backup_store_.getActiveBackup(username);
    }

    nlohmann::json getBackupInfo(const std::string& backup_name,
                                  const std::string& username) {
        return backup_store_.getBackupInfo(backup_name, username);
    }

    std::pair<bool, std::string> validateBackupName(const std::string& backup_name) const {
        return backup_store_.validateBackupName(backup_name);
    }

    std::pair<bool, std::string> uploadBackup(const std::string& backup_name,
                                                const std::string& username,
                                                const std::string& file_content);
};

// ========== IndexManager backup implementations ==========

inline void IndexManager::executeBackupJob(const std::string& index_id, const std::string& backup_name,
                                            std::stop_token st) {
    std::string username;
    size_t upos = index_id.find('/');
    if (upos != std::string::npos) {
        username = index_id.substr(0, upos);
    }

    try {
        std::string index_name;
        if (upos != std::string::npos) {
            index_name = index_id.substr(upos + 1);
        } else {
            throw std::runtime_error("Invalid index ID format");
        }

        std::string user_backup_dir = backup_store_.getUserBackupDir(username);
        std::filesystem::create_directories(user_backup_dir);
        std::string user_temp_dir = backup_store_.getUserTempDir(username);
        std::filesystem::create_directories(user_temp_dir);
        std::string source_dir = data_dir_ + "/" + index_id;
        std::string backup_tar_final = user_backup_dir + "/" + backup_name + ".tar";
        std::string backup_tar_temp = user_temp_dir + "/.tmp_" + backup_name + ".tar";

        if(std::filesystem::exists(backup_tar_final)) {
            throw std::runtime_error("Backup already exists: " + backup_name);
        }

        size_t index_size = 0;
        for(const auto& file : std::filesystem::recursive_directory_iterator(source_dir)) {
            if(!std::filesystem::is_directory(file)) {
                index_size += std::filesystem::file_size(file);
            }
        }

        auto space_info = std::filesystem::space(user_backup_dir);
        if(space_info.available < index_size * 2) {
            throw std::runtime_error("Insufficient disk space: need " +
                std::to_string(index_size * 2 / MB) + " MB");
        }

        auto meta = metadata_manager_->getMetadata(index_id);
        nlohmann::json metadata_json;
        if(meta) {
            metadata_json["original_index"] = index_name;
            metadata_json["timestamp"] = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            metadata_json["size_mb"] = index_size / MB;
            metadata_json["params"] = {{"M", meta->M},
                           {"ef_construction", meta->ef_con},
                           {"dim", meta->dimension},
                           {"sparse_model",
                            ndd::sparseScoringModelToString(meta->sparse_model)},
                           {"space_type", meta->space_type_str},
                           {"quant_level", static_cast<int>(meta->quant_level)},
                           {"layout_version", meta->layout_version},
                           {"total_elements", meta->total_elements},
                           {"checksum", meta->checksum}};
            LOG_DEBUG("Metadata prepared for backup: " << metadata_json.dump());
        } else {
            LOG_ERROR(2041, index_id, "Failed to get metadata for backup");
            throw std::runtime_error("Cannot create backup without index metadata");
        }

        // Check stop_token before expensive operations
        if (st.stop_requested()) {
            LOG_INFO(2056, index_id, "Backup cancelled before backup work started");
            backup_store_.clearActiveBackup(username);
            return;
        }

        const bool legacy_layout = meta->layout_version != settings::INDEX_LAYOUT_VERSION;
        std::shared_ptr<CacheEntry> entry_ptr;
        if(!legacy_layout) {
            entry_ptr = getIndexEntry(index_id);
        } else {
            LOG_INFO(2042,
                     index_id,
                     "Creating raw backup for legacy index layout version "
                             << meta->layout_version);
        }

        std::string metadata_file_in_index = source_dir + "/metadata.json";
        {
            /**
             * NOTE: While making a backup is a reading operation on the index,
             * we are picking a writer's lock here because we have disabled reader's
             * locks on other instances of read in the system right now.
             *
             * This is to enable reads while writes are happening on the index.
             * Check other instances of shared_lock on operation_mutex.
             */
            std::unique_lock<std::shared_mutex> operation_lock;
            if(entry_ptr) {
                operation_lock = std::unique_lock<std::shared_mutex>(entry_ptr->operation_mutex);
            }

            // Check again after acquiring lock (shutdown may have been requested while waiting)
            if (st.stop_requested()) {
                LOG_INFO(2057, index_id, "Backup cancelled");
                backup_store_.clearActiveBackup(username);
                return;
            }

            if(entry_ptr) {
                saveIndexInternal(*entry_ptr);
            }

            if(!metadata_json.empty()) {
                std::ofstream meta_file(metadata_file_in_index, std::ios::binary);
                if(!meta_file) {
                    throw std::runtime_error("Failed to create metadata file: " + metadata_file_in_index);
                }
                meta_file << metadata_json.dump(4);
                meta_file.flush();
                meta_file.close();

                if(!std::filesystem::exists(metadata_file_in_index)) {
                    throw std::runtime_error("Metadata file was not created: " + metadata_file_in_index);
                }
                LOG_DEBUG("Metadata file created: " << metadata_file_in_index << " (size: " << std::filesystem::file_size(metadata_file_in_index) << " bytes)");
            }

            std::string error_msg;
            LOG_DEBUG("Creating tar archive from " << source_dir << " to " << backup_tar_temp);
            if(!backup_store_.createBackupTar(source_dir, backup_tar_temp, error_msg, st)) {
                if(std::filesystem::exists(metadata_file_in_index)) {
                    std::filesystem::remove(metadata_file_in_index);
                }
                throw std::runtime_error("Failed to create tar archive: " + error_msg);
            }

            if(!std::filesystem::exists(backup_tar_temp)) {
                throw std::runtime_error("Tar archive was not created: " + backup_tar_temp);
            }
            LOG_DEBUG("Tar archive created successfully: " << backup_tar_temp << " (size: " << std::filesystem::file_size(backup_tar_temp) << " bytes)");

            if(std::filesystem::exists(metadata_file_in_index)) {
                std::filesystem::remove(metadata_file_in_index);
            }
        }

        backup_store_.clearActiveBackup(username);

        LOG_INFO(2042, index_id, "Backup tar created; write operations resumed");

        std::filesystem::rename(backup_tar_temp, backup_tar_final);

        nlohmann::json backup_db = backup_store_.readBackupJson(username);
        backup_db[backup_name] = metadata_json;
        backup_store_.writeBackupJson(username, backup_db);

        LOG_INFO(2043, index_id, "Backup completed: " << backup_name << " -> " << backup_tar_final);

    } catch (const std::exception& e) {
        std::string user_backup_dir = backup_store_.getUserBackupDir(username);
        std::string user_temp_dir = backup_store_.getUserTempDir(username);
        std::string source_dir = data_dir_ + "/" + index_id;
        std::string backup_tar_final = user_backup_dir + "/" + backup_name + ".tar";
        std::string backup_tar_temp = user_temp_dir + "/.tmp_" + backup_name + ".tar";
        std::string metadata_file_in_index = source_dir + "/metadata.json";

        if(std::filesystem::exists(backup_tar_temp)) {
            std::filesystem::remove(backup_tar_temp);
        }
        if(std::filesystem::exists(backup_tar_final)) {
            std::filesystem::remove(backup_tar_final);
        }
        if(std::filesystem::exists(metadata_file_in_index)) {
            std::filesystem::remove(metadata_file_in_index);
        }

        backup_store_.clearActiveBackup(username);

        LOG_ERROR(2044, index_id, "Backup failed for " << backup_name << ": " << e.what());
    }
}

inline std::pair<bool, std::string> IndexManager::restoreBackup(const std::string& backup_name,
                                                                  const std::string& target_index_name,
                                                                  const std::string& username) {
    std::pair<bool, std::string> result = backup_store_.validateBackupName(backup_name);
    if(!result.first) {
        return result;
    }

    std::string backup_dir_root = backup_store_.getUserBackupDir(username);
    std::string backup_tar = backup_dir_root + "/" + backup_name + ".tar";
    std::string user_temp_dir = backup_store_.getUserTempDir(username);
    std::filesystem::create_directories(user_temp_dir);
    std::string backup_extract_dir = user_temp_dir + "/" + backup_name;
    std::string target_index_id = username + "/" + target_index_name;
    std::string target_dir = data_dir_ + "/" + target_index_id;
    std::string target_temp_dir = user_temp_dir + "/.restore_" + target_index_name;

    if(!std::filesystem::exists(backup_tar)) {
        return {false, "Backup not found: " + backup_name};
    }

    if(metadata_manager_->getMetadata(target_index_id).has_value()) {
        return {false, "Target index already exists"};
    }
    if(std::filesystem::exists(target_dir)) {
        return {false, "Target index directory already exists"};
    }
    std::filesystem::remove_all(target_temp_dir);

    /**
     * Scan-first: read metadata.json directly out of the tar stream and check
     * its layout_version before extracting anything. A v0 backup tar can be
     * tens of GB; the old order (extract -> read metadata -> reject) wrote the
     * whole payload to disk just to throw it away.
     */
    {
        nlohmann::json scanned_meta = backup_store_.readMetadataJsonFromTar(backup_tar);
        const uint32_t scanned_layout_version =
                scanned_meta.is_object() && scanned_meta.contains("params")
                        ? scanned_meta["params"].value("layout_version",
                                                       settings::LEGACY_INDEX_LAYOUT_VERSION)
                        : settings::LEGACY_INDEX_LAYOUT_VERSION;
        if(scanned_layout_version > settings::INDEX_LAYOUT_VERSION) {
            return {false, "Failed to restore backup: " + settings::NEWER_INDEX_LAYOUT_ERROR};
        }
        if(scanned_layout_version != settings::INDEX_LAYOUT_VERSION) {
            return {false, "Failed to restore backup: "
                                   + settings::LEGACY_INDEX_LAYOUT_ERROR
                                   + " Run `ndd-migrate-v0-to-v2 from-backup --backup <tar> --out-dir <dir>` "
                                     "(or `ndd-migrate-v0-to-v2 in-place` against a live index folder) "
                                     "before restoring."};
        }
    }

    std::string error_msg;
    if(!backup_store_.extractBackupTar(backup_tar, backup_extract_dir, error_msg)) {
        return {false, "Failed to extract backup archive: " + error_msg};
    }

    std::vector<std::string> folders;
    for(const auto& entry : std::filesystem::directory_iterator(backup_extract_dir)) {
        if(entry.is_directory()) {
            folders.push_back(entry.path().string());
        }
    }

    if(folders.size() != 1) {
        std::filesystem::remove_all(backup_extract_dir);
        return {false, "Backup extraction failed - directory not found"};
    }

    std::string backup_dir = folders[0];

    try {
        std::ifstream f(backup_dir + "/metadata.json");
        if(!f.good()) {
            std::filesystem::remove_all(backup_extract_dir);
            return {false, "Backup metadata missing"};
        }
        nlohmann::json meta_json = nlohmann::json::parse(f);

        const uint32_t backup_layout_version =
                meta_json["params"].value("layout_version",
                                          settings::LEGACY_INDEX_LAYOUT_VERSION);
        if(backup_layout_version > settings::INDEX_LAYOUT_VERSION) {
            throw std::runtime_error(settings::NEWER_INDEX_LAYOUT_ERROR);
        }
        if(backup_layout_version != settings::INDEX_LAYOUT_VERSION) {
            throw std::runtime_error(
                    settings::LEGACY_INDEX_LAYOUT_ERROR
                    + " Run `ndd-migrate-v0-to-v2 from-backup --backup <tar> --out-dir <dir>` "
                      "(or `ndd-migrate-v0-to-v2 in-place` against a live index folder) "
                      "before restoring.");
        }
        std::filesystem::create_directories(target_temp_dir);
        for(const auto& entry : std::filesystem::directory_iterator(backup_dir)) {
            std::filesystem::copy(entry.path(),
                                  std::filesystem::path(target_temp_dir) / entry.path().filename(),
                                  std::filesystem::copy_options::recursive
                                          | std::filesystem::copy_options::overwrite_existing);
        }
        std::error_code copy_ec;
        std::filesystem::remove(std::filesystem::path(target_temp_dir) / "metadata.json", copy_ec);
        {
            ndd::storage::SharedIndexEnv restored_env(target_temp_dir + "/vectors");
            const uint32_t on_disk_version =
                    ndd::storage::SharedIndexEnv::read_layout_version(restored_env.get());
            if(on_disk_version != settings::INDEX_LAYOUT_VERSION) {
                throw std::runtime_error(settings::indexLayoutError(on_disk_version));
            }
        }

        IndexMetadata new_meta;
        new_meta.name = target_index_name;
        new_meta.dimension = meta_json["params"]["dim"];
        new_meta.M = meta_json["params"]["M"];
        new_meta.ef_con = meta_json["params"]["ef_construction"];
        new_meta.space_type_str = meta_json["params"]["space_type"];
        new_meta.quant_level = static_cast<ndd::quant::QuantizationLevel>(
                meta_json["params"]["quant_level"].get<int>());
        const auto sparse_model = ndd::sparseScoringModelFromString(
                meta_json["params"]["sparse_model"].get<std::string>());
        if(!sparse_model.has_value()) {
            throw std::runtime_error("Backup metadata has invalid sparse_model");
        }
        new_meta.sparse_model = *sparse_model;
        new_meta.created_at = std::chrono::system_clock::now();
        new_meta.total_elements = meta_json["params"].value("total_elements", 0ul);
        new_meta.checksum = meta_json["params"].value("checksum", -1);
        new_meta.layout_version = settings::INDEX_LAYOUT_VERSION;

        std::filesystem::create_directories(std::filesystem::path(target_dir).parent_path());
        std::filesystem::rename(target_temp_dir, target_dir);

        if(!metadata_manager_->storeMetadata(target_index_id, new_meta)) {
            std::filesystem::remove_all(target_dir);
            throw std::runtime_error("Failed to store restored index metadata");
        }

        std::filesystem::remove_all(backup_extract_dir);

        {
            std::unique_lock<std::shared_mutex> write_lock(indices_mutex_);
            loadIndex(target_index_id);
        }

        LOG_INFO(2045, username, target_index_name, "Restored backup from " << backup_tar);
        return {true, ""};
    } catch(const std::exception& e) {
        metadata_manager_->deleteMetadata(target_index_id);
        std::filesystem::remove_all(target_temp_dir);
        std::filesystem::remove_all(target_dir);
        std::filesystem::remove_all(backup_extract_dir);
        return {false, "Failed to restore backup: " + std::string(e.what())};
    }
}

inline std::pair<bool, std::string> IndexManager::createBackupAsync(const std::string& index_id,
                                                                      const std::string& backup_name) {
    std::pair<bool, std::string> result = backup_store_.validateBackupName(backup_name);
    if(!result.first) {
        return result;
    }

    std::string username;
    size_t pos = index_id.find('/');
    if (pos != std::string::npos) {
        username = index_id.substr(0, pos);
    } else {
        return {false, "Invalid index ID format"};
    }

    if (backup_store_.hasActiveBackup(username)) {
        return {false, "Backup already in progress for user: " + username};
    }

    std::string user_backup_dir = backup_store_.getUserBackupDir(username);
    std::filesystem::create_directories(user_backup_dir);
    std::string backup_tar = user_backup_dir + "/" + backup_name + ".tar";
    if (std::filesystem::exists(backup_tar)) {
        return {false, "Backup already exists: " + backup_name};
    }

    std::jthread t([this, index_id, backup_name](std::stop_token st) {
        executeBackupJob(index_id, backup_name, st);
    });
    backup_store_.setActiveBackup(username, index_id, backup_name, std::move(t));

    LOG_INFO(2046, index_id, "Backup started: " << backup_name);

    return {true, backup_name};
}

inline std::pair<bool, std::string> IndexManager::uploadBackup(const std::string& backup_name, const std::string& username, const std::string& file_content) {
    std::string user_backup_dir = backup_store_.getUserBackupDir(username);
    std::filesystem::create_directories(user_backup_dir);
    std::string backup_path = user_backup_dir + "/" + backup_name + ".tar";
    if(std::filesystem::exists(backup_path)) {
        LOG_WARN(1063, username, "Backup upload conflicts with existing backup " << backup_name);
        
        return {false, "Backup with name '" + backup_name +"' already exits"};
    }

    // Write the file
    std::ofstream out(backup_path, std::ios::binary);
    if(!out.is_open()) {
        return {false, "Failed to create backup file"};
    }

    out.write(file_content.data(), file_content.size());
    out.close();

    if(!out.good()) {
        // Clean up partial file on error
        std::filesystem::remove(backup_path);
        return {false, "Failed to write backup file"};
    }

    nlohmann::json backup_json = backup_store_.readMetadataJsonFromTar(backup_path);

    nlohmann::json backup_db = backup_store_.readBackupJson(username);
    backup_db[backup_name] = backup_json;
    backup_store_.writeBackupJson(username, backup_db);

    return {true, "Backup uploaded successfully"};
}
