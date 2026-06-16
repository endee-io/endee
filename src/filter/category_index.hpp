#pragma once

#include <cstddef>
#include <string>
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
                                                 const std::string& value);

            static ndd::OperationResult<ndd::RoaringBitmap>
            read_bitmap_payload(const void* data, size_t len);

            /*
             * Loads the bitmap stored for a formatted category filter key.
             *
             * Return codes:
             * 0 = success
             * 100 = MDBX transaction or read failure; caller should log ERROR and return HTTP 500
             * 200 = corrupt stored bitmap payload; caller should log ERROR and return HTTP 500
             */
            ndd::OperationResult<ndd::RoaringBitmap>
            get_bitmap_internal(const std::string& filter_key) const;

            /*
             * Stores the bitmap for a formatted category filter key.
             *
             * Return codes:
             * 0 = success
             * 100 = MDBX transaction or write failure; caller should log ERROR and return HTTP 500
             * 200 = invalid bitmap serialization; caller should log ERROR and return HTTP 500
             */
            ndd::OperationResult<> store_bitmap_internal(const std::string& filter_key,
                                                         const ndd::RoaringBitmap& bitmap);

        public:
            CategoryIndex(MDBX_env* env);

            /*
             * Lists all unique category values stored for one field.
             *
             * Return codes:
             * 0 = success
             * 100 = MDBX transaction, cursor, or scan failure; caller should log ERROR and return HTTP 500
             */
            ndd::OperationResult<std::vector<std::string>>
            scan_values(const std::string& field) const;

            /*
             * Loads the bitmap for one category field/value pair.
             *
             * Return codes:
             * 0 = success
             * 100-199 = propagated MDBX/storage failure from the bitmap read helper
             * 200-299 = propagated corruption/invariant failure from the bitmap read helper
             */
            ndd::OperationResult<ndd::RoaringBitmap>
            get_bitmap(const std::string& field, const std::string& value) const;

            /*
             * Loads the bitmap for an already formatted category key.
             *
             * Return codes:
             * 0 = success
             * 100-199 = propagated MDBX/storage failure from the bitmap read helper
             * 200-299 = propagated corruption/invariant failure from the bitmap read helper
             */
            ndd::OperationResult<ndd::RoaringBitmap>
            get_bitmap_by_key(const std::string& key) const;

            /*
             * Adds one id to a category field/value bitmap.
             *
             * Return codes:
             * 0 = success
             * 100-199 = propagated MDBX/storage failure from bitmap read/write helpers
             * 200-299 = propagated corruption/invariant failure from bitmap read/write helpers
             */
            ndd::OperationResult<>
            add(const std::string& field, const std::string& value, ndd::idInt id);

            /*
             * Removes one id from a category field/value bitmap.
             *
             * Return codes:
             * 0 = success
             * 100-199 = propagated MDBX/storage failure from bitmap read/write helpers
             * 200-299 = propagated corruption/invariant failure from bitmap read/write helpers
             */
            ndd::OperationResult<>
            remove(const std::string& field, const std::string& value, ndd::idInt id);

            /*
             * Checks whether one id is present in a category field/value bitmap.
             *
             * Return codes:
             * 0 = success
             * 100-199 = propagated MDBX/storage failure from the bitmap read helper
             * 200-299 = propagated corruption/invariant failure from the bitmap read helper
             */
            ndd::OperationResult<bool>
            contains(const std::string& field, const std::string& value, ndd::idInt id) const;

            /*
             * Adds a batch of ids to an already formatted category key.
             *
             * Return codes:
             * 0 = success
             * 100-199 = propagated MDBX/storage failure from bitmap read/write helpers
             * 200-299 = propagated corruption/invariant failure from bitmap read/write helpers
             */
            ndd::OperationResult<>
            add_batch_by_key(const std::string& key, const std::vector<ndd::idInt>& ids);

            // Expose key formatting for external batching logic
            static std::string make_key(const std::string& field, const std::string& value);

            MDBX_dbi get_dbi() const;
        };

    }  // namespace filter
}  // namespace ndd
