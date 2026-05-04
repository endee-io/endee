#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "mdbx/mdbx.h"
#include "../core/types.hpp"
#include "../utils/types.hpp"

namespace ndd {
    namespace filter {

        class CategoryIndex {
        private:
            MDBX_env* env_;
            MDBX_dbi dbi_;

            static std::string format_filter_key(const std::string& field,
                                                 const std::string& value) {
                return field + ":" + value;
            }

            /*
             * Loads the bitmap stored for a formatted category filter key.
             *
             * Return codes:
             * 0 = success
             * 100 = MDBX transaction or read failure; caller should log ERROR and return HTTP 500
             * 200 = corrupt stored bitmap payload; caller should log ERROR and return HTTP 500
             */
            ndd::OperationResult<ndd::RoaringBitmap>
            get_bitmap_internal(const std::string& filter_key) const {
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

                try {
                    ndd::RoaringBitmap bitmap =
                            ndd::RoaringBitmap::read(static_cast<const char*>(data.iov_base));
                    mdbx_txn_abort(txn);
                    return {SUCCESS, "", std::move(bitmap)};
                } catch(const std::exception& e) {
                    mdbx_txn_abort(txn);
                    return {200, "Corrupt category bitmap payload for key '" + filter_key
                                         + "': " + e.what()};
                }
            }

            /*
             * Stores the bitmap for a formatted category filter key.
             *
             * Return codes:
             * 0 = success
             * 100 = MDBX transaction or write failure; caller should log ERROR and return HTTP 500
             * 200 = invalid bitmap serialization; caller should log ERROR and return HTTP 500
             */
            ndd::OperationResult<> store_bitmap_internal(const std::string& filter_key,
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

        public:
            CategoryIndex(MDBX_env* env) :
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

            /*
             * Lists all unique category values stored for one field.
             *
             * Return codes:
             * 0 = success
             * 100 = MDBX transaction, cursor, or scan failure; caller should log ERROR and return HTTP 500
             */
            ndd::OperationResult<std::vector<std::string>>
            scan_values(const std::string& field) const {
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

            /*
             * Loads the bitmap for one category field/value pair.
             *
             * Return codes:
             * 0 = success
             * 100-199 = propagated MDBX/storage failure from the bitmap read helper
             * 200-299 = propagated corruption/invariant failure from the bitmap read helper
             */
            ndd::OperationResult<ndd::RoaringBitmap>
            get_bitmap(const std::string& field, const std::string& value) const {
                return get_bitmap_internal(format_filter_key(field, value));
            }

            /*
             * Loads the bitmap for an already formatted category key.
             *
             * Return codes:
             * 0 = success
             * 100-199 = propagated MDBX/storage failure from the bitmap read helper
             * 200-299 = propagated corruption/invariant failure from the bitmap read helper
             */
            ndd::OperationResult<ndd::RoaringBitmap>
            get_bitmap_by_key(const std::string& key) const {
                return get_bitmap_internal(key);
            }

            /*
             * Adds one id to a category field/value bitmap.
             *
             * Return codes:
             * 0 = success
             * 100-199 = propagated MDBX/storage failure from bitmap read/write helpers
             * 200-299 = propagated corruption/invariant failure from bitmap read/write helpers
             */
            ndd::OperationResult<>
            add(const std::string& field, const std::string& value, ndd::idInt id) {
                std::string filter_key = format_filter_key(field, value);
                auto bitmap_result = get_bitmap_internal(filter_key);
                if(!bitmap_result.ok()) {
                    return {bitmap_result.code, bitmap_result.message};
                }

                bitmap_result.value_or_throw().add(id);
                return store_bitmap_internal(filter_key, bitmap_result.value_or_throw());
            }

            /*
             * Removes one id from a category field/value bitmap.
             *
             * Return codes:
             * 0 = success
             * 100-199 = propagated MDBX/storage failure from bitmap read/write helpers
             * 200-299 = propagated corruption/invariant failure from bitmap read/write helpers
             */
            ndd::OperationResult<>
            remove(const std::string& field, const std::string& value, ndd::idInt id) {
                std::string filter_key = format_filter_key(field, value);
                auto bitmap_result = get_bitmap_internal(filter_key);
                if(!bitmap_result.ok()) {
                    return {bitmap_result.code, bitmap_result.message};
                }

                bitmap_result.value_or_throw().remove(id);
                return store_bitmap_internal(filter_key, bitmap_result.value_or_throw());
            }

            /*
             * Checks whether one id is present in a category field/value bitmap.
             *
             * Return codes:
             * 0 = success
             * 100-199 = propagated MDBX/storage failure from the bitmap read helper
             * 200-299 = propagated corruption/invariant failure from the bitmap read helper
             */
            ndd::OperationResult<bool>
            contains(const std::string& field, const std::string& value, ndd::idInt id) const {
                auto bitmap_result = get_bitmap_internal(format_filter_key(field, value));
                if(!bitmap_result.ok()) {
                    return {bitmap_result.code, bitmap_result.message};
                }
                return {SUCCESS, "", bitmap_result.value_or_throw().contains(id)};
            }

            /*
             * Adds a batch of ids to an already formatted category key.
             *
             * Return codes:
             * 0 = success
             * 100-199 = propagated MDBX/storage failure from bitmap read/write helpers
             * 200-299 = propagated corruption/invariant failure from bitmap read/write helpers
             */
            ndd::OperationResult<>
            add_batch_by_key(const std::string& key, const std::vector<ndd::idInt>& ids) {
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

            static std::string make_key(const std::string& field, const std::string& value) {
                return format_filter_key(field, value);
            }

            MDBX_dbi get_dbi() const { return dbi_; }
        };

    }  // namespace filter
}  // namespace ndd
