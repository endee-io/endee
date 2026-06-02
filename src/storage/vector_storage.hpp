#pragma once

#include "mdbx/mdbx.h"
#include "log.hpp"
#include "../quant/dispatch.hpp"
#include "../filter/filter.hpp"
#include "../core/types.hpp"
#include "json/nlohmann_json.hpp"
#include "msgpack_ndd.hpp"
#include "quant_vector.hpp"
#include "shared_mdbx.hpp"
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>
#include <filesystem>
#include <utility>

// Handles vector storage
class VectorStore {
private:
    MDBX_env* env_;
    MDBX_dbi dbi_;
    std::string index_id_;
    std::string dbi_name_;
    size_t vector_dim_;
    ndd::quant::QuantizationLevel quant_level_;
    size_t bytes_per_vector_;

    void init_dbi() {
        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error(std::string("Failed to begin transaction: ") + mdbx_strerror(rc));
        }

        rc = mdbx_dbi_open(txn, dbi_name_.c_str(), MDBX_CREATE | MDBX_INTEGERKEY, &dbi_);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            throw std::runtime_error(std::string("Failed to open database: ") + mdbx_strerror(rc));
        }

        rc = mdbx_txn_commit(txn);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error("Failed to commit transaction: "
                                        + std::string(mdbx_strerror(rc)));
        }
    }

public:
    VectorStore(MDBX_env* env,
                size_t vector_dim,
                ndd::quant::QuantizationLevel quant_level,
                const std::string& index_id,
                const std::string& dbi_name) :
        env_(env),
        index_id_(index_id),
        dbi_name_(dbi_name),
        vector_dim_(vector_dim),
        quant_level_(quant_level) {
        bytes_per_vector_ =
                ndd::quant::get_quantizer_dispatch(quant_level_).get_storage_size(vector_dim);
        init_dbi();
    }

    ~VectorStore() {
        mdbx_dbi_close(env_, dbi_);
    }
    // Nested Cursor struct

    struct Cursor {
        std::string index_id_;
        MDBX_txn* txn = nullptr;
        MDBX_cursor* cursor = nullptr;
        bool done = false;

        Cursor(MDBX_env* env, MDBX_dbi dbi, const std::string& index_id) :
            index_id_(index_id) {
            int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn);
            if(rc != MDBX_SUCCESS) {
                throw std::runtime_error(std::string("LMDB txn begin failed: ") + mdbx_strerror(rc));
            }

            rc = mdbx_cursor_open(txn, dbi, &cursor);
            if(rc != MDBX_SUCCESS) {
                throw std::runtime_error(std::string("LMDB cursor open failed: ") + mdbx_strerror(rc));
            }
        }

        // prevent copying
        Cursor(const Cursor&) = delete;
        Cursor& operator=(const Cursor&) = delete;

        bool hasNext() { return !done; }

        std::pair<ndd::idInt, std::vector<uint8_t>> next() {
            MDBX_val key, val;
            int rc = mdbx_cursor_get(cursor, &key, &val, MDBX_NEXT);
            if(rc != MDBX_SUCCESS) {
                done = true;
                return {};
            }

            if(key.iov_len != sizeof(ndd::idInt)) {
                LOG_ERROR(1601,
                                index_id_,
                                "Invalid key size " << key.iov_len << ", expected "
                                                    << sizeof(ndd::idInt));
                throw std::runtime_error("Invalid key size in LMDB entry");
            }

            ndd::idInt label;
            std::memcpy(&label, key.iov_base, sizeof(label));

            std::vector<uint8_t> vec((uint8_t*)val.iov_base, (uint8_t*)val.iov_base + val.iov_len);
            return {label, std::move(vec)};
        }

        ~Cursor() {
            if(cursor) {
                mdbx_cursor_close(cursor);
            }
            if(txn) {
                mdbx_txn_abort(txn);
            }
        }
    };

    Cursor getCursor() { return Cursor(env_, dbi_, index_id_); }

    /**
     * Reads quantized vector bytes for one numeric id through the
     * caller's MDBX read transaction. Allocates a new std::vector.
     *
     * Return codes:
     * 0   = success
     * 100 = MDBX read failure (env / I/O)
     * 101 = numeric_id not present
     *
     * Hot-path note: HNSW's per-node fetcher uses the buffer overload
     * `bool get_vector_bytes(MDBX_txn*, idInt, uint8_t*)` for
     * sub-µs cost. This vector-returning overload is for callers that
     * cannot pre-allocate a buffer (e.g. external getVector returning
     * bytes to the HTTP layer).
     */
    ndd::OperationResult<std::vector<uint8_t>>
    get_vector_bytes(MDBX_txn* txn, ndd::idInt numeric_id) const {
        MDBX_val key{const_cast<ndd::idInt*>(&numeric_id), sizeof(ndd::idInt)};
        MDBX_val data;

        int rc = mdbx_get(txn, dbi_, &key, &data);
        if(rc == MDBX_NOTFOUND) {
            return {101, "Vector not found", std::nullopt};
        }
        if(rc != MDBX_SUCCESS) {
            return {100,
                    std::string("Failed to read vector: ") + mdbx_strerror(rc),
                    std::nullopt};
        }

        return {SUCCESS, "",
                std::vector<uint8_t>(static_cast<uint8_t*>(data.iov_base),
                                     static_cast<uint8_t*>(data.iov_base) + data.iov_len)};
    }

    bool get_vector_bytes(MDBX_txn* txn, ndd::idInt numeric_id, uint8_t* buffer) const {
        MDBX_val key{const_cast<ndd::idInt*>(&numeric_id), sizeof(ndd::idInt)};
        MDBX_val data;

        int rc = mdbx_get(txn, dbi_, &key, &data);
        if(rc != MDBX_SUCCESS || data.iov_len != bytes_per_vector_) {
            return false;
        }

        std::memcpy(buffer, data.iov_base, data.iov_len);
        return true;
    }

    /**
     * Buffer-overload no-txn fetch for the HNSW write path: addPoint's
     * graph traversal calls `VectorFetcher` with `txn == nullptr`. The
     * fetcher closure routes here so per-cache-miss reads open their
     * own RDONLY. Hot path - stays on `bool`.
     */
    bool get_vector_bytes(ndd::idInt numeric_id, uint8_t* buffer) const {
        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &txn);
        if(rc != MDBX_SUCCESS) {
            return false;
        }

        bool result = get_vector_bytes(txn, numeric_id, buffer);
        mdbx_txn_abort(txn);
        return result;
    }

    /**
     * Batch read: fetches multiple vectors using the caller's MDBX read
     * transaction. Returns the number of successful fetches; per-id
     * results land in `success[]`. This is the HNSW fetcher hot path -
     * kept on `size_t` rather than `OperationResult` for per-node call
     * cost (sub-µs).
     *
     * Preconditions:
     * - `txn` is a live `MDBX_TXN_RDONLY` on the same thread as this call.
     * - `buffers` points to at least `count * bytes_per_vector_` bytes.
     *
     * The non-txn `get_vectors_batch_into` overload is a thin wrapper
     * that opens its own RDONLY and delegates here. New code should use
     * this method directly so reads share the caller's snapshot and
     * MDBX sticky-thread mode does not refuse a second RDONLY.
     */
    size_t get_vectors_batch_into(MDBX_txn* txn,
                                      const ndd::idInt* labels,
                                      uint8_t* buffers,
                                      bool* success,
                                      size_t count) const {
        if(count == 0) return 0;

        size_t fetched = 0;
        for(size_t i = 0; i < count; i++) {
            MDBX_val key{const_cast<ndd::idInt*>(&labels[i]), sizeof(ndd::idInt)};
            MDBX_val data;
            int rc = mdbx_get(txn, dbi_, &key, &data);
            if(rc == MDBX_SUCCESS && data.iov_len == bytes_per_vector_) {
                std::memcpy(buffers + i * bytes_per_vector_, data.iov_base, bytes_per_vector_);
                success[i] = true;
                fetched++;
            } else {
                success[i] = false;
            }
        }

        return fetched;
    }

    /**
     * Batch fetch wrapper that opens its own RDONLY transaction.
     * Retained for legacy callers; new code should call
     * `get_vectors_batch_into` with the request-scoped txn.
     */
    size_t get_vectors_batch_into(const ndd::idInt* labels, uint8_t* buffers,
                                  bool* success, size_t count) const {
        if(count == 0) return 0;

        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &txn);
        if(rc != MDBX_SUCCESS) {
            for(size_t i = 0; i < count; i++) success[i] = false;
            return 0;
        }

        size_t fetched = get_vectors_batch_into(txn, labels, buffers, success, count);
        mdbx_txn_abort(txn);
        return fetched;
    }

    // Batch operations with raw bytes
    ndd::OperationResult<>
    store_vectors_batch(
            MDBX_txn* txn,
            const std::vector<std::pair<ndd::idInt, std::vector<uint8_t>>>& batch) {
        if(batch.empty()) {
            return {SUCCESS, ""};
        }

        for(const auto& [numeric_id, vector_bytes] : batch) {
            if(vector_bytes.size() != bytes_per_vector_) {
                return {100, "Vector byte size mismatch"};
            }

            MDBX_val key{const_cast<ndd::idInt*>(&numeric_id), sizeof(ndd::idInt)};
            MDBX_val data{const_cast<uint8_t*>(vector_bytes.data()), vector_bytes.size()};

            int rc = mdbx_put(txn, dbi_, &key, &data, MDBX_UPSERT);
            if(rc != MDBX_SUCCESS) {
                return {100, std::string("Failed to store vector: ") + mdbx_strerror(rc)};
            }
        }
        return {SUCCESS, ""};
    }

    template <typename Visitor>
    size_t visit_vectors_by_ids(MDBX_txn* txn,
                                    const std::vector<ndd::idInt>& numeric_ids,
                                    Visitor&& visitor) const {
        if(numeric_ids.empty()) {
            return 0;
        }

        size_t visited = 0;
        for(const auto& numeric_id : numeric_ids) {
            MDBX_val key{const_cast<ndd::idInt*>(&numeric_id), sizeof(ndd::idInt)};
            MDBX_val data;

            int rc = mdbx_get(txn, dbi_, &key, &data);
            if(rc == MDBX_SUCCESS && data.iov_len == bytes_per_vector_) {
                visitor(numeric_id, static_cast<const void*>(data.iov_base));
                visited++;
            }
        }
        return visited;
    }

    void remove(MDBX_txn* txn, ndd::idInt numeric_id) {
        MDBX_val key{const_cast<ndd::idInt*>(&numeric_id), sizeof(ndd::idInt)};

        int rc = mdbx_del(txn, dbi_, &key, nullptr);
        if(rc != MDBX_SUCCESS && rc != MDBX_NOTFOUND) {
            throw std::runtime_error(std::string("Failed to delete vector data: ") + mdbx_strerror(rc));
        }
    }

    ndd::quant::QuantizationLevel getQuantLevel() const { return quant_level_; }
    size_t dimension() const { return vector_dim_; }
    size_t get_vector_size() const { return bytes_per_vector_; }

    // Allow access to LMDB environment for other operations
    MDBX_env* get_env() const { return env_; }
    MDBX_dbi get_dbi() const { return dbi_; }
    const std::string& get_index_id() const { return index_id_; }
};

// Handles meta storage
class MetaStore {
private:
    MDBX_env* env_;
    MDBX_dbi dbi_;
    std::string dbi_name_;

    void init_dbi() {
        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error(std::string("Failed to begin transaction: ") + mdbx_strerror(rc));
        }

        rc = mdbx_dbi_open(txn, dbi_name_.c_str(), MDBX_CREATE | MDBX_INTEGERKEY, &dbi_);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            throw std::runtime_error(std::string("Failed to open database: ") + mdbx_strerror(rc));
        }

        rc = mdbx_txn_commit(txn);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error("Failed to commit transaction: "
                                     + std::string(mdbx_strerror(rc)));
        }
    }

public:
    MetaStore(MDBX_env* env, const std::string& dbi_name) :
        env_(env),
        dbi_name_(dbi_name) {
        init_dbi();
    }

    ~MetaStore() {
        mdbx_dbi_close(env_, dbi_);
    }

    ndd::OperationResult<>
    store_meta_batch(MDBX_txn* txn,
                     const std::vector<std::pair<ndd::idInt, ndd::VectorMeta>>& batch) {
        if(batch.empty()) {
            return {SUCCESS, ""};
        }

        for(const auto& [numeric_id, meta] : batch) {
            msgpack::sbuffer sbuf;
            msgpack::pack(sbuf, meta);

            MDBX_val key{const_cast<ndd::idInt*>(&numeric_id), sizeof(ndd::idInt)};
            MDBX_val data{const_cast<char*>(sbuf.data()), sbuf.size()};

            int rc = mdbx_put(txn, dbi_, &key, &data, MDBX_UPSERT);
            if(rc != MDBX_SUCCESS) {
                return {100, std::string("Failed to store meta: ") + mdbx_strerror(rc)};
            }
        }
        return {SUCCESS, ""};
    }

    /**
     * Reads metadata for one numeric id through the caller's MDBX read
     * transaction.
     *
     * Return codes:
     * 0   = success; `value` holds the decoded VectorMeta
     * 100 = MDBX read failure (env / I/O); caller should log ERROR and
     *       return HTTP 500
     * 101 = numeric_id not present (recoverable; e.g. the result-population
     *       loop may skip the row)
     * 200 = corrupt msgpack payload; caller should log ERROR and return
     *       HTTP 500
     */
    ndd::OperationResult<ndd::VectorMeta>
    get_meta(MDBX_txn* txn, ndd::idInt numeric_id) const {
        MDBX_val key{const_cast<ndd::idInt*>(&numeric_id), sizeof(ndd::idInt)};
        MDBX_val data;

        int rc = mdbx_get(txn, dbi_, &key, &data);
        if(rc == MDBX_NOTFOUND) {
            return {101, "Meta not found", std::nullopt};
        }
        if(rc != MDBX_SUCCESS) {
            return {100,
                    std::string("Failed to read meta: ") + mdbx_strerror(rc),
                    std::nullopt};
        }

        try {
            auto oh = msgpack::unpack(reinterpret_cast<const char*>(data.iov_base),
                                      data.iov_len);
            return {SUCCESS, "", oh.get().as<ndd::VectorMeta>()};
        } catch(const std::exception& e) {
            return {200,
                    std::string("Corrupt meta payload: ") + e.what(),
                    std::nullopt};
        }
    }

    void remove(MDBX_txn* txn, ndd::idInt numeric_id) {
        MDBX_val key{const_cast<ndd::idInt*>(&numeric_id), sizeof(ndd::idInt)};

        int rc = mdbx_del(txn, dbi_, &key, nullptr);
        if(rc != MDBX_SUCCESS && rc != MDBX_NOTFOUND) {
            throw std::runtime_error(std::string("Failed to delete metadata: ") + mdbx_strerror(rc));
        }
    }

    /** Returns the MDBX env used by this meta store; legacy callers
     * open one-shot txns through it. */
    MDBX_env* env() const { return env_; }
};

// Main storage interface combining vector and meta stores
class VectorStorage {
private:
    std::string index_id_;
    std::unique_ptr<ndd::storage::SharedIndexEnv> shared_env_;
    std::unique_ptr<VectorStore> vector_store_;
    std::unique_ptr<MetaStore> meta_store_;

public:
    std::unique_ptr<Filter> filter_store_;

    VectorStorage(const std::string& base_path,
                  const std::string& index_id,
                  size_t vector_dim,
                  ndd::quant::QuantizationLevel quant_level) :
        index_id_(index_id) {
        if(std::filesystem::exists(std::filesystem::path(base_path)
                                   / settings::INDEX_MIGRATION_MARKER)) {
            throw std::runtime_error(settings::INCOMPLETE_INDEX_MIGRATION_ERROR);
        }
        shared_env_ = std::make_unique<ndd::storage::SharedIndexEnv>(
                base_path + "/vectors");

        MDBX_env* env = shared_env_->get();
        // If construction fails after some DBIs are opened, those named DBIs are harmless
        // durable catalog entries and the next open will reuse them.
        vector_store_ = std::make_unique<VectorStore>(
                env, vector_dim, quant_level, index_id_, settings::DEFAULT_SUBINDEX);
        meta_store_ = std::make_unique<MetaStore>(env, "vector_meta");
        filter_store_ = std::make_unique<Filter>(env, index_id_, "filter_schema");
    }
    VectorStore::Cursor getCursor() { return vector_store_->getCursor(); }

    bool matches_filter(ndd::idInt numeric_id,
                        const ndd::VectorMeta& meta,
                        const nlohmann::json& filter_query) {
        if(filter_query.empty()) {
            return true;
        }

        // 1. Fast Pass: Check Numeric Filters using Index
        bool has_non_numeric = false;

        for(const auto& condition : filter_query) {
            if(!condition.is_object() || condition.size() != 1) {
                continue;
            }
            const auto& field = condition.begin().key();
            const auto& expr = condition.begin().value();
            if(!expr.is_object() || expr.size() != 1) {
                continue;
            }

            const std::string op = expr.begin().key();
            const auto& val = expr.begin().value();

            bool is_numeric_query = false;
            if(op == "$range") {
                is_numeric_query = true;
            } else if(op == "$eq" && (val.is_number())) {
                is_numeric_query = true;
            } else if(op == "$in" && val.is_array() && !val.empty() && val[0].is_number()) {
                is_numeric_query = true;
            }

            if(is_numeric_query) {
                auto check_result = filter_store_->check_numeric(field, numeric_id, op, val);
                if(!check_result.ok() || !check_result.value_or_throw()) {
                    return false;
                }
            } else {
                has_non_numeric = true;
            }
        }

        if(!has_non_numeric) {
            return true;
        }

        try {
            // Parse the metadata associated with the vector
            nlohmann::json meta_filter = nlohmann::json::parse(meta.filter);

            // Each filter clause is ANDed
            for(const auto& condition : filter_query) {
                if(!condition.is_object() || condition.size() != 1) {
                    continue;  // Skip malformed conditions
                }

                const auto& field = condition.begin().key();
                const auto& expr = condition.begin().value();

                if(!expr.is_object() || expr.size() != 1) {
                    continue;
                }

                const std::string op = expr.begin().key();
                const auto& val = expr.begin().value();

                // Skip numeric queries as they are already checked
                bool is_numeric_query = false;
                if(op == "$range") {
                    is_numeric_query = true;
                } else if(op == "$eq" && (val.is_number())) {
                    is_numeric_query = true;
                } else if(op == "$in" && val.is_array() && !val.empty() && val[0].is_number()) {
                    is_numeric_query = true;
                }

                if(is_numeric_query) {
                    continue;
                }

                // If field is not present in the vector's metadata
                if(!meta_filter.contains(field)) {
                    return false;
                }

                const auto& actual_value = meta_filter[field];

                if(op == "$eq") {
                    if(actual_value != val) {
                        return false;
                    }
                } else if(op == "$in") {
                    if(!val.is_array()
                       || std::find(val.begin(), val.end(), actual_value) == val.end()) {
                        return false;
                    }
                } else {
                    continue;
                }
            }

            return true;

        } catch(const std::exception& e) {
            // std::cerr << "Error matching filter: " << e.what() << std::endl;
            return false;
        }
    }

    ndd::OperationResult<>
    store_prepared_batches(
            MDBX_txn* txn,
            const std::vector<std::pair<ndd::idInt, std::vector<uint8_t>>>& vector_batch,
            const std::vector<std::pair<ndd::idInt, ndd::VectorMeta>>& meta_batch,
            const std::vector<std::pair<ndd::idInt, std::string>>& filter_batch,
            bool* filter_schema_changed = nullptr) {
        auto vector_result = vector_store_->store_vectors_batch(txn, vector_batch);
        if(!vector_result.ok()) {
            return vector_result;
        }
        auto meta_result = meta_store_->store_meta_batch(txn, meta_batch);
        if(!meta_result.ok()) {
            return meta_result;
        }

        if(!filter_batch.empty()) {
            auto filter_result = filter_store_->add_filters_from_json_batch(
                    txn, filter_batch, filter_schema_changed);
            if(!filter_result.ok()) {
                return filter_result;
            }
        }

        return {SUCCESS, ""};
    }

    ndd::OperationResult<>
    store_vectors_batch(MDBX_txn* txn,
            const std::vector<std::pair<ndd::idInt, QuantVectorObject>>& vectors,
            bool* filter_schema_changed = nullptr) {
        if(vectors.empty()) {
            return {SUCCESS, ""};
        }

        std::vector<std::pair<ndd::idInt, std::vector<uint8_t>>> vector_batch;
        std::vector<std::pair<ndd::idInt, ndd::VectorMeta>> meta_batch;
        std::vector<std::pair<ndd::idInt, std::string>> filter_batch;

        vector_batch.reserve(vectors.size());
        meta_batch.reserve(vectors.size());
        filter_batch.reserve(vectors.size());

        for(const auto& [numeric_id, quant_obj] : vectors) {
            std::vector<uint8_t> vector_bytes = quant_obj.quant_vector;

            ndd::VectorMeta meta;
            meta.id = quant_obj.id;
            meta.filter = quant_obj.filter;
            meta.meta = quant_obj.meta;
            meta.norm = quant_obj.norm;

            vector_batch.emplace_back(numeric_id, std::move(vector_bytes));
            meta_batch.emplace_back(numeric_id, std::move(meta));

            if(!quant_obj.filter.empty()) {
                filter_batch.emplace_back(numeric_id, quant_obj.filter);
            }
        }

        return store_prepared_batches(
                txn, vector_batch, meta_batch, filter_batch, filter_schema_changed);
    }

    /**
     * Opens a one-shot RDONLY transaction on the vector store env and
     * forwards to `VectorStore::get_vector_bytes`. Used by legacy
     * code paths that do not own a snapshot (WAL recovery, legacy
     * getVector). Return codes propagate from the underlying store.
     */
    ndd::OperationResult<std::vector<uint8_t>>
    get_vector(ndd::idInt numeric_id) const {
        MDBX_txn* txn = nullptr;
        int rc = mdbx_txn_begin(vector_store_->get_env(), nullptr, MDBX_TXN_RDONLY, &txn);
        if(rc != MDBX_SUCCESS) {
            return {102,
                    std::string("Failed to begin vector read txn: ") + mdbx_strerror(rc),
                    std::nullopt};
        }
        auto result = vector_store_->get_vector_bytes(txn, numeric_id);
        mdbx_txn_abort(txn);
        return result;
    }

    ndd::OperationResult<std::vector<uint8_t>>
    get_vector(MDBX_txn* txn, ndd::idInt numeric_id) const {
        return vector_store_->get_vector_bytes(txn, numeric_id);
    }

    /**
     * Buffer-overload fetch for HNSW. `txn == nullptr` is the write-path
     * fallback (addPoint runs on worker threads with no request
     * snapshot); the store opens its own RDONLY in that case. A non-null
     * txn comes from the request-scoped snapshot in search.
     */
    bool get_vector(MDBX_txn* txn, ndd::idInt numeric_id, uint8_t* buffer) const {
        return txn != nullptr
                       ? vector_store_->get_vector_bytes(txn, numeric_id, buffer)
                       : vector_store_->get_vector_bytes(numeric_id, buffer);
    }

    /**
     * Batch fetch for the HNSW batch fetcher. `txn == nullptr` opens a
     * one-shot RDONLY; non-null reuses the caller's snapshot.
     */
    size_t get_vectors_batch_into(MDBX_txn* txn,
                                      const ndd::idInt* labels,
                                      uint8_t* buffers,
                                      bool* success,
                                      size_t count) const {
        return txn != nullptr
                       ? vector_store_->get_vectors_batch_into(txn, labels, buffers, success, count)
                       : vector_store_->get_vectors_batch_into(labels, buffers, success, count);
    }

    template <typename Visitor>
    size_t visit_vectors_by_ids(MDBX_txn* txn,
                                    const std::vector<ndd::idInt>& numeric_ids,
                                    Visitor&& visitor) const {
        return vector_store_->visit_vectors_by_ids(
                txn,
                numeric_ids,
                std::forward<Visitor>(visitor));
    }

    ndd::OperationResult<ndd::VectorMeta>
    get_meta(MDBX_txn* txn, ndd::idInt numeric_id) const {
        return meta_store_->get_meta(txn, numeric_id);
    }

    MDBX_env* shared_env() const { return shared_env_->get(); }
    ndd::OperationResult<> reload_filter_schema_cache() {
        if(!filter_store_) {
            return {SUCCESS, ""};
        }
        return filter_store_->reload_schema_cache();
    }

    ndd::OperationResult<> deletePoint(MDBX_txn* txn, ndd::idInt numeric_id) {
        try {
            auto meta_result = meta_store_->get_meta(txn, numeric_id);
            if(!meta_result.ok()) {
                return {meta_result.code, meta_result.message};
            }
            return deletePoint(txn, numeric_id, *meta_result.value);
        } catch(const std::exception& e) {
            return {100, std::string("Failed to remove vector and metadata: ") + e.what()};
        }
    }

    ndd::OperationResult<> deletePoint(MDBX_txn* txn,
                                           ndd::idInt numeric_id,
                                           const ndd::VectorMeta& meta) {
        try {
            if(!meta.filter.empty()) {
                auto filter_result =
                        filter_store_->remove_filters_from_json(txn, numeric_id, meta.filter);
                if(!filter_result.ok()) {
                    return filter_result;
                }
            }
            vector_store_->remove(txn, numeric_id);
            meta_store_->remove(txn, numeric_id);
            return {SUCCESS, ""};
        } catch(const std::exception& e) {
            return {100, std::string("Failed to remove vector and metadata: ") + e.what()};
        }
    }

    ndd::OperationResult<> deleteFilter(MDBX_txn* txn,
                                            ndd::idInt numeric_id,
                                            const std::string& filter) {
        return filter_store_->remove_filters_from_json(txn, numeric_id, filter);
    }

    ndd::OperationResult<> updateFilter(MDBX_txn* txn,
                                            ndd::idInt numeric_id,
                                            const std::string& new_filter_json,
                                            bool* filter_schema_changed = nullptr) {
        auto meta_result = meta_store_->get_meta(txn, numeric_id);
        if(!meta_result.ok()) {
            return {meta_result.code, meta_result.message};
        }
        auto& meta = *meta_result.value;

        if(!meta.filter.empty()) {
            auto remove_result =
                    filter_store_->remove_filters_from_json(txn, numeric_id, meta.filter);
            if(!remove_result.ok()) {
                return remove_result;
            }
        }

        meta.filter = new_filter_json;
        auto meta_store_result = meta_store_->store_meta_batch(txn, {{numeric_id, meta}});
        if(!meta_store_result.ok()) {
            return meta_store_result;
        }

        if(!new_filter_json.empty()) {
            auto add_result =
                    filter_store_->add_filters_from_json(
                            txn, numeric_id, new_filter_json, filter_schema_changed);
            if(!add_result.ok()) {
                return add_result;
            }
        }
        return {SUCCESS, ""};
    }

    ndd::quant::QuantizationLevel getQuantLevel() const { return vector_store_->getQuantLevel(); }
    size_t dimension() const { return vector_store_->dimension(); }
    size_t get_vector_size() const { return vector_store_->get_vector_size(); }
};
