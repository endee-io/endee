#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "mdbx/mdbx.h"
#include "../utils/log.hpp"
#include "../utils/settings.hpp"

namespace ndd {
namespace storage {

inline constexpr const char* LAYOUT_META_DBI = "layout_meta";
inline constexpr const char* LAYOUT_VERSION_KEY = "index_layout_version";

class SharedIndexEnv {
private:
    MDBX_env* env_ = nullptr;
    std::string path_;

    static void throw_mdbx(const std::string& context, int rc) {
        throw std::runtime_error(context + ": " + std::string(mdbx_strerror(rc)));
    }

public:
    explicit SharedIndexEnv(const std::string& env_path) :
        path_(env_path) {
        std::filesystem::create_directories(path_);

        int rc = mdbx_env_create(&env_);
        if(rc != MDBX_SUCCESS) {
            throw_mdbx("Failed to create shared MDBX env", rc);
        }

        rc = mdbx_env_set_geometry(env_,
                                   -1,
                                   1ULL << settings::VECTOR_MAP_SIZE_BITS,
                                   1ULL << settings::VECTOR_MAP_SIZE_MAX_BITS,
                                   1ULL << settings::VECTOR_MAP_SIZE_BITS,
                                   -1,
                                   -1);
        if(rc != MDBX_SUCCESS) {
            mdbx_env_close(env_);
            env_ = nullptr;
            throw_mdbx("Failed to set shared MDBX geometry", rc);
        }

        rc = mdbx_env_set_maxdbs(env_, settings::SHARED_INDEX_MAX_DBS);
        if(rc != MDBX_SUCCESS) {
            mdbx_env_close(env_);
            env_ = nullptr;
            throw_mdbx("Failed to set shared MDBX maxdbs", rc);
        }
        if(settings::SHARED_INDEX_NAMED_DBI_COUNT
           >= settings::SHARED_INDEX_MAX_DBS - settings::SHARED_INDEX_MAX_DBS_WARNING_MARGIN) {
            LOG_WARN("Shared MDBX DBI headroom is low: "
                     << settings::SHARED_INDEX_NAMED_DBI_COUNT << " of "
                     << settings::SHARED_INDEX_MAX_DBS << " configured slots are reserved");
        }

        rc = mdbx_env_open(env_,
                           path_.c_str(),
                           // No MDBX_MAPASYNC here: the shared env is the durable source of truth
                           // for vectors, metadata, filters, sparse docs, and op_log.
                           //
                           // Sticky-thread mode kept on purpose. MDBX_NOSTICKYTHREADS was tried
                           // and rolled back: it disables TLS reader-slot caching, so every
                           // reader txn_begin re-acquires a slot under the env's reader-table
                           // lock. The hot search path runs k get_meta reads per request and
                           // that collapsed concurrency=16 QPS by ~71% in VectorDBBench
                           // Performance768D1M. Write txns already commit on the originating
                           // thread (MDBX requires this regardless of the flag), so we lose
                           // nothing by staying sticky. See docs/mdbx_shared_env_acid_revamp.md
                           // "Durability Flags".
                           MDBX_WRITEMAP | MDBX_NORDAHEAD,
                           0664);
        if(rc != MDBX_SUCCESS) {
            mdbx_env_close(env_);
            env_ = nullptr;
            throw_mdbx("Failed to open shared MDBX env", rc);
        }
    }

    SharedIndexEnv(const SharedIndexEnv&) = delete;
    SharedIndexEnv& operator=(const SharedIndexEnv&) = delete;

    ~SharedIndexEnv() {
        if(env_) {
            mdbx_env_close(env_);
        }
    }

    MDBX_env* get() const { return env_; }
    const std::string& path() const { return path_; }

    static void write_layout_version(MDBX_env* env, uint32_t version) {
        MDBX_txn* txn = nullptr;
        int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
        if(rc != MDBX_SUCCESS) {
            throw_mdbx("Failed to begin layout metadata transaction", rc);
        }

        MDBX_dbi dbi = 0;
        rc = mdbx_dbi_open(txn, LAYOUT_META_DBI, MDBX_CREATE, &dbi);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            throw_mdbx("Failed to open layout_meta DBI", rc);
        }

        MDBX_val key{const_cast<char*>(LAYOUT_VERSION_KEY),
                     std::char_traits<char>::length(LAYOUT_VERSION_KEY)};
        MDBX_val data{&version, sizeof(version)};
        rc = mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            throw_mdbx("Failed to write layout version", rc);
        }

        rc = mdbx_txn_commit(txn);
        if(rc != MDBX_SUCCESS) {
            throw_mdbx("Failed to commit layout metadata transaction", rc);
        }
    }

    /**
     * Writes a defragmented copy of `env`'s data file to `dest_path` (a single
     * file, regardless of the source's subdir mode), omitting free/garbage
     * pages via MDBX_CP_COMPACT. The source env is only read, never modified, so
     * this is safe to run on a live env; the caller should hold the index
     * operation lock so the internal read snapshot does not pin pages against a
     * concurrent writer for longer than necessary.
     *
     * The caller is responsible for atomically renaming `dest_path` over the
     * live `mdbx.dat` and reopening the env. Returns the MDBX result code
     * (MDBX_SUCCESS on success); on failure `dest_path` may be partially written
     * and should be removed by the caller, and the source is untouched.
     */
    static int copy_compact(MDBX_env* env, const std::string& dest_path) {
        return mdbx_env_copy(
                env,
                dest_path.c_str(),
                static_cast<MDBX_copy_flags_t>(MDBX_CP_COMPACT | MDBX_CP_FORCE_DYNAMIC_SIZE));
    }

    static uint32_t read_layout_version(MDBX_env* env) {
        MDBX_txn* txn = nullptr;
        int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn);
        if(rc != MDBX_SUCCESS) {
            throw_mdbx("Failed to begin layout metadata read transaction", rc);
        }

        MDBX_dbi dbi = 0;
        rc = mdbx_dbi_open(txn, LAYOUT_META_DBI, MDBX_DB_ACCEDE, &dbi);
        if(rc == MDBX_NOTFOUND) {
            mdbx_txn_abort(txn);
            return settings::LEGACY_INDEX_LAYOUT_VERSION;
        }
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            throw_mdbx("Failed to open layout_meta DBI", rc);
        }

        MDBX_val key{const_cast<char*>(LAYOUT_VERSION_KEY),
                     std::char_traits<char>::length(LAYOUT_VERSION_KEY)};
        MDBX_val data{};
        rc = mdbx_get(txn, dbi, &key, &data);
        if(rc == MDBX_NOTFOUND || data.iov_len != sizeof(uint32_t)) {
            mdbx_txn_abort(txn);
            return settings::LEGACY_INDEX_LAYOUT_VERSION;
        }
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            throw_mdbx("Failed to read layout version", rc);
        }

        uint32_t version = settings::LEGACY_INDEX_LAYOUT_VERSION;
        std::memcpy(&version, data.iov_base, sizeof(version));
        mdbx_txn_abort(txn);
        return version;
    }
};

}  // namespace storage
}  // namespace ndd
