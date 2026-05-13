#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mdbx/mdbx.h"
#include "../core/types.hpp"
#include "../utils/types.hpp"

namespace ndd {
    namespace filter {

        struct NumericBatchEntry {
            std::string field;
            ndd::idInt id;
            uint32_t value;

            NumericBatchEntry(std::string field_in, ndd::idInt id_in, uint32_t value_in);
        };

        // --- Sortable Key Utilities ---
        uint32_t float_to_sortable(float f);
        float sortable_to_float(uint32_t i);
        uint32_t int_to_sortable(int32_t i);
        int32_t sortable_to_int(uint32_t i);

        // --- Bucket Structure (Hybrid) ---
        struct Bucket {
            /**
             * XXX: Ideally this bucket should be page size
             * bounded. Currently it is difficult to do that
             * here because the size of summary_bitmap depends
             * on the kind of userspace filter upserts and not
             * the number of them.
             */

            static constexpr size_t MAX_SIZE = 1024;
            static constexpr uint32_t MAX_DELTA = 65535;

            // Runtime only, not serialized in the payload
            uint32_t base_value = 0;

            // Data
            std::vector<uint16_t> deltas;
            std::vector<ndd::idInt> ids;
            ndd::RoaringBitmap summary_bitmap;

            bool is_dirty = false;

            static ndd::OperationResult<ndd::RoaringBitmap>
            read_bitmap_payload(const uint8_t* data, size_t len);

            // Helper to get actual value
            uint32_t get_value(size_t index) const;

            void add(uint32_t val, ndd::idInt id);

            bool remove(ndd::idInt id);

            /**
             * Serialization Format:
             *   [BitmapSize (uint32_t)]
             *   [Bitmap Bytes]
             *   [Deltas (nr_array_entries * sizeof(uint16_t))]
             *   [IDs    (nr_array_entries * sizeof(idInt))]
             *
             * nr_array_entries is recovered on read from
             *   (iov_len - sizeof(uint32_t) - bm_size)
             *       / (sizeof(uint16_t) + sizeof(idInt))
             */
            std::vector<uint8_t> serialize() const;

            static Bucket deserialize(const void* data, size_t len, uint32_t base_val);

            /**
             * Fast access to just the bitmap.
             *
             * Used by range() when a bucket is fully covered by the query
             * extent and we don't need the deltas/ids arrays. Skips the
             * memcpy + vector allocations that full deserialize would do
             * for those arrays.
             */
            static ndd::RoaringBitmap read_summary_bitmap(const void* data,
                                                          size_t len);

            bool is_full() const;
            bool is_empty() const;
        };

        class NumericIndex {
        private:
            MDBX_env* env_;
            MDBX_dbi forward_dbi_;   // ID -> Value (Field:ID -> Value)
            MDBX_dbi inverted_dbi_;  // BucketKey -> BucketBlob
            static constexpr size_t BATCH_TXN_CHUNK_SIZE = 256;

            std::string make_forward_key(const std::string& field, ndd::idInt id);

            // Key Format: [Field]:[BigEndian_BaseValue]
            std::string make_bucket_key(const std::string& field, uint32_t start_val);

            uint32_t parse_bucket_key_val(const std::string& key);

            /*
             * Removes one id from the numeric inverted bucket that currently owns its old value.
             *
             * Return codes:
             * 0 = success
             * 100 = MDBX cursor, read, delete, or write failure; caller should log ERROR and return HTTP 500
             * 200 = corrupt numeric bucket payload; caller should log ERROR and return HTTP 500
             */
            ndd::OperationResult<> remove_from_buckets(MDBX_txn* txn,
                                                       const std::string& field,
                                                       uint32_t value,
                                                       ndd::idInt id);

            /*
             * Adds one id/value pair into the numeric inverted bucket index.
             *
             * Return codes:
             * 0 = success
             * 100 = MDBX cursor, read, or write failure; caller should log ERROR and return HTTP 500
             * 200 = corrupt numeric bucket payload or invalid bucket invariant; caller should log ERROR and return HTTP 500
             */
            ndd::OperationResult<> add_to_buckets(MDBX_txn* txn,
                                                  const std::string& field,
                                                  uint32_t value,
                                                  ndd::idInt id);

            /*
             * Writes one numeric forward entry and updates the inverted buckets inside a caller transaction.
             *
             * Return codes:
             * 0 = success
             * 100 = MDBX read or write failure; caller should log ERROR and return HTTP 500
             * 100-199 = propagated MDBX/storage failure from bucket helpers
             * 200 = corrupt numeric forward value; caller should log ERROR and return HTTP 500
             * 200-299 = propagated corruption/invariant failure from bucket helpers
             */
            ndd::OperationResult<> put_internal(MDBX_txn* txn,
                                                const std::string& field,
                                                ndd::idInt id,
                                                uint32_t value);

        public:
            NumericIndex(MDBX_env* env);

            /*
             * Writes a batch of numeric filter entries in bounded MDBX write transaction chunks.
             *
             * Return codes:
             * 0 = success
             * 100 = MDBX transaction or commit failure; caller should log ERROR and return HTTP 500
             * 100-199 = propagated MDBX/storage failure from per-entry writes
             * 200-299 = propagated corruption/invariant failure from per-entry writes
             */
            ndd::OperationResult<> put_batch(const std::vector<NumericBatchEntry>& entries);

            /*
             * Removes one id from the numeric forward and inverted indexes for a field.
             *
             * Return codes:
             * 0 = success
             * 100 = MDBX transaction, read, delete, or commit failure; caller should log ERROR and return HTTP 500
             * 100-199 = propagated MDBX/storage failure from bucket helpers
             * 200 = corrupt numeric forward value; caller should log ERROR and return HTTP 500
             * 200-299 = propagated corruption/invariant failure from bucket helpers
             */
            ndd::OperationResult<> remove(const std::string& field, ndd::idInt id);

            /*
             * Computes a bitmap of ids whose numeric field value falls within an inclusive sortable range.
             *
             * Return codes:
             * 0 = success
             * 100 = MDBX transaction, cursor, or scan failure; caller should log ERROR and return HTTP 500
             * 200 = corrupt numeric bucket payload; caller should log ERROR and return HTTP 500
             */
            ndd::OperationResult<ndd::RoaringBitmap>
            range(const std::string& field, uint32_t min_val, uint32_t max_val);

            /*
             * Checks whether one id has a numeric field value inside an inclusive sortable range.
             *
             * Return codes:
             * 0 = success
             * 100 = MDBX transaction or read failure; caller should log ERROR and return HTTP 500
             * 200 = corrupt numeric forward value; caller should log ERROR and return HTTP 500
             */
            ndd::OperationResult<bool>
            check_range(const std::string& field,
                        ndd::idInt id,
                        uint32_t min_val,
                        uint32_t max_val);
        };

    }  // namespace filter
}  // namespace ndd
