#pragma once

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <unordered_set>
#include <filesystem>
#include "db_backend.hpp"
#include "inverted_index.hpp"
#include "../utils/log.hpp"

namespace ndd {

    // Thin storage facade that keeps the raw sparse vectors and the derived
    // inverted index in the same MDBX environment and updates them transactionally.
    class SparseVectorStorage {
    public:
        SparseVectorStorage(const std::string& db_path, const std::string& index_id) :
            db_path_(db_path),
            index_id_(index_id),
            env_(nullptr),
            map_config_("sparse-store", settings::SPARSE_MAP_SIZE_MAX_BITS, settings::SPARSE_MAP_GROWTH_BITS) {
            sparse_index_ = nullptr;
        }

        ~SparseVectorStorage() { closeMDBX(); }

        // Initialize storage
        bool initialize() {
            if(!initializeMDBX()) {
                return false;
            }

            sparse_index_ = std::make_unique<InvertedIndex>(env_, 0, index_id_, &map_config_);
            if(!sparse_index_->initialize()) {
                return false;
            }

            updateVectorCount();
            LOG_INFO(2241,
                     index_id_,
                     "SparseVectorStorage initialized at " << db_path_ << " with " << vector_count_ << " vectors");
            return true;
        }

        // Transaction support
        class Transaction {
        public:
            Transaction(SparseVectorStorage* storage, bool read_only = false) :
                storage_(storage),
                read_only_(read_only),
                committed_(false) {
                int flags = read_only ? MDBX_TXN_RDONLY : MDBX_TXN_READWRITE;
                int rc = mdbx_txn_begin(
                        storage_->env_, nullptr, static_cast<MDBX_txn_flags_t>(flags), &txn_);
                if(rc != 0) {
                    throw std::runtime_error("Failed to begin transaction: "
                                                + std::string(mdbx_strerror(rc)));
                }
            }

            ~Transaction() {
                if(!committed_) {
                    abort();
                }
            }

            bool commit() {
                if(committed_) {
                    return true;
                }
                int rc = mdbx_txn_commit(txn_);
                if(rc == 0) {
                    committed_ = true;
                    return true;
                }
                return false;
            }

            void abort() {
                if(!committed_) {
                    mdbx_txn_abort(txn_);
                    committed_ = true;  // effectively closed
                }
            }

            MDBX_txn* getTxn() { return txn_; }

            bool store_vector(ndd::idInt doc_id, const SparseVector& vec) {
                if(read_only_) {
                    return false;
                }

                // Always write the source-of-truth document payload first, then update the
                // derived inverted index in the same transaction.
                if(!storage_->storeVectorInternal(txn_, doc_id, vec)) {
                    return false;
                }

                if(!storage_->sparse_index_->addDocumentsBatch(txn_, {{doc_id, vec}})) {
                    return false;
                }

                // 3. Save Metadata (Handled internally by InvertedIndex per term)
                // if (!storage_->sparse_index_->saveMetadata(txn_)) return false;

                storage_->vector_count_++;
                return true;
            }

            std::optional<SparseVector> get_vector(ndd::idInt doc_id) const {
                return storage_->getVectorInternal(txn_, doc_id);
            }


            bool delete_vector(ndd::idInt doc_id) {
                if(read_only_) {
                    return false;
                }

                // Deletion runs in the opposite order: look up the stored vector, remove its
                // terms from the inverted index, then delete the raw payload row.
                auto vec = get_vector(doc_id);
                if(!vec) {
                    LOG_WARN(2242, storage_->index_id_, "delete_vector could not find doc_id=" << doc_id);
                    return false;
                }

                if(!storage_->sparse_index_->removeDocument(txn_, doc_id, *vec)) {
                    return false;
                }

                if(!storage_->deleteVectorInternal(txn_, doc_id)) {
                    return false;
                }

                // 4. Save Metadata (Handled internally)
                // if (!storage_->sparse_index_->saveMetadata(txn_)) return false;

                storage_->vector_count_--;
                return true;
            }

        private:
            SparseVectorStorage* storage_;
            MDBX_txn* txn_;
            bool committed_;
            bool read_only_;
        };

        std::unique_ptr<Transaction> begin_transaction(bool read_only = false) {
            return std::make_unique<Transaction>(this, read_only);
        }

        // Vector management
        bool delete_vector(ndd::idInt doc_id) {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            try {
                const bool committed =
                        ndd::db::with_write_txn_retry(env_, map_config_, index_id_, [&](MDBX_txn* txn) {
                    auto vec = getVectorInternal(txn, doc_id);
                    if(!vec) {
                        LOG_WARN(2242, index_id_, "delete_vector could not find doc_id=" << doc_id);
                        return false;
                    }

                    if(!sparse_index_->removeDocument(txn, doc_id, *vec)) {
                        return false;
                    }

                    if(!deleteVectorInternal(txn, doc_id)) {
                        return false;
                    }

                    return true;
                });

                if(committed) {
                    vector_count_--;
                    return true;
                }
                return false;
            } catch(const std::exception& e) {
                LOG_ERROR(2255, index_id_, "delete_vector failed for doc_id=" << doc_id << ": " << e.what());
                return false;
            }
        }

        // Batch operations
        bool store_vectors_batch(const std::vector<std::pair<ndd::idInt, SparseVector>>& batch) {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            try {
                const bool committed =
                        ndd::db::with_write_txn_retry(env_, map_config_, index_id_, [&](MDBX_txn* txn) {
                            for(const auto& [doc_id, sparse_vec] : batch) {
                                if(!storeVectorInternal(txn, doc_id, sparse_vec)) {
                                    LOG_ERROR(2243,
                                              index_id_,
                                              "store_vectors_batch failed to store doc_id=" << doc_id);
                                    return false;
                                }
                            }

                            if(!sparse_index_->addDocumentsBatch(txn, batch)) {
                                LOG_ERROR(2244,
                                          index_id_,
                                          "store_vectors_batch failed to update the inverted index for batch size "
                                                  << batch.size());
                                return false;
                            }

                            return true;
                        });

                if(committed) {
                    vector_count_ += batch.size();
                    return true;
                }
                return false;
            } catch(const std::exception& e) {
                LOG_ERROR(2256, index_id_, "store_vectors_batch failed: " << e.what());
                return false;
            }
        }

        /*NOT BEING USED FOR NOW*/
#if 0
        bool delete_vectors_batch(const std::vector<ndd::idInt>& doc_ids) {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            auto txn = begin_transaction(false);

            for(ndd::idInt doc_id : doc_ids) {
                if(!txn->delete_vector(doc_id)) {
                    // Continue or abort? Usually continue for batch delete
                }
            }
            return txn->commit();
        }
#endif //if 0

        std::vector<std::pair<ndd::idInt, float>> search(const SparseVector& query,
                                                        size_t k,
                                                        const ndd::RoaringBitmap* filter = nullptr)
        {
            return sparse_index_->search(query, k, filter);
        }

        // Statistics
        size_t get_vector_count() const { return vector_count_; }
        size_t get_term_count() const { return sparse_index_ ? sparse_index_->getTermCount() : 0; }

    private:
        std::string db_path_;
        std::string index_id_;
        MDBX_env* env_;
        MDBX_dbi docs_dbi_;
        ndd::db::EnvResizeConfig map_config_;

        std::unique_ptr<InvertedIndex> sparse_index_;
        mutable std::shared_mutex mutex_;

        std::atomic<size_t> vector_count_{0};
        std::unordered_set<ndd::idInt> deleted_docs_;

        // Helper methods
        bool initializeMDBX() {
            int rc = mdbx_env_create(&env_);
            if(rc != 0) {
                LOG_ERROR(2245, index_id_, "mdbx_env_create failed: " << mdbx_strerror(rc));
                return false;
            }

            rc = ndd::db::configure_env_mapsize(env_, map_config_);
            if(rc != 0) {
                LOG_ERROR(2246, index_id_, "mdbx_env_set_geometry failed: " << mdbx_strerror(rc));
                return false;
            }

            // Set maxdbs to allow named databases
            rc = mdbx_env_set_maxdbs(env_, 10);
            if(rc != 0) {
                LOG_ERROR(2247, index_id_, "mdbx_env_set_maxdbs failed: " << mdbx_strerror(rc));
                return false;
            }

            std::error_code ec;
            std::filesystem::create_directories(db_path_, ec);
            if(ec) {
                LOG_ERROR(2248, index_id_, "create_directories failed for " << db_path_ << ": " << ec.message());
                return false;
            }

            rc = mdbx_env_open(env_,
                               db_path_.c_str(),
                               MDBX_NOSTICKYTHREADS | MDBX_NORDAHEAD | MDBX_LIFORECLAIM,
                               0664);
            if(rc != 0) {
                LOG_ERROR(2249,
                          index_id_,
                          "mdbx_env_open failed for " << db_path_ << ": " << mdbx_strerror(rc));
                return false;
            }

            try {
                ndd::db::with_write_txn_retry(env_, map_config_, index_id_, [&](MDBX_txn* txn) {
                    const int open_rc =
                            mdbx_dbi_open(txn, "sparse_docs", MDBX_CREATE | MDBX_INTEGERKEY, &docs_dbi_);
                    ndd::db::throw_if_error(open_rc, "Failed to open sparse_docs dbi");
                });
            } catch(const std::exception& e) {
                LOG_ERROR(2252, index_id_, "Failed to initialize sparse docs dbi: " << e.what());
                return false;
            }
            return true;
        }

        void closeMDBX() {
            if(env_) {
                mdbx_env_close(env_);
                env_ = nullptr;
            }
        }

        bool storeVectorInternal(MDBX_txn* txn, ndd::idInt doc_id, const SparseVector& vec) {
            auto packed = vec.pack();
            MDBX_val key, data;
            key.iov_base = &doc_id;
            key.iov_len = sizeof(ndd::idInt);
            data.iov_base = packed.data();
            data.iov_len = packed.size();

            int rc = mdbx_put(txn, docs_dbi_, &key, &data, MDBX_UPSERT);
            ndd::db::throw_if_error(
                    rc,
                    "storeVectorInternal failed for doc_id=" + std::to_string(doc_id));
            return true;
        }

        std::optional<SparseVector> getVectorInternal(MDBX_txn* txn, ndd::idInt doc_id) const {
            MDBX_val key, data;
            key.iov_base = const_cast<ndd::idInt*>(&doc_id);
            key.iov_len = sizeof(ndd::idInt);

            int rc = mdbx_get(txn, docs_dbi_, &key, &data);
            if(rc == MDBX_SUCCESS) {
                return SparseVector(static_cast<const uint8_t*>(data.iov_base), data.iov_len);
            }
            return std::nullopt;
        }

        bool deleteVectorInternal(MDBX_txn* txn, ndd::idInt doc_id) {
            MDBX_val key;
            key.iov_base = &doc_id;
            key.iov_len = sizeof(ndd::idInt);
            int rc = mdbx_del(txn, docs_dbi_, &key, nullptr);
            if (rc != 0 && rc != MDBX_NOTFOUND) {
                LOG_ERROR(2254,
                          index_id_,
                          "deleteVectorInternal MDBX delete failed for doc_id="
                                  << doc_id << ": " << mdbx_strerror(rc));
            }
            return rc == 0;
        }

        void updateVectorCount() {
            MDBX_txn* txn;
            if(mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &txn) == 0) {
                MDBX_stat stat;
                if(mdbx_dbi_stat(txn, docs_dbi_, &stat, sizeof(stat)) == 0) {
                    vector_count_ = stat.ms_entries;
                }
                mdbx_txn_abort(txn);
            }
        }
    };

}  // namespace ndd
