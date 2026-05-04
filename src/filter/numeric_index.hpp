#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <utility>
#include "mdbx/mdbx.h"
#include "../utils/log.hpp"
#include "../core/types.hpp"
#include "../utils/types.hpp"

namespace ndd {
    namespace filter {

        struct NumericBatchEntry {
            std::string field;
            ndd::idInt id;
            uint32_t value;

            NumericBatchEntry(std::string field_in, ndd::idInt id_in, uint32_t value_in) :
                field(std::move(field_in)),
                id(id_in),
                value(value_in) {}
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

            // Helper to get actual value
            uint32_t get_value(size_t index) const {
                return base_value + deltas[index];
            }

            void add(uint32_t val, ndd::idInt id) {
                if (val < base_value) {
                     // Should not happen if Key logic is correct
                     throw std::runtime_error("Insert value < Base Value"); 
                }
                uint32_t delta_32 = val - base_value;
                if (delta_32 > MAX_DELTA) {
                    throw std::runtime_error("Delta overflow");
                }
                
                // Maintain sorted order by Value (Delta)
                uint16_t delta = static_cast<uint16_t>(delta_32);
                
                // Find insertion point
                auto it = std::lower_bound(deltas.begin(), deltas.end(), delta);
                size_t index = std::distance(deltas.begin(), it);

                deltas.insert(it, delta);
                ids.insert(ids.begin() + index, id);
                
                summary_bitmap.add(id);
                is_dirty = true;
            }

            bool remove(ndd::idInt id) {
                // Find index by ID (linear scan needed as ids are not sorted)
                for (size_t i = 0; i < ids.size(); ++i) {
                    if (ids[i] == id) {
                        ids.erase(ids.begin() + i);
                        deltas.erase(deltas.begin() + i);
                        
                        // Rebuild or update bitmap? Roaring remove is fast
                        summary_bitmap.remove(id);
                        is_dirty = true;
                        return true;
                    }
                }
                return false;
            }

            // Serialization Format:
            // [BitmapSize (4)]
            // [Bitmap Bytes]
            // [Count (2)]
            // [Deltas (Count * 2)]
            // [IDs (Count * sizeof(idInt))]
            std::vector<uint8_t> serialize() const {
                // Optimize bitmap
                const_cast<ndd::RoaringBitmap&>(summary_bitmap).runOptimize();
                
                size_t bm_size = summary_bitmap.getSizeInBytes();
                uint16_t count = static_cast<uint16_t>(ids.size());
                
                size_t total_size = 4 + bm_size + 2 + (count * 2) + (count * sizeof(ndd::idInt));
                std::vector<uint8_t> buffer(total_size);
                uint8_t* ptr = buffer.data();

                // 1. Bitmap Header
                uint32_t bm_size_32 = static_cast<uint32_t>(bm_size);
                std::memcpy(ptr, &bm_size_32, 4); ptr += 4;

                // 2. Bitmap Data
                if (bm_size > 0) {
                    summary_bitmap.write(reinterpret_cast<char*>(ptr));
                    ptr += bm_size;
                }

                // 3. Count
                std::memcpy(ptr, &count, 2); ptr += 2;

                // 4. Deltas
                if (count > 0) {
                    std::memcpy(ptr, deltas.data(), count * 2); ptr += count * 2;
                }

                // 5. IDs
                if (count > 0) {
                    std::memcpy(ptr, ids.data(), count * sizeof(ndd::idInt)); 
                }
                
                return buffer;
            }

            static Bucket deserialize(const void* data, size_t len, uint32_t base_val) {
                Bucket b;
                b.base_value = base_val;
                
                if (len < 6) return b; // Min valid size

                const uint8_t* ptr = static_cast<const uint8_t*>(data);
                const uint8_t* end = ptr + len;
                
                // 1. Bitmap Size
                uint32_t bm_size;
                std::memcpy(&bm_size, ptr, 4); ptr += 4;

                if (ptr + bm_size > end) {
                    throw std::runtime_error("Bucket corrupt: invalid bitmap size");
                }

                // 2. Bitmap
                if (bm_size > 0) {
                   b.summary_bitmap = ndd::RoaringBitmap::read(reinterpret_cast<const char*>(ptr));
                   ptr += bm_size;
                }

                if (ptr + 2 > end) throw std::runtime_error("Bucket corrupt: truncated count");

                // 3. Count
                uint16_t count;
                std::memcpy(&count, ptr, 2); ptr += 2;

                // 4. Deltas & IDs
                if (count > 0) {
                    size_t delta_size = count * 2;
                    size_t id_size = count * sizeof(ndd::idInt);
                    
                    if (ptr + delta_size + id_size > end) {
                         throw std::runtime_error("Bucket corrupt: truncated Data");
                    }

                    b.deltas.resize(count);
                    std::memcpy(b.deltas.data(), ptr, delta_size); ptr += delta_size;

                    b.ids.resize(count);
                    std::memcpy(b.ids.data(), ptr, id_size); 
                }
                
                return b;
            }

            // Fast access to just the bitmap (for middle buckets)
            static ndd::RoaringBitmap read_summary_bitmap(const void* data, size_t len) {
               const uint8_t* ptr = static_cast<const uint8_t*>(data);
               uint32_t bm_size;
               std::memcpy(&bm_size, ptr, 4); ptr += 4;
               if(bm_size == 0) return ndd::RoaringBitmap();
               return ndd::RoaringBitmap::read(reinterpret_cast<const char*>(ptr));
            }

            bool is_full() const { return ids.size() >= MAX_SIZE; }
            bool is_empty() const { return ids.empty(); }
        };

        class NumericIndex {
        private:
            MDBX_env* env_;
            MDBX_dbi forward_dbi_;   // ID -> Value (Field:ID -> Value)
            MDBX_dbi inverted_dbi_;  // BucketKey -> BucketBlob

            std::string make_forward_key(const std::string& field, ndd::idInt id) {
                return field + ":" + std::to_string(id);
            }

            std::string make_bucket_key(const std::string& field, uint32_t start_val) {
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

            uint32_t parse_bucket_key_val(const std::string& key) {
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
            ndd::OperationResult<> remove_from_buckets(MDBX_txn* txn,
                                                       const std::string& field,
                                                       uint32_t value,
                                                       ndd::idInt id) {
                std::string bkey_str = make_bucket_key(field, value);
                MDBX_val key{const_cast<char*>(bkey_str.data()), bkey_str.size()};
                MDBX_val data;
                MDBX_cursor* cursor = nullptr;
                int rc = mdbx_cursor_open(txn, inverted_dbi_, &cursor);
                if(rc != MDBX_SUCCESS) {
                    return {100, "Failed to open numeric bucket remove cursor: "
                                         + std::string(mdbx_strerror(rc))};
                }

                rc = mdbx_cursor_get(cursor, &key, &data, MDBX_SET_RANGE);
                if(rc == MDBX_SUCCESS) {
                    std::string found_key(static_cast<char*>(key.iov_base), key.iov_len);
                    if(found_key.rfind(field + ":", 0) != 0
                       || parse_bucket_key_val(found_key) > value) {
                        rc = mdbx_cursor_get(cursor, &key, &data, MDBX_PREV);
                    }
                } else if(rc == MDBX_NOTFOUND) {
                    rc = mdbx_cursor_get(cursor, &key, &data, MDBX_LAST);
                }

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
            ndd::OperationResult<> add_to_buckets(MDBX_txn* txn,
                                                  const std::string& field,
                                                  uint32_t value,
                                                  ndd::idInt id) {
                MDBX_cursor* cursor = nullptr;
                int rc = mdbx_cursor_open(txn, inverted_dbi_, &cursor);
                if(rc != MDBX_SUCCESS) {
                    return {100, "Failed to open numeric bucket add cursor: "
                                         + std::string(mdbx_strerror(rc))};
                }

                std::string search_key = make_bucket_key(field, value);
                MDBX_val key{const_cast<char*>(search_key.data()), search_key.size()};
                MDBX_val data;

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
                        MDBX_val k{const_cast<char*>(target_key_str.data()),
                                   target_key_str.size()};
                        MDBX_val v;
                        rc = mdbx_cursor_get(cursor, &k, &v, MDBX_SET);
                        if(rc != MDBX_SUCCESS) {
                            mdbx_cursor_close(cursor);
                            return {200, "Failed to resync numeric bucket cursor: "
                                                 + std::string(mdbx_strerror(rc))};
                        }

                        Bucket bucket = Bucket::deserialize(v.iov_base, v.iov_len, target_base);
                        if(bucket.ids.size() >= Bucket::MAX_SIZE) {
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
                                size_t probe_left = mid_idx;
                                while(probe_left > 0
                                      && bucket.deltas[probe_left]
                                                 == bucket.deltas[probe_left - 1]) {
                                    probe_left--;
                                }
                                mid_idx = probe_left > 0 ? probe_left : bucket.deltas.size();
                            }

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

                            Bucket right_bucket;
                            right_bucket.base_value = bucket.base_value + bucket.deltas[mid_idx];
                            for(size_t i = mid_idx; i < bucket.deltas.size(); ++i) {
                                right_bucket.add(bucket.base_value + bucket.deltas[i],
                                                 bucket.ids[i]);
                            }

                            bucket.deltas.resize(mid_idx);
                            bucket.ids.resize(mid_idx);
                            bucket.summary_bitmap = ndd::RoaringBitmap();
                            for(auto bucket_id : bucket.ids) {
                                bucket.summary_bitmap.add(bucket_id);
                            }

                            if(value >= right_bucket.base_value) {
                                right_bucket.add(value, id);
                            } else {
                                bucket.add(value, id);
                            }

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

                            auto right_bytes = right_bucket.serialize();
                            std::string right_k_str =
                                    make_bucket_key(field, right_bucket.base_value);
                            MDBX_val right_k{const_cast<char*>(right_k_str.data()),
                                             right_k_str.size()};
                            MDBX_val right_v{right_bytes.data(), right_bytes.size()};
                            rc = mdbx_put(txn, inverted_dbi_, &right_k, &right_v, MDBX_UPSERT);
                            if(rc != MDBX_SUCCESS) {
                                mdbx_cursor_close(cursor);
                                return {100, "Failed to write split numeric bucket: "
                                                     + std::string(mdbx_strerror(rc))};
                            }
                        } else {
                            bucket.add(value, id);
                            auto bytes = bucket.serialize();
                            MDBX_val new_data{bytes.data(), bytes.size()};
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
            ndd::OperationResult<> put_internal(MDBX_txn* txn,
                                                const std::string& field,
                                                ndd::idInt id,
                                                uint32_t value) {
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

                MDBX_val new_val_data{&value, sizeof(uint32_t)};
                rc = mdbx_put(txn, forward_dbi_, &fwd_key, &new_val_data, MDBX_UPSERT);
                if(rc != MDBX_SUCCESS) {
                    return {100, "Failed to write numeric forward value: "
                                         + std::string(mdbx_strerror(rc))};
                }

                return add_to_buckets(txn, field, value, id);
            }

        public:
            NumericIndex(MDBX_env* env) :
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
             * Writes a batch of numeric filter entries in one MDBX write transaction.
             *
             * Return codes:
             * 0 = success
             * 100 = MDBX transaction or commit failure; caller should log ERROR and return HTTP 500
             * 100-199 = propagated MDBX/storage failure from per-entry writes
             * 200-299 = propagated corruption/invariant failure from per-entry writes
             */
            ndd::OperationResult<> put_batch(const std::vector<NumericBatchEntry>& entries) {
                if(entries.empty()) {
                    return {SUCCESS, ""};
                }

                MDBX_txn* txn = nullptr;
                int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
                if(rc != MDBX_SUCCESS) {
                    return {100, "Failed to begin numeric batch write transaction: "
                                         + std::string(mdbx_strerror(rc))};
                }

                for(const auto& entry : entries) {
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
            ndd::OperationResult<> remove(const std::string& field, ndd::idInt id) {
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
            range(const std::string& field, uint32_t min_val, uint32_t max_val) {
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

                std::string start_k = make_bucket_key(field, min_val);
                MDBX_val key{const_cast<char*>(start_k.data()), start_k.size()};
                MDBX_val data;

                rc = mdbx_cursor_get(cursor, &key, &data, MDBX_SET_RANGE);
                if(rc == MDBX_SUCCESS) {
                    std::string fkey(static_cast<char*>(key.iov_base), key.iov_len);
                    if(fkey.rfind(field + ":", 0) != 0 || parse_bucket_key_val(fkey) > min_val) {
                        MDBX_val prev_key = key;
                        MDBX_val prev_data;
                        int prev_rc = mdbx_cursor_get(cursor, &prev_key, &prev_data, MDBX_PREV);
                        if(prev_rc == MDBX_SUCCESS) {
                            std::string prev_key_str(static_cast<char*>(prev_key.iov_base),
                                                     prev_key.iov_len);
                            if(prev_key_str.rfind(field + ":", 0) == 0) {
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
                    while(rc == MDBX_SUCCESS) {
                        std::string cur_key(static_cast<char*>(key.iov_base), key.iov_len);
                        if(cur_key.rfind(field + ":", 0) != 0) {
                            break;
                        }

                        uint32_t bucket_base = parse_bucket_key_val(cur_key);
                        if(bucket_base > max_val) {
                            break;
                        }

                        Bucket bucket = Bucket::deserialize(data.iov_base,
                                                            data.iov_len,
                                                            bucket_base);
                        if(!bucket.ids.empty()) {
                            uint32_t bucket_min = bucket.get_value(0);
                            uint32_t bucket_max = bucket.get_value(bucket.ids.size() - 1);

                            if(bucket_min >= min_val && bucket_max <= max_val) {
                                result |= bucket.summary_bitmap;
                            } else {
                                for(size_t i = 0; i < bucket.ids.size(); ++i) {
                                    uint32_t value = bucket.get_value(i);
                                    if(value >= min_val && value <= max_val) {
                                        result.add(bucket.ids[i]);
                                    }
                                }
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
            check_range(const std::string& field,
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
        };

    } // namespace filter
} // namespace ndd
