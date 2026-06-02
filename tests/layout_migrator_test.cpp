#include <gtest/gtest.h>

#include <cstdint>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "mdbx/mdbx.h"
#include "json/nlohmann_json.hpp"
#include "storage/index_meta.hpp"
#include "storage/shared_mdbx.hpp"
#include "storage/wal.hpp"
#include "tools/index_layout_migrator_v0_to_v2.hpp"

namespace fs = std::filesystem;

namespace {

struct EnvDeleter {
    void operator()(MDBX_env* env) const {
        if(env) {
            mdbx_env_close(env);
        }
    }
};

using EnvPtr = std::unique_ptr<MDBX_env, EnvDeleter>;

EnvPtr open_env(const fs::path& path, bool no_subdir = false) {
    if(no_subdir) {
        fs::create_directories(path.parent_path());
    } else {
        fs::create_directories(path);
    }

    MDBX_env* raw_env = nullptr;
    int rc = mdbx_env_create(&raw_env);
    EXPECT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);
    EnvPtr env(raw_env);

    rc = mdbx_env_set_maxdbs(env.get(), settings::SHARED_INDEX_MAX_DBS);
    EXPECT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);

    rc = mdbx_env_set_geometry(env.get(),
                               -1,
                               1ULL << settings::VECTOR_MAP_SIZE_BITS,
                               1ULL << settings::VECTOR_MAP_SIZE_MAX_BITS,
                               1ULL << settings::VECTOR_MAP_SIZE_BITS,
                               -1,
                               -1);
    EXPECT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);

    // Matches production env-open flags. No MDBX_NOSTICKYTHREADS: see
    // docs/mdbx_shared_env_acid_revamp.md "Durability Flags".
    MDBX_env_flags_t flags = static_cast<MDBX_env_flags_t>(
            MDBX_WRITEMAP | MDBX_NORDAHEAD);
    if(no_subdir) {
        flags = static_cast<MDBX_env_flags_t>(flags | MDBX_NOSUBDIR);
    }

    rc = mdbx_env_open(env.get(), path.string().c_str(), flags, 0664);
    EXPECT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);
    return env;
}

void put_row(MDBX_env* env,
             const char* dbi_name,
             MDBX_db_flags_t flags,
             const MDBX_val& key,
             const MDBX_val& value) {
    MDBX_txn* txn = nullptr;
    int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
    ASSERT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);

    MDBX_dbi dbi = 0;
    rc = mdbx_dbi_open(txn, dbi_name, static_cast<MDBX_db_flags_t>(flags | MDBX_CREATE), &dbi);
    ASSERT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);

    MDBX_val mutable_key = key;
    MDBX_val mutable_value = value;
    rc = mdbx_put(txn, dbi, &mutable_key, &mutable_value, MDBX_UPSERT);
    ASSERT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);

    rc = mdbx_txn_commit(txn);
    ASSERT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);
}

std::vector<uint8_t> get_row(MDBX_env* env,
                             const char* dbi_name,
                             MDBX_db_flags_t flags,
                             const MDBX_val& key) {
    MDBX_txn* txn = nullptr;
    int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn);
    EXPECT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);

    MDBX_dbi dbi = 0;
    rc = mdbx_dbi_open(txn, dbi_name, flags, &dbi);
    EXPECT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);

    MDBX_val mutable_key = key;
    MDBX_val value{};
    rc = mdbx_get(txn, dbi, &mutable_key, &value);
    EXPECT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);

    std::vector<uint8_t> bytes(static_cast<uint8_t*>(value.iov_base),
                               static_cast<uint8_t*>(value.iov_base) + value.iov_len);
    mdbx_txn_abort(txn);
    return bytes;
}

fs::path unique_temp_dir(const std::string& name) {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path()
                 / (name + "_" + std::to_string(ticks));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

struct TempDir {
    explicit TempDir(const std::string& name) :
        path(unique_temp_dir(name)) {}

    ~TempDir() { fs::remove_all(path); }

    fs::path path;
};

}  // namespace

TEST(IndexLayoutVersionTest, MissingMetadataVersionIsLegacy) {
    nlohmann::json j = {{"name", "idx"},
                        {"dimension", 8},
                        {"sparse_model", "None"},
                        {"space_type_str", "cosine"},
                        {"quant_level", 1},
                        {"checksum", -1},
                        {"total_elements", 0},
                        {"M", 16},
                        {"ef_con", 128},
                        {"created_at", static_cast<time_t>(0)}};

    IndexMetadata metadata = IndexMetadata::from_json(j);
    EXPECT_EQ(metadata.layout_version, settings::LEGACY_INDEX_LAYOUT_VERSION);
}

TEST(SharedIndexEnvTest, PersistsLayoutVersion) {
    TempDir root("shared_layout_env");

    {
        ndd::storage::SharedIndexEnv env(root.path.string());
        ndd::storage::SharedIndexEnv::write_layout_version(
                env.get(), settings::INDEX_LAYOUT_VERSION);
        EXPECT_EQ(ndd::storage::SharedIndexEnv::read_layout_version(env.get()),
                  settings::INDEX_LAYOUT_VERSION);
    }

    ndd::storage::SharedIndexEnv reopened(root.path.string());
    EXPECT_EQ(ndd::storage::SharedIndexEnv::read_layout_version(reopened.get()),
              settings::INDEX_LAYOUT_VERSION);
}

TEST(IndexLayoutMigratorTest, MigratesLegacySplitDbsIntoSharedNamedDbs) {
    TempDir root("legacy_layout_migration");

    fs::path backup_dir = root.path / "legacy";
    fs::path target_dir = root.path / "restored";
    fs::create_directories(backup_dir / "vectors");
    {
        std::ofstream hnsw(backup_dir / "vectors" / (settings::DEFAULT_SUBINDEX + ".idx"),
                           std::ios::binary);
        hnsw << "fake-hnsw";
    }

    ndd::idInt id = 1;
    std::vector<uint8_t> vector_payload = {1, 2, 3, 4};
    std::vector<uint8_t> meta_payload = {9, 8, 7};
    std::string external_id = "doc-1";

    auto vector_env = open_env(backup_dir / "vectors");
    MDBX_val id_key{&id, sizeof(id)};
    MDBX_val vector_value{vector_payload.data(), vector_payload.size()};
    put_row(vector_env.get(),
            settings::DEFAULT_SUBINDEX.c_str(),
            MDBX_INTEGERKEY,
            id_key,
            vector_value);
    vector_env.reset();

    auto meta_env = open_env(backup_dir / "meta", true);
    MDBX_val meta_value{meta_payload.data(), meta_payload.size()};
    put_row(meta_env.get(), nullptr, MDBX_INTEGERKEY, id_key, meta_value);
    meta_env.reset();

    auto id_env = open_env(backup_dir / "ids");
    MDBX_val external_key{external_id.data(), external_id.size()};
    MDBX_val mapped_id{&id, sizeof(id)};
    put_row(id_env.get(), nullptr, MDBX_DB_DEFAULTS, external_key, mapped_id);
    id_env.reset();

    ndd::tools::IndexLayoutMigratorV0toV2::migrateBackupV0toV2(backup_dir, target_dir);

    ndd::storage::SharedIndexEnv target_env((target_dir / "vectors").string());
    EXPECT_EQ(ndd::storage::SharedIndexEnv::read_layout_version(target_env.get()),
              settings::INDEX_LAYOUT_VERSION);

    EXPECT_EQ(get_row(target_env.get(),
                      settings::DEFAULT_SUBINDEX.c_str(),
                      MDBX_INTEGERKEY,
                      id_key),
              vector_payload);
    EXPECT_EQ(get_row(target_env.get(), "vector_meta", MDBX_INTEGERKEY, id_key), meta_payload);

    std::vector<uint8_t> mapped_bytes =
            get_row(target_env.get(), "id_map", MDBX_DB_DEFAULTS, external_key);
    ASSERT_EQ(mapped_bytes.size(), sizeof(ndd::idInt));
    ndd::idInt mapped = 0;
    std::memcpy(&mapped, mapped_bytes.data(), sizeof(mapped));
    EXPECT_EQ(mapped, id);

    MDBX_txn* op_txn = nullptr;
    ASSERT_EQ(mdbx_txn_begin(target_env.get(), nullptr, MDBX_TXN_RDONLY, &op_txn),
              MDBX_SUCCESS);
    MDBX_dbi op_dbi = 0;
    int op_open_rc = mdbx_dbi_open(op_txn, "op_log", MDBX_INTEGERKEY, &op_dbi);
    if(op_open_rc == MDBX_SUCCESS) {
        MDBX_stat op_stat{};
        ASSERT_EQ(mdbx_dbi_stat(op_txn, op_dbi, &op_stat, sizeof(op_stat)), MDBX_SUCCESS);
        EXPECT_EQ(op_stat.ms_entries, 0u)
                << "op_log must be empty when the legacy backup has no pending wal.bin";
    } else {
        EXPECT_EQ(op_open_rc, MDBX_NOTFOUND)
                << "expected op_log DBI to be absent or empty, got " << mdbx_strerror(op_open_rc);
    }
    mdbx_txn_abort(op_txn);
}

TEST(IndexLayoutMigratorTest, TranslatesLegacyWalIntoOpLog) {
    TempDir root("legacy_wal_translation");

    fs::path backup_dir = root.path / "legacy";
    fs::path target_dir = root.path / "restored";
    fs::create_directories(backup_dir / "vectors");
    {
        std::ofstream hnsw(backup_dir / "vectors" / (settings::DEFAULT_SUBINDEX + ".idx"),
                           std::ios::binary);
        hnsw << "fake-hnsw";
    }

    ndd::idInt id = 42;
    std::vector<uint8_t> vector_payload = {4, 2};
    auto vector_env = open_env(backup_dir / "vectors");
    MDBX_val id_key{&id, sizeof(id)};
    MDBX_val vector_value{vector_payload.data(), vector_payload.size()};
    put_row(vector_env.get(),
            settings::DEFAULT_SUBINDEX.c_str(),
            MDBX_INTEGERKEY,
            id_key,
            vector_value);
    vector_env.reset();

    auto meta_env = open_env(backup_dir / "meta", true);
    std::vector<uint8_t> meta_payload = {0};
    MDBX_val meta_value{meta_payload.data(), meta_payload.size()};
    put_row(meta_env.get(), nullptr, MDBX_INTEGERKEY, id_key, meta_value);
    meta_env.reset();

    auto id_env = open_env(backup_dir / "ids");
    std::string external_id = "doc-42";
    MDBX_val external_key{external_id.data(), external_id.size()};
    MDBX_val mapped_id{&id, sizeof(id)};
    put_row(id_env.get(), nullptr, MDBX_DB_DEFAULTS, external_key, mapped_id);
    id_env.reset();

    /**
     * Two pending ops in the legacy wal.bin: one VECTOR_UPDATE and one
     * VECTOR_DELETE for the same id. The matching default.idx is intentionally
     * a stub, mirroring the master on-disk format where these ops had not yet
     * been checkpointed at backup time.
     */
    {
        std::ofstream wal(backup_dir / "wal.bin", std::ios::binary);
        const uint8_t update_op = static_cast<uint8_t>(WALOperationType::VECTOR_UPDATE);
        wal.write(reinterpret_cast<const char*>(&update_op), sizeof(update_op));
        wal.write(reinterpret_cast<const char*>(&id), sizeof(id));
        const uint8_t delete_op = static_cast<uint8_t>(WALOperationType::VECTOR_DELETE);
        wal.write(reinterpret_cast<const char*>(&delete_op), sizeof(delete_op));
        wal.write(reinterpret_cast<const char*>(&id), sizeof(id));
    }

    ndd::tools::IndexLayoutMigratorV0toV2::migrateBackupV0toV2(backup_dir, target_dir);

    ndd::storage::SharedIndexEnv target_env((target_dir / "vectors").string());

    uint64_t seq0 = 0;
    MDBX_val key0{&seq0, sizeof(seq0)};
    std::vector<uint8_t> first =
            get_row(target_env.get(), "op_log", MDBX_INTEGERKEY, key0);
    ASSERT_EQ(first.size(), WriteAheadLog::PACKED_ENTRY_SIZE);
    EXPECT_EQ(first[0], static_cast<uint8_t>(WALOperationType::VECTOR_UPDATE));
    ndd::idInt first_id = 0;
    std::memcpy(&first_id, first.data() + 1, sizeof(first_id));
    EXPECT_EQ(first_id, id);

    uint64_t seq1 = 1;
    MDBX_val key1{&seq1, sizeof(seq1)};
    std::vector<uint8_t> second =
            get_row(target_env.get(), "op_log", MDBX_INTEGERKEY, key1);
    ASSERT_EQ(second.size(), WriteAheadLog::PACKED_ENTRY_SIZE);
    EXPECT_EQ(second[0], static_cast<uint8_t>(WALOperationType::VECTOR_DELETE));
    ndd::idInt second_id = 0;
    std::memcpy(&second_id, second.data() + 1, sizeof(second_id));
    EXPECT_EQ(second_id, id);
}

TEST(IndexLayoutMigratorTest, RejectsTruncatedLegacyWal) {
    TempDir root("legacy_wal_truncated");

    fs::path backup_dir = root.path / "legacy";
    fs::path target_dir = root.path / "restored";
    fs::create_directories(backup_dir / "vectors");
    {
        std::ofstream hnsw(backup_dir / "vectors" / (settings::DEFAULT_SUBINDEX + ".idx"),
                           std::ios::binary);
        hnsw << "fake-hnsw";
    }

    ndd::idInt id = 1;
    std::vector<uint8_t> vector_payload = {1};
    auto vector_env = open_env(backup_dir / "vectors");
    MDBX_val id_key{&id, sizeof(id)};
    MDBX_val vector_value{vector_payload.data(), vector_payload.size()};
    put_row(vector_env.get(),
            settings::DEFAULT_SUBINDEX.c_str(),
            MDBX_INTEGERKEY,
            id_key,
            vector_value);
    vector_env.reset();

    auto meta_env = open_env(backup_dir / "meta", true);
    std::vector<uint8_t> meta_payload = {0};
    MDBX_val meta_value{meta_payload.data(), meta_payload.size()};
    put_row(meta_env.get(), nullptr, MDBX_INTEGERKEY, id_key, meta_value);
    meta_env.reset();

    auto id_env = open_env(backup_dir / "ids");
    std::string external_id = "doc-1";
    MDBX_val external_key{external_id.data(), external_id.size()};
    MDBX_val mapped_id{&id, sizeof(id)};
    put_row(id_env.get(), nullptr, MDBX_DB_DEFAULTS, external_key, mapped_id);
    id_env.reset();

    {
        std::ofstream wal(backup_dir / "wal.bin", std::ios::binary);
        const uint8_t op = static_cast<uint8_t>(WALOperationType::VECTOR_ADD);
        wal.write(reinterpret_cast<const char*>(&op), sizeof(op));
    }

    EXPECT_THROW(ndd::tools::IndexLayoutMigratorV0toV2::migrateBackupV0toV2(backup_dir, target_dir),
                 std::runtime_error);
}

TEST(IndexLayoutMigratorTest, RetriesInterruptedLegacyMigration) {
    TempDir root("legacy_layout_migration_retry");

    fs::path backup_dir = root.path / "legacy";
    fs::path target_dir = root.path / "restored";
    fs::create_directories(backup_dir / "vectors");
    {
        std::ofstream hnsw(backup_dir / "vectors" / (settings::DEFAULT_SUBINDEX + ".idx"),
                           std::ios::binary);
        hnsw << "fake-hnsw";
    }

    ndd::idInt id = 7;
    std::vector<uint8_t> vector_payload = {7, 6, 5};
    auto vector_env = open_env(backup_dir / "vectors");
    MDBX_val id_key{&id, sizeof(id)};
    MDBX_val vector_value{vector_payload.data(), vector_payload.size()};
    put_row(vector_env.get(),
            settings::DEFAULT_SUBINDEX.c_str(),
            MDBX_INTEGERKEY,
            id_key,
            vector_value);
    vector_env.reset();

    auto meta_env = open_env(backup_dir / "meta", true);
    std::vector<uint8_t> meta_payload = {1, 2};
    MDBX_val meta_value{meta_payload.data(), meta_payload.size()};
    put_row(meta_env.get(), nullptr, MDBX_INTEGERKEY, id_key, meta_value);
    meta_env.reset();

    auto id_env = open_env(backup_dir / "ids");
    std::string external_id = "doc-7";
    MDBX_val external_key{external_id.data(), external_id.size()};
    MDBX_val mapped_id{&id, sizeof(id)};
    put_row(id_env.get(), nullptr, MDBX_DB_DEFAULTS, external_key, mapped_id);
    id_env.reset();

    fs::create_directories(target_dir);
    {
        std::ofstream marker(target_dir / settings::INDEX_MIGRATION_MARKER);
        marker << "previous attempt";
    }
    {
        std::ofstream stale(target_dir / "stale");
        stale << "stale";
    }

    ndd::tools::IndexLayoutMigratorV0toV2::migrateBackupV0toV2(backup_dir, target_dir);

    EXPECT_FALSE(fs::exists(target_dir / settings::INDEX_MIGRATION_MARKER));
    ndd::storage::SharedIndexEnv target_env((target_dir / "vectors").string());
    EXPECT_EQ(get_row(target_env.get(),
                      settings::DEFAULT_SUBINDEX.c_str(),
                      MDBX_INTEGERKEY,
                      id_key),
              vector_payload);
}

TEST(IndexLayoutMigratorTest, LeavesMarkerWhenLegacyMigrationFails) {
    TempDir root("legacy_layout_migration_marker");
    fs::path backup_dir = root.path / "broken_legacy";
    fs::path target_dir = root.path / "restored";
    fs::create_directories(backup_dir);

    EXPECT_THROW(ndd::tools::IndexLayoutMigratorV0toV2::migrateBackupV0toV2(backup_dir, target_dir),
                 std::runtime_error);
    EXPECT_TRUE(fs::exists(target_dir / settings::INDEX_MIGRATION_MARKER));
}

TEST(IndexLayoutMigratorTest, MigratesInPlaceWithoutDoubleStorage) {
    TempDir root("in_place_migration");
    fs::path index_dir = root.path / "live_index";
    fs::create_directories(index_dir / "vectors");
    {
        std::ofstream hnsw(index_dir / "vectors" / (settings::DEFAULT_SUBINDEX + ".idx"),
                           std::ios::binary);
        hnsw << "fake-hnsw";
    }

    ndd::idInt id = 11;
    std::vector<uint8_t> vector_payload = {1, 1};
    auto vector_env = open_env(index_dir / "vectors");
    MDBX_val id_key{&id, sizeof(id)};
    MDBX_val vector_value{vector_payload.data(), vector_payload.size()};
    put_row(vector_env.get(),
            settings::DEFAULT_SUBINDEX.c_str(),
            MDBX_INTEGERKEY,
            id_key,
            vector_value);
    vector_env.reset();

    auto meta_env = open_env(index_dir / "meta");
    std::vector<uint8_t> meta_payload = {9};
    MDBX_val meta_value{meta_payload.data(), meta_payload.size()};
    put_row(meta_env.get(), nullptr, MDBX_INTEGERKEY, id_key, meta_value);
    meta_env.reset();

    auto id_env = open_env(index_dir / "ids");
    std::string external_id = "doc-11";
    MDBX_val external_key{external_id.data(), external_id.size()};
    MDBX_val mapped_id{&id, sizeof(id)};
    put_row(id_env.get(), nullptr, MDBX_DB_DEFAULTS, external_key, mapped_id);
    id_env.reset();

    ndd::tools::IndexLayoutMigratorV0toV2::migrateInPlaceV0toV2(index_dir);

    EXPECT_FALSE(fs::exists(index_dir / settings::INDEX_MIGRATION_MARKER));
    EXPECT_FALSE(fs::exists(index_dir / "_legacy_v0"));
    EXPECT_FALSE(fs::exists(index_dir / "meta"));
    EXPECT_FALSE(fs::exists(index_dir / "ids"));
    EXPECT_TRUE(fs::exists(index_dir / "vectors" / (settings::DEFAULT_SUBINDEX + ".idx")));
    EXPECT_TRUE(fs::exists(index_dir / "vectors" / "mdbx.dat"));

    ndd::storage::SharedIndexEnv target_env((index_dir / "vectors").string());
    EXPECT_EQ(ndd::storage::SharedIndexEnv::read_layout_version(target_env.get()),
              settings::INDEX_LAYOUT_VERSION);
    EXPECT_EQ(get_row(target_env.get(),
                      settings::DEFAULT_SUBINDEX.c_str(),
                      MDBX_INTEGERKEY,
                      id_key),
              vector_payload);
    EXPECT_EQ(get_row(target_env.get(), "vector_meta", MDBX_INTEGERKEY, id_key), meta_payload);

    std::vector<uint8_t> mapped_bytes =
            get_row(target_env.get(), "id_map", MDBX_DB_DEFAULTS, external_key);
    ASSERT_EQ(mapped_bytes.size(), sizeof(ndd::idInt));
    ndd::idInt mapped = 0;
    std::memcpy(&mapped, mapped_bytes.data(), sizeof(mapped));
    EXPECT_EQ(mapped, id);
}

TEST(IndexLayoutMigratorTest, InPlaceRefusesWhenMarkerPresent) {
    TempDir root("in_place_marker_refusal");
    fs::path index_dir = root.path / "live_index";
    fs::create_directories(index_dir / "vectors");
    {
        std::ofstream hnsw(index_dir / "vectors" / (settings::DEFAULT_SUBINDEX + ".idx"),
                           std::ios::binary);
        hnsw << "fake-hnsw";
    }
    {
        std::ofstream marker(index_dir / settings::INDEX_MIGRATION_MARKER);
        marker << "stale";
    }

    EXPECT_THROW(ndd::tools::IndexLayoutMigratorV0toV2::migrateInPlaceV0toV2(index_dir),
                 std::runtime_error);
}
