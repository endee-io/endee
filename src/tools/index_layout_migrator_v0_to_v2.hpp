#pragma once

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "mdbx/mdbx.h"
#include "core/types.hpp"
#include "json/nlohmann_json.hpp"
#include "storage/shared_mdbx.hpp"
#include "storage/wal.hpp"
#include "utils/settings.hpp"

namespace ndd::tools {

/**
 * Offline migrator that rewrites a layout-v0 (legacy) index into the layout-v2
 * shared-MDBX format. Lives in the standalone ndd-migrate-v0-to-v2 binary so
 * the server never carries the legacy-format code.
 *
 * Two entry points:
 *   - migrateBackupV0toV2: source is an already-extracted backup directory
 *     (with metadata.json at the top), target is a fresh sibling directory.
 *   - migrateInPlaceV0toV2: source and target are the SAME live index folder.
 *     Legacy per-concern envs are renamed under the index dir, copied into the
 *     new shared env, and then deleted - peak extra storage is the size of
 *     the largest single legacy DBI file rather than a full extra copy.
 */
class IndexLayoutMigratorV0toV2 {
public:
    static void migrateBackupV0toV2(const std::filesystem::path& backup_dir,
                                    const std::filesystem::path& target_dir) {
        /**
         * Guard against re-running the migrator on an already-current backup.
         * The v0->v2 migrator assumes the legacy per-concern layout (separate
         * meta/, ids/, filters/, sparse/ MDBX envs); a layout-v2 backup has
         * only the single shared vectors/ env, so migration would copy a DBI or
         * two and then die on the missing meta env with a cryptic "Backup MDBX
         * environment missing" error. Read the carried-forward layout_version
         * up front and refuse cleanly. The parsed metadata is reused below to
         * stamp the migrated target, so metadata.json is read only once.
         */
        const std::filesystem::path source_metadata = backup_dir / "metadata.json";
        const bool have_metadata = std::filesystem::exists(source_metadata);
        nlohmann::json meta_json;
        if(have_metadata) {
            std::ifstream meta_in(source_metadata);
            meta_json = nlohmann::json::parse(meta_in);
            uint32_t source_layout = settings::LEGACY_INDEX_LAYOUT_VERSION;
            if(meta_json.contains("params")) {
                source_layout = meta_json["params"].value(
                        "layout_version", settings::LEGACY_INDEX_LAYOUT_VERSION);
            }
            if(source_layout >= settings::INDEX_LAYOUT_VERSION) {
                throw std::runtime_error(
                        "Backup is already layout_version="
                        + std::to_string(source_layout)
                        + "; this tool only upgrades legacy layout_version="
                        + std::to_string(settings::LEGACY_INDEX_LAYOUT_VERSION)
                        + " backups to layout_version="
                        + std::to_string(settings::INDEX_LAYOUT_VERSION)
                        + ". Nothing to migrate - restore this backup as-is.");
            }
        }

        prepareMigrationTarget(target_dir);
        std::filesystem::create_directories(target_dir / "vectors");

        const std::filesystem::path source_hnsw =
                backup_dir / "vectors" / (settings::DEFAULT_SUBINDEX + ".idx");
        const std::filesystem::path target_hnsw =
                target_dir / "vectors" / (settings::DEFAULT_SUBINDEX + ".idx");
        if(!std::filesystem::exists(source_hnsw)) {
            throw std::runtime_error("Backup is missing HNSW index file");
        }
        std::filesystem::copy_file(source_hnsw,
                                   target_hnsw,
                                   std::filesystem::copy_options::overwrite_existing);

        const std::filesystem::path recover_file = backup_dir / "recover.txt";
        if(std::filesystem::exists(recover_file)) {
            std::filesystem::copy_file(recover_file,
                                       target_dir / "recover.txt",
                                       std::filesystem::copy_options::overwrite_existing);
        }

        /**
         * The server's restoreBackup path reads metadata.json from the inner
         * folder to recover index params (dim, M, ef_con, ...) and gates on
         * params.layout_version. Carry the legacy metadata.json forward (parsed
         * above) and stamp layout_version = current so the restored tar is
         * accepted.
         */
        if(have_metadata) {
            meta_json["params"]["layout_version"] = settings::INDEX_LAYOUT_VERSION;
            std::ofstream meta_out(target_dir / "metadata.json", std::ios::binary);
            meta_out << meta_json.dump(4);
        }

        ndd::storage::SharedIndexEnv target_env((target_dir / "vectors").string());
        ndd::storage::SharedIndexEnv::write_layout_version(
                target_env.get(), settings::INDEX_LAYOUT_VERSION);
        copyAllLegacyDbis(backup_dir, target_env.get(), /*meta_try_no_subdir_first=*/true);
        translateLegacyWal(backup_dir / "wal.bin", target_env.get());
        clearMigrationMarker(target_dir);
    }

    static void migrateInPlaceV0toV2(const std::filesystem::path& index_dir) {
        const std::filesystem::path hnsw_file =
                index_dir / "vectors" / (settings::DEFAULT_SUBINDEX + ".idx");
        if(!std::filesystem::exists(hnsw_file)) {
            throw std::runtime_error("Index folder is missing HNSW index file: "
                                     + hnsw_file.string());
        }
        if(std::filesystem::exists(migrationMarkerPath(index_dir))) {
            throw std::runtime_error(
                    "Index folder has an in-progress migration marker. Restore from a "
                    "backup tar before retrying - in-place migration cannot resume "
                    "automatically.");
        }
        acquireMigrationMarker(index_dir);

        /**
         * Stage every legacy env out of the way so the new shared env can be
         * created at <index>/vectors/. We move the per-concern directories
         * (meta/, ids/, filters/, sparse/) verbatim, and for vectors/ we move
         * only mdbx.dat / mdbx.lck into a sibling _legacy_vectors/ directory so
         * default.idx can stay where it is.
         */
        const std::filesystem::path legacy_root = index_dir / "_legacy_v0";
        std::filesystem::create_directories(legacy_root);

        const auto move_dir_if_exists = [&](const char* name) {
            std::filesystem::path src = index_dir / name;
            if(std::filesystem::exists(src)) {
                std::filesystem::rename(src, legacy_root / name);
            }
        };
        move_dir_if_exists("meta");
        move_dir_if_exists("ids");
        move_dir_if_exists("filters");
        move_dir_if_exists("sparse");

        std::filesystem::create_directories(legacy_root / "vectors");
        const std::filesystem::path legacy_vectors_dat = legacy_root / "vectors" / "mdbx.dat";
        const std::filesystem::path legacy_vectors_lck = legacy_root / "vectors" / "mdbx.lck";
        if(std::filesystem::exists(index_dir / "vectors" / "mdbx.dat")) {
            std::filesystem::rename(index_dir / "vectors" / "mdbx.dat", legacy_vectors_dat);
        }
        if(std::filesystem::exists(index_dir / "vectors" / "mdbx.lck")) {
            std::filesystem::rename(index_dir / "vectors" / "mdbx.lck", legacy_vectors_lck);
        }

        if(std::filesystem::exists(index_dir / "wal.bin")) {
            std::filesystem::rename(index_dir / "wal.bin", legacy_root / "wal.bin");
        }

        ndd::storage::SharedIndexEnv target_env((index_dir / "vectors").string());
        ndd::storage::SharedIndexEnv::write_layout_version(
                target_env.get(), settings::INDEX_LAYOUT_VERSION);
        copyAllLegacyDbis(legacy_root, target_env.get(), /*meta_try_no_subdir_first=*/false);
        translateLegacyWal(legacy_root / "wal.bin", target_env.get());

        std::error_code ec;
        std::filesystem::remove_all(legacy_root, ec);

        clearMigrationMarker(index_dir);
    }

private:
    struct EnvDeleter {
        void operator()(MDBX_env* env) const {
            if(env) {
                mdbx_env_close(env);
            }
        }
    };

    using EnvPtr = std::unique_ptr<MDBX_env, EnvDeleter>;

    static std::filesystem::path migrationMarkerPath(const std::filesystem::path& target_dir) {
        return target_dir / settings::INDEX_MIGRATION_MARKER;
    }

    static void prepareMigrationTarget(const std::filesystem::path& target_dir) {
        const auto marker = migrationMarkerPath(target_dir);
        if(std::filesystem::exists(target_dir)) {
            if(!std::filesystem::exists(marker) && !std::filesystem::is_empty(target_dir)) {
                throw std::runtime_error(
                        "Target migration directory already exists without an in-progress marker");
            }
            std::filesystem::remove_all(target_dir);
        }
        std::filesystem::create_directories(target_dir);
        acquireMigrationMarker(target_dir);
    }

    static void acquireMigrationMarker(const std::filesystem::path& target_dir) {
        std::ofstream marker_file(migrationMarkerPath(target_dir), std::ios::binary);
        if(!marker_file) {
            throw std::runtime_error("Failed to create migration marker");
        }
        marker_file << "migration in progress\n";
    }

    static void clearMigrationMarker(const std::filesystem::path& target_dir) {
        std::error_code ec;
        std::filesystem::remove(migrationMarkerPath(target_dir), ec);
    }

    static void copyAllLegacyDbis(const std::filesystem::path& legacy_root,
                                  MDBX_env* target_env,
                                  bool meta_try_no_subdir_first) {
        copyDatabase(legacy_root / "vectors",
                     settings::DEFAULT_SUBINDEX.c_str(),
                     MDBX_INTEGERKEY,
                     target_env,
                     settings::DEFAULT_SUBINDEX.c_str(),
                     MDBX_INTEGERKEY,
                     false,
                     false);

        copyLegacyMeta(legacy_root, target_env, meta_try_no_subdir_first);

        copyDatabase(legacy_root / "ids",
                     nullptr,
                     MDBX_DB_DEFAULTS,
                     target_env,
                     "id_map",
                     MDBX_DB_DEFAULTS,
                     false,
                     false);

        copyDatabase(legacy_root / "filters",
                     nullptr,
                     MDBX_DB_DEFAULTS,
                     target_env,
                     "filter_schema",
                     MDBX_DB_DEFAULTS,
                     true,
                     false);
        copyDatabase(legacy_root / "filters",
                     "category_idx",
                     MDBX_DB_DEFAULTS,
                     target_env,
                     "category_idx",
                     MDBX_DB_DEFAULTS,
                     true,
                     false);
        copyDatabase(legacy_root / "filters",
                     "numeric_forward",
                     MDBX_DB_DEFAULTS,
                     target_env,
                     "numeric_forward",
                     MDBX_DB_DEFAULTS,
                     true,
                     false);
        copyDatabase(legacy_root / "filters",
                     "numeric_inverted",
                     MDBX_DB_DEFAULTS,
                     target_env,
                     "numeric_inverted",
                     MDBX_DB_DEFAULTS,
                     true,
                     false);

        copyDatabase(legacy_root / "sparse",
                     "sparse_docs",
                     MDBX_INTEGERKEY,
                     target_env,
                     "sparse_docs",
                     MDBX_INTEGERKEY,
                     true,
                     false);
        copyDatabase(legacy_root / "sparse",
                     "blocked_term_postings",
                     MDBX_INTEGERKEY,
                     target_env,
                     "blocked_term_postings",
                     MDBX_INTEGERKEY,
                     true,
                     false);
    }

    static EnvPtr openSourceEnv(const std::filesystem::path& path,
                                bool optional,
                                bool no_subdir) {
        if(!std::filesystem::exists(path)) {
            if(optional) {
                return nullptr;
            }
            throw std::runtime_error("Backup MDBX environment missing: " + path.string());
        }

        MDBX_env* raw_env = nullptr;
        int rc = mdbx_env_create(&raw_env);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error("Failed to create source MDBX env: "
                                     + std::string(mdbx_strerror(rc)));
        }
        EnvPtr env(raw_env);

        rc = mdbx_env_set_maxdbs(env.get(), settings::SHARED_INDEX_MAX_DBS);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error("Failed to configure source MDBX env: "
                                     + std::string(mdbx_strerror(rc)));
        }

        /**
         * Open read-write, not read-only: legacy backup tars are copied from
         * live MDBX_WRITEMAP+MDBX_MAPASYNC envs without an env_sync, so the
         * on-disk image often lands in MDBX_WANNA_RECOVERY. MDBX can recover
         * automatically, but only with write access. The path here is the
         * throwaway staging extraction, so mutating it is safe.
         */
        MDBX_env_flags_t flags = MDBX_NORDAHEAD;
        if(no_subdir) {
            flags = static_cast<MDBX_env_flags_t>(flags | MDBX_NOSUBDIR);
        }

        rc = mdbx_env_open(env.get(), path.string().c_str(), flags, 0664);
        if(rc != MDBX_SUCCESS) {
            if(optional) {
                return nullptr;
            }
            throw std::runtime_error("Failed to open source MDBX env " + path.string()
                                     + ": " + mdbx_strerror(rc));
        }
        return env;
    }

    static bool copyDatabase(const std::filesystem::path& source_env_path,
                             const char* source_dbi_name,
                             MDBX_db_flags_t source_flags,
                             MDBX_env* target_env,
                             const char* target_dbi_name,
                             MDBX_db_flags_t target_flags,
                             bool optional,
                             bool source_no_subdir) {
        EnvPtr source_env = openSourceEnv(source_env_path, optional, source_no_subdir);
        if(!source_env) {
            return false;
        }

        MDBX_txn* source_txn = nullptr;
        int rc = mdbx_txn_begin(source_env.get(), nullptr, MDBX_TXN_RDONLY, &source_txn);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error("Failed to begin source read transaction: "
                                     + std::string(mdbx_strerror(rc)));
        }

        MDBX_dbi source_dbi = 0;
        rc = mdbx_dbi_open(source_txn, source_dbi_name, source_flags, &source_dbi);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(source_txn);
            if(optional) {
                return false;
            }
            throw std::runtime_error("Failed to open source DBI: "
                                     + std::string(mdbx_strerror(rc)));
        }

        MDBX_txn* target_txn = nullptr;
        rc = mdbx_txn_begin(target_env, nullptr, MDBX_TXN_READWRITE, &target_txn);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(source_txn);
            throw std::runtime_error("Failed to begin target write transaction: "
                                     + std::string(mdbx_strerror(rc)));
        }

        MDBX_dbi target_dbi = 0;
        rc = mdbx_dbi_open(target_txn,
                           target_dbi_name,
                           static_cast<MDBX_db_flags_t>(target_flags | MDBX_CREATE),
                           &target_dbi);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(target_txn);
            mdbx_txn_abort(source_txn);
            throw std::runtime_error("Failed to open target DBI: "
                                     + std::string(mdbx_strerror(rc)));
        }

        MDBX_cursor* cursor = nullptr;
        rc = mdbx_cursor_open(source_txn, source_dbi, &cursor);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(target_txn);
            mdbx_txn_abort(source_txn);
            throw std::runtime_error("Failed to open source cursor: "
                                     + std::string(mdbx_strerror(rc)));
        }

        MDBX_val key{};
        MDBX_val data{};
        rc = mdbx_cursor_get(cursor, &key, &data, MDBX_FIRST);
        while(rc == MDBX_SUCCESS) {
            rc = mdbx_put(target_txn, target_dbi, &key, &data, MDBX_UPSERT);
            if(rc != MDBX_SUCCESS) {
                mdbx_cursor_close(cursor);
                mdbx_txn_abort(target_txn);
                mdbx_txn_abort(source_txn);
                throw std::runtime_error("Failed to copy MDBX row: "
                                         + std::string(mdbx_strerror(rc)));
            }
            rc = mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT);
        }

        if(rc != MDBX_NOTFOUND) {
            mdbx_cursor_close(cursor);
            mdbx_txn_abort(target_txn);
            mdbx_txn_abort(source_txn);
            throw std::runtime_error("Failed while scanning source DBI: "
                                     + std::string(mdbx_strerror(rc)));
        }

        mdbx_cursor_close(cursor);
        rc = mdbx_txn_commit(target_txn);
        mdbx_txn_abort(source_txn);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error("Failed to commit copied DBI: "
                                     + std::string(mdbx_strerror(rc)));
        }
        return true;
    }

    static void copyLegacyMeta(const std::filesystem::path& legacy_root,
                               MDBX_env* target_env,
                               bool try_no_subdir_first) {
        if(try_no_subdir_first
           && copyDatabase(legacy_root / "meta",
                           nullptr,
                           MDBX_INTEGERKEY,
                           target_env,
                           "vector_meta",
                           MDBX_INTEGERKEY,
                           true,
                           true)) {
            return;
        }

        copyDatabase(legacy_root / "meta",
                     nullptr,
                     MDBX_INTEGERKEY,
                     target_env,
                     "vector_meta",
                     MDBX_INTEGERKEY,
                     false,
                     false);
    }

    /**
     * Translate the legacy file-based wal.bin into the new MDBX-backed op_log
     * DBI. The legacy entry layout (1 byte op_type + sizeof(idInt) id bytes,
     * see WriteAheadLog::PACKED_ENTRY_SIZE) matches the new packed format, so
     * bytes are forwarded verbatim with monotonic sequence keys. The copied
     * default.idx already encodes every completed operation; only ops that
     * master had logged but not yet checkpointed belong in op_log.
     */
    static void translateLegacyWal(const std::filesystem::path& wal_path,
                                   MDBX_env* target_env) {
        std::error_code ec;
        if(!std::filesystem::exists(wal_path, ec)) {
            return;
        }
        const auto file_size = std::filesystem::file_size(wal_path, ec);
        if(ec || file_size == 0) {
            return;
        }
        if(file_size % WriteAheadLog::PACKED_ENTRY_SIZE != 0) {
            throw std::runtime_error("Legacy wal.bin has a truncated entry: size "
                                     + std::to_string(file_size));
        }

        std::ifstream wal_file(wal_path, std::ios::binary);
        if(!wal_file) {
            throw std::runtime_error("Failed to open legacy wal.bin: " + wal_path.string());
        }

        MDBX_txn* txn = nullptr;
        int rc = mdbx_txn_begin(target_env, nullptr, MDBX_TXN_READWRITE, &txn);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error("Failed to begin op_log translation transaction: "
                                     + std::string(mdbx_strerror(rc)));
        }

        MDBX_dbi op_log_dbi = 0;
        rc = mdbx_dbi_open(txn, "op_log", MDBX_CREATE | MDBX_INTEGERKEY, &op_log_dbi);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            throw std::runtime_error("Failed to open op_log DBI: "
                                     + std::string(mdbx_strerror(rc)));
        }

        uint8_t packed[WriteAheadLog::PACKED_ENTRY_SIZE];
        uint64_t seq = 0;
        while(wal_file.read(reinterpret_cast<char*>(packed), sizeof(packed))) {
            MDBX_val key{&seq, sizeof(seq)};
            MDBX_val data{packed, sizeof(packed)};
            rc = mdbx_put(txn, op_log_dbi, &key, &data, MDBX_APPEND);
            if(rc != MDBX_SUCCESS) {
                mdbx_txn_abort(txn);
                throw std::runtime_error("Failed to translate legacy wal.bin row: "
                                         + std::string(mdbx_strerror(rc)));
            }
            ++seq;
        }
        if(wal_file.bad()) {
            mdbx_txn_abort(txn);
            throw std::runtime_error("Read error while translating legacy wal.bin");
        }

        rc = mdbx_txn_commit(txn);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error("Failed to commit translated op_log: "
                                     + std::string(mdbx_strerror(rc)));
        }
    }
};

}  // namespace ndd::tools
