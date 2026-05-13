#include "numeric_index.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "../utils/log.hpp"

namespace ndd {
    namespace filter {

        NumericBatchEntry::NumericBatchEntry(std::string field_in,
                                             ndd::idInt id_in,
                                             uint32_t value_in) :
            field(std::move(field_in)),
            id(id_in),
            value(value_in) {}

        // --- Sortable Key Utilities ---
        uint32_t float_to_sortable(float f) {
            uint32_t i;
            std::memcpy(&i, &f, sizeof(float));
            // IEEE 754: if sign bit set, flip all bits. Else flip just sign.
            // This makes negatives < positives order correctly.
            uint32_t mask = (int32_t(i) >> 31) | 0x80000000;
            return i ^ mask;
        }

        float sortable_to_float(uint32_t i) {
            uint32_t mask = ((i >> 31) - 1) | 0x80000000;
            uint32_t result = i ^ mask;
            float f;
            std::memcpy(&f, &result, sizeof(float));
            return f;
        }

        uint32_t int_to_sortable(int32_t i) {
            return static_cast<uint32_t>(i) ^ 0x80000000;
        }

        int32_t sortable_to_int(uint32_t i) {
            return static_cast<int32_t>(i ^ 0x80000000);
        }

        ndd::OperationResult<ndd::RoaringBitmap>
        Bucket::read_bitmap_payload(const uint8_t* data, size_t len) {
            if(len == 0) {
                return {SUCCESS, "", ndd::RoaringBitmap()};
            }
            if(data == nullptr) {
                return {200, "empty bitmap payload"};
            }

            const char* bytes = reinterpret_cast<const char*>(data);
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
            if(!roaring::api::roaring_bitmap_internal_validate(&bitmap.roaring,
                                                                &reason)) {
                return {200,
                        std::string("invalid bitmap internals")
                        + (reason != nullptr ? ": " + std::string(reason) : "")};
            }
            return {SUCCESS, "", std::move(bitmap)};
        }

        // Helper to get actual value
        uint32_t Bucket::get_value(size_t index) const {
            return base_value + deltas[index];
        }

        void Bucket::add(uint32_t val, ndd::idInt id) {
            if (val < base_value) {
                 // Should not happen if Key logic is correct
                 throw std::runtime_error("Insert value < Base Value");
            }
            uint32_t delta_32 = val - base_value;
            if (delta_32 > MAX_DELTA) {
                throw std::runtime_error("Delta overflow");
            }

            summary_bitmap.add(id);
            is_dirty = true;

            /**
             * If the bucket is already at MAX_SIZE in the parallel
             * arrays AND the new value equals base_value, route the
             * id into summary_bitmap only. Every id in the bitmap
             * with no matching delta is implicitly value-tagged by
             * base_value, so range queries can recover its value
             * without a per-id delta entry. This caps the on-disk
             * deltas/ids growth for duplicate-heavy values.
             *
             * Non-duplicate inserts (delta_32 != 0) still go into
             * the sorted arrays even when the bucket is "full" --
             * the slide-split fallthrough then pushes the bucket
             * one over MAX_SIZE momentarily and the next insert's
             * slide-split finds the new value boundary and splits.
             */
            if (delta_32 == 0 && ids.size() >= MAX_SIZE) {
                return;
            }

            uint16_t delta = static_cast<uint16_t>(delta_32);
            auto it = std::lower_bound(deltas.begin(), deltas.end(), delta);
            size_t index = std::distance(deltas.begin(), it);
            deltas.insert(it, delta);
            ids.insert(ids.begin() + index, id);
        }

        bool Bucket::remove(ndd::idInt id) {
            if (!summary_bitmap.contains(id)) {
                return false;
            }
            /**
             * The id might live only in the bitmap (added past MAX_SIZE).
             * The linear scan is best effort to also clear the ordered
             * arrays; the bitmap is the source of truth.
             */
            for (size_t i = 0; i < ids.size(); ++i) {
                if (ids[i] == id) {
                    ids.erase(ids.begin() + i);
                    deltas.erase(deltas.begin() + i);
                    break;
                }
            }
            summary_bitmap.remove(id);
            is_dirty = true;
            return true;
        }

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
        std::vector<uint8_t> Bucket::serialize() const {
            const_cast<ndd::RoaringBitmap&>(summary_bitmap).runOptimize();

            /**
             * Note: ids.size() can transiently exceed MAX_SIZE when
             * the slide-split fallthrough in add_to_buckets has just
             * pushed a non-duplicate into a saturated bucket. The
             * very next insert into that bucket will trigger a
             * standard slide-split that splits on the new boundary,
             * so the on-disk over-MAX_SIZE state is short-lived.
             * Saturated-with-duplicate inserts go bitmap-only via
             * Bucket::add and do not grow ids/deltas.
             */
            size_t bm_size = summary_bitmap.getSizeInBytes();
            size_t nr_array_entries = ids.size();
            size_t total_size = sizeof(uint32_t) + bm_size
                                + (nr_array_entries * sizeof(uint16_t))
                                + (nr_array_entries * sizeof(ndd::idInt));
            std::vector<uint8_t> buffer(total_size);
            uint8_t* ptr = buffer.data();

            uint32_t bm_size_32 = static_cast<uint32_t>(bm_size);
            std::memcpy(ptr, &bm_size_32, sizeof(uint32_t));
            ptr += sizeof(uint32_t);

            if (bm_size > 0) {
                summary_bitmap.write(reinterpret_cast<char*>(ptr));
                ptr += bm_size;
            }

            if (nr_array_entries > 0) {
                std::memcpy(ptr, deltas.data(),
                            nr_array_entries * sizeof(uint16_t));
                ptr += nr_array_entries * sizeof(uint16_t);
                std::memcpy(ptr, ids.data(),
                            nr_array_entries * sizeof(ndd::idInt));
            }

            return buffer;
        }

        Bucket Bucket::deserialize(const void* data, size_t len, uint32_t base_val) {
            Bucket b;
            b.base_value = base_val;

            if (len < sizeof(uint32_t)) return b; // Just the bm_size header

            const uint8_t* ptr = static_cast<const uint8_t*>(data);
            const uint8_t* end = ptr + len;

            // 1. Bitmap size
            uint32_t bm_size;
            std::memcpy(&bm_size, ptr, sizeof(uint32_t));
            ptr += sizeof(uint32_t);
            if (bm_size > static_cast<size_t>(end - ptr)) {
                throw std::runtime_error("Bucket corrupt: invalid bitmap size");
            }

            // 2. Bitmap
            if (bm_size > 0) {
                auto bitmap_result = read_bitmap_payload(ptr, bm_size);
                if(!bitmap_result.ok()) {
                    throw std::runtime_error("Bucket corrupt: "
                                             + bitmap_result.message);
                }
                if(!bitmap_result.value.has_value()) {
                    throw std::runtime_error(
                        "Bucket corrupt: bitmap reader succeeded without a bitmap");
                }
                b.summary_bitmap = std::move(*bitmap_result.value);
                ptr += bm_size;
            }

            // 3. Derive nr_array_entries from the residual.
            size_t remaining = static_cast<size_t>(end - ptr);
            constexpr size_t per_entry =
                sizeof(uint16_t) + sizeof(ndd::idInt);
            if (remaining % per_entry != 0) {
                throw std::runtime_error(
                    "Bucket corrupt: residual bytes not aligned");
            }
            size_t nr_array_entries = remaining / per_entry;

            if (nr_array_entries > 0) {
                size_t delta_size = nr_array_entries * sizeof(uint16_t);
                size_t id_size = nr_array_entries * sizeof(ndd::idInt);
                if (ptr + delta_size + id_size > end) {
                    throw std::runtime_error("Bucket corrupt: truncated arrays");
                }
                b.deltas.resize(nr_array_entries);
                std::memcpy(b.deltas.data(), ptr, delta_size);
                ptr += delta_size;
                b.ids.resize(nr_array_entries);
                std::memcpy(b.ids.data(), ptr, id_size);
            }

            return b;
        }

        /**
         * Fast access to just the bitmap.
         *
         * Used by range() when a bucket is fully covered by the query
         * extent and we don't need the deltas/ids arrays. Skips the
         * memcpy + vector allocations that full deserialize would do
         * for those arrays.
         */
        ndd::RoaringBitmap Bucket::read_summary_bitmap(const void* data,
                                                       size_t len) {
            if (len < sizeof(uint32_t)) {
                throw std::runtime_error("Bucket corrupt: missing bitmap size");
            }
            const uint8_t* ptr = static_cast<const uint8_t*>(data);
            const uint8_t* end = ptr + len;
            uint32_t bm_size;
            std::memcpy(&bm_size, ptr, sizeof(uint32_t));
            ptr += sizeof(uint32_t);
            if (bm_size > static_cast<size_t>(end - ptr)) {
                throw std::runtime_error("Bucket corrupt: invalid bitmap size");
            }
            constexpr size_t per_entry =
                sizeof(uint16_t) + sizeof(ndd::idInt);
            const size_t remaining =
                static_cast<size_t>(end - ptr - bm_size);
            if (remaining % per_entry != 0) {
                throw std::runtime_error(
                    "Bucket corrupt: residual bytes not aligned");
            }
            auto bitmap_result = read_bitmap_payload(ptr, bm_size);
            if(!bitmap_result.ok()) {
                throw std::runtime_error("Bucket corrupt: "
                                         + bitmap_result.message);
            }
            if(!bitmap_result.value.has_value()) {
                throw std::runtime_error(
                    "Bucket corrupt: bitmap reader succeeded without a bitmap");
            }
            return std::move(*bitmap_result.value);
        }

        bool Bucket::is_full() const {
            return ids.size() >= MAX_SIZE;
        }

        bool Bucket::is_empty() const {
            return ids.empty();
        }

        std::string NumericIndex::make_forward_key(const std::string& field, ndd::idInt id) {
            return field + ":" + std::to_string(id);
        }

        // Key Format: [Field]:[BigEndian_BaseValue]
        std::string NumericIndex::make_bucket_key(const std::string& field, uint32_t start_val) {
            uint32_t be_val = 0;
#if defined(__GNUC__) || defined(__clang__)
            be_val = __builtin_bswap32(start_val);
#else
            be_val = ((start_val >> 24) & 0xff) | ((start_val << 8) & 0xff0000)
                     | ((start_val >> 8) & 0xff00) | ((start_val << 24) & 0xff000000);
#endif
            std::string key = field + ":";
            key.append(reinterpret_cast<char*>(&be_val), 4);
            return key;
        }

        uint32_t NumericIndex::parse_bucket_key_val(const std::string& key) {
            if(key.size() < 4) {
                return 0;
            }
            uint32_t be_val;
            std::memcpy(&be_val, key.data() + key.size() - 4, 4);
#if defined(__GNUC__) || defined(__clang__)
            return __builtin_bswap32(be_val);
#else
            return ((be_val >> 24) & 0xff) | ((be_val << 8) & 0xff0000)
                   | ((be_val >> 8) & 0xff00) | ((be_val << 24) & 0xff000000);
#endif
        }

        /*
         * Removes one id from the numeric inverted bucket that currently owns its old value.
         *
         * Return codes:
         * 0 = success
         * 100 = MDBX cursor, read, delete, or write failure; caller should log ERROR and return HTTP 500
         * 200 = corrupt numeric bucket payload; caller should log ERROR and return HTTP 500
         */
        ndd::OperationResult<> NumericIndex::remove_from_buckets(MDBX_txn* txn,
                                                                 const std::string& field,
                                                                 uint32_t value,
                                                                 ndd::idInt id) {
            // Find bucket
            std::string bkey_str = make_bucket_key(field, value);
            MDBX_val key{const_cast<char*>(bkey_str.data()), bkey_str.size()};
            MDBX_val data;
            MDBX_cursor* cursor = nullptr;
            int rc = mdbx_cursor_open(txn, inverted_dbi_, &cursor);
            if(rc != MDBX_SUCCESS) {
                return {100, "Failed to open numeric bucket remove cursor: "
                                     + std::string(mdbx_strerror(rc))};
            }

            /**
             * Scan backward to find bucket covering 'value'.
             * Logic to find correct bucket:
             */
            rc = mdbx_cursor_get(cursor, &key, &data, MDBX_SET_RANGE);
            if(rc == MDBX_SUCCESS) {
                // Check if we are in right field & range
                std::string found_key(static_cast<char*>(key.iov_base), key.iov_len);
                if(found_key.rfind(field + ":", 0) != 0
                   || parse_bucket_key_val(found_key) > value) {
                    rc = mdbx_cursor_get(cursor, &key, &data, MDBX_PREV);
                }
            } else if(rc == MDBX_NOTFOUND) {
                /**
                 * The only possible bucket that could still contain
                 * value is the very last bucket in the database.
                 * Hence jumping there.
                 */
                rc = mdbx_cursor_get(cursor, &key, &data, MDBX_LAST);
            }

            // Should be at correct bucket now
            if(rc != MDBX_SUCCESS) {
                mdbx_cursor_close(cursor);
                if(rc == MDBX_NOTFOUND) {
                    return {SUCCESS, ""};
                }
                return {100, "Failed to locate numeric bucket for remove: "
                                     + std::string(mdbx_strerror(rc))};
            }

            std::string found_key(static_cast<char*>(key.iov_base), key.iov_len);
            if(found_key.rfind(field + ":", 0) != 0) {
                mdbx_cursor_close(cursor);
                return {SUCCESS, ""};
            }

            uint32_t bucket_base = parse_bucket_key_val(found_key);
            if(value < bucket_base) {
                mdbx_cursor_close(cursor);
                return {SUCCESS, ""};
            }

            try {
                Bucket bucket = Bucket::deserialize(data.iov_base, data.iov_len, bucket_base);
                if(bucket.remove(id)) {
                    // Save back or Delete if empty
                    if(bucket.is_empty()) {
                        rc = mdbx_cursor_del(cursor, static_cast<MDBX_put_flags_t>(0));
                        if(rc != MDBX_SUCCESS) {
                            mdbx_cursor_close(cursor);
                            return {100, "Failed to delete empty numeric bucket: "
                                                 + std::string(mdbx_strerror(rc))};
                        }
                    } else {
                        auto bytes = bucket.serialize();
                        MDBX_val new_data{bytes.data(), bytes.size()};
                        rc = mdbx_cursor_put(cursor, &key, &new_data, MDBX_CURRENT);
                        if(rc != MDBX_SUCCESS) {
                            mdbx_cursor_close(cursor);
                            return {100, "Failed to update numeric bucket after remove: "
                                                 + std::string(mdbx_strerror(rc))};
                        }
                    }
                }
            } catch(const std::exception& e) {
                mdbx_cursor_close(cursor);
                return {200, "Corrupt numeric bucket while removing id: "
                                     + std::string(e.what())};
            }

            mdbx_cursor_close(cursor);
            return {SUCCESS, ""};
        }

        /*
         * Adds one id/value pair into the numeric inverted bucket index.
         *
         * Return codes:
         * 0 = success
         * 100 = MDBX cursor, read, or write failure; caller should log ERROR and return HTTP 500
         * 200 = corrupt numeric bucket payload or invalid bucket invariant; caller should log ERROR and return HTTP 500
         */
        ndd::OperationResult<> NumericIndex::add_to_buckets(MDBX_txn* txn,
                                                            const std::string& field,
                                                            uint32_t value,
                                                            ndd::idInt id) {
            MDBX_cursor* cursor = nullptr;
            int rc = mdbx_cursor_open(txn, inverted_dbi_, &cursor);
            if(rc != MDBX_SUCCESS) {
                return {100, "Failed to open numeric bucket add cursor: "
                                     + std::string(mdbx_strerror(rc))};
            }

            // Find candidate bucket
            std::string search_key = make_bucket_key(field, value);
            MDBX_val key{const_cast<char*>(search_key.data()), search_key.size()};
            MDBX_val data;

            // Move logic to find predecessor
            rc = mdbx_cursor_get(cursor, &key, &data, MDBX_SET_RANGE);
            if(rc == MDBX_SUCCESS) {
                std::string found_key(static_cast<char*>(key.iov_base), key.iov_len);
                if(found_key.rfind(field + ":", 0) != 0
                   || parse_bucket_key_val(found_key) > value) {
                    int prev_rc = mdbx_cursor_get(cursor, &key, &data, MDBX_PREV);
                    if(prev_rc == MDBX_SUCCESS) {
                        rc = prev_rc;
                    } else if(prev_rc != MDBX_NOTFOUND) {
                        mdbx_cursor_close(cursor);
                        return {100, "Failed to seek previous numeric bucket: "
                                             + std::string(mdbx_strerror(prev_rc))};
                    } else {
                        rc = MDBX_NOTFOUND;
                    }
                }
            } else if(rc == MDBX_NOTFOUND) {
                rc = mdbx_cursor_get(cursor, &key, &data, MDBX_LAST);
                if(rc != MDBX_SUCCESS && rc != MDBX_NOTFOUND) {
                    mdbx_cursor_close(cursor);
                    return {100, "Failed to seek last numeric bucket: "
                                         + std::string(mdbx_strerror(rc))};
                }
            } else {
                mdbx_cursor_close(cursor);
                return {100, "Failed to seek numeric bucket: "
                                     + std::string(mdbx_strerror(rc))};
            }

            bool create_new = true;
            std::string target_key_str;
            uint32_t target_base = 0;
            if(rc == MDBX_SUCCESS) {
                std::string found_key(static_cast<char*>(key.iov_base), key.iov_len);
                if(found_key.rfind(field + ":", 0) == 0) {
                    target_base = parse_bucket_key_val(found_key);

                    // Check range condition
                    if(value >= target_base
                       && (static_cast<uint64_t>(value) - target_base)
                                  <= Bucket::MAX_DELTA) {
                        target_key_str = found_key;
                        create_new = false;
                    }
                }
            }

            try {
                if(create_new) {
                    // Create new bucket at exact value
                    Bucket bucket;
                    bucket.base_value = value;
                    bucket.add(value, id);
                    auto bytes = bucket.serialize();

                    target_key_str = make_bucket_key(field, value);
                    MDBX_val k{const_cast<char*>(target_key_str.data()),
                               target_key_str.size()};
                    MDBX_val v{bytes.data(), bytes.size()};
                    rc = mdbx_put(txn, inverted_dbi_, &k, &v, MDBX_UPSERT);
                    if(rc != MDBX_SUCCESS) {
                        mdbx_cursor_close(cursor);
                        return {100, "Failed to create numeric bucket: "
                                             + std::string(mdbx_strerror(rc))};
                    }
                } else {
                    /**
                     * Update existing.
                     * We must re-fetch current key/data because cursor move might have updated key/data.
                     */
                    MDBX_val k{const_cast<char*>(target_key_str.data()),
                               target_key_str.size()};
                    MDBX_val v;
                    rc = mdbx_cursor_get(cursor, &k, &v, MDBX_SET);
                    if(rc != MDBX_SUCCESS) {
                        // Should not happen if logic is correct
                        mdbx_cursor_close(cursor);
                        return {200, "Failed to resync numeric bucket cursor: "
                                             + std::string(mdbx_strerror(rc))};
                    }

                    Bucket bucket = Bucket::deserialize(v.iov_base, v.iov_len, target_base);
                    // Capacity Check
                    if(bucket.ids.size() >= Bucket::MAX_SIZE) {
                        /**
                         * SPLIT LOGIC
                         * Sort is maintained by arrays.
                         * "Slide Split": Scan right from median.
                         * Ensure we don't split a group of identical values.
                         */
                        size_t mid_idx = bucket.ids.size() / 2;
                        size_t probe_right = mid_idx;
                        while(probe_right < bucket.deltas.size() && probe_right > 0
                              && bucket.deltas[probe_right]
                                         == bucket.deltas[probe_right - 1]) {
                            probe_right++;
                        }

                        if(probe_right < bucket.deltas.size()) {
                            mid_idx = probe_right;
                        } else {
                            // Fallback: Try scanning left
                            size_t probe_left = mid_idx;
                            while(probe_left > 0
                                  && bucket.deltas[probe_left]
                                             == bucket.deltas[probe_left - 1]) {
                                probe_left--;
                            }
                            // All identical
                            mid_idx = probe_left > 0 ? probe_left : bucket.deltas.size();
                        }

                        /**
                         * Slide-split could not find a value boundary
                         * -- the bucket is all duplicates of
                         * base_value, so there is no clean place to
                         * cut. Just append the new entry; the bucket
                         * goes momentarily past MAX_SIZE.
                         *
                         * If the new value equals base_value, it
                         * extends the duplicate run; the next insert
                         * of any value will fall into the same
                         * fallthrough.
                         *
                         * If the new value is greater than base_value,
                         * it introduces the boundary that was missing,
                         * and the very next insert hitting this
                         * bucket will split cleanly via the standard
                         * slide-split path below.
                         *
                         * The on-disk count field has been removed
                         * from the bucket payload, so this transient
                         * over-MAX_SIZE state can no longer cause the
                         * uint16_t cliff at 65,536 entries -- the
                         * deserializer derives N from the residual
                         * bytes after the bitmap.
                         */
                        if(mid_idx == bucket.deltas.size()) {
                            bucket.add(value, id);
                            auto bytes = bucket.serialize();
                            MDBX_val k2{const_cast<char*>(target_key_str.data()),
                                        target_key_str.size()};
                            MDBX_val v2{bytes.data(), bytes.size()};
                            rc = mdbx_cursor_put(cursor, &k2, &v2, MDBX_CURRENT);
                            mdbx_cursor_close(cursor);
                            if(rc != MDBX_SUCCESS) {
                                return {100, "Failed to update overfull numeric bucket: "
                                                     + std::string(mdbx_strerror(rc))};
                            }
                            return {SUCCESS, ""};
                        }

                        // Standard Slide Split
                        Bucket right_bucket;
                        right_bucket.base_value = bucket.base_value + bucket.deltas[mid_idx];
                        // Move entries
                        for(size_t i = mid_idx; i < bucket.deltas.size(); ++i) {
                            right_bucket.add(bucket.base_value + bucket.deltas[i],
                                             bucket.ids[i]);
                        }

                        // Truncate left
                        bucket.deltas.resize(mid_idx);
                        bucket.ids.resize(mid_idx);
                        // Rebuild left bitmap
                        bucket.summary_bitmap = ndd::RoaringBitmap();
                        for(auto bucket_id : bucket.ids) {
                            bucket.summary_bitmap.add(bucket_id);
                        }

                        // Now add new value to correct bucket
                        if(value >= right_bucket.base_value) {
                            right_bucket.add(value, id);
                        } else {
                            /**
                             * If value < right, goes to left.
                             * But wait, split point was determined by existing items.
                             * If new value is >= base+split_delta, it goes right.
                             * BUT we just cleared right from b.
                             * Correct logic:
                             * Oh wait, if we added to left, we might overflow again or break order?
                             * Simply: Check which bucket covers it.
                             * Left covers [Base, RightBase-1].
                             * Right covers [RightBase, ...].
                             */
                            bucket.add(value, id);
                        }

                        // Save Left
                        auto left_bytes = bucket.serialize();
                        MDBX_val left_v{left_bytes.data(), left_bytes.size()};
                        MDBX_val left_k{const_cast<char*>(target_key_str.data()),
                                        target_key_str.size()};
                        rc = mdbx_cursor_put(cursor, &left_k, &left_v, MDBX_CURRENT);
                        if(rc != MDBX_SUCCESS) {
                            mdbx_cursor_close(cursor);
                            return {100, "Failed to update split numeric bucket: "
                                                 + std::string(mdbx_strerror(rc))};
                        }

                        // Save Right
                        auto right_bytes = right_bucket.serialize();
                        std::string right_k_str =
                                make_bucket_key(field, right_bucket.base_value);
                        MDBX_val right_k{const_cast<char*>(right_k_str.data()),
                                         right_k_str.size()};
                        MDBX_val right_v{right_bytes.data(), right_bytes.size()};
                        // Use put for new key
                        rc = mdbx_put(txn, inverted_dbi_, &right_k, &right_v, MDBX_UPSERT);
                        if(rc != MDBX_SUCCESS) {
                            mdbx_cursor_close(cursor);
                            return {100, "Failed to write split numeric bucket: "
                                                 + std::string(mdbx_strerror(rc))};
                        }
                    } else {
                        // Normal Insert
                        bucket.add(value, id);
                        auto bytes = bucket.serialize();
                        MDBX_val new_data{bytes.data(), bytes.size()};
                        // Use cursor put to update current
                        rc = mdbx_cursor_put(cursor, &k, &new_data, MDBX_CURRENT);
                        if(rc != MDBX_SUCCESS) {
                            mdbx_cursor_close(cursor);
                            return {100, "Failed to update numeric bucket: "
                                                 + std::string(mdbx_strerror(rc))};
                        }
                    }
                }
            } catch(const std::exception& e) {
                mdbx_cursor_close(cursor);
                return {200, "Corrupt numeric bucket while adding id: "
                                     + std::string(e.what())};
            }

            mdbx_cursor_close(cursor);
            return {SUCCESS, ""};
        }

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
        ndd::OperationResult<> NumericIndex::put_internal(MDBX_txn* txn,
                                                          const std::string& field,
                                                          ndd::idInt id,
                                                          uint32_t value) {

            // 1. Check Forward Index
            std::string fwd_key_str = make_forward_key(field, id);
            MDBX_val fwd_key{const_cast<char*>(fwd_key_str.data()), fwd_key_str.size()};
            MDBX_val fwd_val;

            int rc = mdbx_get(txn, forward_dbi_, &fwd_key, &fwd_val);
            if(rc == MDBX_SUCCESS) {
                if(fwd_val.iov_len != sizeof(uint32_t)) {
                    return {200, "Corrupt numeric forward value for field '" + field + "'"};
                }
                uint32_t old_val;
                std::memcpy(&old_val, fwd_val.iov_base, sizeof(uint32_t));
                if(old_val == value) {
                    return {SUCCESS, ""};
                }
                auto remove_result = remove_from_buckets(txn, field, old_val, id);
                if(!remove_result.ok()) {
                    return remove_result;
                }
            } else if(rc != MDBX_NOTFOUND) {
                return {100, "Failed to read numeric forward value: "
                                     + std::string(mdbx_strerror(rc))};
            }

            // 2. Update Forward
            MDBX_val new_val_data{&value, sizeof(uint32_t)};
            rc = mdbx_put(txn, forward_dbi_, &fwd_key, &new_val_data, MDBX_UPSERT);
            if(rc != MDBX_SUCCESS) {
                return {100, "Failed to write numeric forward value: "
                                     + std::string(mdbx_strerror(rc))};
            }

            // 3. Add to Inverted Buckets
            return add_to_buckets(txn, field, value, id);
        }

        NumericIndex::NumericIndex(MDBX_env* env) :
            env_(env) {
            MDBX_txn* txn = nullptr;
            int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
            if(rc != MDBX_SUCCESS) {
                throw std::runtime_error(std::string("Failed to begin NumericIndex init: ")
                                         + mdbx_strerror(rc));
            }

            rc = mdbx_dbi_open(txn, "numeric_forward", MDBX_CREATE, &forward_dbi_);
            if(rc != MDBX_SUCCESS) {
                mdbx_txn_abort(txn);
                throw std::runtime_error(std::string("Failed to open numeric_forward dbi: ")
                                         + mdbx_strerror(rc));
            }

            rc = mdbx_dbi_open(txn, "numeric_inverted", MDBX_CREATE, &inverted_dbi_);
            if(rc != MDBX_SUCCESS) {
                mdbx_txn_abort(txn);
                throw std::runtime_error(std::string("Failed to open numeric_inverted dbi: ")
                                         + mdbx_strerror(rc));
            }

            rc = mdbx_txn_commit(txn);
            if(rc != MDBX_SUCCESS) {
                throw std::runtime_error(std::string("Failed to commit NumericIndex init: ")
                                         + mdbx_strerror(rc));
            }
        }

        /*
         * Writes a batch of numeric filter entries in bounded MDBX write transaction chunks.
         *
         * Return codes:
         * 0 = success
         * 100 = MDBX transaction or commit failure; caller should log ERROR and return HTTP 500
         * 100-199 = propagated MDBX/storage failure from per-entry writes
         * 200-299 = propagated corruption/invariant failure from per-entry writes
         */
        ndd::OperationResult<> NumericIndex::put_batch(
                const std::vector<NumericBatchEntry>& entries) {
            if(entries.empty()) {
                return {SUCCESS, ""};
            }

            for(size_t start = 0; start < entries.size(); start += BATCH_TXN_CHUNK_SIZE) {
                size_t end = std::min(start + BATCH_TXN_CHUNK_SIZE, entries.size());

                MDBX_txn* txn = nullptr;
                int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
                if(rc != MDBX_SUCCESS) {
                    return {100, "Failed to begin numeric batch write transaction: "
                                         + std::string(mdbx_strerror(rc))};
                }

                for(size_t i = start; i < end; ++i) {
                    const auto& entry = entries[i];
                    auto put_result = put_internal(txn, entry.field, entry.id, entry.value);
                    if(!put_result.ok()) {
                        mdbx_txn_abort(txn);
                        return put_result;
                    }
                }

                rc = mdbx_txn_commit(txn);
                if(rc != MDBX_SUCCESS) {
                    return {100, "Failed to commit numeric batch write transaction: "
                                         + std::string(mdbx_strerror(rc))};
                }
            }
            return {SUCCESS, ""};
        }

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
        ndd::OperationResult<> NumericIndex::remove(const std::string& field, ndd::idInt id) {
            MDBX_txn* txn = nullptr;
            int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
            if(rc != MDBX_SUCCESS) {
                return {100, "Failed to begin numeric remove transaction: "
                                     + std::string(mdbx_strerror(rc))};
            }

            std::string fwd_key_str = make_forward_key(field, id);
            MDBX_val fwd_key{const_cast<char*>(fwd_key_str.data()), fwd_key_str.size()};
            MDBX_val fwd_val;

            rc = mdbx_get(txn, forward_dbi_, &fwd_key, &fwd_val);
            if(rc == MDBX_NOTFOUND) {
                mdbx_txn_abort(txn);
                return {SUCCESS, ""};
            }
            if(rc != MDBX_SUCCESS) {
                mdbx_txn_abort(txn);
                return {100, "Failed to read numeric forward value for remove: "
                                     + std::string(mdbx_strerror(rc))};
            }
            if(fwd_val.iov_len != sizeof(uint32_t)) {
                mdbx_txn_abort(txn);
                return {200, "Corrupt numeric forward value for field '" + field + "'"};
            }

            uint32_t old_val;
            std::memcpy(&old_val, fwd_val.iov_base, sizeof(uint32_t));
            auto remove_result = remove_from_buckets(txn, field, old_val, id);
            if(!remove_result.ok()) {
                mdbx_txn_abort(txn);
                return remove_result;
            }

            rc = mdbx_del(txn, forward_dbi_, &fwd_key, nullptr);
            if(rc != MDBX_SUCCESS && rc != MDBX_NOTFOUND) {
                mdbx_txn_abort(txn);
                return {100, "Failed to delete numeric forward value: "
                                     + std::string(mdbx_strerror(rc))};
            }

            rc = mdbx_txn_commit(txn);
            if(rc != MDBX_SUCCESS) {
                return {100, "Failed to commit numeric remove transaction: "
                                     + std::string(mdbx_strerror(rc))};
            }
            return {SUCCESS, ""};
        }

        /*
         * Computes a bitmap of ids whose numeric field value falls within an inclusive sortable range.
         *
         * Return codes:
         * 0 = success
         * 100 = MDBX transaction, cursor, or scan failure; caller should log ERROR and return HTTP 500
         * 200 = corrupt numeric bucket payload; caller should log ERROR and return HTTP 500
         */
        ndd::OperationResult<ndd::RoaringBitmap>
        NumericIndex::range(const std::string& field, uint32_t min_val, uint32_t max_val) {
            ndd::RoaringBitmap result;
            MDBX_txn* txn = nullptr;
            int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &txn);
            if(rc != MDBX_SUCCESS) {
                return {100, "Failed to begin numeric range transaction: "
                                     + std::string(mdbx_strerror(rc))};
            }

            MDBX_cursor* cursor = nullptr;
            rc = mdbx_cursor_open(txn, inverted_dbi_, &cursor);
            if(rc != MDBX_SUCCESS) {
                mdbx_txn_abort(txn);
                return {100, "Failed to open numeric range cursor: "
                                     + std::string(mdbx_strerror(rc))};
            }

            // 1. Find Start Bucket
            std::string start_k = make_bucket_key(field, min_val);
            MDBX_val key{const_cast<char*>(start_k.data()), start_k.size()};
            MDBX_val data;

            rc = mdbx_cursor_get(cursor, &key, &data, MDBX_SET_RANGE);
            if(rc == MDBX_SUCCESS) {
                std::string fkey(static_cast<char*>(key.iov_base), key.iov_len);
                if(fkey.rfind(field + ":", 0) != 0 || parse_bucket_key_val(fkey) > min_val) {
                    // Check if we need to back up
                    MDBX_val prev_key = key;
                    MDBX_val prev_data;
                    // Check prev
                    int prev_rc = mdbx_cursor_get(cursor, &prev_key, &prev_data, MDBX_PREV);
                    if(prev_rc == MDBX_SUCCESS) {
                        std::string prev_key_str(static_cast<char*>(prev_key.iov_base),
                                                 prev_key.iov_len);
                        if(prev_key_str.rfind(field + ":", 0) == 0) {
                            // Prev is valid start
                            key = prev_key;
                            data = prev_data;
                        }
                    } else if(prev_rc != MDBX_NOTFOUND) {
                        mdbx_cursor_close(cursor);
                        mdbx_txn_abort(txn);
                        return {100, "Failed to seek previous numeric range bucket: "
                                             + std::string(mdbx_strerror(prev_rc))};
                    }
                }
            } else if(rc == MDBX_NOTFOUND) {
                rc = mdbx_cursor_get(cursor, &key, &data, MDBX_LAST);
                if(rc == MDBX_SUCCESS) {
                    std::string fkey(static_cast<char*>(key.iov_base), key.iov_len);
                    if(fkey.rfind(field + ":", 0) != 0) {
                        rc = MDBX_NOTFOUND;
                    }
                } else if(rc != MDBX_NOTFOUND) {
                    mdbx_cursor_close(cursor);
                    mdbx_txn_abort(txn);
                    return {100, "Failed to seek last numeric range bucket: "
                                         + std::string(mdbx_strerror(rc))};
                }
            } else {
                mdbx_cursor_close(cursor);
                mdbx_txn_abort(txn);
                return {100, "Failed to seek numeric range bucket: "
                                     + std::string(mdbx_strerror(rc))};
            }

            try {
                // Iterate forward
                while(rc == MDBX_SUCCESS) {
                    std::string cur_key(static_cast<char*>(key.iov_base), key.iov_len);
                    if(cur_key.rfind(field + ":", 0) != 0) {
                        break;
                    }

                    uint32_t bucket_base = parse_bucket_key_val(cur_key);
                    if(bucket_base > max_val) {
                        break;
                    }

                    /**
                     * Coarse full-coverage fast path.
                     *
                     * A bucket can hold values in [base, base+MAX_DELTA]
                     * by construction. If that whole extent is inside
                     * [min_val, max_val], we don't need to look at the
                     * deltas/ids arrays -- the bucket's summary_bitmap
                     * already enumerates every id that belongs in the
                     * result. Skip the full deserialize and read just
                     * the bitmap header.
                     *
                     * This fires on every interior bucket of a wide
                     * range scan, so for "score >= a AND score <= b"
                     * with a wide [a,b] only the start and end buckets
                     * pay the deltas/ids parsing cost.
                     */
                    if(bucket_base >= min_val
                       && static_cast<uint64_t>(bucket_base) + Bucket::MAX_DELTA
                                  <= max_val) {
                        result |= Bucket::read_summary_bitmap(data.iov_base,
                                                              data.iov_len);
                        rc = mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT);
                        continue;
                    }

                    Bucket bucket = Bucket::deserialize(data.iov_base,
                                                        data.iov_len,
                                                        bucket_base);

                    if(bucket.ids.empty()) {
                        rc = mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT);
                        continue;
                    }

                    uint32_t bucket_min = bucket.get_value(0);
                    uint32_t bucket_max = bucket.get_value(bucket.ids.size() - 1);

                    if(bucket_min >= min_val && bucket_max <= max_val) {
                        /**
                         * Full overlap. summary_bitmap is a superset
                         * of bucket.ids (it also carries any bitmap-
                         * only entries from the saturated-duplicate
                         * path, all of which have value == base_value
                         * and therefore lie inside the query since
                         * bucket_min == base_value is inside).
                         */
                        result |= bucket.summary_bitmap;
                    } else {
                        // Partial overlap on the parallel arrays.
                        for(size_t i = 0; i < bucket.ids.size(); ++i) {
                            uint32_t value = bucket.get_value(i);
                            if(value >= min_val && value <= max_val) {
                                result.add(bucket.ids[i]);
                            }
                        }
                        /**
                         * Bitmap-only entries (cardinality > ids.size())
                         * exist when Bucket::add saturated and absorbed
                         * duplicates of base_value into summary_bitmap
                         * only. Every such entry has value == base_value
                         * by construction. Include them iff base_value
                         * lies in [min_val, max_val].
                         *
                         * "bitmap-only" set =
                         *     summary_bitmap minus { ids[i] : deltas[i] != 0 }
                         * because the delta-zero ids in ids[] are also
                         * in the bitmap and would be redundantly added,
                         * but Roaring set union is idempotent so
                         * removing only the delta>0 entries is enough
                         * to leave us with all delta-zero ids (whether
                         * they live in ids[] or only in the bitmap).
                         */
                        if(bucket_base >= min_val && bucket_base <= max_val
                           && bucket.summary_bitmap.cardinality()
                                      > bucket.ids.size()) {
                            ndd::RoaringBitmap bitmap_only =
                                    bucket.summary_bitmap;
                            for(size_t i = 0; i < bucket.ids.size(); ++i) {
                                if(bucket.deltas[i] != 0) {
                                    bitmap_only.remove(bucket.ids[i]);
                                }
                            }
                            result |= bitmap_only;
                        }
                    }

                    rc = mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT);
                }
            } catch(const std::exception& e) {
                mdbx_cursor_close(cursor);
                mdbx_txn_abort(txn);
                return {200, "Corrupt numeric bucket during range scan: "
                                     + std::string(e.what())};
            }

            mdbx_cursor_close(cursor);
            mdbx_txn_abort(txn);
            if(rc != MDBX_SUCCESS && rc != MDBX_NOTFOUND) {
                return {100, "Failed during numeric range scan: "
                                     + std::string(mdbx_strerror(rc))};
            }
            return {SUCCESS, "", std::move(result)};
        }

        /*
         * Checks whether one id has a numeric field value inside an inclusive sortable range.
         *
         * Return codes:
         * 0 = success
         * 100 = MDBX transaction or read failure; caller should log ERROR and return HTTP 500
         * 200 = corrupt numeric forward value; caller should log ERROR and return HTTP 500
         */
        ndd::OperationResult<bool>
        NumericIndex::check_range(const std::string& field,
                                  ndd::idInt id,
                                  uint32_t min_val,
                                  uint32_t max_val) {
            MDBX_txn* txn = nullptr;
            int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &txn);
            if(rc != MDBX_SUCCESS) {
                return {100, "Failed to begin numeric check transaction: "
                                     + std::string(mdbx_strerror(rc))};
            }

            std::string fwd_key_str = make_forward_key(field, id);
            MDBX_val fwd_key{const_cast<char*>(fwd_key_str.data()), fwd_key_str.size()};
            MDBX_val fwd_val;

            rc = mdbx_get(txn, forward_dbi_, &fwd_key, &fwd_val);
            if(rc == MDBX_NOTFOUND) {
                mdbx_txn_abort(txn);
                return {SUCCESS, "", false};
            }
            if(rc != MDBX_SUCCESS) {
                mdbx_txn_abort(txn);
                return {100, "Failed to read numeric forward value during check: "
                                     + std::string(mdbx_strerror(rc))};
            }
            if(fwd_val.iov_len != sizeof(uint32_t)) {
                mdbx_txn_abort(txn);
                return {200, "Corrupt numeric forward value for field '" + field + "'"};
            }

            uint32_t value;
            std::memcpy(&value, fwd_val.iov_base, sizeof(uint32_t));
            mdbx_txn_abort(txn);
            return {SUCCESS, "", value >= min_val && value <= max_val};
        }

    }  // namespace filter
}  // namespace ndd
