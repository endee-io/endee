#pragma once

#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "settings.hpp"
#include "log.hpp"

#if defined(NDD_DB_BACKEND_LMDB)
#    include "lmdb/lmdb.h"

using MDBX_env = MDB_env;
using MDBX_txn = MDB_txn;
using MDBX_dbi = MDB_dbi;
using MDBX_cursor = MDB_cursor;
using MDBX_stat = MDB_stat;
using MDBX_env_flags_t = unsigned int;
using MDBX_txn_flags_t = unsigned int;
using MDBX_put_flags_t = unsigned int;
using MDBX_cursor_op = MDB_cursor_op;

struct MDBX_val {
    void* iov_base;
    size_t iov_len;
};

static constexpr int MDBX_SUCCESS = MDB_SUCCESS;
static constexpr int MDBX_NOTFOUND = MDB_NOTFOUND;
static constexpr int MDBX_MAP_FULL = MDB_MAP_FULL;
static constexpr int MDBX_MAP_RESIZED = MDB_MAP_RESIZED;
static constexpr int MDBX_UNABLE_EXTEND_MAPSIZE = MDB_MAP_RESIZED;
static constexpr int MDBX_PROBLEM = MDB_PROBLEM;

static constexpr MDBX_txn_flags_t MDBX_TXN_RDONLY = MDB_RDONLY;
static constexpr MDBX_txn_flags_t MDBX_TXN_READWRITE = 0;

static constexpr MDBX_put_flags_t MDBX_CREATE = MDB_CREATE;
static constexpr MDBX_put_flags_t MDBX_INTEGERKEY = MDB_INTEGERKEY;
static constexpr MDBX_put_flags_t MDBX_CURRENT = MDB_CURRENT;
static constexpr MDBX_put_flags_t MDBX_UPSERT = 0;

static constexpr MDBX_cursor_op MDBX_FIRST = MDB_FIRST;
static constexpr MDBX_cursor_op MDBX_LAST = MDB_LAST;
static constexpr MDBX_cursor_op MDBX_NEXT = MDB_NEXT;
static constexpr MDBX_cursor_op MDBX_PREV = MDB_PREV;
static constexpr MDBX_cursor_op MDBX_SET = MDB_SET;
static constexpr MDBX_cursor_op MDBX_SET_RANGE = MDB_SET_RANGE;

static constexpr unsigned MDBX_WRITEMAP = MDB_WRITEMAP;
static constexpr unsigned MDBX_MAPASYNC = MDB_MAPASYNC;
static constexpr unsigned MDBX_NORDAHEAD = MDB_NORDAHEAD;
static constexpr unsigned MDBX_NOSUBDIR = MDB_NOSUBDIR;
static constexpr unsigned MDBX_NOSTICKYTHREADS = MDB_NOTLS;
static constexpr unsigned MDBX_LIFORECLAIM = 0;

inline MDB_val to_lmdb_val(const MDBX_val& value) {
    MDB_val converted{};
    converted.mv_size = value.iov_len;
    converted.mv_data = value.iov_base;
    return converted;
}

inline void from_lmdb_val(const MDB_val& value, MDBX_val* out) {
    out->iov_len = value.mv_size;
    out->iov_base = value.mv_data;
}

inline bool cursor_get_requires_key(MDBX_cursor_op op) {
    return op == MDBX_SET || op == MDBX_SET_RANGE;
}

inline int mdbx_env_create(MDBX_env** env) { return mdb_env_create(env); }

inline int mdbx_env_set_geometry(MDBX_env* env,
                                 intptr_t size_lower,
                                 intptr_t size_now,
                                 intptr_t size_upper,
                                 intptr_t growth_step,
                                 intptr_t shrink_threshold,
                                 intptr_t pagesize) {
    (void)size_lower;
    (void)growth_step;
    (void)shrink_threshold;
    (void)pagesize;

    const intptr_t requested_size = size_now > 0 ? size_now : size_upper;
    if(requested_size <= 0) {
        return MDBX_SUCCESS;
    }

    return mdb_env_set_mapsize(env, static_cast<mdb_size_t>(requested_size));
}

inline int mdbx_env_open(MDBX_env* env, const char* path, MDBX_env_flags_t flags, mdb_mode_t mode) {
    return mdb_env_open(env, path, flags, mode);
}

inline int mdbx_env_set_maxdbs(MDBX_env* env, MDB_dbi dbs) { return mdb_env_set_maxdbs(env, dbs); }

inline void mdbx_env_close(MDBX_env* env) { mdb_env_close(env); }

inline int mdbx_txn_begin(MDBX_env* env, MDBX_txn* parent, MDBX_txn_flags_t flags, MDBX_txn** txn) {
    return mdb_txn_begin(env, parent, flags, txn);
}

inline int mdbx_txn_commit(MDBX_txn* txn) { return mdb_txn_commit(txn); }

inline void mdbx_txn_abort(MDBX_txn* txn) { mdb_txn_abort(txn); }

inline int mdbx_dbi_open(MDBX_txn* txn, const char* name, MDBX_put_flags_t flags, MDBX_dbi* dbi) {
    return mdb_dbi_open(txn, name, flags, dbi);
}

inline void mdbx_dbi_close(MDBX_env* env, MDBX_dbi dbi) { mdb_dbi_close(env, dbi); }

inline int mdbx_dbi_stat(MDBX_txn* txn, MDBX_dbi dbi, MDBX_stat* stat, size_t stat_bytes) {
    (void)stat_bytes;
    return mdb_stat(txn, dbi, stat);
}

inline int mdbx_get(MDBX_txn* txn, MDBX_dbi dbi, MDBX_val* key, MDBX_val* data) {
    MDB_val lmdb_key = to_lmdb_val(*key);
    MDB_val lmdb_data{};
    const int rc = mdb_get(txn, dbi, &lmdb_key, &lmdb_data);
    if(rc == MDBX_SUCCESS) {
        from_lmdb_val(lmdb_data, data);
    }
    return rc;
}

inline int mdbx_put(MDBX_txn* txn,
                    MDBX_dbi dbi,
                    MDBX_val* key,
                    MDBX_val* data,
                    MDBX_put_flags_t flags) {
    MDB_val lmdb_key = to_lmdb_val(*key);
    MDB_val lmdb_data = to_lmdb_val(*data);
    return mdb_put(txn, dbi, &lmdb_key, &lmdb_data, flags);
}

inline int mdbx_del(MDBX_txn* txn, MDBX_dbi dbi, MDBX_val* key, MDBX_val* data) {
    MDB_val lmdb_key = to_lmdb_val(*key);
    MDB_val lmdb_data{};
    MDB_val* lmdb_data_ptr = nullptr;

    if(data) {
        lmdb_data = to_lmdb_val(*data);
        lmdb_data_ptr = &lmdb_data;
    }

    return mdb_del(txn, dbi, &lmdb_key, lmdb_data_ptr);
}

inline int mdbx_cursor_open(MDBX_txn* txn, MDBX_dbi dbi, MDBX_cursor** cursor) {
    return mdb_cursor_open(txn, dbi, cursor);
}

inline void mdbx_cursor_close(MDBX_cursor* cursor) { mdb_cursor_close(cursor); }

inline int mdbx_cursor_get(MDBX_cursor* cursor,
                           MDBX_val* key,
                           MDBX_val* data,
                           MDBX_cursor_op op) {
    MDB_val lmdb_key{};
    MDB_val lmdb_data{};

    if(cursor_get_requires_key(op) && key) {
        lmdb_key = to_lmdb_val(*key);
    }

    const int rc = mdb_cursor_get(cursor, &lmdb_key, &lmdb_data, op);
    if(rc == MDBX_SUCCESS) {
        if(key) {
            from_lmdb_val(lmdb_key, key);
        }
        if(data) {
            from_lmdb_val(lmdb_data, data);
        }
    }
    return rc;
}

inline int mdbx_cursor_put(MDBX_cursor* cursor,
                           MDBX_val* key,
                           MDBX_val* data,
                           MDBX_put_flags_t flags) {
    MDB_val lmdb_key = to_lmdb_val(*key);
    MDB_val lmdb_data = to_lmdb_val(*data);
    return mdb_cursor_put(cursor, &lmdb_key, &lmdb_data, flags);
}

inline int mdbx_cursor_del(MDBX_cursor* cursor, MDBX_put_flags_t flags) {
    return mdb_cursor_del(cursor, flags);
}

inline const char* mdbx_strerror(int err) { return mdb_strerror(err); }

inline void print_mdbx_stats(void) {}

#elif defined(NDD_DB_BACKEND_MDBX)
#    include "mdbx/mdbx.h"
#else
#    error "Define exactly one database backend: NDD_DB_BACKEND_MDBX or NDD_DB_BACKEND_LMDB"
#endif

namespace ndd::db {

inline constexpr const char* backend_name() {
#if defined(NDD_DB_BACKEND_LMDB)
    return "lmdb";
#else
    return "mdbx";
#endif
}

class MapFullError final : public std::runtime_error {
public:
    explicit MapFullError(const std::string& what_arg) :
        std::runtime_error(what_arg) {}
};

inline uint64_t page_size_bytes() { return 1ULL << settings::MDBX_PAGE_SIZE_BITS; }

inline uint64_t bytes_from_bits(size_t bits) { return 1ULL << bits; }

inline uint64_t round_up_to_page(uint64_t value) {
    const uint64_t page_size = page_size_bytes();
    if(page_size == 0) {
        return value;
    }

    const uint64_t remainder = value % page_size;
    if(remainder == 0) {
        return value;
    }

    if(value > std::numeric_limits<uint64_t>::max() - (page_size - remainder)) {
        return std::numeric_limits<uint64_t>::max();
    }

    return value + (page_size - remainder);
}

inline std::string format_bytes(uint64_t value) {
    std::ostringstream stream;
    constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    constexpr size_t kUnitCount = sizeof(kUnits) / sizeof(kUnits[0]);

    double scaled = static_cast<double>(value);
    size_t unit_index = 0;
    while(scaled >= 1024.0 && unit_index + 1 < kUnitCount) {
        scaled /= 1024.0;
        ++unit_index;
    }

    stream.setf(std::ios::fixed);
    stream.precision(unit_index == 0 ? 0 : 2);
    stream << scaled << ' ' << kUnits[unit_index];
    return stream.str();
}

struct EnvResizeConfig {
    std::string label;
    uint64_t size_limit_bytes;
    uint64_t growth_step_bytes;
    uint64_t page_size_override_bytes;
    mutable std::mutex resize_mutex;

    EnvResizeConfig(std::string label_in, size_t size_max_bits, size_t growth_bits) :
        label(std::move(label_in)),
        size_limit_bytes(round_up_to_page(bytes_from_bits(size_max_bits))),
        growth_step_bytes(round_up_to_page(bytes_from_bits(growth_bits))),
        page_size_override_bytes(page_size_bytes()) {}

    EnvResizeConfig(const EnvResizeConfig&) = delete;
    EnvResizeConfig& operator=(const EnvResizeConfig&) = delete;
};

inline int apply_mapsize_limit(MDBX_env* env,
                               const EnvResizeConfig& config,
                               uint64_t requested_limit_bytes) {
    const uint64_t limit_bytes = round_up_to_page(requested_limit_bytes);

#if defined(NDD_DB_BACKEND_LMDB)
    return mdbx_env_set_geometry(env,
                                 -1,
                                 static_cast<intptr_t>(limit_bytes),
                                 static_cast<intptr_t>(limit_bytes),
                                 static_cast<intptr_t>(config.growth_step_bytes),
                                 -1,
                                 static_cast<intptr_t>(config.page_size_override_bytes));
#else
    return mdbx_env_set_geometry(env,
                                 -1,
                                 -1,
                                 static_cast<intptr_t>(limit_bytes),
                                 static_cast<intptr_t>(config.growth_step_bytes),
                                 -1,
                                 static_cast<intptr_t>(config.page_size_override_bytes));
#endif
}

inline int configure_env_mapsize(MDBX_env* env, const EnvResizeConfig& config) {
    return apply_mapsize_limit(env, config, config.size_limit_bytes);
}

inline int grow_env_mapsize(MDBX_env* env, EnvResizeConfig& config, const std::string& context) {
    std::lock_guard<std::mutex> lock(config.resize_mutex);

    const uint64_t old_limit = config.size_limit_bytes;
    const uint64_t growth_step =
            config.growth_step_bytes == 0 ? config.page_size_override_bytes : config.growth_step_bytes;

    uint64_t new_limit = old_limit;
    if(new_limit > std::numeric_limits<uint64_t>::max() - growth_step) {
        new_limit = std::numeric_limits<uint64_t>::max();
    } else {
        new_limit += growth_step;
    }
    new_limit = round_up_to_page(new_limit);

    const int rc = apply_mapsize_limit(env, config, new_limit);
    if(rc == MDBX_SUCCESS) {
        config.size_limit_bytes = new_limit;
        LOG_INFO(0,
                 context,
                 "Resized " << config.label << " map for " << backend_name() << ": "
                            << format_bytes(old_limit) << " -> " << format_bytes(new_limit) << " ("
                            << old_limit << " -> " << new_limit << " bytes)");
    }

    return rc;
}

inline bool is_map_full_rc(int rc) { return rc == MDBX_MAP_FULL; }

inline void throw_if_map_full(int rc, const std::string& message) {
    if(is_map_full_rc(rc)) {
        throw MapFullError(message + ": " + mdbx_strerror(rc));
    }
}

inline void throw_if_error(int rc, const std::string& message) {
    throw_if_map_full(rc, message);
    if(rc != MDBX_SUCCESS) {
        throw std::runtime_error(message + ": " + mdbx_strerror(rc));
    }
}

template <typename Fn>
decltype(auto)
with_write_txn_retry(MDBX_env* env, EnvResizeConfig& config, const std::string& context, Fn&& fn) {
    using Result = std::invoke_result_t<Fn, MDBX_txn*>;

    for(;;) {
        MDBX_txn* txn = nullptr;
        const int begin_rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
        throw_if_error(begin_rc, "Failed to begin write transaction for " + config.label);

        bool txn_open = true;

        try {
            if constexpr(std::is_void_v<Result>) {
                fn(txn);

                const int commit_rc = mdbx_txn_commit(txn);
                txn_open = false;

                if(is_map_full_rc(commit_rc)) {
                    const int resize_rc = grow_env_mapsize(env, config, context);
                    throw_if_error(resize_rc, "Failed to resize " + config.label + " after map-full commit");
                    continue;
                }

                throw_if_error(commit_rc, "Failed to commit write transaction for " + config.label);
                return;
            } else {
                auto result = fn(txn);

                if constexpr(std::is_same_v<Result, bool>) {
                    if(!result) {
                        mdbx_txn_abort(txn);
                        return false;
                    }
                }

                const int commit_rc = mdbx_txn_commit(txn);
                txn_open = false;

                if(is_map_full_rc(commit_rc)) {
                    const int resize_rc = grow_env_mapsize(env, config, context);
                    throw_if_error(resize_rc, "Failed to resize " + config.label + " after map-full commit");
                    continue;
                }

                throw_if_error(commit_rc, "Failed to commit write transaction for " + config.label);
                return result;
            }
        } catch(const MapFullError&) {
            if(txn_open) {
                mdbx_txn_abort(txn);
            }

            const int resize_rc = grow_env_mapsize(env, config, context);
            throw_if_error(resize_rc, "Failed to resize " + config.label + " after map-full write");
        } catch(...) {
            if(txn_open) {
                mdbx_txn_abort(txn);
            }
            throw;
        }
    }
}

}  // namespace ndd::db
