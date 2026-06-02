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
#include "../storage/shared_mdbx.hpp"
#include "../utils/log.hpp"

namespace ndd {

    // Thin storage facade that keeps the raw sparse vectors and the derived
    // inverted index in the same MDBX environment and updates them transactionally.
    class SparseVectorStorage {
    public:
        SparseVectorStorage(MDBX_env* env,
                            const std::string& index_id,
                            ndd::SparseScoringModel sparse_model =
                                ndd::SparseScoringModel::DEFAULT) :
            index_id_(index_id),
            sparse_model_(sparse_model),
            env_(env) {
            sparse_index_ = nullptr;
        }

        ~SparseVectorStorage() {
            if(env_ && docs_dbi_) {
                mdbx_dbi_close(env_, docs_dbi_);
            }
        }

        // Initialize storage
        bool initialize() {
            if(!initializeDBIs()) {
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
                     "SparseVectorStorage initialized with " << vector_count_ << " vectors");
            return true;
        }

        /*
         * Public txn-taking delete. The returned OperationResult.value carries
         * the term_info_ mutations that must be applied via
         * apply_term_info_changes() only after the caller's mdbx_txn_commit
         * succeeds. If the caller aborts, the result is dropped and term_info_
         * stays in sync with the rolled-back MDBX state - that is the ACID
         * atomicity invariant tests/acid_regression_test.cpp pins down.
         *
         * Return codes:
         * 0 = success; value carries the deferred term_info_ changes
         * 100 = propagated MDBX/storage failure from the inverted index or
         *       the raw sparse doc table; caller should log ERROR and return
         *       HTTP 500
         */
        ndd::OperationResult<std::vector<ndd::TermInfoChange>>
        delete_vector(MDBX_txn* txn,
                          ndd::idInt doc_id,
                          int64_t* vector_count_delta = nullptr,
                          bool missing_ok = true) {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            std::vector<ndd::TermInfoChange> term_info_changes;
            int64_t delta = 0;
            if(!deleteVectorTxn(txn, doc_id, &delta, missing_ok, &term_info_changes)) {
                return {100, "delete_vector failed for doc_id=" + std::to_string(doc_id)};
            }
            if(vector_count_delta) {
                *vector_count_delta += delta;
            }
            return {SUCCESS, "", std::move(term_info_changes)};
        }

        /*
         * Public txn-taking batch upsert. Same ACID contract as
         * delete_vector - the value member of the returned OperationResult
         * carries deferred term_info_ mutations that the caller must apply via
         * apply_term_info_changes() ONLY after their own mdbx_txn_commit
         * returns success. On abort the result object is dropped.
         *
         * Return codes:
         * 0 = success; value carries the deferred term_info_ changes
         * 100 = propagated MDBX/storage failure from the inverted index or
         *       the raw sparse doc table; caller should log ERROR and return
         *       HTTP 500
         */
        ndd::OperationResult<std::vector<ndd::TermInfoChange>>
        store_vectors_batch(
                MDBX_txn* txn,
                const std::vector<std::pair<ndd::idInt, SparseVector>>& batch,
                int64_t* vector_count_delta = nullptr) {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            std::vector<ndd::TermInfoChange> term_info_changes;
            int64_t delta = 0;

            for(const auto& [doc_id, sparse_vec] : batch) {
                if(!storeVectorTxn(txn, doc_id, sparse_vec, &delta, &term_info_changes)) {
                    LOG_ERROR(2243,
                              index_id_,
                              "store_vectors_batch failed to replace doc_id=" << doc_id);
                    return {100,
                            "store_vectors_batch failed to replace doc_id="
                                    + std::to_string(doc_id)};
                }
            }

            if(vector_count_delta) {
                *vector_count_delta += delta;
            }
            return {SUCCESS, "", std::move(term_info_changes)};
        }

        /*
         * Forwarder for InvertedIndex::apply_term_info_changes. The caller
         * passes the OperationResult.value returned by the txn-taking writers
         * here only AFTER they have committed the MDBX txn that produced
         * those rows. See the ACID note on the writers above.
         */
        void apply_term_info_changes(const std::vector<ndd::TermInfoChange>& changes) {
            sparse_index_->apply_term_info_changes(changes);
        }

        std::optional<SparseVector> get_vector(MDBX_txn* txn, ndd::idInt doc_id) const {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            return getVectorInternal(txn, doc_id);
        }

        void apply_vector_count_delta(int64_t delta) { applyVectorCountDelta(delta); }

        MDBX_env* env() const { return env_; }

        /**
         * Routes the inverted-index posting reads through the caller's
         * MDBX read transaction so a single shared search request stays
         * on one snapshot.
         */
        std::vector<std::pair<ndd::idInt, float>>
        search(MDBX_txn* txn,
                   const SparseVector& query,
                   size_t k,
                   const ndd::RoaringBitmap* filter = nullptr)
        {
            return sparse_index_->search(
                txn, query, k, vector_count_.load(std::memory_order_relaxed), filter);
        }

        // Statistics
        size_t get_vector_count() const { return vector_count_; }
        size_t get_term_count() const { return sparse_index_ ? sparse_index_->getTermCount() : 0; }

    private:
        std::string index_id_;
        ndd::SparseScoringModel sparse_model_;
        MDBX_env* env_;
        MDBX_dbi docs_dbi_;

        std::unique_ptr<InvertedIndex> sparse_index_;
        mutable std::shared_mutex mutex_;

        std::atomic<size_t> vector_count_{0};
        std::unordered_set<ndd::idInt> deleted_docs_;

        bool initializeDBIs() {
            MDBX_txn* txn;
            int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
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

        /*
         * Internal helper. `term_info_changes` MUST be non-null. Term-info
         * mutations from the InvertedIndex calls are appended to it; the
         * caller is responsible for applying those changes only AFTER the
         * MDBX txn commits successfully.
         */
        bool storeVectorTxn(MDBX_txn* txn,
                            ndd::idInt doc_id,
                            const SparseVector& vec,
                            int64_t* vector_count_delta,
                            std::vector<ndd::TermInfoChange>* term_info_changes) {
            const auto existing_vec = getVectorInternal(txn, doc_id);
            const bool had_sparse_terms = existing_vec.has_value() && !existing_vec->empty();
            const bool has_sparse_terms = !vec.empty();

            // Sparse upserts must behave as replacements: remove old postings first,
            // then write and index the new vector only if it still has sparse terms.
            if(had_sparse_terms) {
                auto remove_result = sparse_index_->removeDocument(txn, doc_id, *existing_vec);
                if(!remove_result.ok()) {
                    return false;
                }
                auto& changes = remove_result.value_or_throw();
                term_info_changes->insert(term_info_changes->end(),
                                          std::make_move_iterator(changes.begin()),
                                          std::make_move_iterator(changes.end()));
            }

            if(existing_vec.has_value() && !deleteVectorInternal(txn, doc_id)) {
                return false;
            }

            if(has_sparse_terms) {
                if(!storeVectorInternal(txn, doc_id, vec)) {
                    return false;
                }

                auto add_result = sparse_index_->addDocumentsBatch(txn, {{doc_id, vec}});
                if(!add_result.ok()) {
                    return false;
                }
                auto& changes = add_result.value_or_throw();
                term_info_changes->insert(term_info_changes->end(),
                                          std::make_move_iterator(changes.begin()),
                                          std::make_move_iterator(changes.end()));
            }

            if(vector_count_delta) {
                *vector_count_delta += static_cast<int64_t>(has_sparse_terms)
                                     - static_cast<int64_t>(had_sparse_terms);
            }
            return true;
        }

        /*
         * Internal helper. `term_info_changes` MUST be non-null. See storeVectorTxn.
         */
        bool deleteVectorTxn(MDBX_txn* txn,
                             ndd::idInt doc_id,
                             int64_t* vector_count_delta,
                             bool missing_ok,
                             std::vector<ndd::TermInfoChange>* term_info_changes) {
            auto vec = getVectorInternal(txn, doc_id);
            if(!vec) {
                LOG_WARN(2242, index_id_, "delete_vector could not find doc_id=" << doc_id);
                return missing_ok;
            }

            const bool had_sparse_terms = !vec->empty();
            if(had_sparse_terms) {
                auto remove_result = sparse_index_->removeDocument(txn, doc_id, *vec);
                if(!remove_result.ok()) {
                    return false;
                }
                auto& changes = remove_result.value_or_throw();
                term_info_changes->insert(term_info_changes->end(),
                                          std::make_move_iterator(changes.begin()),
                                          std::make_move_iterator(changes.end()));
            }

            if(!deleteVectorInternal(txn, doc_id)) {
                return false;
            }

            if(had_sparse_terms && vector_count_delta) {
                (*vector_count_delta)--;
            }
            return true;
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
