#include <filesystem>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <gtest/gtest.h>

#include "rebuild.hpp"
#include "ndd.hpp"
#include "utils/msgpack_ndd.hpp"
#include "server/auth.hpp"

namespace fs = std::filesystem;

// ============================================================
// Layer 1 — Rebuild state management (no IndexManager needed)
// ============================================================

class RebuildStateTest : public ::testing::Test {
protected:
    Rebuild rebuild;
};

TEST_F(RebuildStateTest, NoRebuild_HasActiveIsFalse) {
    EXPECT_FALSE(rebuild.hasActiveRebuild("alice"));
}

TEST_F(RebuildStateTest, NoRebuild_GetProgressIsIdle) {
    auto p = rebuild.getProgress("alice", "alice/idx");
    EXPECT_EQ(p["status"], "idle");
}

TEST_F(RebuildStateTest, SetActive_HasActiveIsTrue) {
    rebuild.setActiveRebuild("alice", "alice/idx", 100);
    EXPECT_TRUE(rebuild.hasActiveRebuild("alice"));
}

TEST_F(RebuildStateTest, SetActive_GetProgressShowsInProgress) {
    rebuild.setActiveRebuild("alice", "alice/idx", 200);
    auto p = rebuild.getProgress("alice", "alice/idx");
    EXPECT_EQ(p["status"], "in_progress");
    EXPECT_EQ(p["total_vectors"], 200);
    EXPECT_EQ(p["vectors_processed"], 0);
}

TEST_F(RebuildStateTest, UpdateProgress_ReflectedInGetProgress) {
    rebuild.setActiveRebuild("alice", "alice/idx", 100);
    rebuild.updateProgress("alice", 50);
    auto p = rebuild.getProgress("alice", "alice/idx");
    EXPECT_EQ(p["vectors_processed"], 50);
}

TEST_F(RebuildStateTest, PercentComplete_CalculatedCorrectly) {
    rebuild.setActiveRebuild("alice", "alice/idx", 100);
    rebuild.updateProgress("alice", 50);
    auto p = rebuild.getProgress("alice", "alice/idx");
    EXPECT_DOUBLE_EQ(p["percent_complete"].get<double>(), 50.0);
}

TEST_F(RebuildStateTest, PercentComplete_ZeroTotal_IsZero) {
    rebuild.setActiveRebuild("alice", "alice/idx", 0);
    auto p = rebuild.getProgress("alice", "alice/idx");
    EXPECT_DOUBLE_EQ(p["percent_complete"].get<double>(), 0.0);
}

TEST_F(RebuildStateTest, Complete_StatusIsCompleted) {
    rebuild.setActiveRebuild("alice", "alice/idx", 100);
    rebuild.completeActiveRebuild("alice");
    auto p = rebuild.getProgress("alice", "alice/idx");
    EXPECT_EQ(p["status"], "completed");
}

TEST_F(RebuildStateTest, Complete_HasActiveIsFalse) {
    rebuild.setActiveRebuild("alice", "alice/idx", 100);
    rebuild.completeActiveRebuild("alice");
    EXPECT_FALSE(rebuild.hasActiveRebuild("alice"));
}

TEST_F(RebuildStateTest, Complete_CompletedAtPresent) {
    rebuild.setActiveRebuild("alice", "alice/idx", 100);
    rebuild.completeActiveRebuild("alice");
    auto p = rebuild.getProgress("alice", "alice/idx");
    EXPECT_TRUE(p.contains("completed_at"));
}

TEST_F(RebuildStateTest, Fail_StatusIsFailed) {
    rebuild.setActiveRebuild("alice", "alice/idx", 100);
    rebuild.failActiveRebuild("alice", "disk full");
    auto p = rebuild.getProgress("alice", "alice/idx");
    EXPECT_EQ(p["status"], "failed");
}

TEST_F(RebuildStateTest, Fail_HasActiveIsFalse) {
    rebuild.setActiveRebuild("alice", "alice/idx", 100);
    rebuild.failActiveRebuild("alice", "disk full");
    EXPECT_FALSE(rebuild.hasActiveRebuild("alice"));
}

TEST_F(RebuildStateTest, Fail_ErrorMessagePresent) {
    rebuild.setActiveRebuild("alice", "alice/idx", 100);
    rebuild.failActiveRebuild("alice", "disk full");
    auto p = rebuild.getProgress("alice", "alice/idx");
    EXPECT_EQ(p["error"], "disk full");
}

TEST_F(RebuildStateTest, Fail_CompletedAtPresent) {
    rebuild.setActiveRebuild("alice", "alice/idx", 100);
    rebuild.failActiveRebuild("alice", "oom");
    auto p = rebuild.getProgress("alice", "alice/idx");
    EXPECT_TRUE(p.contains("completed_at"));
}

TEST_F(RebuildStateTest, TwoUsers_IndependentState) {
    rebuild.setActiveRebuild("alice", "alice/idx", 100);
    EXPECT_TRUE(rebuild.hasActiveRebuild("alice"));
    EXPECT_FALSE(rebuild.hasActiveRebuild("bob"));
    rebuild.setActiveRebuild("bob", "bob/idx", 50);
    EXPECT_TRUE(rebuild.hasActiveRebuild("bob"));
    rebuild.completeActiveRebuild("alice");
    EXPECT_FALSE(rebuild.hasActiveRebuild("alice"));
    EXPECT_TRUE(rebuild.hasActiveRebuild("bob"));
}

TEST_F(RebuildStateTest, GetProgress_WrongIndex_ReturnsIdle) {
    rebuild.setActiveRebuild("alice", "alice/idx", 100);
    auto p = rebuild.getProgress("alice", "alice/other");
    EXPECT_EQ(p["status"], "idle");
}

TEST_F(RebuildStateTest, SetActive_OverwritesPreviousCompleted) {
    rebuild.setActiveRebuild("alice", "alice/idx", 100);
    rebuild.completeActiveRebuild("alice");
    rebuild.setActiveRebuild("alice", "alice/idx", 200);
    auto p = rebuild.getProgress("alice", "alice/idx");
    EXPECT_EQ(p["status"], "in_progress");
    EXPECT_EQ(p["total_vectors"], 200);
}

// ============================================================
// Layer 2 — Temp file cleanup and path helpers
// ============================================================

class RebuildCleanupTest : public ::testing::Test {
protected:
    std::string dir_;
    Rebuild rebuild_;

    void SetUp() override {
        dir_ = "./test_rebuild_cleanup_" + std::to_string(rand());
        fs::create_directories(dir_ + "/user/idx/vectors");
    }

    void TearDown() override {
        if (fs::exists(dir_)) fs::remove_all(dir_);
    }

    void touch(const std::string& rel_path) {
        std::ofstream f(dir_ + "/" + rel_path);
        f << "x";
    }

    bool exists(const std::string& rel_path) {
        return fs::exists(dir_ + "/" + rel_path);
    }
};

TEST_F(RebuildCleanupTest, CleanupTempFiles_NonExistentDir_NoOp) {
    EXPECT_NO_THROW(rebuild_.cleanupTempFiles("/nonexistent/path/xyz"));
}

TEST_F(RebuildCleanupTest, CleanupTempFiles_RemovesTempFile) {
    touch("user/idx/vectors/default.idx.temp");
    rebuild_.cleanupTempFiles(dir_);
    EXPECT_FALSE(exists("user/idx/vectors/default.idx.temp"));
}

TEST_F(RebuildCleanupTest, CleanupTempFiles_RemovesTimestampedFile) {
    touch("user/idx/vectors/default.idx.1714900000");
    rebuild_.cleanupTempFiles(dir_);
    EXPECT_FALSE(exists("user/idx/vectors/default.idx.1714900000"));
}

TEST_F(RebuildCleanupTest, CleanupTempFiles_LeavesCanonicalIndex) {
    touch("user/idx/vectors/default.idx");
    rebuild_.cleanupTempFiles(dir_);
    EXPECT_TRUE(exists("user/idx/vectors/default.idx"));
}

TEST_F(RebuildCleanupTest, CleanupTempFiles_EmptyDir_NoOp) {
    EXPECT_NO_THROW(rebuild_.cleanupTempFiles(dir_));
}

TEST(RebuildPathTest, GetTempPath_Format) {
    auto path = Rebuild::getTempPath("/data/user/idx");
    EXPECT_EQ(path, "/data/user/idx/vectors/default.idx.temp");
}

TEST(RebuildPathTest, GetTimestampedPath_HasTimestamp) {
    auto path = Rebuild::getTimestampedPath("/data/user/idx");
    // Should match /data/user/idx/vectors/default.idx.<digits>
    std::string prefix = "/data/user/idx/vectors/default.idx.";
    ASSERT_GT(path.size(), prefix.size());
    EXPECT_EQ(path.substr(0, prefix.size()), prefix);
    std::string suffix = path.substr(prefix.size());
    EXPECT_FALSE(suffix.empty());
    EXPECT_TRUE(std::all_of(suffix.begin(), suffix.end(), ::isdigit));
}

// ============================================================
// Layer 3 — Integration tests via IndexManager
// ============================================================

class RebuildIntegrationTest : public ::testing::Test {
protected:
    static constexpr const char* USERNAME  = "testuser";
    static constexpr const char* IDX_NAME  = "testidx";
    static constexpr const char* INDEX_ID  = "testuser/testidx";
    static constexpr size_t DIM            = 32;
    static constexpr size_t N_VECTORS      = 100;

    std::string data_dir_;
    std::unique_ptr<IndexManager> manager_;

    void SetUp() override {
        data_dir_ = "./test_rebuild_integration_" + std::to_string(rand());
        fs::create_directories(data_dir_);
        PersistenceConfig pcfg;
        pcfg.save_on_shutdown = false;
        manager_ = std::make_unique<IndexManager>(data_dir_, pcfg);
    }

    void TearDown() override {
        manager_.reset();
        if (fs::exists(data_dir_)) fs::remove_all(data_dir_);
    }

    void createTestIndex(size_t M = 8, size_t ef_con = 64) {
        IndexConfig config{
            .dim             = DIM,
            .max_elements    = 1000,
            .space_type_str  = "cosine",
            .M               = M,
            .ef_construction = ef_con,
            .quant_level     = ndd::quant::QuantizationLevel::FP32,
            .checksum        = 0
        };
        manager_->createIndex(INDEX_ID, config, UserType::Admin, 0);
    }

    void insertVectors(size_t n = N_VECTORS) {
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
        manager_->addVectors(INDEX_ID, vecs);
    }

    // Returns true if rebuild completed successfully within timeout_sec.
    bool waitForRebuild(int timeout_sec = 10) {
        auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(timeout_sec);
        while (std::chrono::steady_clock::now() < deadline) {
            auto progress = manager_->getRebuildProgress(USERNAME, INDEX_ID);
            std::string status = progress.value("status", "");
            if (status == "completed") return true;
            if (status == "failed")    return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }
};

TEST_F(RebuildIntegrationTest, RebuildAsync_ReturnSuccessCode) {
    createTestIndex();
    insertVectors();
    auto result = manager_->rebuildIndexAsync(INDEX_ID, 16, 128);
    EXPECT_EQ(result.code, 0);
    waitForRebuild();
}

TEST_F(RebuildIntegrationTest, RebuildCompletes_ConfigUpdated) {
    createTestIndex(8, 64);
    insertVectors();
    manager_->rebuildIndexAsync(INDEX_ID, 16, 128);
    ASSERT_TRUE(waitForRebuild());
    auto meta = manager_->getMetadata(INDEX_ID);
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->M, 16u);
    EXPECT_EQ(meta->ef_con, 128u);
}

TEST_F(RebuildIntegrationTest, RebuildCompletes_VectorCountPreserved) {
    createTestIndex();
    insertVectors(N_VECTORS);
    size_t before = manager_->getElementCount(INDEX_ID);
    manager_->rebuildIndexAsync(INDEX_ID, 16, 128);
    ASSERT_TRUE(waitForRebuild());
    size_t after = manager_->getElementCount(INDEX_ID);
    EXPECT_EQ(before, after);
}

TEST_F(RebuildIntegrationTest, RebuildWhileInProgress_Returns409Code) {
    createTestIndex();
    insertVectors();
    // setActiveRebuild is synchronous — second call sees IN_PROGRESS before thread starts
    auto r1 = manager_->rebuildIndexAsync(INDEX_ID, 16, 128);
    ASSERT_EQ(r1.code, 0);
    auto r2 = manager_->rebuildIndexAsync(INDEX_ID, 32, 256);
    EXPECT_EQ(r2.code, 2);
    waitForRebuild();
}

TEST_F(RebuildIntegrationTest, RebuildNonExistentIndex_Returns404Code) {
    auto result = manager_->rebuildIndexAsync("testuser/doesnotexist", 16, 128);
    EXPECT_EQ(result.code, 1);
}

TEST_F(RebuildIntegrationTest, RebuildNoChange_Returns400Code) {
    createTestIndex(8, 64);
    insertVectors();
    auto result = manager_->rebuildIndexAsync(INDEX_ID, 8, 64);
    EXPECT_EQ(result.code, 3);
}

TEST_F(RebuildIntegrationTest, RebuildExcludesDeletedVectors) {
    createTestIndex();
    insertVectors(N_VECTORS);  // 100 vectors: vec_0 .. vec_99

    for (size_t i = 0; i < 10; ++i)
        manager_->deleteVector(INDEX_ID, "vec_" + std::to_string(i));

    EXPECT_EQ(manager_->getElementCount(INDEX_ID), 90u);

    manager_->rebuildIndexAsync(INDEX_ID, 16, 128);
    ASSERT_TRUE(waitForRebuild());

    EXPECT_EQ(manager_->getElementCount(INDEX_ID), 90u);
}
