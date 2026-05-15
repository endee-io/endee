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
        CategoryIndex::get_bitmap_internal(const std::string& filter_key) const {
            MDBX_txn* txn = nullptr;
            int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &txn);
            if(rc != MDBX_SUCCESS) {
                return {100,
                        "Failed to begin category bitmap read transaction: "
                                + std::string(mdbx_strerror(rc))};
            }

            MDBX_val key{const_cast<char*>(filter_key.c_str()), filter_key.size()};
            MDBX_val data;

            rc = mdbx_get(txn, dbi_, &key, &data);
            if(rc == MDBX_NOTFOUND || (rc == MDBX_SUCCESS && data.iov_len == 0)) {
                mdbx_txn_abort(txn);
                return {SUCCESS, "", ndd::RoaringBitmap()};
            }
            if(rc != MDBX_SUCCESS) {
                mdbx_txn_abort(txn);
                return {100,
                        "Failed to read category bitmap key '" + filter_key
                                + "': " + std::string(mdbx_strerror(rc))};
            }

            auto bitmap_result = read_bitmap_payload(data.iov_base, data.iov_len);
            if(!bitmap_result.ok()) {
                mdbx_txn_abort(txn);
                return {bitmap_result.code,
                        "Corrupt category bitmap payload for key '" + filter_key
                                + "': " + bitmap_result.message};
            }
            if(!bitmap_result.value.has_value()) {
                mdbx_txn_abort(txn);
                return {200, "Category bitmap reader succeeded without a bitmap for key '"
                                     + filter_key + "'"};
            }
            mdbx_txn_abort(txn);
            return {SUCCESS, "", std::move(*bitmap_result.value)};
        }

        ndd::OperationResult<>
        CategoryIndex::store_bitmap_internal(const std::string& filter_key,
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

            MDBX_txn* txn = nullptr;
            int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
            if(rc != MDBX_SUCCESS) {
                return {100,
                        "Failed to begin category bitmap write transaction: "
                                + std::string(mdbx_strerror(rc))};
            }

            rc = mdbx_put(txn, dbi_, &key, &data, MDBX_UPSERT);
            if(rc != MDBX_SUCCESS) {
                mdbx_txn_abort(txn);
                return {100, "Failed to store category bitmap key '" + filter_key
                                     + "': " + std::string(mdbx_strerror(rc))};
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

        ndd::OperationResult<std::vector<std::string>>
        CategoryIndex::scan_values(const std::string& field) const {
            std::vector<std::string> values;
            MDBX_txn* txn = nullptr;
            int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &txn);
            if(rc != MDBX_SUCCESS) {
                return {100,
                        "Failed to begin category value scan transaction: "
                                + std::string(mdbx_strerror(rc))};
            }

            MDBX_cursor* cursor = nullptr;
            rc = mdbx_cursor_open(txn, dbi_, &cursor);
            if(rc != MDBX_SUCCESS) {
                mdbx_txn_abort(txn);
                return {100,
                        "Failed to open category value scan cursor: "
                                + std::string(mdbx_strerror(rc))};
            }

            std::string prefix = field + ":";
            MDBX_val key{const_cast<char*>(prefix.c_str()), prefix.size()};
            MDBX_val data;

            rc = mdbx_cursor_get(cursor, &key, &data, MDBX_SET_RANGE);
            while(rc == MDBX_SUCCESS) {
                std::string found_key(static_cast<char*>(key.iov_base), key.iov_len);
                if(found_key.rfind(prefix, 0) != 0) {
                    break;
                }

                values.push_back(found_key.substr(prefix.size()));
                rc = mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT);
            }

            mdbx_cursor_close(cursor);
            mdbx_txn_abort(txn);

            if(rc != MDBX_SUCCESS && rc != MDBX_NOTFOUND) {
                return {100, "Failed during category value scan: "
                                     + std::string(mdbx_strerror(rc))};
            }
            return {SUCCESS, "", std::move(values)};
        }

        ndd::OperationResult<ndd::RoaringBitmap>
        CategoryIndex::get_bitmap(const std::string& field, const std::string& value) const {
            return get_bitmap_internal(format_filter_key(field, value));
        }

        ndd::OperationResult<ndd::RoaringBitmap>
        CategoryIndex::get_bitmap_by_key(const std::string& key) const {
            return get_bitmap_internal(key);
        }

        ndd::OperationResult<>
        CategoryIndex::add(const std::string& field, const std::string& value, ndd::idInt id) {
            std::string filter_key = format_filter_key(field, value);
            auto bitmap_result = get_bitmap_internal(filter_key);
            if(!bitmap_result.ok()) {
                return {bitmap_result.code, bitmap_result.message};
            }

            bitmap_result.value_or_throw().add(id);
            return store_bitmap_internal(filter_key, bitmap_result.value_or_throw());
        }

        ndd::OperationResult<>
        CategoryIndex::remove(const std::string& field,
                              const std::string& value,
                              ndd::idInt id) {
            std::string filter_key = format_filter_key(field, value);
            auto bitmap_result = get_bitmap_internal(filter_key);
            if(!bitmap_result.ok()) {
                return {bitmap_result.code, bitmap_result.message};
            }

            bitmap_result.value_or_throw().remove(id);
            return store_bitmap_internal(filter_key, bitmap_result.value_or_throw());
        }

        ndd::OperationResult<bool>
        CategoryIndex::contains(const std::string& field,
                                const std::string& value,
                                ndd::idInt id) const {
            auto bitmap_result = get_bitmap_internal(format_filter_key(field, value));
            if(!bitmap_result.ok()) {
                return {bitmap_result.code, bitmap_result.message};
            }
            return {SUCCESS, "", bitmap_result.value_or_throw().contains(id)};
        }

        ndd::OperationResult<>
        CategoryIndex::add_batch_by_key(const std::string& key,
                                        const std::vector<ndd::idInt>& ids) {
            if(ids.empty()) {
                return {SUCCESS, ""};
            }
            auto bitmap_result = get_bitmap_internal(key);
            if(!bitmap_result.ok()) {
                return {bitmap_result.code, bitmap_result.message};
            }

            bitmap_result.value_or_throw().addMany(ids.size(), ids.data());
            return store_bitmap_internal(key, bitmap_result.value_or_throw());
        }

        std::string CategoryIndex::make_key(const std::string& field,
                                            const std::string& value) {
            return format_filter_key(field, value);
        }

    }  // namespace filter
}  // namespace ndd
