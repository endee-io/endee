#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <gtest/gtest.h>

#include "backup_store.hpp"
#include "ndd.hpp"
#include "utils/msgpack_ndd.hpp"
#include "server/auth.hpp"

namespace fs = std::filesystem;

// ============================================================
// Layer 1 — BackupStore state management (no IndexManager)
// ============================================================

class BackupStoreStateTest : public ::testing::Test {
protected:
    std::string dir_;
    std::unique_ptr<BackupStore> store_;

    void SetUp() override {
        dir_ = "./test_backup_state_" + std::to_string(rand());
        fs::create_directories(dir_);
        store_ = std::make_unique<BackupStore>(dir_);
    }

    void TearDown() override {
        store_.reset();
        if (fs::exists(dir_)) fs::remove_all(dir_);
    }
};

// --- validateBackupName ---

TEST_F(BackupStoreStateTest, ValidateName_AlphanumericUnderscore_Passes) {
    auto [ok, msg] = store_->validateBackupName("my_backup");
    EXPECT_TRUE(ok);
    EXPECT_TRUE(msg.empty());
}

TEST_F(BackupStoreStateTest, ValidateName_WithHyphen_Passes) {
    auto [ok, msg] = store_->validateBackupName("backup-2024");
    EXPECT_TRUE(ok);
}

TEST_F(BackupStoreStateTest, ValidateName_Empty_Fails) {
    auto [ok, msg] = store_->validateBackupName("");
    EXPECT_FALSE(ok);
    EXPECT_FALSE(msg.empty());
}

TEST_F(BackupStoreStateTest, ValidateName_TooLong_Fails) {
    auto [ok, msg] = store_->validateBackupName(std::string(201, 'a'));
    EXPECT_FALSE(ok);
    EXPECT_NE(msg.find("too long"), std::string::npos);
}

TEST_F(BackupStoreStateTest, ValidateName_Slash_Fails) {
    auto [ok, msg] = store_->validateBackupName("bad/name");
    EXPECT_FALSE(ok);
}

TEST_F(BackupStoreStateTest, ValidateName_Space_Fails) {
    auto [ok, msg] = store_->validateBackupName("bad name");
    EXPECT_FALSE(ok);
}

TEST_F(BackupStoreStateTest, ValidateName_Dot_Fails) {
    auto [ok, msg] = store_->validateBackupName("backup.tar");
    EXPECT_FALSE(ok);
}

// --- Active backup tracking ---

TEST_F(BackupStoreStateTest, NoActive_HasActiveIsFalse) {
    EXPECT_FALSE(store_->hasActiveBackup("alice"));
}

TEST_F(BackupStoreStateTest, SetActive_HasActiveIsTrue) {
    store_->setActiveBackup("alice", "bk1", BackupOperation::Creation);
    EXPECT_TRUE(store_->hasActiveBackup("alice"));
}

TEST_F(BackupStoreStateTest, SetActive_GetActiveReturnsNameAndOperation) {
    store_->setActiveBackup("alice", "bk1", BackupOperation::Creation);
    auto active = store_->getActiveBackup("alice");
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(active->first, "bk1");
    EXPECT_EQ(active->second, "creation");
}

TEST_F(BackupStoreStateTest, SetActive_Restoration_OperationString) {
    store_->setActiveBackup("alice", "bk1", BackupOperation::Restoration);
    auto active = store_->getActiveBackup("alice");
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(active->second, "restoration");
}

TEST_F(BackupStoreStateTest, ClearActive_HasActiveIsFalse) {
    store_->setActiveBackup("alice", "bk1", BackupOperation::Creation);
    store_->clearActiveBackup("alice");
    EXPECT_FALSE(store_->hasActiveBackup("alice"));
}

TEST_F(BackupStoreStateTest, ClearActive_GetActiveReturnsNullopt) {
    store_->setActiveBackup("alice", "bk1", BackupOperation::Creation);
    store_->clearActiveBackup("alice");
    EXPECT_FALSE(store_->getActiveBackup("alice").has_value());
}

TEST_F(BackupStoreStateTest, ClearNonExistent_NoOp) {
    EXPECT_NO_THROW(store_->clearActiveBackup("nobody"));
}

TEST_F(BackupStoreStateTest, TwoUsers_IndependentState) {
    store_->setActiveBackup("alice", "bk1", BackupOperation::Creation);
    EXPECT_TRUE(store_->hasActiveBackup("alice"));
    EXPECT_FALSE(store_->hasActiveBackup("bob"));

    store_->setActiveBackup("bob", "bk2", BackupOperation::Restoration);
    EXPECT_TRUE(store_->hasActiveBackup("bob"));

    store_->clearActiveBackup("alice");
    EXPECT_FALSE(store_->hasActiveBackup("alice"));
    EXPECT_TRUE(store_->hasActiveBackup("bob"));
}

// --- Backup JSON & listing ---

TEST_F(BackupStoreStateTest, ReadBackupJson_MissingFile_ReturnsEmptyObject) {
    auto json = store_->readBackupJson("alice");
    EXPECT_TRUE(json.empty());
}

TEST_F(BackupStoreStateTest, WriteAndReadBackupJson_RoundTrip) {
    nlohmann::json data;
    data["bk1"]["original_index"] = "my_idx";
    data["bk1"]["size_mb"] = 10;

    fs::create_directories(store_->getUserBackupDir("alice"));
    store_->writeBackupJson("alice", data);
    auto read = store_->readBackupJson("alice");

    EXPECT_TRUE(read.contains("bk1"));
    EXPECT_EQ(read["bk1"]["original_index"], "my_idx");
}

TEST_F(BackupStoreStateTest, ListBackups_EmptyWhenNoneExist) {
    EXPECT_TRUE(store_->listBackups("alice").empty());
}

TEST_F(BackupStoreStateTest, ListBackups_ReturnsAllWrittenEntries) {
    nlohmann::json data;
    data["bk1"] = {{"original_index", "idx1"}};
    data["bk2"] = {{"original_index", "idx2"}};
    fs::create_directories(store_->getUserBackupDir("alice"));
    store_->writeBackupJson("alice", data);

    auto list = store_->listBackups("alice");
    EXPECT_TRUE(list.contains("bk1"));
    EXPECT_TRUE(list.contains("bk2"));
}

TEST_F(BackupStoreStateTest, GetBackupInfo_ExistingEntry) {
    nlohmann::json data;
    data["bk1"] = {{"original_index", "idx1"}, {"size_mb", 5}};
    fs::create_directories(store_->getUserBackupDir("alice"));
    store_->writeBackupJson("alice", data);

    auto info = store_->getBackupInfo("bk1", "alice");
    EXPECT_FALSE(info.is_null());
    EXPECT_EQ(info["original_index"], "idx1");
}

TEST_F(BackupStoreStateTest, GetBackupInfo_NonExistent_ReturnsNull) {
    EXPECT_TRUE(store_->getBackupInfo("nonexistent", "alice").is_null());
}

// --- Backup deletion ---

TEST_F(BackupStoreStateTest, DeleteBackup_NonExistent_ReturnsFalse) {
    auto [ok, msg] = store_->deleteBackup("nonexistent", "alice");
    EXPECT_FALSE(ok);
    EXPECT_EQ(msg, "Backup not found");
}

TEST_F(BackupStoreStateTest, DeleteBackup_InvalidName_ReturnsFalse) {
    auto [ok, msg] = store_->deleteBackup("bad/name", "alice");
    EXPECT_FALSE(ok);
}

TEST_F(BackupStoreStateTest, DeleteBackup_RemovesTarAndJsonEntry) {
    std::string backup_dir = store_->getUserBackupDir("alice");
    fs::create_directories(backup_dir);

    std::string tar_path = backup_dir + "/bk1.tar";
    std::ofstream(tar_path) << "fake tar content";

    nlohmann::json data;
    data["bk1"] = {{"original_index", "idx1"}};
    store_->writeBackupJson("alice", data);

    auto [ok, msg] = store_->deleteBackup("bk1", "alice");
    EXPECT_TRUE(ok);
    EXPECT_FALSE(fs::exists(tar_path));
    EXPECT_FALSE(store_->listBackups("alice").contains("bk1"));
}

// ============================================================
// Layer 2 — Archive (tar) operations
// ============================================================

class BackupArchiveTest : public ::testing::Test {
protected:
    std::string dir_;
    std::unique_ptr<BackupStore> store_;

    void SetUp() override {
        dir_ = "./test_backup_archive_" + std::to_string(rand());
        fs::create_directories(dir_);
        store_ = std::make_unique<BackupStore>(dir_);
    }

    void TearDown() override {
        store_.reset();
        if (fs::exists(dir_)) fs::remove_all(dir_);
    }

    std::string makeSourceDir(const std::string& name) {
        std::string src = dir_ + "/" + name;
        fs::create_directories(src);
        std::ofstream(src + "/file_a.bin") << "hello from file_a";
        std::ofstream(src + "/file_b.bin") << "hello from file_b";
        return src;
    }
};

TEST_F(BackupArchiveTest, CreateBackupTar_ProducesNonEmptyFile) {
    std::string src = makeSourceDir("myidx");
    std::string archive = dir_ + "/out.tar";
    std::string err;
    bool ok = store_->createBackupTar(src, archive, err);
    EXPECT_TRUE(ok) << "error: " << err;
    EXPECT_TRUE(fs::exists(archive));
    EXPECT_GT(fs::file_size(archive), 0u);
}

TEST_F(BackupArchiveTest, ExtractBackupTar_FilesRoundTrip) {
    std::string src = makeSourceDir("myidx");
    std::string archive = dir_ + "/out.tar";
    std::string err;
    ASSERT_TRUE(store_->createBackupTar(src, archive, err)) << err;

    std::string dest = dir_ + "/extracted";
    ASSERT_TRUE(store_->extractBackupTar(archive, dest, err)) << err;

    EXPECT_TRUE(fs::exists(dest + "/myidx/file_a.bin"));
    EXPECT_TRUE(fs::exists(dest + "/myidx/file_b.bin"));
}

TEST_F(BackupArchiveTest, ExtractBackupTar_ContentPreserved) {
    std::string src = makeSourceDir("myidx");
    std::string archive = dir_ + "/out.tar";
    std::string err;
    ASSERT_TRUE(store_->createBackupTar(src, archive, err));

    std::string dest = dir_ + "/extracted";
    ASSERT_TRUE(store_->extractBackupTar(archive, dest, err));

    std::ifstream f(dest + "/myidx/file_a.bin");
    std::string content((std::istreambuf_iterator<char>(f)), {});
    EXPECT_EQ(content, "hello from file_a");
}

TEST_F(BackupArchiveTest, ExtractBackupTar_NonExistentArchive_Fails) {
    std::string err;
    bool ok = store_->extractBackupTar(dir_ + "/no.tar", dir_ + "/dest", err);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(err.empty());
}

TEST_F(BackupArchiveTest, CreateBackupTar_PreCancelledStopToken_ReturnsFalse) {
    std::string src = makeSourceDir("myidx");
    for (int i = 0; i < 10; ++i)
        std::ofstream(src + "/extra_" + std::to_string(i) + ".bin") << std::string(512, 'x');

    std::string archive = dir_ + "/out.tar";
    std::string err;

    std::stop_source ss;
    ss.request_stop();

    bool ok = store_->createBackupTar(src, archive, err, ss.get_token());
    EXPECT_FALSE(ok);
    EXPECT_EQ(err, "Backup cancelled");
}

// ============================================================
// Layer 3 — Integration tests via IndexManager
// ============================================================

class BackupIntegrationTest : public ::testing::Test {
protected:
    static constexpr const char* USERNAME    = "testuser";
    static constexpr const char* IDX_NAME    = "testidx";
    static constexpr const char* INDEX_ID    = "testuser/testidx";
    static constexpr const char* BACKUP_NAME = "mybk";
    static constexpr size_t      DIM         = 32;
    static constexpr size_t      N_VECTORS   = 50;

    std::string data_dir_;
    std::unique_ptr<IndexManager> manager_;

    void SetUp() override {
        data_dir_ = "./test_backup_integration_" + std::to_string(rand());
        fs::create_directories(data_dir_);
        PersistenceConfig pcfg;
        pcfg.save_on_shutdown = false;
        manager_ = std::make_unique<IndexManager>(data_dir_, pcfg);
    }

    void TearDown() override {
        manager_.reset();
        if (fs::exists(data_dir_)) fs::remove_all(data_dir_);
    }

    void createTestIndex(const std::string& index_id = INDEX_ID) {
        IndexConfig config{
            .dim             = DIM,
            .max_elements    = 1000,
            .space_type_str  = "cosine",
            .M               = 8,
            .ef_construction = 64,
            .quant_level     = ndd::quant::QuantizationLevel::FP32,
            .checksum        = 0
        };
        manager_->createIndex(index_id, config, UserType::Admin, 0);
    }

    void insertVectors(size_t n = N_VECTORS, const std::string& index_id = INDEX_ID) {
        std::vector<ndd::HybridVectorObject> vecs;
        vecs.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            ndd::HybridVectorObject v;
            v.id = "vec_" + std::to_string(i);
            v.vector.resize(DIM);
            for (size_t d = 0; d < DIM; ++d)
                v.vector[d] = static_cast<float>(rand()) / RAND_MAX;
            vecs.push_back(std::move(v));
        }
        manager_->addVectors(index_id, vecs);
    }

    // Waits until the named backup appears in listBackups (signals successful write).
    // Used for create-backup completion because clearActiveBackup fires before the
    // final rename + writeBackupJson, so polling getActiveBackup is not sufficient.
    bool waitForBackupInList(const std::string& backup_name, int timeout_sec = 15) {
        auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(timeout_sec);
        while (std::chrono::steady_clock::now() < deadline) {
            if (manager_->listBackups(USERNAME).contains(backup_name))
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }

    // Waits until no active backup remains for USERNAME.
    // Reliable for restore because clearActiveBackup fires after loadIndex.
    bool waitForNoActiveBackup(int timeout_sec = 15) {
        auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(timeout_sec);
        while (std::chrono::steady_clock::now() < deadline) {
            if (!manager_->getActiveBackup(USERNAME).has_value())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }
};

TEST_F(BackupIntegrationTest, CreateBackupAsync_ReturnsTrueAndBackupName) {
    createTestIndex();
    insertVectors();
    auto [ok, name] = manager_->createBackupAsync(INDEX_ID, BACKUP_NAME);
    EXPECT_TRUE(ok);
    EXPECT_EQ(name, BACKUP_NAME);
    waitForBackupInList(BACKUP_NAME);
}

TEST_F(BackupIntegrationTest, CreateBackup_SetsActiveBackupDuringRun) {
    createTestIndex();
    insertVectors();
    manager_->createBackupAsync(INDEX_ID, BACKUP_NAME);
    // setActiveBackup is synchronous — active backup must be visible immediately
    auto active = manager_->getActiveBackup(USERNAME);
    EXPECT_TRUE(active.has_value());
    EXPECT_EQ(active->first, BACKUP_NAME);
    EXPECT_EQ(active->second, "creation");
    waitForBackupInList(BACKUP_NAME);
}

TEST_F(BackupIntegrationTest, CreateBackup_ProducesTarFile) {
    createTestIndex();
    insertVectors();
    manager_->createBackupAsync(INDEX_ID, BACKUP_NAME);
    ASSERT_TRUE(waitForBackupInList(BACKUP_NAME));

    std::string tar = data_dir_ + "/backups/" + USERNAME + "/" + BACKUP_NAME + ".tar";
    EXPECT_TRUE(fs::exists(tar));
    EXPECT_GT(fs::file_size(tar), 0u);
}

TEST_F(BackupIntegrationTest, CreateBackup_AppearsInListBackups) {
    createTestIndex();
    insertVectors();
    manager_->createBackupAsync(INDEX_ID, BACKUP_NAME);
    ASSERT_TRUE(waitForBackupInList(BACKUP_NAME));

    EXPECT_TRUE(manager_->listBackups(USERNAME).contains(BACKUP_NAME));
}

TEST_F(BackupIntegrationTest, CreateBackup_MetadataHasExpectedFields) {
    createTestIndex();
    insertVectors();
    manager_->createBackupAsync(INDEX_ID, BACKUP_NAME);
    ASSERT_TRUE(waitForBackupInList(BACKUP_NAME));

    auto info = manager_->getBackupInfo(BACKUP_NAME, USERNAME);
    EXPECT_FALSE(info.is_null());
    EXPECT_EQ(info["original_index"], IDX_NAME);
    ASSERT_TRUE(info.contains("params"));
    EXPECT_EQ(info["params"]["dim"].get<size_t>(), DIM);
    EXPECT_TRUE(info.contains("timestamp"));
}

TEST_F(BackupIntegrationTest, CreateBackup_WhileInProgress_ReturnsFalse) {
    createTestIndex();
    insertVectors();
    auto [ok1, _1] = manager_->createBackupAsync(INDEX_ID, BACKUP_NAME);
    ASSERT_TRUE(ok1);

    // setActiveBackup is synchronous; second call must be rejected
    auto [ok2, msg] = manager_->createBackupAsync(INDEX_ID, "another_bk");
    EXPECT_FALSE(ok2);
    EXPECT_NE(msg.find("in progress"), std::string::npos);
    waitForBackupInList(BACKUP_NAME);
}

TEST_F(BackupIntegrationTest, CreateBackup_DuplicateName_ReturnsFalse) {
    createTestIndex();
    insertVectors();
    manager_->createBackupAsync(INDEX_ID, BACKUP_NAME);
    ASSERT_TRUE(waitForBackupInList(BACKUP_NAME));

    auto [ok, msg] = manager_->createBackupAsync(INDEX_ID, BACKUP_NAME);
    EXPECT_FALSE(ok);
}

TEST_F(BackupIntegrationTest, CreateBackup_InvalidName_ReturnsFalse) {
    createTestIndex();
    auto [ok, msg] = manager_->createBackupAsync(INDEX_ID, "bad/name");
    EXPECT_FALSE(ok);
}

TEST_F(BackupIntegrationTest, DeleteBackup_RemovesTarAndJsonEntry) {
    createTestIndex();
    insertVectors();
    manager_->createBackupAsync(INDEX_ID, BACKUP_NAME);
    ASSERT_TRUE(waitForBackupInList(BACKUP_NAME));

    auto [ok, msg] = manager_->deleteBackup(BACKUP_NAME, USERNAME);
    EXPECT_TRUE(ok);

    std::string tar = data_dir_ + "/backups/" + USERNAME + "/" + BACKUP_NAME + ".tar";
    EXPECT_FALSE(fs::exists(tar));
    EXPECT_FALSE(manager_->listBackups(USERNAME).contains(BACKUP_NAME));
}

TEST_F(BackupIntegrationTest, DeleteBackup_NonExistent_ReturnsFalse) {
    auto [ok, msg] = manager_->deleteBackup("no_such_backup", USERNAME);
    EXPECT_FALSE(ok);
}

TEST_F(BackupIntegrationTest, RestoreBackupAsync_ReturnsTrueAndTargetName) {
    createTestIndex();
    insertVectors();
    manager_->createBackupAsync(INDEX_ID, BACKUP_NAME);
    ASSERT_TRUE(waitForBackupInList(BACKUP_NAME));

    auto [ok, name] = manager_->restoreBackupAsync(BACKUP_NAME, "restored_idx", USERNAME);
    EXPECT_TRUE(ok);
    EXPECT_EQ(name, "restored_idx");
    waitForNoActiveBackup();
}

TEST_F(BackupIntegrationTest, RestoreBackup_CreatesIndexWithCorrectMetadata) {
    createTestIndex();
    insertVectors();
    manager_->createBackupAsync(INDEX_ID, BACKUP_NAME);
    ASSERT_TRUE(waitForBackupInList(BACKUP_NAME));

    manager_->restoreBackupAsync(BACKUP_NAME, "restored_idx", USERNAME);
    ASSERT_TRUE(waitForNoActiveBackup());

    auto meta = manager_->getMetadata(USERNAME + std::string("/restored_idx"));
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->name, "restored_idx");
    EXPECT_EQ(meta->dimension, DIM);
    EXPECT_EQ(meta->M, 8u);
}

TEST_F(BackupIntegrationTest, RestoreBackup_PreservesVectorCount) {
    createTestIndex();
    insertVectors(N_VECTORS);
    size_t original_count = manager_->getElementCount(INDEX_ID);

    manager_->createBackupAsync(INDEX_ID, BACKUP_NAME);
    ASSERT_TRUE(waitForBackupInList(BACKUP_NAME));

    manager_->restoreBackupAsync(BACKUP_NAME, "restored_idx", USERNAME);
    ASSERT_TRUE(waitForNoActiveBackup());

    std::string restored_id = USERNAME + std::string("/restored_idx");
    EXPECT_EQ(manager_->getElementCount(restored_id), original_count);
}

TEST_F(BackupIntegrationTest, RestoreBackup_NonExistentBackup_ReturnsFalse) {
    auto [ok, msg] = manager_->restoreBackupAsync("no_such_backup", "some_idx", USERNAME);
    EXPECT_FALSE(ok);
    EXPECT_NE(msg.find("not found"), std::string::npos);
}

TEST_F(BackupIntegrationTest, RestoreBackup_TargetIndexAlreadyExists_ReturnsFalse) {
    createTestIndex();
    insertVectors();
    manager_->createBackupAsync(INDEX_ID, BACKUP_NAME);
    ASSERT_TRUE(waitForBackupInList(BACKUP_NAME));

    // IDX_NAME index already exists
    auto [ok, msg] = manager_->restoreBackupAsync(BACKUP_NAME, IDX_NAME, USERNAME);
    EXPECT_FALSE(ok);
    EXPECT_NE(msg.find("already exists"), std::string::npos);
}

TEST_F(BackupIntegrationTest, RestoreBackup_WhileCreateInProgress_ReturnsFalse) {
    createTestIndex();
    insertVectors();
    manager_->createBackupAsync(INDEX_ID, BACKUP_NAME);

    // setActiveBackup is synchronous — restore must be rejected immediately
    auto [ok, msg] = manager_->restoreBackupAsync(BACKUP_NAME, "restored_idx", USERNAME);
    EXPECT_FALSE(ok);
    EXPECT_NE(msg.find("in progress"), std::string::npos);
    waitForNoActiveBackup();
}
