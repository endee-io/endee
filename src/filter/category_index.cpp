#include "category_index.hpp"

#include <stdexcept>
#include <utility>

namespace ndd {
    namespace filter {

        std::string CategoryIndex::format_filter_key(const std::string& field,
                                                     const std::string& value) {
            return field + ":" + value;
        }

        ndd::OperationResult<ndd::RoaringBitmap>
        CategoryIndex::read_bitmap_payload(const void* data, size_t len) {
            if(data == nullptr || len == 0) {
                return {200, "empty bitmap payload"};
            }

            const char* bytes = static_cast<const char*>(data);
            const size_t consumed =
                    roaring::api::roaring_bitmap_portable_deserialize_size(bytes, len);
            if(consumed == 0) {
                return {200, "invalid or truncated bitmap payload"};
            }
            if(consumed != len) {
                return {200,
                        "bitmap payload length mismatch: consumed "
                                + std::to_string(consumed) + " of "
                                + std::to_string(len) + " bytes"};
            }

            ndd::RoaringBitmap bitmap;
            try {
                bitmap = ndd::RoaringBitmap::readSafe(bytes, len);
            } catch(const std::exception& e) {
                return {200,
                        "failed to deserialize bitmap payload: " + std::string(e.what())};
            }

            const char* reason = nullptr;
            if(!roaring::api::roaring_bitmap_internal_validate(&bitmap.roaring, &reason)) {
                return {200,
                        std::string("invalid bitmap internals")
                                + (reason != nullptr ? ": " + std::string(reason) : "")};
            }
            return {SUCCESS, "", std::move(bitmap)};
        }

        ndd::OperationResult<ndd::RoaringBitmap>
        CategoryIndex::get_bitmap_internal(MDBX_txn* txn, const std::string& filter_key) const {
            MDBX_val key{const_cast<char*>(filter_key.c_str()), filter_key.size()};
            MDBX_val data;

            int rc = mdbx_get(txn, dbi_, &key, &data);
            if(rc == MDBX_NOTFOUND || (rc == MDBX_SUCCESS && data.iov_len == 0)) {
                return {SUCCESS, "", ndd::RoaringBitmap()};
            }
            if(rc != MDBX_SUCCESS) {
                return {100,
                        "Failed to read category bitmap key '" + filter_key
                                + "': " + std::string(mdbx_strerror(rc))};
            }

            auto bitmap_result = read_bitmap_payload(data.iov_base, data.iov_len);
            if(!bitmap_result.ok()) {
                return {bitmap_result.code,
                        "Corrupt category bitmap payload for key '" + filter_key
                                + "': " + bitmap_result.message};
            }
            if(!bitmap_result.value.has_value()) {
                return {200, "Category bitmap reader succeeded without a bitmap for key '"
                                     + filter_key + "'"};
            }
            return {SUCCESS, "", std::move(*bitmap_result.value)};
        }

        ndd::OperationResult<>
        CategoryIndex::store_bitmap_internal(MDBX_txn* txn,
                                             const std::string& filter_key,
                                             const ndd::RoaringBitmap& bitmap) {
            size_t required_size = bitmap.getSizeInBytes();
            if(required_size == 0) {
                return {200, "Invalid category bitmap serialization size for key '"
                                     + filter_key + "'"};
            }

            std::vector<char> buffer(required_size);
            bitmap.write(buffer.data(), true);

            MDBX_val key{const_cast<char*>(filter_key.c_str()), filter_key.size()};
            MDBX_val data{const_cast<char*>(buffer.data()), buffer.size()};

            int rc = mdbx_put(txn, dbi_, &key, &data, MDBX_UPSERT);
            if(rc != MDBX_SUCCESS) {
                return {100, "Failed to store category bitmap key '" + filter_key
                                     + "': " + std::string(mdbx_strerror(rc))};
            }
            return {SUCCESS, ""};
        }

        ndd::OperationResult<>
        CategoryIndex::store_bitmap_internal(const std::string& filter_key,
                                             const ndd::RoaringBitmap& bitmap) {
            MDBX_txn* txn = nullptr;
            int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
            if(rc != MDBX_SUCCESS) {
                return {100,
                        "Failed to begin category bitmap write transaction: "
                                + std::string(mdbx_strerror(rc))};
            }

            auto result = store_bitmap_internal(txn, filter_key, bitmap);
            if(!result.ok()) {
                mdbx_txn_abort(txn);
                return result;
            }

            rc = mdbx_txn_commit(txn);
            if(rc != MDBX_SUCCESS) {
                return {100,
                        "Failed to commit category bitmap write transaction: "
                                + std::string(mdbx_strerror(rc))};
            }
            return {SUCCESS, ""};
        }

        CategoryIndex::CategoryIndex(MDBX_env* env) :
            env_(env) {
            MDBX_txn* txn = nullptr;
            int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
            if(rc != MDBX_SUCCESS) {
                throw std::runtime_error(
                        std::string("Failed to begin txn for CategoryIndex init: ")
                        + mdbx_strerror(rc));
            }

            rc = mdbx_dbi_open(txn, "category_idx", MDBX_CREATE, &dbi_);
            if(rc != MDBX_SUCCESS) {
                mdbx_txn_abort(txn);
                throw std::runtime_error(std::string("Failed to open category_idx dbi: ")
                                         + mdbx_strerror(rc));
            }

            rc = mdbx_txn_commit(txn);
            if(rc != MDBX_SUCCESS) {
                throw std::runtime_error(std::string("Failed to commit CategoryIndex init: ")
                                         + mdbx_strerror(rc));
            }
        }

        ndd::OperationResult<ndd::RoaringBitmap>
        CategoryIndex::get_bitmap(MDBX_txn* txn,
                                      const std::string& field,
                                      const std::string& value) const {
            return get_bitmap_internal(txn, format_filter_key(field, value));
        }

        ndd::OperationResult<ndd::RoaringBitmap>
        CategoryIndex::get_bitmap_by_key(MDBX_txn* txn, const std::string& key) const {
            return get_bitmap_internal(txn, key);
        }

        ndd::OperationResult<>
        CategoryIndex::remove(MDBX_txn* txn,
                              const std::string& field,
                              const std::string& value,
                              ndd::idInt id) {
            std::string filter_key = format_filter_key(field, value);
            auto bitmap_result = get_bitmap_internal(txn, filter_key);
            if(!bitmap_result.ok()) {
                return {bitmap_result.code, bitmap_result.message};
            }

            bitmap_result.value_or_throw().remove(id);
            return store_bitmap_internal(txn, filter_key, bitmap_result.value_or_throw());
        }

        ndd::OperationResult<>
        CategoryIndex::add_batch_by_key(MDBX_txn* txn,
                                        const std::string& key,
                                        const std::vector<ndd::idInt>& ids) {
            if(ids.empty()) {
                return {SUCCESS, ""};
            }
            auto bitmap_result = get_bitmap_internal(txn, key);
            if(!bitmap_result.ok()) {
                return {bitmap_result.code, bitmap_result.message};
            }

            bitmap_result.value_or_throw().addMany(ids.size(), ids.data());
            return store_bitmap_internal(txn, key, bitmap_result.value_or_throw());
        }

    }  // namespace filter
}  // namespace ndd
