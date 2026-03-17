#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <unordered_set>
#include <filesystem>
#include "mdbx/mdbx.h"
#include "inverted_index.hpp"
#include "../utils/log.hpp"

namespace ndd {

    // Thin storage facade that keeps the raw sparse vectors and the derived
    // inverted index in the same MDBX environment and updates them transactionally.
    class SparseVectorStorage {
    public:
        SparseVectorStorage(const std::string& db_path,
                            const std::string& index_id,
                            ndd::SparseScoringModel sparse_model =
                                ndd::SparseScoringModel::DEFAULT) :
            db_path_(db_path),
            index_id_(index_id),
            sparse_model_(sparse_model),
            env_(nullptr) {
            sparse_index_ = nullptr;
        }

        ~SparseVectorStorage() { closeMDBX(); }

        // Initialize storage
        bool initialize() {
            if(!initializeMDBX()) {
                return false;
            }

            sparse_index_ =
                std::make_unique<InvertedIndex>(env_, 0, index_id_, sparse_model_);
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
                committed_(false),
                vector_count_delta_(0) {
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
                    storage_->applyVectorCountDelta(vector_count_delta_);
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

            /**
             * Replace one document's sparse state inside the current transaction.
             * Used by single-doc writes and batch upserts so missing terms are removed from the
             * inverted index and empty vectors clear sparse state instead of leaving stale postings.
             */
            bool store_vector(ndd::idInt doc_id, const SparseVector& vec) {
                if(read_only_) {
                    return false;
                }

                const auto existing_vec = storage_->getVectorInternal(txn_, doc_id);
                const bool had_sparse_terms = existing_vec.has_value() && !existing_vec->empty();
                const bool has_sparse_terms = !vec.empty();

                // Sparse upserts must behave as replacements: remove the old postings first,
                // then write and index the new vector if it still has sparse terms.
                if(had_sparse_terms
                   && !storage_->sparse_index_->removeDocument(txn_, doc_id, *existing_vec)) {
                    return false;
                }

                if(existing_vec.has_value() && !storage_->deleteVectorInternal(txn_, doc_id)) {
                    return false;
                }

                if(has_sparse_terms) {
                    if(!storage_->storeVectorInternal(txn_, doc_id, vec)) {
                        return false;
                    }

                    if(!storage_->sparse_index_->addDocumentsBatch(txn_, {{doc_id, vec}})) {
                        return false;
                    }
                }

                vector_count_delta_ += static_cast<int64_t>(has_sparse_terms)
                                     - static_cast<int64_t>(had_sparse_terms);
                return true;
            }

            std::optional<SparseVector> get_vector(ndd::idInt doc_id) const {
                return storage_->getVectorInternal(txn_, doc_id);
            }


            /**
             * Remove one document's sparse state from both the inverted index and the raw sparse
             * doc table inside the current transaction.
             * Used for explicit deletes and when an upsert transitions a document to an empty
             * sparse vector.
             */
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

                const bool had_sparse_terms = !vec->empty();
                if(had_sparse_terms
                   && !storage_->sparse_index_->removeDocument(txn_, doc_id, *vec)) {
                    return false;
                }

                if(!storage_->deleteVectorInternal(txn_, doc_id)) {
                    return false;
                }

                if(had_sparse_terms) {
                    vector_count_delta_--;
                }
                return true;
            }

        private:
            SparseVectorStorage* storage_;
            MDBX_txn* txn_;
            bool committed_;
            bool read_only_;
            int64_t vector_count_delta_;
        };

        std::unique_ptr<Transaction> begin_transaction(bool read_only = false) {
            return std::make_unique<Transaction>(this, read_only);
        }

        // Vector management
        bool delete_vector(ndd::idInt doc_id) {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            auto txn = begin_transaction(false);
            if(!txn->delete_vector(doc_id)) {
                txn->abort();
                return false;
            }
            return txn->commit();
        }

        /**
         * Apply a batch of sparse upserts as document replacements inside one MDBX transaction.
         * The hybrid ingest path uses this so repeated IDs do not accumulate stale postings or
         * overcount the sparse-doc total used as N for server-side IDF.
         */
        bool store_vectors_batch(const std::vector<std::pair<ndd::idInt, SparseVector>>& batch) {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            auto txn = begin_transaction(false);

            for(const auto& [doc_id, sparse_vec] : batch) {
                if(!txn->store_vector(doc_id, sparse_vec)) {
                    LOG_ERROR(2243,
                              index_id_,
                              "store_vectors_batch failed to replace doc_id=" << doc_id);
                    txn->abort();
                    return false;
                }
            }

            return txn->commit();
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
            return sparse_index_->search(
                query, k, vector_count_.load(std::memory_order_relaxed), filter);
        }

        // Statistics
        size_t get_vector_count() const { return vector_count_; }
        size_t get_term_count() const { return sparse_index_ ? sparse_index_->getTermCount() : 0; }

    private:
        std::string db_path_;
        std::string index_id_;
        ndd::SparseScoringModel sparse_model_;
        MDBX_env* env_;
        MDBX_dbi docs_dbi_;

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

            // Set geometry (max 1TB for now, can be configured)
            rc = mdbx_env_set_geometry(env_, -1, -1, TB, -1, -1, -1);
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

            MDBX_txn* txn;
            rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
            if(rc != 0) {
                LOG_ERROR(2250, index_id_, "mdbx_txn_begin failed: " << mdbx_strerror(rc));
                return false;
            }

            rc = mdbx_dbi_open(txn, "sparse_docs", MDBX_CREATE | MDBX_INTEGERKEY, &docs_dbi_);
            if(rc != 0) {
                LOG_ERROR(2251, index_id_, "mdbx_dbi_open failed for sparse_docs: " << mdbx_strerror(rc));
                mdbx_txn_abort(txn);
                return false;
            }

            rc = mdbx_txn_commit(txn);
            if(rc != 0) {
                LOG_ERROR(2252, index_id_, "mdbx_txn_commit failed: " << mdbx_strerror(rc));
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
            if (rc != 0) {
                LOG_ERROR(2253,
                          index_id_,
                          "storeVectorInternal MDBX put failed for doc_id="
                                  << doc_id << ": " << mdbx_strerror(rc));
            }
            return rc == 0;
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

        /**
         * Apply the committed sparse-doc delta to the in-memory count used as N for server-side
         * IDF. Transaction code accumulates this delta first and only flushes it after commit.
         */
        void applyVectorCountDelta(int64_t delta) {
            if(delta > 0) {
                vector_count_.fetch_add(static_cast<size_t>(delta), std::memory_order_relaxed);
            } else if(delta < 0) {
                vector_count_.fetch_sub(static_cast<size_t>(-delta), std::memory_order_relaxed);
            }
        }

        /**
         * Peek at the packed sparse row to see whether it contains any terms.
         * Used during startup recounts so only non-empty sparse docs contribute to the
         * server-side IDF corpus.
         */
        static bool packedSparseVectorHasTerms(const MDBX_val& data) {
            if(data.iov_len < sizeof(uint16_t)) {
                return false;
            }

            uint16_t nr_nonzero = 0;
            std::memcpy(&nr_nonzero, data.iov_base, sizeof(uint16_t));
            return nr_nonzero > 0;
        }

        /**
         * Rebuild the sparse-doc count from disk on startup/reload.
         * Keeps vector_count_ aligned with persisted state and intentionally ignores empty
         * sparse rows.
         */
        void updateVectorCount() {
            MDBX_txn* txn;
            if(mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &txn) != 0) {
                return;
            }

            size_t live_sparse_docs = 0;
            MDBX_cursor* cursor = nullptr;
            if(mdbx_cursor_open(txn, docs_dbi_, &cursor) == 0) {
                MDBX_val key{};
                MDBX_val data{};
                int rc = mdbx_cursor_get(cursor, &key, &data, MDBX_FIRST);
                while(rc == MDBX_SUCCESS) {
                    if(packedSparseVectorHasTerms(data)) {
                        live_sparse_docs++;
                    }
                    rc = mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT);
                }
                mdbx_cursor_close(cursor);
            }

            vector_count_.store(live_sparse_docs, std::memory_order_relaxed);
            mdbx_txn_abort(txn);
        }
    };

}  // namespace ndd
