#pragma once

#include <cstdint>
#include <cstring>
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
        inline uint32_t float_to_sortable(float f) {
            uint32_t i;
            std::memcpy(&i, &f, sizeof(float));
            // IEEE 754: if sign bit set, flip all bits. Else flip just sign.
            // This makes negatives < positives order correctly.
            uint32_t mask = (int32_t(i) >> 31) | 0x80000000;
            return i ^ mask;
        }

        inline float sortable_to_float(uint32_t i) {
            uint32_t mask = ((i >> 31) - 1) | 0x80000000;
            uint32_t result = i ^ mask;
            float f;
            std::memcpy(&f, &result, sizeof(float));
            return f;
        }

        inline uint32_t int_to_sortable(int32_t i) {
            return static_cast<uint32_t>(i) ^ 0x80000000;
        }

        inline int32_t sortable_to_int(uint32_t i) {
            return static_cast<int32_t>(i ^ 0x80000000);
        }

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
            uint32_t get_value(size_t index) const {
                return base_value + deltas[index];
            }

            void add(uint32_t val, ndd::idInt id);

            bool remove(ndd::idInt id);

            /**
             * Serialization Format:
             *   [BitmapSize (4)]
             *   [Bitmap Bytes]
             *   [Count (2)]
             *   [Deltas (Count * 2)]
             *   [IDs (Count * sizeof(idInt))]
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
             *
             * On the `count` field and why it is intentionally ignored here:
             *
             * The on-disk bucket layout this function reads is:
             *
             *   [bm_size : uint32_t]
             *   [bitmap  : bm_size bytes]
             *   [count   : uint16_t]                       <-- not needed by us
             *   [deltas  : count * sizeof(uint16_t)]       <-- not read here
             *   [ids     : count * sizeof(idInt)]          <-- not read here
             *
             * `count` exists only so the older full-deserialize path knows how
             * many delta/id entries follow the bitmap. That value can be
             * recovered without an explicit field by computing
             *   (record_len - sizeof(uint32_t) - bm_size)
             *       / (sizeof(uint16_t) + sizeof(idInt))
             * which is exactly how the next major version of the bucket format
             * will work -- the `count` field will be dropped to save 2 bytes
             * per bucket on disk and to remove the redundancy between the
             * stored count and the byte-length-derived count.
             *
             * For now `count` is preserved in the bucket layout to keep
             * backward compatibility with existing on-disk indexes built by
             * prior versions: those buckets carry the `count` field, and any
             * code path that round-trips a bucket (read + modify + write)
             * must continue to honor it. The full `Bucket::deserialize` /
             * `Bucket::serialize` pair still reads and writes `count`.
             *
             * `read_summary_bitmap` only needs the bitmap, so it stops after
             * the bitmap bytes and never touches `count` or anything after
             * it. Skipping over `count` here is safe for both the current
             * (count-bearing) layout and the future (count-less) layout, so
             * this code path will continue to work unchanged when `count` is
             * removed in a subsequent version. The corresponding alignment
             * sanity check on the trailing bytes is intentionally omitted:
             * any corruption in the trailing region is caught by the full
             * `Bucket::deserialize` path that actually consumes those bytes.
             */
            static ndd::RoaringBitmap read_summary_bitmap(const void* data, size_t len);

            bool is_full() const { return ids.size() >= MAX_SIZE; }
            bool is_empty() const { return ids.empty(); }
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

    } // namespace filter
} // namespace ndd
