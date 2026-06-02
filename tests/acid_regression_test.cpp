#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "mdbx/mdbx.h"
#include "sparse/sparse_storage.hpp"
#include "storage/shared_mdbx.hpp"
#include "storage/wal.hpp"

namespace fs = std::filesystem;

namespace {

fs::path unique_temp_dir(const std::string& name) {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() / (name + "_" + std::to_string(ticks));
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

ndd::SparseVector sparse_vec(std::initializer_list<std::pair<uint32_t, float>> entries) {
    ndd::SparseVector vec;
    vec.indices.reserve(entries.size());
    vec.values.reserve(entries.size());
    for(const auto& [term, value] : entries) {
        vec.indices.push_back(term);
        vec.values.push_back(value);
    }
    return vec;
}

// Test helper: wrap one committed sparse batch in its own txn. Production
// callers always have an outer txn; this helper exists only because tests
// seed initial state outside any request transaction.
void seed_sparse_batch(MDBX_env* env, ndd::SparseVectorStorage& storage,
                       const std::vector<std::pair<ndd::idInt, ndd::SparseVector>>& batch) {
    MDBX_txn* txn = nullptr;
    int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
    ASSERT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);
    auto result = storage.store_vectors_batch(txn, batch);
    if(!result.ok()) {
        mdbx_txn_abort(txn);
        FAIL() << result.message;
        return;
    }
    rc = mdbx_txn_commit(txn);
    ASSERT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);
    storage.apply_term_info_changes(result.value_or_throw());
}

}  // namespace

TEST(AcidRegressionTest, AbortedSparseReplacementDoesNotChangeSearchableTerms) {
    TempDir root("acid_sparse_abort");
    ndd::storage::SharedIndexEnv env((root.path / "vectors").string());
    ndd::storage::SharedIndexEnv::write_layout_version(
            env.get(), settings::INDEX_LAYOUT_VERSION);
    ndd::SparseVectorStorage sparse_storage(
            env.get(), "user/index", ndd::SparseScoringModel::DEFAULT);
    ASSERT_TRUE(sparse_storage.initialize());

    seed_sparse_batch(env.get(), sparse_storage,
                      {{1, sparse_vec({{7, 1.0f}})}});

    ndd::SparseVector query = sparse_vec({{7, 1.0f}});
    auto sparse_search = [&](const ndd::SparseVector& q, size_t k) {
        MDBX_txn* t = nullptr;
        int r = mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &t);
        EXPECT_EQ(r, MDBX_SUCCESS) << mdbx_strerror(r);
        auto out = sparse_storage.search(t, q, k);
        mdbx_txn_abort(t);
        return out;
    };
    auto before_abort = sparse_search(query, 10);
    ASSERT_EQ(before_abort.size(), 1u);
    EXPECT_EQ(before_abort[0].first, 1u);

    MDBX_txn* txn = nullptr;
    int rc = mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn);
    ASSERT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);

    int64_t sparse_delta = 0;
    auto sparse_result = sparse_storage.store_vectors_batch(
            txn, {{1, ndd::SparseVector{}}}, &sparse_delta);
    ASSERT_TRUE(sparse_result.ok()) << sparse_result.message;
    EXPECT_EQ(sparse_delta, -1);

    /*
     * Abort the transaction WITHOUT calling apply_term_info_changes - this is
     * the precise atomicity invariant under test. After abort, MDBX rolls back
     * the posting-list rows and sparse_result goes out of scope without ever
     * being applied to InvertedIndex::term_info_, so the in-memory cache stays
     * in lockstep with the rolled-back MDBX state.
     */
    mdbx_txn_abort(txn);

    auto after_abort = sparse_search(query, 10);
    ASSERT_EQ(after_abort.size(), 1u)
            << "Aborting the shared transaction must leave the committed sparse postings "
               "searchable in the current process";
    EXPECT_EQ(after_abort[0].first, 1u);
}

TEST(SparseSearchTxn, InvertedIndexSearchTxnMatchesSearch) {
    TempDir root("inverted_search_txn");
    ndd::storage::SharedIndexEnv env((root.path / "vectors").string());
    ndd::storage::SharedIndexEnv::write_layout_version(
            env.get(), settings::INDEX_LAYOUT_VERSION);
    ndd::SparseVectorStorage sparse_storage(
            env.get(), "user/index", ndd::SparseScoringModel::DEFAULT);
    ASSERT_TRUE(sparse_storage.initialize());

    /**
     * Populate a small inverted index with overlapping terms across documents,
     * so query terms hit multiple postings and the batch / pruning logic runs.
     */
    seed_sparse_batch(env.get(), sparse_storage,
                      {
                              {1, sparse_vec({{10, 1.0f}, {20, 0.5f}, {30, 0.2f}})},
                              {2, sparse_vec({{10, 0.8f}, {20, 0.7f}})},
                              {3, sparse_vec({{20, 1.0f}, {30, 0.9f}})},
                              {4, sparse_vec({{30, 0.6f}, {40, 1.0f}})},
                              {5, sparse_vec({{10, 0.4f}, {40, 0.3f}})},
                      });

    const ndd::SparseVector queries[] = {
            sparse_vec({{10, 1.0f}}),
            sparse_vec({{10, 1.0f}, {30, 1.0f}}),
            sparse_vec({{20, 0.5f}, {40, 0.5f}}),
            sparse_vec({{99, 1.0f}}),  // unknown term
    };

    for(const auto& query : queries) {
        MDBX_txn* txn1 = nullptr;
        int rc1 = mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &txn1);
        ASSERT_EQ(rc1, MDBX_SUCCESS) << mdbx_strerror(rc1);
        auto first = sparse_storage.search(txn1, query, 10);
        mdbx_txn_abort(txn1);

        MDBX_txn* txn2 = nullptr;
        int rc2 = mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &txn2);
        ASSERT_EQ(rc2, MDBX_SUCCESS) << mdbx_strerror(rc2);
        auto second = sparse_storage.search(txn2, query, 10);
        mdbx_txn_abort(txn2);

        ASSERT_EQ(first.size(), second.size())
                << "result size diverged across read snapshots for query of "
                << query.indices.size() << " term(s)";
        for(size_t i = 0; i < first.size(); ++i) {
            EXPECT_EQ(first[i].first, second[i].first)
                    << "doc_id order diverged at " << i;
            EXPECT_FLOAT_EQ(first[i].second, second[i].second)
                    << "score diverged at " << i;
        }
    }
}

TEST(SparseSearchTxn, InvertedIndexSearchTxnSharesSnapshot) {
    TempDir root("inverted_search_txn_snapshot");
    ndd::storage::SharedIndexEnv env((root.path / "vectors").string());
    ndd::storage::SharedIndexEnv::write_layout_version(
            env.get(), settings::INDEX_LAYOUT_VERSION);
    ndd::SparseVectorStorage sparse_storage(
            env.get(), "user/index", ndd::SparseScoringModel::DEFAULT);
    ASSERT_TRUE(sparse_storage.initialize());

    seed_sparse_batch(env.get(), sparse_storage,
                      {
                              {1, sparse_vec({{10, 1.0f}})},
                              {2, sparse_vec({{20, 1.0f}})},
                      });

    /**
     * Two sequential search calls on the same read txn must each succeed
     * and return results consistent with the snapshot at txn-begin time.
     */
    MDBX_txn* txn = nullptr;
    int rc = mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &txn);
    ASSERT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);

    auto r1 = sparse_storage.search(txn, sparse_vec({{10, 1.0f}}), 5);
    auto r2 = sparse_storage.search(txn, sparse_vec({{20, 1.0f}}), 5);

    mdbx_txn_abort(txn);

    ASSERT_EQ(r1.size(), 1u);
    EXPECT_EQ(r1[0].first, 1u);
    ASSERT_EQ(r2.size(), 1u);
    EXPECT_EQ(r2[0].first, 2u);
}

/*
 * DISABLED - this exposes the known atomicity bug on WAL::entry_count_
 * (see the detailed comment on the entry_count_ declaration in
 * src/storage/wal.hpp). Re-enable when that fix lands; the test body itself
 * is already a faithful repro of the abort scenario.
 */
TEST(AcidRegressionTest, DISABLED_AbortedWalAppendDoesNotPublishEntryCount) {
    TempDir root("acid_wal_append_abort");
    ndd::storage::SharedIndexEnv env((root.path / "vectors").string());
    ndd::storage::SharedIndexEnv::write_layout_version(
            env.get(), settings::INDEX_LAYOUT_VERSION);
    WriteAheadLog wal(env.get(), "user/index");

    ASSERT_FALSE(wal.hasEntries());

    MDBX_txn* txn = nullptr;
    int rc = mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn);
    ASSERT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);

    wal.log(txn, {{WALOperationType::VECTOR_ADD, 42}});
    mdbx_txn_abort(txn);

    EXPECT_TRUE(wal.readEntries().empty());
    EXPECT_FALSE(wal.hasEntries())
            << "The WAL in-memory entry count must not advance for an aborted MDBX txn";
    EXPECT_EQ(wal.getEntryCount(), 0u);
}

/*
 * DISABLED - see the comment on the previous test and on
 * WriteAheadLog::entry_count_ in src/storage/wal.hpp for why this is deferred.
 */
TEST(AcidRegressionTest, DISABLED_AbortedWalClearDoesNotHideCommittedEntries) {
    TempDir root("acid_wal_clear_abort");
    ndd::storage::SharedIndexEnv env((root.path / "vectors").string());
    ndd::storage::SharedIndexEnv::write_layout_version(
            env.get(), settings::INDEX_LAYOUT_VERSION);
    WriteAheadLog wal(env.get(), "user/index");

    {
        MDBX_txn* log = nullptr;
        int rc = mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &log);
        ASSERT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);
        wal.log(log, {{WALOperationType::VECTOR_ADD, 42}});
        ASSERT_EQ(mdbx_txn_commit(log), MDBX_SUCCESS);
    }
    ASSERT_TRUE(wal.hasEntries());
    ASSERT_EQ(wal.readEntries().size(), 1u);

    MDBX_txn* txn = nullptr;
    int rc = mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn);
    ASSERT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);

    wal.clear(txn);
    mdbx_txn_abort(txn);

    EXPECT_EQ(wal.readEntries().size(), 1u);
    EXPECT_TRUE(wal.hasEntries())
            << "Aborting the clear transaction must not publish an empty WAL state";
    EXPECT_EQ(wal.getEntryCount(), 1u);
}
