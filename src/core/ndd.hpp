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
    std::chrono::system_clock::time_point last_access;
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
     * because in-flight readers still hold shared_ptr references to it. In that
     * window, weak_ptr.lock() can still succeed. cache_valid prevents new lookups
     * from reusing that stale entry while allowing existing users of the entry to
     * finish safely.
     *
     * NOTE: This need not be an atomic bool because its a huge performance penalty
     * and negligible correctness improvement.
     */
    bool cache_valid{true};

    /**
     * Number of searches performed on this index. For a search with k=10
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
     * readers: searchKNN, getVector, getIndexInfo (loaded-index path only)
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
    void resetSearchCount() { searchCount = 0; }
    // Delete copy constructor and assignment
    CacheEntry(const CacheEntry&) = delete;
    CacheEntry& operator=(const CacheEntry&) = delete;

    // Disable move operations since mutex is not movable
    CacheEntry(CacheEntry&&) = delete;
    CacheEntry& operator=(CacheEntry&&) = delete;
};

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

    std::unique_ptr<WriteAheadLog> createWAL(const std::string& index_id) {
        const std::string wal_dir = data_dir_ + "/" + index_id;
        return std::make_unique<WriteAheadLog>(wal_dir, index_id);
    }

    WriteAheadLog* getOrCreateWAL(CacheEntry& entry) {
        if(!entry.wal) {
            entry.wal = createWAL(entry.index_id);
        }
        return entry.wal.get();
    }

    void clearWAL(CacheEntry& entry) {
        getOrCreateWAL(entry)->clear();
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
                        auto vector_bytes = entry.vector_storage->get_vector(wal_entry.numeric_id);
                        if(!vector_bytes.empty()) {
                            entry.alg->addPoint<true>(vector_bytes.data(), wal_entry.numeric_id);
                        } else {
                            // Vector doesn't exist - this VECTOR_ADD failed
                            failed_vector_add_ids.push_back(wal_entry.numeric_id);
                            LOG_DEBUG("VECTOR_ADD failed for ID " << wal_entry.numeric_id
                                                                  << " - adding to deleted_ids");
                        }
                    } else if(wal_entry.op_type == WALOperationType::VECTOR_UPDATE) {
                        // Recover vector update
                        auto vector_bytes = entry.vector_storage->get_vector(wal_entry.numeric_id);
                        if(!vector_bytes.empty()) {
                            entry.alg->addPoint<false>(vector_bytes.data(), wal_entry.numeric_id);
                        }
                    } else if(wal_entry.op_type == WALOperationType::VECTOR_DELETE) {
                        // For deletions, just mark the vector as deleted
                        entry.alg->markDelete(wal_entry.numeric_id);
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
        std::string lmdb_dir = index_dir + "/ids";

        //create the directory and initialize sequence for IDMapper
        LOG_INFO(2021,
                       index_id,
                       "Creating ID mapper with user type " << userTypeToString(user_type));

        // IDMapper now uses tier-based fixed bloom filter sizing based on user_type
        auto id_mapper = std::make_shared<IDMapper>(lmdb_dir, true, user_type);


        // Create HNSW directly with all necessary parameters
        ndd::quant::QuantizationLevel quant_level = config.quant_level;
        auto vector_storage =
                std::make_shared<VectorStorage>(index_dir, index_id, config.dim, config.quant_level);

        // Initialize Sparse Storage if needed
        std::unique_ptr<ndd::SparseVectorStorage> sparse_storage = nullptr;
        if(ndd::sparseModelEnabled(config.sparse_model)) {
            std::string sparse_storage_dir = index_dir + "/sparse";
            sparse_storage = std::make_unique<ndd::SparseVectorStorage>(
                sparse_storage_dir, index_id, config.sparse_model);
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

        alg->setVectorFetcher([vs = vector_storage](ndd::idInt label, uint8_t* buffer) {
            return vs->get_vector(label, buffer);
        });

        alg->setVectorFetcherBatch([vs = vector_storage](const ndd::idInt* labels, uint8_t* buffers, bool* success, size_t count) -> size_t {
            return vs->get_vectors_batch_into(labels, buffers, success, count);
        });

        auto wal = createWAL(index_id);

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
        std::string lmdb_dir = index_dir + "/ids";
        std::string vector_storage_dir = index_dir + "/vectors";
        std::string index_path = vector_storage_dir + "/" + settings::DEFAULT_SUBINDEX + ".idx";

        if(!std::filesystem::exists(index_path) || !std::filesystem::exists(lmdb_dir)
           || !std::filesystem::exists(vector_storage_dir)) {
            throw std::runtime_error("Required files missing for index: " + index_id);
        }

        // Load metadata to get sparse_model
        auto metadata = metadata_manager_->getMetadata(index_id);
        if(!metadata) {
            throw std::runtime_error("Missing or incompatible index metadata for index: "
                                     + index_id);
        }
        const ndd::SparseScoringModel sparse_model = metadata->sparse_model;

        // Step 1: Load HNSW index (automatically adjusts cache based on element count and cache
        // percentage)
        std::unique_ptr<hnswlib::HierarchicalNSW<float>> alg;
        try {
            alg = std::make_unique<hnswlib::HierarchicalNSW<float>>(index_path, 0);

        } catch(const std::exception& e) {
            throw std::runtime_error("Cannot load index '" + index_id + "': " + e.what());
        }

        // Step 2: Create IDMapper and VectorStorage - IDMapper handles bloom filter initialization
        auto id_mapper = std::make_shared<IDMapper>(lmdb_dir, false);
        auto vector_storage = std::make_shared<VectorStorage>(
                index_dir, index_id, alg->getDimension(), alg->getQuantLevel());

        // Initialize Sparse Storage if sparse_model is enabled
        std::unique_ptr<ndd::SparseVectorStorage> sparse_storage;
        if(ndd::sparseModelEnabled(sparse_model)) {
            std::string sparse_storage_dir = index_dir + "/sparse";
            sparse_storage = std::make_unique<ndd::SparseVectorStorage>(
                sparse_storage_dir, index_id, sparse_model);
            if(!sparse_storage->initialize()) {
                throw std::runtime_error("Failed to initialize sparse storage for index: "
                                         + index_id);
            }
        }

        // Set up vector fetcher
        alg->setVectorFetcher([vs = vector_storage](ndd::idInt label, uint8_t* buffer) {
            return vs->get_vector(label, buffer);
        });

        alg->setVectorFetcherBatch([vs = vector_storage](const ndd::idInt* labels, uint8_t* buffers, bool* success, size_t count) -> size_t {
            return vs->get_vectors_batch_into(labels, buffers, success, count);
        });

        auto wal = createWAL(index_id);

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
        new_alg->setVectorFetcher([vs = entry->vector_storage](ndd::idInt label, uint8_t* buffer) {
            return vs->get_vector(label, buffer);
        });

        new_alg->setVectorFetcherBatch([vs = entry->vector_storage](const ndd::idInt* labels, uint8_t* buffers, bool* success, size_t count) -> size_t {
            return vs->get_vectors_batch_into(labels, buffers, success, count);
        });

        // Replace the algorithm in the existing entry
        entry->alg = std::move(new_alg);
    }

    template <typename VectorType>
    bool addVectors(const std::string& index_id, const std::vector<VectorType>& vectors) {
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
                return false;
            }

            // CRITICAL FIX: Pass WAL to create_ids_batch for atomic logging
            WriteAheadLog* wal = getOrCreateWAL(entry);

            std::vector<std::string> str_ids;
            str_ids.reserve(vectors.size());
            for(const auto& vec : vectors) {
                str_ids.push_back(vec.id);
            }
            LOG_DEBUG("Extracted " << str_ids.size() << " string IDs from vectors");
            std::vector<std::pair<idInt, bool>> numeric_ids;
            // Get or create numeric IDs in batch - this returns ids.
            // If str_id already exists, it will return the old numeric ID
            if(entry.alg->getDeletedCount() > 0) {
                // There are deleted IDs, we need to reuse them
                numeric_ids = entry.id_mapper->create_ids_batch<true>(str_ids, wal);
            } else {
                // No deleted IDs, just create new ones
                numeric_ids = entry.id_mapper->create_ids_batch<false>(str_ids, wal);
            }
            LOG_DEBUG("Created " << numeric_ids.size() << " numeric IDs for string IDs");

            // Handle Sparse Vectors if storage is initialized
            if(entry.sparse_storage) {
                if constexpr(std::is_same_v<VectorType, ndd::HybridVectorObject>) {
                    /**
                     * Forward every hybrid doc, including empty sparse payloads, so the sparse
                     * storage layer can treat upserts as replacements and clear old sparse state.
                     */
                    std::vector<std::pair<ndd::idInt, ndd::SparseVector>> sparse_batch;
                    sparse_batch.reserve(vectors.size());

                    for(size_t i = 0; i < vectors.size(); ++i) {
                        const auto& vec = vectors[i];
                        ndd::SparseVector sparse_vec;
                        if(!vec.sparse_ids.empty()) {
                            // Sort indices and values together so replacement writes preserve the
                            // inverted index ordering invariants.
                            std::vector<std::pair<uint32_t, float>> pairs;
                            pairs.reserve(vec.sparse_ids.size());
                            for(size_t k = 0; k < vec.sparse_ids.size(); ++k) {
                                pairs.emplace_back(vec.sparse_ids[k], vec.sparse_values[k]);
                            }
                            std::sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) {
                                return a.first < b.first;
                            });

                            sparse_vec.indices.reserve(pairs.size());
                            sparse_vec.values.reserve(pairs.size());
                            for(const auto& p : pairs) {
                                sparse_vec.indices.push_back(p.first);
                                sparse_vec.values.push_back(p.second);
                            }
                        }

                        sparse_batch.emplace_back(numeric_ids[i].first, std::move(sparse_vec));
                    }

                    if(!sparse_batch.empty()) {
                        if(!entry.sparse_storage->store_vectors_batch(sparse_batch)) {
                            LOG_ERROR(2053,
                                      index_id,
                                      "Failed to update sparse storage for batch size "
                                              << sparse_batch.size());
                            return false;
                        }
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

            // Create a mutable copy for move operations
            std::vector<VectorType> mutable_vectors = vectors;

            for(auto& vec_obj : mutable_vectors) {
                // Use efficient move constructor with internal quantization
                quantized_vectors.emplace_back(std::move(vec_obj), quant_level, dist_params);
            }
            LOG_DEBUG("QuantVectorObject conversion completed with move semantics");

            // Store quantized vectors using optimized batch function (no double conversion!)
            std::vector<std::pair<idInt, QuantVectorObject>> storage_vectors;
            storage_vectors.reserve(quantized_vectors.size());
            for(size_t i = 0; i < quantized_vectors.size(); i++) {
                // Copy QuantVectorObject for storage (we need to keep original for HNSW)
                storage_vectors.emplace_back(numeric_ids[i].first, quantized_vectors[i]);
            }
            entry.vector_storage->store_vectors_batch(storage_vectors);
            LOG_DEBUG("Stored " << storage_vectors.size()
                                << " pre-quantized vectors in vector storage");

            // Add to write ahead log using IndexManager's method
            logInsertsAndUpdates(entry, numeric_ids);

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
            return true;
        } catch(const std::runtime_error& e) {
            // Re-throw runtime_error (includes backup-in-progress check)
            // so it can be caught by API layer and returned as proper JSON error
            throw;
        } catch(const std::exception& e) {
            LOG_ERROR(2027, index_id, "Batch insertion failed: " << e.what());
            throw std::runtime_error(std::string("Batch insertion failed: ") + e.what());
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

            ndd::idInt numeric_id = entry.id_mapper->get_id(str_id);
            if(numeric_id == 0) {
                return std::nullopt;
            }

            std::vector<uint8_t> vec_bytes = entry.vector_storage->get_vector(numeric_id);
            ndd::VectorMeta meta = entry.vector_storage->get_meta(numeric_id);

            ndd::HybridVectorObject obj;
            obj.id = meta.id;
            obj.meta = meta.meta;
            obj.filter = meta.filter;
            obj.norm = meta.norm;

            // Convert raw bytes to float vector using unified dequantization function
            ndd::quant::QuantizationLevel quant_level = entry.alg->getQuantLevel();
            std::vector<float> float_data =
                    ndd::quant::get_quantizer_dispatch(quant_level)
                            .dequantize(vec_bytes.data(), entry.alg->getDimension());

            // Add the float data to the msgpack
            obj.vector = {float_data.begin(), float_data.end()};

            if(entry.sparse_storage) {
                auto sparse_txn = entry.sparse_storage->begin_transaction(true);
                auto sparse_vec = sparse_txn->get_vector(numeric_id);
                if(sparse_vec.has_value()) {
                    obj.sparse_ids = std::move(sparse_vec->indices);
                    obj.sparse_values = std::move(sparse_vec->values);
                }
            }

            return obj;
        } catch(const std::exception& e) {
            LOG_ERROR(2034, index_id, "Error retrieving vector: " << e.what());
            return std::nullopt;
        }
    }

    // Delete vectors from id mapper, delete filter and mark as deleted in HNSW. Does not delete
    // meta, vector data Meta and vector data will be overwritten when the id is reused
    bool deleteVectorsByIds(CacheEntry& entry, const std::vector<ndd::idInt>& numeric_ids) {
        try {
            for(ndd::idInt numeric_id : numeric_ids) {
                auto meta = entry.vector_storage->get_meta(numeric_id);
                // Remove ID mapping by getting the string id from metadata
                auto stored_ids = entry.id_mapper->deletePoints({meta.id});
                if(stored_ids[0] != numeric_id) {
                    LOG_DEBUG("Error: Mismatch in stored ID and numeric ID "
                              << stored_ids[0] << " != " << numeric_id);
                    continue;
                }
                // Remove the filter
                entry.vector_storage->deleteFilter(numeric_id, meta.filter);
                // Mark as deleted in HNSW index

                entry.alg->markDelete(numeric_id);

                // Delete from sparse storage if hybrid index
                if(entry.sparse_storage) {
                    entry.sparse_storage->delete_vector(numeric_id);
                }
            }
            // Add the list to write ahead log using IndexManager's method
            logDeletions(entry, numeric_ids);

            // Mark the index as dirty
            entry.markDirty();

            return true;
        } catch(const std::exception& e) {
            LOG_ERROR(2035, entry.index_id, "Failed to delete vectors: " << e.what());
            return false;
        }
    }

    size_t deleteVectorsByFilter(const std::string& index_id, const nlohmann::json& filter_array) {
        try {
            auto entry_ptr = getIndexEntry(index_id);
            auto& entry = *entry_ptr;

            // Use per-index operation mutex to prevent concurrent operations
            std::unique_lock<std::shared_mutex> operation_lock(entry.operation_mutex);

            auto numeric_ids =
                    entry.vector_storage->filter_store_->getIdsMatchingFilter(filter_array);
            LOG_DEBUG("Filter matched " << numeric_ids.size() << " vectors");

            if(deleteVectorsByIds(entry, numeric_ids)) {
                // Check if we need to save based on WAL entry count after logging
                WriteAheadLog* wal = getOrCreateWAL(entry);
                if(wal->getEntryCount() >= persistence_config_.save_every_n_updates) {
                    LOG_DEBUG("Saving index " << index_id << " after " << wal->getEntryCount()
                                              << " updates");
                    saveIndexInternal(entry);
                }
                return numeric_ids.size();
            } else {
                return 0;
            }
        } catch(const std::runtime_error& e) {
            // Re-throw runtime_error (includes backup-in-progress check)
            throw;
        } catch(const std::exception& e) {
            LOG_ERROR(2036, index_id, "Failed to delete vectors by filter: " << e.what());
            return 0;
        }
    }

    // Update filters for a batch of vectors
    size_t updateFilters(const std::string& index_id,
                         const std::vector<std::pair<std::string, std::string>>& updates) {
        try {
            auto entry_ptr = getIndexEntry(index_id);
            auto& entry = *entry_ptr;

            std::unique_lock<std::shared_mutex> operation_lock(entry.operation_mutex);

            size_t updated_count = 0;
            for(const auto& [str_id, new_filter] : updates) {
                ndd::idInt numeric_id = entry.id_mapper->get_id(str_id);
                if(numeric_id == 0) {
                    LOG_DEBUG("updateFilters: ID not found: " << str_id);
                    continue;
                }

                entry.vector_storage->updateFilter(numeric_id, new_filter);
                updated_count++;
            }

            if(updated_count > 0) {
                entry.markDirty();
            }

            return updated_count;
        } catch(const std::runtime_error& e) {
            // Re-throw runtime_error (includes backup-in-progress check)
            throw;
        } catch(const std::exception& e) {
            LOG_ERROR(2037, index_id, "Failed to update filters: " << e.what());
            return 0;
        }
    }

    // Delete a single vector by string ID - vector data will not be deleted. The meta and filter
    // will be deleted and the vector will be marked as deleted in HNSW. The id will be put in the
    // deleted_ids in id mapper and will be reused for new vectors
    bool deleteVector(const std::string& index_id, const std::string& str_id) {
        try {
            auto entry_ptr = getIndexEntry(index_id);
            auto& entry = *entry_ptr;

            // Use per-index operation mutex to prevent concurrent operations
            std::unique_lock<std::shared_mutex> operation_lock(entry.operation_mutex);

            size_t numeric_id = entry.id_mapper->get_id(str_id);
            if(numeric_id == 0) {
                return false;
            }
            bool result = deleteVectorsByIds(entry, {static_cast<idInt>(numeric_id)});

            // Check if we need to save based on WAL entry count after logging
            if(result) {
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
            return false;
        }
    }

    std::optional<std::vector<ndd::VectorResult>> searchKNN(const std::string& index_id,
                                                            const std::vector<float>& query,
                                                            size_t k,
                                                            const nlohmann::json& filter_array,
                                                            ndd::FilterParams params = {},
                                                            bool include_vectors = false,
                                                            size_t ef = 0) {
        return searchKNN(index_id, query, {}, {}, k, filter_array, params, include_vectors, ef);
    }

    std::optional<std::vector<ndd::VectorResult>>
    searchKNN(const std::string& index_id,
                const std::vector<float>& query,
                const std::vector<uint32_t>& sparse_indices,
                const std::vector<float>& sparse_values,
                size_t k,
                const nlohmann::json& filter_array,
                ndd::FilterParams params = {},
                bool include_vectors = false,
                size_t ef = settings::DEFAULT_EF_SEARCH ,
                float kDenseRrfWeight = settings::DEFAULT_DENSE_RRF_WEIGHT,
                float kRrfRankConstant = settings::DEFAULT_RRF_RANK_CONSTANT)
    {
        const float kSparseRrfWeight = 1.0f - kDenseRrfWeight;
        try {
            auto entry_ptr = getIndexEntry(index_id);
            auto& entry = *entry_ptr;

            /**
             * XXX: We aren't using reader's lock here to enable reads while
             * writing.
             * TODO: check correctness when stressing the system.
             */
            // std::shared_lock<std::shared_mutex> operation_lock(entry.operation_mutex);

            entry.searchCount += k;
            const bool run_dense_search = kDenseRrfWeight > 0.0f && !query.empty();

            const bool run_sparse_search =
                    kSparseRrfWeight > 0.0f && entry.sparse_storage && !sparse_indices.empty();

            // Zero-weight sources cannot influence the final ranking, so skip their retrieval
            // work entirely.
            if(!run_dense_search && !run_sparse_search) {
                return std::vector<ndd::VectorResult>();
            }

            // 0. Compute Filter Bitmap (Shared)
            std::optional<ndd::RoaringBitmap> active_filter_bitmap;
            if (!filter_array.empty()) {
                active_filter_bitmap = entry.vector_storage->filter_store_->computeFilterBitmap(filter_array);
            }
            const ndd::RoaringBitmap* filter_ptr =
                    active_filter_bitmap ? &(*active_filter_bitmap) : nullptr;

            // 1. Sparse Search (Async)
            std::future<std::vector<std::pair<ndd::idInt, float>>> sparse_future;
            if(run_sparse_search) {
                sparse_future = std::async(std::launch::async, [&, filter_ptr]() {
                    ndd::SparseVector sparse_query;

                    // Reuse the caller's ordering when it is already sorted so we do not copy
                    // the same sparse payload into an extra temporary representation.
                    if(std::is_sorted(sparse_indices.begin(), sparse_indices.end())) {
                        sparse_query.indices = sparse_indices;
                        sparse_query.values = sparse_values;
                    } else {
                        std::vector<std::pair<uint32_t, float>> pairs;
                        pairs.reserve(sparse_indices.size());
                        for(size_t i = 0; i < sparse_indices.size(); ++i) {
                            pairs.emplace_back(sparse_indices[i], sparse_values[i]);
                        }
                        std::sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) {
                            return a.first < b.first;
                        });

                        sparse_query.indices.resize(pairs.size());
                        sparse_query.values.resize(pairs.size());
                        for(size_t i = 0; i < pairs.size(); ++i) {
                            sparse_query.indices[i] = pairs[i].first;
                            sparse_query.values[i] = pairs[i].second;
                        }
                    }

                    return entry.sparse_storage->search(sparse_query, k, filter_ptr);
                });
            }

            // 2. Dense Search (Main Thread)
            std::vector<std::pair<float, ndd::idInt>> dense_results;
            if(run_dense_search) {
                // Convert query to bytes using the wrapper method
                ndd::quant::QuantizationLevel quant_level = entry.alg->getQuantLevel();
                auto space = entry.alg->getSpace();
                std::vector<uint8_t> query_bytes =
                        ndd::quant::get_quantizer_dispatch(quant_level).quantize(query);

                if(!filter_ptr) {
                    dense_results = entry.alg->searchKnn(query_bytes.data(), k, ef);
                } else {
                    // Smart Filter Execution Strategy
                    const auto& bitmap = *filter_ptr;
                    size_t card = bitmap.cardinality();

                    if (card == 0) {
                        // No results match filter
                    } else if (card < params.prefilter_threshold) {
                         // Strategy A: Brute Force on Small Subset
                        std::vector<ndd::idInt> valid_ids;
                        valid_ids.reserve(card);
                        bitmap.iterate([](ndd::idInt id, void* ptr){
                        static_cast<std::vector<ndd::idInt>*>(ptr)->push_back(id);
                        return true;
                        }, &valid_ids);

                         // Fetch vectors
                        auto vector_batch = entry.vector_storage->get_vectors_batch(valid_ids);

                        // Prepare subset for bruteforce search
                        std::vector<std::pair<idInt, std::vector<uint8_t>>> vector_subset;
                        vector_subset.reserve(vector_batch.size());
                        for(auto& [nid, vbytes] : vector_batch) {
                            vector_subset.emplace_back(nid, std::move(vbytes));
                        }

                        dense_results = hnswlib::searchKnnSubset<float>(
                            query_bytes.data(), vector_subset, k, space);

                    } else {
                        // Strategy B: Filtered HNSW Search
                        BitMapFilterFunctor functor(bitmap);
                        size_t effective_ef = ef > 0 ? ef : settings::DEFAULT_EF_SEARCH;

                        // Try to use optimized templated search if algorithm matches
                        auto* hnsw_alg = dynamic_cast<hnswlib::HierarchicalNSW<float>*>(entry.alg.get());
                        if (hnsw_alg) {
                            dense_results = hnsw_alg->searchKnn(query_bytes.data(),
                                                                k,
                                                                effective_ef,
                                                                &functor,
                                                                params.boost_percentage);
                        } else {
                            dense_results = entry.alg->searchKnn(query_bytes.data(),
                                                                    k,
                                                                    effective_ef,
                                                                    &functor,
                                                                    params.boost_percentage);
                        }
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
                return std::vector<ndd::VectorResult>();
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

                // Reuse the dense and sparse result buffers directly so hybrid fusion does not
                // build another copied view of the same ranked lists.
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
            results.reserve(final_candidates.size());
            LOG_DEBUG("Search results size: " << final_candidates.size());

            // Process and filter results
            size_t filtered_count = 0;
            for(const auto& p : final_candidates) {
                // Get metadata
                ndd::VectorMeta meta = entry.vector_storage->get_meta(p.second);

                // Apply filter
                if(filter_ptr && !filter_ptr->contains(p.second)) {
                    continue;
                }

                ndd::VectorResult result;
                result.id = meta.id;
                result.filter = meta.filter;
                result.meta = meta.meta;
                result.similarity = p.first;

                result.norm = meta.norm;

                if(include_vectors) {
                    std::vector<uint8_t> vec_bytes = entry.vector_storage->get_vector(p.second);
                    if(!vec_bytes.empty()) {
                        ndd::quant::QuantizationLevel quant_level = entry.alg->getQuantLevel();
                        std::vector<float> float_data =
                                ndd::quant::get_quantizer_dispatch(quant_level)
                                        .dequantize(vec_bytes.data(), entry.alg->getDimension());
                        result.vector = std::move(float_data);
                    }
                }

                results.push_back(std::move(result));
                filtered_count++;

                // Early exit when we have enough results
                if(filtered_count >= k) {
                    break;
                }
            }

            // Ensure we don't return more than k results
            if(results.size() > k) {
                results.resize(k);
            }
            return results;
        } catch(const std::exception& e) {
            LOG_ERROR(2039, index_id, "Search failed: " << e.what());
            return std::nullopt;
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

        auto metadata = metadata_manager_->getMetadata(index_id);
        if(!metadata) {
            return std::nullopt;
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

    // Method to log vector additions with both numeric and string IDs
    void logInsertsAndUpdates(CacheEntry& entry,
                              const std::vector<std::pair<idInt, bool>>& numeric_ids) {
        WriteAheadLog* wal = getOrCreateWAL(entry);

        // Create WAL entries for each vector addition
        std::vector<WriteAheadLog::WALEntry> entries;
        entries.reserve(numeric_ids.size());

        for(size_t i = 0; i < numeric_ids.size(); i++) {
            if(numeric_ids[i].second) {
                entries.push_back({
                        WALOperationType::VECTOR_ADD,
                        numeric_ids[i].first,
                });
            } else {
                entries.push_back({
                        WALOperationType::VECTOR_UPDATE,
                        numeric_ids[i].first,
                });
            }
        }

        // Log the entries
        wal->log(entries);

        // FIX: Don't call saveIndex here to avoid circular lock
        // The calling function (addVectors) already holds operation_mutex
        // and will call save at appropriate time
    }

    // Method to log vector deletions (only numeric IDs needed)
    void logDeletions(CacheEntry& entry, const std::vector<idInt>& numeric_ids) {
        WriteAheadLog* wal = getOrCreateWAL(entry);

        // Create WAL entries for each vector deletion
        std::vector<WriteAheadLog::WALEntry> entries;
        entries.reserve(numeric_ids.size());

        for(size_t i = 0; i < numeric_ids.size(); i++) {
            entries.push_back({
                    WALOperationType::VECTOR_DELETE,
                    numeric_ids[i],
            });
        }

        // Log the entries
        wal->log(entries);

        // FIX: Don't call saveIndex here to avoid circular lock
        // The calling function already holds operation_mutex
        // and will call save at appropriate time
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

        auto entry_ptr = getIndexEntry(index_id);
        auto& entry = *entry_ptr;
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
            std::unique_lock<std::shared_mutex> operation_lock(entry.operation_mutex);

            // Check again after acquiring lock (shutdown may have been requested while waiting)
            if (st.stop_requested()) {
                LOG_INFO(2057, index_id, "Backup cancelled");
                backup_store_.clearActiveBackup(username);
                return;
            }

            saveIndexInternal(entry);

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

    if(!std::filesystem::exists(backup_tar)) {
        return {false, "Backup not found: " + backup_name};
    }

    if(metadata_manager_->getMetadata(target_index_id).has_value()) {
        return {false, "Target index already exists"};
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

        std::filesystem::create_directories(target_dir);
        std::filesystem::copy(backup_dir,
                              target_dir,
                              std::filesystem::copy_options::recursive
                                      | std::filesystem::copy_options::overwrite_existing);

        std::filesystem::remove(target_dir + "/metadata.json");

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
        new_meta.sparse_model = *sparse_model;
        new_meta.created_at = std::chrono::system_clock::now();
        new_meta.total_elements = meta_json["params"].value("total_elements", 0ul);
        new_meta.checksum = meta_json["params"].value("checksum", -1);

        metadata_manager_->storeMetadata(target_index_id, new_meta);

        std::filesystem::remove_all(backup_extract_dir);

        {
            std::unique_lock<std::shared_mutex> write_lock(indices_mutex_);
            loadIndex(target_index_id);
        }

        LOG_INFO(2045, username, target_index_name, "Restored backup from " << backup_tar);
        return {true, ""};
    } catch(const std::exception& e) {
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
