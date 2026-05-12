#pragma once

#include "mdbx/mdbx.h"
#include "log.hpp"
#include "../quant/dispatch.hpp"
#include "../filter/filter.hpp"
#include "../core/types.hpp"
#include "json/nlohmann_json.hpp"
#include "msgpack_ndd.hpp"
#include "quant_vector.hpp"
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
    std::string path_;
    size_t vector_dim_;
    ndd::quant::QuantizationLevel quant_level_;
    size_t bytes_per_vector_;

    void init_environment() {
        int rc = mdbx_env_create(&env_);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error(std::string("Failed to create LMDB env: ") + mdbx_strerror(rc));
        }

        // Set geometry for auto-grow using the vector map size settings
        rc = mdbx_env_set_geometry(env_,
                                   -1,  // lower size bound (use default)
                                   1ULL << settings::VECTOR_MAP_SIZE_BITS,      // current/now size
                                   1ULL << settings::VECTOR_MAP_SIZE_MAX_BITS,  // upper size bound
                                   1ULL << settings::VECTOR_MAP_SIZE_BITS,      // growth step
                                   -1,   // shrink threshold (use default)
                                   -1);  // pagesize (use default)
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error(std::string("Failed to set geometry: ") + mdbx_strerror(rc));
        }

        mdbx_env_set_maxdbs(env_, settings::MAX_NR_SUBINDEX);

        rc = mdbx_env_open(
                env_, path_.c_str(), MDBX_WRITEMAP | MDBX_MAPASYNC | MDBX_NORDAHEAD, 0664);
        if(rc != MDBX_SUCCESS) {
            // throw std::runtime_error("Failed to open environment");
            throw std::runtime_error(std::string("Failed to open environment: ") + mdbx_strerror(rc));

        }

        MDBX_txn* txn;
        rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error(std::string("Failed to begin transaction: ") + mdbx_strerror(rc));
        }

        rc = mdbx_dbi_open(txn, settings::DEFAULT_SUBINDEX.c_str(), MDBX_CREATE | MDBX_INTEGERKEY, &dbi_);
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
    VectorStore(const std::string& path,
                size_t vector_dim,
                ndd::quant::QuantizationLevel quant_level,
                const std::string& index_id) :
        index_id_(index_id),
        path_(path),
        vector_dim_(vector_dim),
        quant_level_(quant_level) {
        bytes_per_vector_ =
                ndd::quant::get_quantizer_dispatch(quant_level_).get_storage_size(vector_dim);
        std::filesystem::create_directories(path);
        init_environment();
    }

    ~VectorStore() {
        mdbx_dbi_close(env_, dbi_);
        mdbx_env_close(env_);
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

    void store_vector_bytes(ndd::idInt id, const std::vector<uint8_t>& vec) {
        store_vectors_batch({{id, vec}});
    }

    std::vector<uint8_t> get_vector_bytes(ndd::idInt numeric_id) const {
        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &txn);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error(std::string("Failed to begin transaction: ") + mdbx_strerror(rc));
        }

        try {
            MDBX_val key{const_cast<ndd::idInt*>(&numeric_id), sizeof(ndd::idInt)};
            MDBX_val data;

            rc = mdbx_get(txn, dbi_, &key, &data);
            if(rc == MDBX_NOTFOUND) {
                mdbx_txn_abort(txn);
                return std::vector<uint8_t>();
            }

            std::vector<uint8_t> result(static_cast<uint8_t*>(data.iov_base),
                                        static_cast<uint8_t*>(data.iov_base) + data.iov_len);

            mdbx_txn_abort(txn);
            return result;
        } catch(...) {
            mdbx_txn_abort(txn);
            throw;
        }
    }

    bool get_vector_bytes(ndd::idInt numeric_id, uint8_t* buffer) const {
        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &txn);
        if(rc != MDBX_SUCCESS) {
            return false;
        }

        try {
            MDBX_val key{const_cast<ndd::idInt*>(&numeric_id), sizeof(ndd::idInt)};
            MDBX_val data;

            rc = mdbx_get(txn, dbi_, &key, &data);
            if(rc == MDBX_NOTFOUND) {
                mdbx_txn_abort(txn);
                return false;
            }

            if(data.iov_len != bytes_per_vector_) {
                mdbx_txn_abort(txn);
                // Warning: data size mismatch.
                // We could log this but for now just fail or copy what is there if smaller?
                // Safer to fail or copy min to avoid overflow if buffer is assumed to be
                // bytes_per_vector_
                return false;
            }

            std::memcpy(buffer, data.iov_base, data.iov_len);

            mdbx_txn_abort(txn);
            return true;
        } catch(...) {
            mdbx_txn_abort(txn);
            return false;
        }
    }

    // Batch fetch: retrieves multiple vectors in a single MDBX read transaction.
    // labels: array of external numeric IDs to fetch
    // buffers: pre-allocated flat buffer of size (count * bytes_per_vector_)
    // success: output array of bool indicating which fetches succeeded
    // Returns number of successful fetches
    size_t get_vectors_batch_into(const ndd::idInt* labels, uint8_t* buffers,
                                  bool* success, size_t count) const {
        if(count == 0) return 0;

        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &txn);
        if(rc != MDBX_SUCCESS) {
            for(size_t i = 0; i < count; i++) success[i] = false;
            return 0;
        }

        size_t fetched = 0;
        for(size_t i = 0; i < count; i++) {
            MDBX_val key{const_cast<ndd::idInt*>(&labels[i]), sizeof(ndd::idInt)};
            MDBX_val data;
            rc = mdbx_get(txn, dbi_, &key, &data);
            if(rc == MDBX_SUCCESS && data.iov_len == bytes_per_vector_) {
                std::memcpy(buffers + i * bytes_per_vector_, data.iov_base, bytes_per_vector_);
                success[i] = true;
                fetched++;
            } else {
                success[i] = false;
            }
        }

        mdbx_txn_abort(txn);
        return fetched;
    }

    // Batch operations with raw bytes
    void
    store_vectors_batch(const std::vector<std::pair<ndd::idInt, std::vector<uint8_t>>>& batch) {
        if(batch.empty()) {
            return;
        }

        auto try_commit = [&](MDBX_txn* txn) {
            int rc = mdbx_txn_commit(txn);
            if(rc != MDBX_SUCCESS) {
                throw std::runtime_error("Failed to commit transaction: "
                                         + std::string(mdbx_strerror(rc)));
            }
        };

        auto write_batch = [&](MDBX_txn* txn) -> int {
            for(const auto& [numeric_id, vector_bytes] : batch) {
                if(vector_bytes.size() != bytes_per_vector_) {
                    throw std::runtime_error("Vector byte size mismatch");
                }

                MDBX_val key{const_cast<ndd::idInt*>(&numeric_id), sizeof(ndd::idInt)};
                MDBX_val data{const_cast<uint8_t*>(vector_bytes.data()), vector_bytes.size()};

                int rc = mdbx_put(txn, dbi_, &key, &data, MDBX_UPSERT);
                if(rc != MDBX_SUCCESS) {
                    return rc;
                }
            }
            return MDBX_SUCCESS;
        };

        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error(std::string("Failed to begin transaction: ") + mdbx_strerror(rc));
        }

        rc = write_batch(txn);
        // MDBX auto-grows, no manual resize needed
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            throw std::runtime_error(std::string("Failed to store vector: ") + mdbx_strerror(rc));
        }

        try_commit(txn);
    }

    std::vector<std::pair<ndd::idInt, std::vector<uint8_t>>>
    get_vectors_batch(const std::vector<ndd::idInt>& numeric_ids) const {
        std::vector<std::pair<ndd::idInt, std::vector<uint8_t>>> result;
        if(numeric_ids.empty()) {
            return result;
        }

        result.reserve(numeric_ids.size());

        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &txn);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error(std::string("Failed to begin transaction: ") + mdbx_strerror(rc));
        }

        try {
            for(const auto& numeric_id : numeric_ids) {
                MDBX_val key{const_cast<ndd::idInt*>(&numeric_id), sizeof(ndd::idInt)};
                MDBX_val data;

                rc = mdbx_get(txn, dbi_, &key, &data);
                if(rc == MDBX_SUCCESS) {  // Found the vector
                    std::vector<uint8_t> bytes(static_cast<uint8_t*>(data.iov_base),
                                               static_cast<uint8_t*>(data.iov_base) + data.iov_len);
                    result.emplace_back(numeric_id, std::move(bytes));
                }
            }

            mdbx_txn_abort(txn);
            return result;
        } catch(...) {
            mdbx_txn_abort(txn);
            throw;
        }
    }

    template <typename Visitor>
    size_t visit_vectors_by_ids(const std::vector<ndd::idInt>& numeric_ids,
                                Visitor&& visitor) const {
        if(numeric_ids.empty()) {
            return 0;
        }

        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &txn);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error(std::string("Failed to begin transaction: ") + mdbx_strerror(rc));
        }

        size_t visited = 0;
        try {
            for(const auto& numeric_id : numeric_ids) {
                MDBX_val key{const_cast<ndd::idInt*>(&numeric_id), sizeof(ndd::idInt)};
                MDBX_val data;

                rc = mdbx_get(txn, dbi_, &key, &data);
                if(rc == MDBX_SUCCESS && data.iov_len == bytes_per_vector_) {
                    visitor(numeric_id, static_cast<const void*>(data.iov_base));
                    visited++;
                }
            }

            mdbx_txn_abort(txn);
            return visited;
        } catch(...) {
            mdbx_txn_abort(txn);
            throw;
        }
    }

    void remove(ndd::idInt numeric_id) {
        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error(std::string("Failed to begin transaction: ") + mdbx_strerror(rc));
        }

        try {
            MDBX_val key{const_cast<ndd::idInt*>(&numeric_id), sizeof(ndd::idInt)};

            rc = mdbx_del(txn, dbi_, &key, nullptr);
            if(rc != MDBX_SUCCESS && rc != MDBX_NOTFOUND) {
                throw std::runtime_error(std::string("Failed to delete vector data: ") + mdbx_strerror(rc));
            }

            rc = mdbx_txn_commit(txn);
            if(rc != MDBX_SUCCESS) {
                throw std::runtime_error(std::string("Failed to commit vector deletion: ") + mdbx_strerror(rc));
            }
        } catch(...) {
            mdbx_txn_abort(txn);
            throw;
        }
    }

    ndd::quant::QuantizationLevel getQuantLevel() const { return quant_level_; }
    size_t dimension() const { return vector_dim_; }
    size_t get_vector_size() const { return bytes_per_vector_; }

    // Allow access to LMDB environment for other operations
    MDBX_env* get_env() const { return env_; }
    MDBX_dbi get_dbi() const { return dbi_; }
};

// Handles meta storage
class MetaStore {
private:
    MDBX_env* env_;
    MDBX_dbi dbi_;
    std::string path_;

    void init_environment() {
        int rc = mdbx_env_create(&env_);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error(std::string("Failed to create LMDB env: ") + mdbx_strerror(rc));
        }

        // Set geometry for auto-grow
        rc = mdbx_env_set_geometry(
                env_,
                -1,                                            // lower size bound (use default)
                1ULL << settings::METADATA_MAP_SIZE_BITS,      // current/now size
                1ULL << settings::METADATA_MAP_SIZE_MAX_BITS,  // upper size bound
                1ULL << settings::METADATA_MAP_SIZE_BITS,      // growth step
                -1,                                            // shrink threshold (use default)
                -1);                                           // pagesize (use default)
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error(std::string("Failed to set geometry: ") + mdbx_strerror(rc));
        }

        rc = mdbx_env_open(env_,
                           path_.c_str(),
                           MDBX_NOSUBDIR | MDBX_WRITEMAP | MDBX_MAPASYNC | MDBX_NORDAHEAD,
                           0664);
        if(rc != MDBX_SUCCESS) {
            // throw std::runtime_error("Failed to open environment");
            throw std::runtime_error(std::string("Failed to open environment: ") + mdbx_strerror(rc));
        }

        MDBX_txn* txn;
        rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error(std::string("Failed to begin transaction: ") + mdbx_strerror(rc));
        }

        rc = mdbx_dbi_open(txn, nullptr, MDBX_CREATE | MDBX_INTEGERKEY, &dbi_);
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
    MetaStore(const std::string& path) :
        path_(path) {
        std::filesystem::create_directories(path);
        init_environment();
    }

    ~MetaStore() {
        mdbx_dbi_close(env_, dbi_);
        mdbx_env_close(env_);
    }

    void store_meta_batch(const std::vector<std::pair<ndd::idInt, ndd::VectorMeta>>& batch) {
        if(batch.empty()) {
            return;
        }

        auto try_commit = [&](MDBX_txn* txn) {
            int rc = mdbx_txn_commit(txn);
            if(rc != MDBX_SUCCESS) {
                throw std::runtime_error("Failed to commit transaction: "
                                         + std::string(mdbx_strerror(rc)));
            }
        };

        auto write_batch = [&](MDBX_txn* txn) -> int {
            for(const auto& [numeric_id, meta] : batch) {
                msgpack::sbuffer sbuf;
                msgpack::pack(sbuf, meta);

                MDBX_val key{const_cast<ndd::idInt*>(&numeric_id), sizeof(ndd::idInt)};
                MDBX_val data{const_cast<char*>(sbuf.data()), sbuf.size()};

                int rc = mdbx_put(txn, dbi_, &key, &data, MDBX_UPSERT);
                if(rc != MDBX_SUCCESS) {
                    return rc;
                }
            }
            return MDBX_SUCCESS;
        };

        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error(std::string("Failed to begin transaction: ") + mdbx_strerror(rc));
        }

        rc = write_batch(txn);
        // MDBX auto-grows, no manual resize needed
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            throw std::runtime_error(std::string("Failed to store meta: ") + mdbx_strerror(rc));
        }

        try_commit(txn);
    }

    void store_meta(ndd::idInt id, const ndd::VectorMeta& meta) { store_meta_batch({{id, meta}}); }
    ndd::VectorMeta get_meta(ndd::idInt numeric_id) const {
        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &txn);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error(std::string("Failed to begin transaction: ") + mdbx_strerror(rc));
        }

        try {
            MDBX_val key{const_cast<ndd::idInt*>(&numeric_id), sizeof(ndd::idInt)};
            MDBX_val data;

            rc = mdbx_get(txn, dbi_, &key, &data);
            if(rc == MDBX_NOTFOUND) {
                mdbx_txn_abort(txn);
                throw std::runtime_error("Meta not found");
            }
            auto oh = msgpack::unpack(reinterpret_cast<const char*>(data.iov_base), data.iov_len);
            auto meta = oh.get().as<ndd::VectorMeta>();
            mdbx_txn_abort(txn);
            return meta;
        } catch(...) {
            mdbx_txn_abort(txn);
            throw;
        }
    }

    void remove(ndd::idInt numeric_id) {
        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error(std::string("Failed to begin transaction: ") + mdbx_strerror(rc));
        }

        try {
            MDBX_val key{const_cast<ndd::idInt*>(&numeric_id), sizeof(ndd::idInt)};

            rc = mdbx_del(txn, dbi_, &key, nullptr);
            if(rc != MDBX_SUCCESS && rc != MDBX_NOTFOUND) {
                throw std::runtime_error(std::string("Failed to delete metadata: ") + mdbx_strerror(rc));
            }

            rc = mdbx_txn_commit(txn);
            if(rc != MDBX_SUCCESS) {
                throw std::runtime_error(std::string("Failed to commit metadata deletion: ") + mdbx_strerror(rc));
            }
        } catch(...) {
            mdbx_txn_abort(txn);
            throw;
        }
    }
};

// Main storage interface combining vector and meta stores
class VectorStorage {
private:
    std::string index_id_;
    std::unique_ptr<VectorStore> vector_store_;
    std::unique_ptr<MetaStore> meta_store_;

public:
    std::unique_ptr<Filter> filter_store_;

    VectorStorage(const std::string& base_path,
                  const std::string& index_id,
                  size_t vector_dim,
                  ndd::quant::QuantizationLevel quant_level) :
        index_id_(index_id) {
        vector_store_ = std::make_unique<VectorStore>(
                base_path + "/vectors", vector_dim, quant_level, index_id_);
        meta_store_ = std::make_unique<MetaStore>(base_path + "/meta");
        filter_store_ = std::make_unique<Filter>(base_path + "/filters", index_id_);
    }
    VectorStore::Cursor getCursor() { return vector_store_->getCursor(); }
    /*
     * Returns numeric ids matching legacy category filter pairs.
     *
     * Return codes:
     * 0 = success
     * 100-199 = propagated MDBX/storage failure from filter store
     * 200-299 = propagated corruption/invariant failure from filter store
     */
    ndd::OperationResult<std::vector<ndd::idInt>> getIdsMatchingFilters(
            const std::vector<std::pair<std::string, std::string>>& filter_pairs) const {
        auto bitmap_result = filter_store_->combine_filters_and(filter_pairs);
        if(!bitmap_result.ok()) {
            return {bitmap_result.code, bitmap_result.message};
        }

        std::vector<ndd::idInt> numeric_ids;
        bitmap_result.value_or_throw().iterate(
                [](ndd::idInt value, void* ptr) -> bool {
                    auto* ids = static_cast<std::vector<ndd::idInt>*>(ptr);
                    ids->push_back(value);
                    return true;
                },
                &numeric_ids);
        return {SUCCESS, "", std::move(numeric_ids)};
    }

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

    /*
     * Stores vectors, metadata, and associated filter documents for one pre-quantized batch.
     *
     * High-level shape:
     *   1. Cleanup pass: for every entry that is an upsert of an already-live numeric_id,
     *      read its previous meta.filter and remove the corresponding entries from the
     *      filter index. Without this step every prior filter value would remain matchable
     *      via the category / numeric indexes even though the vector now carries a
     *      different filter document.
     *   2. Write the new vector bytes (vector_store_).
     *   3. Overwrite meta (meta_store_) — this is where meta.filter takes its new value.
     *   4. Add the new filter index entries (filter_store_).
     *
     * `is_new_to_db[i]` mirrors the id_mapper's "did this str_id already exist?" signal:
     *   - true  : the str_id was NOT in the id_mapper when this batch began. That covers
     *             both genuinely fresh ids and reuses of a previously-deleted slot. In the
     *             reuse case the old filter index entries were already scrubbed at delete
     *             time (see deletePoint / deleteFilter), so there is nothing to clean up.
     *   - false : the str_id was already mapped to this numeric_id, i.e. this is an upsert
     *             of a live point. Its prior filter index entries are still present and
     *             must be removed before the new filter is added — otherwise queries with
     *             the OLD filter value will keep matching this id.
     *
     * If `is_new_to_db` is empty (default), the caller has not opted into the cleanup
     * contract and we conservatively skip the cleanup pass entirely. This preserves the
     * pre-fix semantics for any caller that has not yet been updated. New callers should
     * always pass the id_mapper signal.
     *
     * Atomicity, by design (and constrained by the filter roadmap):
     *   - The cleanup pass, vector_store, meta_store, and filter_store writes are each
     *     internally transactional, but the four phases are NOT a single distributed
     *     transaction. A crash between phases leaves torn state (e.g. old filter index
     *     entries removed but new vector / meta not written).
     *   - `updateFilter` already operates under the same constraint; cross-store ACID
     *     work is tracked separately in the filter roadmap.
     *
     * Limitations:
     *   - This patch only prevents NEW stale filter index entries from accumulating. It
     *     does not retroactively scrub entries left behind by previous upserts written
     *     before the fix landed. A targeted rebuild is required to clean historical drift.
     *   - The cleanup pass issues one MDBX read per upsert id to fetch the prior meta.
     *     For a batch that is mostly fresh inserts (the common case) this cost stays low;
     *     a batch dominated by upserts will see N extra reads.
     *
     * Return codes:
     * 0 = success
     * 1-99 = propagated filter validation failure from filter store, or argument shape
     * 100-199 = propagated MDBX/storage failure from filter or meta store
     * 200-299 = propagated corruption/invariant failure from filter store
     */
    ndd::OperationResult<>
    store_vectors_batch(const std::vector<std::pair<ndd::idInt, QuantVectorObject>>& vectors,
                        const std::vector<bool>& is_new_to_db = {}) {
        if(vectors.empty()) {
            return {SUCCESS, ""};
        }

        /*
         * `have_flags` distinguishes the new opt-in cleanup contract from legacy callers
         * that pass nothing. When flags are present they must match the batch one-to-one;
         * a size mismatch is a programmer error and we surface it rather than silently
         * applying the cleanup to a misaligned subset of the batch.
         */
        const bool have_flags = !is_new_to_db.empty();
        if(have_flags && is_new_to_db.size() != vectors.size()) {
            LOG_ERROR(1223,
                      index_id_,
                      "store_vectors_batch: is_new_to_db size mismatch ("
                              << is_new_to_db.size() << " vs " << vectors.size() << ")");
            return {1, "store_vectors_batch: is_new_to_db size mismatch"};
        }

        /*
         * ---- Phase 1: upsert cleanup ----
         *
         * For each entry the id_mapper flagged as already-live (is_new_to_db == false),
         * read its previous meta.filter and remove the corresponding filter index entries.
         * We do this BEFORE writing the new meta in phase 3, because the new meta would
         * overwrite the only record of which filter document used to be associated with
         * this id — and once that record is gone, we cannot tell the filter index which
         * entries belong to "the old filter" anymore.
         *
         * Skipping the entire pass when flags are absent is intentional: without the
         * id_mapper signal we cannot tell apart a fresh slot (no cleanup needed) from
         * an upsert (cleanup required), so trying to "always clean up" would issue
         * get_meta() on every fresh id and either throw or short-circuit on empty.
         */
        if(have_flags) {
            for(size_t i = 0; i < vectors.size(); ++i) {
                // Fresh slot or reuse of a deleted slot — nothing to clean.
                if(is_new_to_db[i]) {
                    continue;
                }
                ndd::idInt numeric_id = vectors[i].first;

                /*
                 * Fetch the prior filter document. This is the only point where we can
                 * recover what the OLD filter was; once we overwrite meta in phase 3 it
                 * is gone. We pay one MDBX read per upserted id here.
                 */
                std::string old_filter;
                try {
                    old_filter = meta_store_->get_meta(numeric_id).filter;
                } catch(const std::exception& e) {
                    /*
                     * Mapper says this numeric_id is already live, but its meta cannot
                     * be loaded. That contradiction means an earlier write was torn
                     * (e.g. id_mapper got committed but meta did not). Surfacing this
                     * is preferable to silently overwriting: the operator can then
                     * decide whether to repair via rebuild, and the user gets HTTP 500
                     * instead of a "successful" write that leaves the index inconsistent.
                     */
                    LOG_ERROR(1224,
                              index_id_,
                              "Upsert cleanup: meta missing for existing numeric_id "
                                      << numeric_id << ": " << e.what());
                    return {103,
                            "Upsert cleanup: meta missing for numeric_id "
                                    + std::to_string(numeric_id) + ": " + e.what()};
                }

                /*
                 * The previous version of this vector had no filter document, so there
                 * is nothing in the filter index to remove. Common after deleteFilter
                 * or after an insert that omitted filters entirely.
                 */
                if(old_filter.empty()) {
                    continue;
                }

                /*
                 * Drop the old filter's category and numeric index entries for this id.
                 * Any failure here aborts the batch — partially-cleaned state would be
                 * worse than the original drift, because the bitmap would no longer
                 * match meta.filter even temporarily.
                 */
                auto remove_result =
                        filter_store_->remove_filters_from_json(numeric_id, old_filter);
                if(!remove_result.ok()) {
                    LOG_ERROR(1225,
                              index_id_,
                              "Upsert cleanup: failed to remove old filter for numeric_id "
                                      << numeric_id << " (code=" << remove_result.code
                                      << "): " << remove_result.message);
                    return remove_result;
                }
            }
        }

        /*
         * ---- Phase 2 prep: assemble per-store batches from the input ----
         * We unzip the input vector into one batch per backing store so each store can
         * commit its batch in a single MDBX transaction.
         */
        std::vector<std::pair<ndd::idInt, std::vector<uint8_t>>> vector_batch;
        std::vector<std::pair<ndd::idInt, ndd::VectorMeta>> meta_batch;
        std::vector<std::pair<ndd::idInt, std::string>> filter_batch;

        vector_batch.reserve(vectors.size());
        meta_batch.reserve(vectors.size());
        filter_batch.reserve(vectors.size());

        for(const auto& [numeric_id, quant_obj] : vectors) {
            /*
             * The vector bytes were quantized upstream (HNSW path) so we can store them
             * verbatim; copying here only because store_vectors_batch wants ownership.
             */
            std::vector<uint8_t> vector_bytes = quant_obj.quant_vector;

            /*
             * VectorMeta is the durable record of what filter document this id carries.
             * After phase 3 overwrites it, get_meta() returns these new values and the
             * upsert-cleanup pass on a future upsert will read THIS old_filter to drop.
             */
            ndd::VectorMeta meta;
            meta.id = quant_obj.id;
            meta.filter = quant_obj.filter;
            meta.meta = quant_obj.meta;
            meta.norm = quant_obj.norm;

            vector_batch.emplace_back(numeric_id, std::move(vector_bytes));
            meta_batch.emplace_back(numeric_id, std::move(meta));

            /*
             * Empty filter docs are intentionally excluded: there is nothing to add to
             * the filter index. Meta.filter still records the empty string so the next
             * upsert's cleanup pass correctly observes "no prior filter to remove".
             */
            if(!quant_obj.filter.empty()) {
                filter_batch.emplace_back(numeric_id, quant_obj.filter);
            }
        }

        // Phase 2: write vector bytes. One MDBX txn for the whole batch inside vector_store_.
        vector_store_->store_vectors_batch(vector_batch);

        /*
         * Phase 3: overwrite meta. This makes meta.filter authoritative for the new
         * state. After this point, get_meta() returns the new filter document — any
         * later upsert-cleanup pass on this id will use it as the "old" value to
         * remove from the filter index.
         */
        meta_store_->store_meta_batch(meta_batch);

        /*
         * Phase 4: add new filter index entries. The filter store iterates the batched
         * JSON documents and inserts the appropriate category and numeric index
         * entries for each id.
         */
        if(!filter_batch.empty()) {
            auto filter_result = filter_store_->add_filters_from_json_batch(filter_batch);
            if(!filter_result.ok()) {
                return filter_result;
            }
        }
        return {SUCCESS, ""};
    }

    std::vector<uint8_t> get_vector(ndd::idInt numeric_id) const {
        return vector_store_->get_vector_bytes(numeric_id);
    }

    bool get_vector(ndd::idInt numeric_id, uint8_t* buffer) const {
        return vector_store_->get_vector_bytes(numeric_id, buffer);
    }

    // Batch fetch: multiple vectors in one MDBX txn
    size_t get_vectors_batch_into(const ndd::idInt* labels, uint8_t* buffers,
                                  bool* success, size_t count) const {
        return vector_store_->get_vectors_batch_into(labels, buffers, success, count);
    }

    std::vector<std::pair<ndd::idInt, std::vector<uint8_t>>>
    get_vectors_batch(const std::vector<ndd::idInt>& numeric_ids) const {
        return vector_store_->get_vectors_batch(numeric_ids);
    }

    template <typename Visitor>
    size_t visit_vectors_by_ids(const std::vector<ndd::idInt>& numeric_ids,
                                Visitor&& visitor) const {
        return vector_store_->visit_vectors_by_ids(
                numeric_ids,
                std::forward<Visitor>(visitor));
    }

    ndd::VectorMeta get_meta(ndd::idInt numeric_id) const {
        return meta_store_->get_meta(numeric_id);
    }

    /*
     * Deletes filter, metadata, and vector data for one numeric id.
     *
     * Return codes:
     * 0 = success
     * 1-99 = propagated filter validation failure from filter store
     * 100-199 = propagated MDBX/storage failure from filter store
     * 200-299 = propagated corruption/invariant failure from filter store
     */
    ndd::OperationResult<> deletePoint(ndd::idInt numeric_id) {
        try {
            // Get metadata first to get filter info
            auto meta = meta_store_->get_meta(numeric_id);

            // Remove filter entries if they exist
            if(!meta.filter.empty()) {
                auto filter_result = filter_store_->remove_filters_from_json(numeric_id, meta.filter);
                if(!filter_result.ok()) {
                    return filter_result;
                }
            }
            // Try to remove both vector and meta data
            vector_store_->remove(numeric_id);
            meta_store_->remove(numeric_id);
            return {SUCCESS, ""};
        } catch(const std::exception& e) {
            return {100, std::string("Failed to remove vector and metadata: ") + e.what()};
        }
    }

    /*
     * Deletes filter index entries for one numeric id and keeps `meta.filter` in sync.
     *
     * Why both? The filter index (category + numeric) and meta.filter are two records
     * of the same fact ("this vector matches the following filter document"). If we
     * remove from the index but leave meta.filter populated, a later get_meta() returns
     * a JSON document whose index entries are gone — searches via that filter no longer
     * return this id, even though meta still advertises the filter. Other code paths
     * (notably the upsert cleanup in store_vectors_batch) read meta.filter as the
     * source of truth for "what is the prior filter" and would incorrectly try to
     * remove already-removed entries. Keeping them in lockstep avoids both confusions.
     *
     * Contract:
     *   - The caller passes the exact filter document being removed. The single
     *     in-tree caller is deleteVectorsByIds at ndd.hpp, which always passes the
     *     entire meta.filter for a full filter clear.
     *   - If meta.filter matches the input, it is cleared in-place.
     *   - If meta.filter differs (caller is asking to remove a partial document or
     *     something that does not match the current state), we leave meta alone — the
     *     index removal is still performed best-effort, but we do not over-clear meta
     *     because we cannot tell which subset of meta.filter the caller intended.
     *
     * Atomicity / limitations:
     *   - Index removal and meta sync are separate MDBX transactions. A crash between
     *     them leaves meta.filter populated while index entries are gone (the very
     *     drift this function is meant to prevent on the happy path). Cross-store
     *     ACID is tracked separately in the filter roadmap.
     *   - Meta read or write failure AFTER successful index removal is surfaced as
     *     code 102 so the caller can return 500. We do NOT try to re-add the index
     *     entries on rollback — that would itself require another transaction and
     *     could also fail. The operator is expected to repair via rebuild if this
     *     ever fires.
     *
     * Return codes:
     * 0 = success
     * 1-99 = propagated filter validation failure from filter store
     * 100-199 = propagated MDBX/storage failure from filter or meta store
     * 200-299 = propagated corruption/invariant failure from filter store
     */
    ndd::OperationResult<> deleteFilter(ndd::idInt numeric_id, std::string filter) {
        /*
         * Step 1: remove the index entries first. If this fails we have not touched
         * meta yet — caller gets a clean error, no drift introduced.
         */
        auto remove_result = filter_store_->remove_filters_from_json(numeric_id, filter);
        if(!remove_result.ok()) {
            return remove_result;
        }

        /*
         * Step 2: sync meta.filter. We only clear it when it matches the caller's
         * input — see the contract note above for why we cannot just unconditionally
         * clear meta.filter. The empty-check is a fast path for ids that already
         * have no filter recorded.
         */
        try {
            auto meta = meta_store_->get_meta(numeric_id);
            if(!meta.filter.empty() && meta.filter == filter) {
                meta.filter.clear();
                meta_store_->store_meta(numeric_id, meta);
            }
        } catch(const std::exception& e) {
            /*
             * Either get_meta failed (no meta for this id, or MDBX error) or
             * store_meta failed. The index entries are already gone at this point,
             * so meta.filter is now potentially stale relative to the index. We
             * surface the failure as 102 rather than swallow it: the operator sees
             * the inconsistency and can repair via rebuild, and the user does not
             * receive a "successful" response on top of an inconsistent state.
             */
            LOG_ERROR(1226,
                      index_id_,
                      "deleteFilter meta sync failed for numeric_id "
                              << numeric_id << ": " << e.what());
            return {102,
                    "deleteFilter meta sync failed for numeric_id "
                            + std::to_string(numeric_id) + ": " + e.what()};
        }
        return {SUCCESS, ""};
    }

    /*
     * Replaces the filter document for one vector.
     *
     * Return codes:
     * 0 = success
     * 1-99 = propagated filter validation failure from filter store
     * 100-199 = propagated MDBX/storage failure from filter store
     * 200-299 = propagated corruption/invariant failure from filter store
     */
    ndd::OperationResult<> updateFilter(ndd::idInt numeric_id,
                                        const std::string& new_filter_json) {
        // Get existing meta
        auto meta = meta_store_->get_meta(numeric_id);

        // Remove old filters
        if(!meta.filter.empty()) {
            auto remove_result = filter_store_->remove_filters_from_json(numeric_id, meta.filter);
            if(!remove_result.ok()) {
                return remove_result;
            }
        }

        // Update meta
        meta.filter = new_filter_json;
        meta_store_->store_meta(numeric_id, meta);

        // Add new filters
        if(!new_filter_json.empty()) {
            auto add_result = filter_store_->add_filters_from_json(numeric_id, new_filter_json);
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
