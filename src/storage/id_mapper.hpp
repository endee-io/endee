#pragma once

#include "mdbx/mdbx.h"
#include "log.hpp"
#include "auth.hpp"
#include "wal.hpp"
#include <string>
#include <stdexcept>
#include <memory>
#include <mutex>
#include <vector>
#include <numeric>
#include <filesystem>
#include <set>
#include <unordered_set>
#include "../core/types.hpp"
#include "../utils/settings.hpp"

using ndd::idInt;
class IDMapper {
public:
    IDMapper(MDBX_env* env,
             const std::string& dbi_name) :
        env_(env),
        dbi_(0),
        dbi_name_(dbi_name) {
        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error(std::string("Failed to begin transaction: ")
                                     + mdbx_strerror(rc));
        }

        rc = mdbx_dbi_open(txn, dbi_name_.c_str(), MDBX_CREATE, &dbi_);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            throw std::runtime_error(std::string("Failed to open database: ") + mdbx_strerror(rc));
        }

        rc = mdbx_txn_commit(txn);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error(std::string("Failed to commit transaction: ")
                                     + mdbx_strerror(rc));
        }
    }

    // Seed the sequence counter for a freshly-created index. Caller is
    // responsible for invoking this exactly once at createIndex.
    void init_sequence() {
        init_next_id();
    }

    ~IDMapper() {
        mdbx_dbi_close(env_, dbi_);
    }

    // Create string ID to numeric ID mapping. If string ids exists in the database, it will return
    // the existing numeric ID along with flag It will also use old numeric IDs of deleted points
    template <bool use_deleted_ids>
    std::vector<std::pair<idInt, bool>>
    create_ids_batch(MDBX_txn* txn, const std::vector<std::string>& str_ids) {
        if(str_ids.empty()) {
            return {};
        }

        constexpr idInt INVALID_LABEL = static_cast<idInt>(-1);
        std::vector<std::tuple<std::string, idInt, bool, bool>> id_tuples;
        id_tuples.reserve(str_ids.size());
        for(const auto& str_id : str_ids) {
            id_tuples.emplace_back(str_id, INVALID_LABEL, true, false);
        }

        for(auto& tup : id_tuples) {
            const std::string& str_id = std::get<0>(tup);
            MDBX_val key{(void*)str_id.c_str(), str_id.size()};
            MDBX_val data;

            int rc = mdbx_get(txn, dbi_, &key, &data);
            if(rc == MDBX_SUCCESS) {
                idInt existing_id = *(idInt*)data.iov_base;
                std::get<1>(tup) = existing_id;
                std::get<2>(tup) = false;
            } else if(rc == MDBX_NOTFOUND) {
                std::get<1>(tup) = 0;
            } else {
                throw std::runtime_error("Database error checking ID: "
                                         + std::string(mdbx_strerror(rc)));
            }
        }

        size_t total_new_ids_needed =
                std::count_if(id_tuples.begin(), id_tuples.end(), [](const auto& t) {
                    return std::get<1>(t) == 0;
                });

        size_t fresh_ids_count = total_new_ids_needed;
        size_t deleted_index = 0;

        if(use_deleted_ids) {
            std::vector<idInt> deletedIds = getDeletedIds(txn, fresh_ids_count);
            for(auto& tup : id_tuples) {
                if(std::get<1>(tup) == 0 && std::get<2>(tup) == true
                   && deleted_index < deletedIds.size()) {
                    std::get<1>(tup) = deletedIds[deleted_index++];
                    std::get<3>(tup) = true;
                }
            }
            fresh_ids_count -= deleted_index;
        }

        std::vector<idInt> new_ids;
        if(fresh_ids_count > 0) {
            new_ids = get_next_ids(txn, fresh_ids_count);
        }

        size_t new_id_index = 0;
        for(auto& tup : id_tuples) {
            if(std::get<2>(tup) == true && std::get<1>(tup) != 0) {
                const std::string& str_id = std::get<0>(tup);
                idInt id = std::get<1>(tup);

                MDBX_val key{(void*)str_id.c_str(), str_id.size()};
                MDBX_val data{&id, sizeof(idInt)};

                int rc = mdbx_put(txn, dbi_, &key, &data, MDBX_UPSERT);
                if(rc != MDBX_SUCCESS) {
                    throw std::runtime_error("Failed to insert IDs: "
                                             + std::string(mdbx_strerror(rc)));
                }
            } else if(std::get<1>(tup) == 0) {
                if(new_id_index >= new_ids.size()) {
                    throw std::runtime_error("Mismatch in generated ID count");
                }
                idInt new_id = new_ids[new_id_index++];
                const std::string& str_id = std::get<0>(tup);

                MDBX_val key{(void*)str_id.c_str(), str_id.size()};
                MDBX_val data{&new_id, sizeof(idInt)};

                int rc = mdbx_put(txn, dbi_, &key, &data, MDBX_UPSERT);
                if(rc != MDBX_SUCCESS) {
                    throw std::runtime_error("Failed to insert IDs: "
                                             + std::string(mdbx_strerror(rc)));
                }

                std::get<1>(tup) = new_id;
            }
        }

        std::vector<std::pair<idInt, bool>> result;
        result.reserve(id_tuples.size());
        for(const auto& tup : id_tuples) {
            // A reused numeric_id paired with a brand-new string_id is a fresh
            // insert from HNSW's perspective, not an update. The old slot for
            // that numeric_id has been markDelete'd; addPoint<true> allocates
            // a fresh slot. Conflating numeric_id reuse with string_id update
            // was the root cause of the HNSW-recovery-loses-labels bug.
            result.emplace_back(std::get<1>(tup), std::get<2>(tup));
        }
        return result;
    }

    // Get the number of key-value pairs in the database
    size_t get_count() const {
        MDBX_txn* txn;
        MDBX_stat stat;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &txn);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error(std::string("Failed to begin transaction: ")
                                     + mdbx_strerror(rc));
        }

        rc = mdbx_dbi_stat(txn, dbi_, &stat, sizeof(stat));
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            throw std::runtime_error(std::string("Failed to get database statistics: ")
                                     + mdbx_strerror(rc));
        }

        mdbx_txn_abort(txn);
        return stat.ms_entries - 1;  // Subtract 1 for NEXT_ID_KEY
    }

    // Get ID for a string (returns 0 if not found)
    idInt get_id(MDBX_txn* txn, const std::string& str_id) const {
        MDBX_val key, data;
        key.iov_len = str_id.size();
        key.iov_base = (void*)str_id.c_str();

        int rc = mdbx_get(txn, dbi_, &key, &data);
        if(rc == MDBX_SUCCESS) {
            return *(idInt*)data.iov_base;
        }
        return 0;
    }

    std::vector<idInt> deletePoints(MDBX_txn* txn,
                                        const std::vector<std::string>& external_ids) {
        std::vector<idInt> deleted_ids;

        MDBX_val key, data;
        for(const auto& ext_id : external_ids) {
            key.iov_len = ext_id.size();
            key.iov_base = const_cast<char*>(ext_id.data());

            int rc = mdbx_get(txn, dbi_, &key, &data);
            if(rc == MDBX_SUCCESS) {
                idInt label = *reinterpret_cast<idInt*>(data.iov_base);
                deleted_ids.push_back(label);
                rc = mdbx_del(txn, dbi_, &key, nullptr);
                if(rc != MDBX_SUCCESS) {
                    throw std::runtime_error("Failed to delete ID mapping: "
                                             + std::string(mdbx_strerror(rc)));
                }
            } else if(rc == MDBX_NOTFOUND) {
                deleted_ids.push_back(0);
            } else {
                throw std::runtime_error("Failed to read ID mapping for delete: "
                                         + std::string(mdbx_strerror(rc)));
            }
        }

        if(!deleted_ids.empty()) {
            std::string del_key = DELETED_IDS_KEY;
            MDBX_val del_mdb_key, del_mdb_val;

            del_mdb_key.iov_len = del_key.size();
            del_mdb_key.iov_base = const_cast<char*>(del_key.data());

            std::vector<idInt> existing;
            if(mdbx_get(txn, dbi_, &del_mdb_key, &del_mdb_val) == MDBX_SUCCESS) {
                size_t count = del_mdb_val.iov_len / sizeof(idInt);
                idInt* raw = reinterpret_cast<idInt*>(del_mdb_val.iov_base);
                existing.insert(existing.end(), raw, raw + count);
            }

            for(idInt l : deleted_ids) {
                if(l != 0) {
                    existing.push_back(l);
                }
            }

            del_mdb_val.iov_len = existing.size() * sizeof(idInt);
            del_mdb_val.iov_base = existing.data();
            int rc = mdbx_put(txn, dbi_, &del_mdb_key, &del_mdb_val, MDBX_UPSERT);
            if(rc != MDBX_SUCCESS) {
                throw std::runtime_error("Failed to update deleted ID list: "
                                         + std::string(mdbx_strerror(rc)));
            }
        }

        return deleted_ids;
    }

    // Deletes mapping from string_id to numeric_id, append to DELETED_IDS_KEY
    // Returns the deleted numeric_ids, if strings is not found, returns 0
    // Public method to add failed IDs back to deleted_ids for reuse
    void reclaim_failed_ids(const std::vector<idInt>& failed_ids) {
        add_to_deleted_ids(failed_ids);
    }

    MDBX_env* get_env() const { return env_; }

private:
    MDBX_env* env_;
    MDBX_dbi dbi_;
    std::string dbi_name_;
    mutable std::mutex mutex_;  // Only used for next_id management
    // Along with string:number pairs, the database also stores a key for next_id. They key for next
    // id also has random alphanumeric characters to avoid collision with other keys. The key is
    // stored as a string.
    static const std::string NEXT_ID_KEY;
    static const std::string DELETED_IDS_KEY;

    // Atomic operation to get and increment next_ids
    std::vector<idInt> get_next_ids(MDBX_txn* txn, size_t size = 1) {
        std::lock_guard<std::mutex> lock(mutex_);

        MDBX_val key{(void*)NEXT_ID_KEY.c_str(), NEXT_ID_KEY.size()};
        MDBX_val data;
        idInt current_id = 0;

        int rc = mdbx_get(txn, dbi_, &key, &data);
        if(rc == MDBX_SUCCESS) {
            current_id = *(idInt*)data.iov_base;
        } else if(rc != MDBX_NOTFOUND) {
            throw std::runtime_error(std::string("Failed to get next_id: ") + mdbx_strerror(rc));
        }

        idInt next_id = current_id + size;
        data.iov_len = sizeof(idInt);
        data.iov_base = &next_id;

        rc = mdbx_put(txn, dbi_, &key, &data, MDBX_UPSERT);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error(std::string("Failed to store next_id: ")
                                     + mdbx_strerror(rc));
        }

        std::vector<idInt> ids(size);
        std::iota(ids.begin(), ids.end(), current_id);
        return ids;
    }

    std::vector<idInt> getDeletedIds(MDBX_txn* txn, size_t max_count) {
        std::vector<idInt> result;

        std::string del_key = DELETED_IDS_KEY;
        MDBX_val key, val;
        key.iov_len = del_key.size();
        key.iov_base = const_cast<char*>(del_key.data());

        int rc = mdbx_get(txn, dbi_, &key, &val);
        if(rc == MDBX_NOTFOUND) {
            return result;
        }
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error("Failed to read deleted IDs: "
                                     + std::string(mdbx_strerror(rc)));
        }

        size_t total = val.iov_len / sizeof(idInt);
        idInt* raw = reinterpret_cast<idInt*>(val.iov_base);

        size_t count = std::min(max_count, total);
        result.insert(result.end(), raw, raw + count);

        if(count < total) {
            // Copy the remainder out of MDBX-managed memory before calling
            // mdbx_put on the same key. With MDBX_WRITEMAP, passing a pointer
            // that aliases the existing value to mdbx_put is undefined
            // behaviour; copying out first avoids that class of bug, regardless
            // of the dedup fix in add_to_deleted_ids.
            std::vector<idInt> remainder(raw + count, raw + total);
            MDBX_val new_val;
            new_val.iov_len = remainder.size() * sizeof(idInt);
            new_val.iov_base = remainder.data();
            rc = mdbx_put(txn, dbi_, &key, &new_val, MDBX_UPSERT);
        } else {
            rc = mdbx_del(txn, dbi_, &key, nullptr);
        }
        if(rc != MDBX_SUCCESS && rc != MDBX_NOTFOUND) {
            throw std::runtime_error("Failed to update deleted IDs: "
                                     + std::string(mdbx_strerror(rc)));
        }

        return result;
    }

    // Helper method to add IDs to deleted_ids list
    void add_to_deleted_ids(const std::vector<idInt>& ids) {
        if(ids.empty()) {
            return;
        }

        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
        if(rc != 0) {
            return;  // Silently fail for recovery
        }

        try {
            std::string del_key = DELETED_IDS_KEY;
            MDBX_val del_mdb_key, del_mdb_val;

            del_mdb_key.iov_len = del_key.size();
            del_mdb_key.iov_base = const_cast<char*>(del_key.data());

            // Fetch existing deleted IDs
            std::vector<idInt> existing;
            if(mdbx_get(txn, dbi_, &del_mdb_key, &del_mdb_val) == MDBX_SUCCESS) {
                size_t count = del_mdb_val.iov_len / sizeof(idInt);
                idInt* raw = reinterpret_cast<idInt*>(del_mdb_val.iov_base);
                existing.insert(existing.end(), raw, raw + count);
            }

            // Add new IDs, skipping those already present. This path is called
            // from reclaim_failed_ids during WAL recovery for VECTOR_ADD entries
            // whose vector_storage row is missing. If the row is missing because
            // a later VECTOR_DELETE already reclaimed the numeric_id via
            // deletePoints, the id is already in DELETED - adding it again
            // would produce a duplicate that getDeletedIds would later hand
            // to two different string_ids, corrupting MDBX storage.
            std::unordered_set<idInt> seen(existing.begin(), existing.end());
            for(idInt id : ids) {
                if(seen.insert(id).second) {
                    existing.push_back(id);
                }
            }

            // Write back to DB
            del_mdb_val.iov_len = existing.size() * sizeof(idInt);
            del_mdb_val.iov_base = existing.data();
            mdbx_put(txn, dbi_, &del_mdb_key, &del_mdb_val, MDBX_UPSERT);

            mdbx_txn_commit(txn);
        } catch(...) {
            mdbx_txn_abort(txn);
        }
    }

    // Initialize next_id .. called only once during construction
    void init_next_id() {
        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
        if(rc != 0) {
            throw std::runtime_error(std::string("Failed to begin transaction: ")
                                     + mdbx_strerror(rc));
        }

        try {
            MDBX_val key{(void*)NEXT_ID_KEY.c_str(), NEXT_ID_KEY.size()};
            MDBX_val data;
            idInt next_id = 1;  // Default starting value

            // Store the next_id (whether new or existing)
            data.iov_len = sizeof(idInt);
            data.iov_base = &next_id;
            rc = mdbx_put(txn, dbi_, &key, &data, MDBX_UPSERT);
            if(rc != 0) {
                throw std::runtime_error(std::string("Failed to store next_id: ")
                                         + mdbx_strerror(rc));
            }

            rc = mdbx_txn_commit(txn);
            if(rc != 0) {
                throw std::runtime_error(std::string("Failed to commit transaction: ")
                                         + mdbx_strerror(rc));
            }

        } catch(...) {
            mdbx_txn_abort(txn);
            throw;
        }
    }
};

inline const std::string IDMapper::NEXT_ID_KEY = "__next_id_px7b39lw__";
inline const std::string IDMapper::DELETED_IDS_KEY = "__deleted_ids_px7b39lw__";
