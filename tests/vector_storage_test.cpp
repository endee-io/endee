// Tests for VectorStorage upsert cleanup and deleteFilter meta sync.
// These cover the two correctness gaps fixed alongside this file:
//   1. store_vectors_batch must remove the prior filter index entries when the
//      id_mapper signals the str_id was already live (is_new_to_db == false).
//   2. deleteFilter must clear meta.filter so subsequent get_meta calls do not
//      return a document whose index entries are gone.

#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "json/nlohmann_json.hpp"
// hnswlib must be included before vector_storage.hpp: vector_storage.hpp pulls in
// quant/dispatch.hpp first, but space.hpp (reached transitively) uses
// ndd::quant::get_quantizer_dispatch, which is only defined further down in
// dispatch.hpp. Routing through hnswlib.h first lets dispatch.hpp resolve
// fully before space.hpp parses its constructor body.
#include "hnsw/hnswlib.h"
#include "storage/vector_storage.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

QuantVectorObject make_obj(const std::string& id, const std::string& filter_json) {
    QuantVectorObject obj;
    obj.id = id;
    obj.filter = filter_json;
    obj.norm = 1.0f;
    // FP32 quantization with dim=4 => 16 bytes of vector payload.
    obj.quant_vector.assign(4 * sizeof(float), 0);
    return obj;
}

}  // namespace

class VectorStorageTest : public ::testing::Test {
protected:
    std::string base_path;
    std::unique_ptr<VectorStorage> storage;

    void SetUp() override {
        base_path = "./test_vs_" + std::to_string(std::rand());
        if(fs::exists(base_path)) {
            fs::remove_all(base_path);
        }
        fs::create_directories(base_path);
        storage = std::make_unique<VectorStorage>(
                base_path, "test_index", 4, ndd::quant::QuantizationLevel::FP32);
    }

    void TearDown() override {
        storage.reset();
        if(fs::exists(base_path)) {
            fs::remove_all(base_path);
        }
    }

    bool filter_matches(const json& filter_array, ndd::idInt id) {
        auto result = storage->filter_store_->getIdsMatchingFilter(filter_array);
        EXPECT_TRUE(result.ok()) << result.message;
        if(!result.ok()) return false;
        const auto& ids = result.value_or_throw();
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    }
};

// Upsert path: re-inserting a live point with a different filter must drop the
// old filter's index entries. Before the fix, the old "color":"red" mapping
// kept matching id 42 even after the upsert wrote "color":"blue".
TEST_F(VectorStorageTest, UpsertRemovesStaleCategoryFilter) {
    auto v1 = make_obj("v1", R"({"color":"red"})");
    ASSERT_TRUE(storage->store_vectors_batch({{42, v1}}, {true}).ok());
    ASSERT_TRUE(filter_matches(json::array({{{"color", {{"$eq", "red"}}}}}), 42));

    auto v1_new = make_obj("v1", R"({"color":"blue"})");
    auto result = storage->store_vectors_batch({{42, v1_new}}, {false});
    ASSERT_TRUE(result.ok()) << result.message;

    EXPECT_FALSE(filter_matches(json::array({{{"color", {{"$eq", "red"}}}}}), 42))
            << "Stale 'color=red' index entry still matches id 42 after upsert";
    EXPECT_TRUE(filter_matches(json::array({{{"color", {{"$eq", "blue"}}}}}), 42));
    EXPECT_EQ(storage->get_meta(42).filter, R"({"color":"blue"})");
}

// Same upsert invariant for numeric filters: the prior numeric index entry must
// be removed before the new one is written.
TEST_F(VectorStorageTest, UpsertRemovesStaleNumericFilter) {
    auto v1 = make_obj("v1", R"({"age":30})");
    ASSERT_TRUE(storage->store_vectors_batch({{7, v1}}, {true}).ok());
    ASSERT_TRUE(filter_matches(json::array({{{"age", {{"$eq", 30}}}}}), 7));

    auto v1_new = make_obj("v1", R"({"age":40})");
    auto result = storage->store_vectors_batch({{7, v1_new}}, {false});
    ASSERT_TRUE(result.ok()) << result.message;

    EXPECT_FALSE(filter_matches(json::array({{{"age", {{"$eq", 30}}}}}), 7))
            << "Stale 'age=30' index entry still matches id 7 after upsert";
    EXPECT_TRUE(filter_matches(json::array({{{"age", {{"$eq", 40}}}}}), 7));
    EXPECT_EQ(storage->get_meta(7).filter, R"({"age":40})");
}

// is_new_to_db == true means the slot is fresh or was reused from a deleted slot
// whose filter index was already scrubbed. The cleanup pass must skip it; no
// spurious meta read should occur on a slot that has no prior write.
TEST_F(VectorStorageTest, UpsertFreshSlotSkipsCleanup) {
    auto v1 = make_obj("v1", R"({"color":"red"})");
    auto result = storage->store_vectors_batch({{100, v1}}, {true});
    ASSERT_TRUE(result.ok()) << result.message;
    EXPECT_TRUE(filter_matches(json::array({{{"color", {{"$eq", "red"}}}}}), 100));
}

// If is_new_to_db is omitted (empty), we fall back to legacy "treat all as new"
// semantics for callers that have not been updated to forward the flag.
TEST_F(VectorStorageTest, EmptyFlagsVectorTreatsAllAsNew) {
    auto v1 = make_obj("v1", R"({"color":"red"})");
    auto result = storage->store_vectors_batch({{55, v1}});  // no flags
    ASSERT_TRUE(result.ok()) << result.message;
    EXPECT_TRUE(filter_matches(json::array({{{"color", {{"$eq", "red"}}}}}), 55));
}

// Mismatched flag length is a programmer error and must be surfaced rather than
// silently treating some entries one way and others another.
TEST_F(VectorStorageTest, MismatchedFlagsRejected) {
    auto v1 = make_obj("v1", R"({"color":"red"})");
    auto v2 = make_obj("v2", R"({"color":"blue"})");
    auto result = storage->store_vectors_batch({{1, v1}, {2, v2}}, {true});  // 1 flag for 2 vecs
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.code, 1u);
}

// is_new_to_db == false claims the slot is live, but if meta does not exist
// that is torn state from a previous partial write. Surface it instead of
// silently overwriting.
TEST_F(VectorStorageTest, UpsertOnMissingMetaSurfacesTornState) {
    auto v = make_obj("v1", R"({"color":"red"})");
    auto result = storage->store_vectors_batch({{999, v}}, {false});
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.code, 103u) << result.message;
}

// Upsert from a vector that had no filter into one that does: cleanup pass
// reads empty meta.filter, skips remove, and the new filter is added.
TEST_F(VectorStorageTest, UpsertFromEmptyFilterToPopulatedCategory) {
    auto v1 = make_obj("v1", "");
    ASSERT_TRUE(storage->store_vectors_batch({{20, v1}}, {true}).ok());

    auto v1_new = make_obj("v1", R"({"color":"green"})");
    auto result = storage->store_vectors_batch({{20, v1_new}}, {false});
    ASSERT_TRUE(result.ok()) << result.message;

    EXPECT_TRUE(filter_matches(json::array({{{"color", {{"$eq", "green"}}}}}), 20));
}

TEST_F(VectorStorageTest, UpsertFromEmptyFilterToPopulatedNumeric) {
    auto v1 = make_obj("v1", "");
    ASSERT_TRUE(storage->store_vectors_batch({{120, v1}}, {true}).ok());

    auto v1_new = make_obj("v1", R"({"score":75})");
    auto result = storage->store_vectors_batch({{120, v1_new}}, {false});
    ASSERT_TRUE(result.ok()) << result.message;

    EXPECT_TRUE(filter_matches(json::array({{{"score", {{"$eq", 75}}}}}), 120));
}

// Upsert from a populated filter into an empty filter: the old index entries
// must be removed, and the new filter add path is a no-op for an empty doc.
TEST_F(VectorStorageTest, UpsertFromPopulatedFilterToEmptyCategory) {
    auto v1 = make_obj("v1", R"({"color":"yellow"})");
    ASSERT_TRUE(storage->store_vectors_batch({{21, v1}}, {true}).ok());
    ASSERT_TRUE(filter_matches(json::array({{{"color", {{"$eq", "yellow"}}}}}), 21));

    auto v1_new = make_obj("v1", "");
    auto result = storage->store_vectors_batch({{21, v1_new}}, {false});
    ASSERT_TRUE(result.ok()) << result.message;

    EXPECT_FALSE(filter_matches(json::array({{{"color", {{"$eq", "yellow"}}}}}), 21))
            << "Old 'color=yellow' index entry survived upsert to empty filter";
    EXPECT_TRUE(storage->get_meta(21).filter.empty());
}

TEST_F(VectorStorageTest, UpsertFromPopulatedFilterToEmptyNumeric) {
    auto v1 = make_obj("v1", R"({"score":99})");
    ASSERT_TRUE(storage->store_vectors_batch({{121, v1}}, {true}).ok());
    ASSERT_TRUE(filter_matches(json::array({{{"score", {{"$eq", 99}}}}}), 121));

    auto v1_new = make_obj("v1", "");
    auto result = storage->store_vectors_batch({{121, v1_new}}, {false});
    ASSERT_TRUE(result.ok()) << result.message;

    EXPECT_FALSE(filter_matches(json::array({{{"score", {{"$eq", 99}}}}}), 121))
            << "Old 'score=99' numeric index entry survived upsert to empty filter";
    EXPECT_TRUE(storage->get_meta(121).filter.empty());
}

// deleteFilter must clear meta.filter when the stored doc matches the input
// document, so a get_meta call after deletion does not return a filter whose
// index entries are gone.
TEST_F(VectorStorageTest, DeleteFilterClearsMetaWhenMatchCategory) {
    auto v1 = make_obj("v1", R"({"color":"red"})");
    ASSERT_TRUE(storage->store_vectors_batch({{42, v1}}, {true}).ok());
    ASSERT_EQ(storage->get_meta(42).filter, R"({"color":"red"})");

    auto result = storage->deleteFilter(42, R"({"color":"red"})");
    ASSERT_TRUE(result.ok()) << result.message;

    EXPECT_FALSE(filter_matches(json::array({{{"color", {{"$eq", "red"}}}}}), 42));
    EXPECT_TRUE(storage->get_meta(42).filter.empty())
            << "meta.filter still set after deleteFilter: " << storage->get_meta(42).filter;
}

TEST_F(VectorStorageTest, DeleteFilterClearsMetaWhenMatchNumeric) {
    auto v1 = make_obj("v1", R"({"score":80})");
    ASSERT_TRUE(storage->store_vectors_batch({{142, v1}}, {true}).ok());
    ASSERT_EQ(storage->get_meta(142).filter, R"({"score":80})");

    auto result = storage->deleteFilter(142, R"({"score":80})");
    ASSERT_TRUE(result.ok()) << result.message;

    EXPECT_FALSE(filter_matches(json::array({{{"score", {{"$eq", 80}}}}}), 142));
    EXPECT_TRUE(storage->get_meta(142).filter.empty())
            << "meta.filter still set after deleteFilter: " << storage->get_meta(142).filter;
}

// deleteFilter should leave meta untouched if the caller asks to remove a
// document that no longer matches what is stored. Index entries are still
// removed (best effort).
TEST_F(VectorStorageTest, DeleteFilterLeavesMetaWhenDifferentCategory) {
    auto v1 = make_obj("v1", R"({"color":"red"})");
    ASSERT_TRUE(storage->store_vectors_batch({{42, v1}}, {true}).ok());

    auto result = storage->deleteFilter(42, R"({"color":"orange"})");
    ASSERT_TRUE(result.ok()) << result.message;

    // meta.filter was not the same doc we tried to remove, so it stays.
    EXPECT_EQ(storage->get_meta(42).filter, R"({"color":"red"})");
}

TEST_F(VectorStorageTest, DeleteFilterLeavesMetaWhenDifferentNumeric) {
    auto v1 = make_obj("v1", R"({"score":80})");
    ASSERT_TRUE(storage->store_vectors_batch({{143, v1}}, {true}).ok());

    auto result = storage->deleteFilter(143, R"({"score":81})");
    ASSERT_TRUE(result.ok()) << result.message;

    // meta.filter was not the same doc we tried to remove, so it stays.
    EXPECT_EQ(storage->get_meta(143).filter, R"({"score":80})");
}

// Explicit updateFilter path (the API surface invoked by `index_manager.updateFilters`):
// after replacing filter A with filter B, queries against the OLD filter must no
// longer return the vector and queries against the NEW filter must.
TEST_F(VectorStorageTest, UpdateFilterReplacesCategoryFilter) {
    auto v1 = make_obj("v1", R"({"color":"red"})");
    ASSERT_TRUE(storage->store_vectors_batch({{42, v1}}, {true}).ok());
    ASSERT_TRUE(filter_matches(json::array({{{"color", {{"$eq", "red"}}}}}), 42));

    auto result = storage->updateFilter(42, R"({"color":"blue"})");
    ASSERT_TRUE(result.ok()) << result.message;

    EXPECT_FALSE(filter_matches(json::array({{{"color", {{"$eq", "red"}}}}}), 42))
            << "Stale 'color=red' index entry still matches id 42 after updateFilter";
    EXPECT_TRUE(filter_matches(json::array({{{"color", {{"$eq", "blue"}}}}}), 42));
    EXPECT_EQ(storage->get_meta(42).filter, R"({"color":"blue"})");
}

TEST_F(VectorStorageTest, UpdateFilterReplacesNumericFilter) {
    auto v1 = make_obj("v1", R"({"age":30})");
    ASSERT_TRUE(storage->store_vectors_batch({{7, v1}}, {true}).ok());
    ASSERT_TRUE(filter_matches(json::array({{{"age", {{"$eq", 30}}}}}), 7));

    auto result = storage->updateFilter(7, R"({"age":40})");
    ASSERT_TRUE(result.ok()) << result.message;

    EXPECT_FALSE(filter_matches(json::array({{{"age", {{"$eq", 30}}}}}), 7))
            << "Stale 'age=30' index entry still matches id 7 after updateFilter";
    EXPECT_TRUE(filter_matches(json::array({{{"age", {{"$eq", 40}}}}}), 7));
    EXPECT_EQ(storage->get_meta(7).filter, R"({"age":40})");
}

// updateFilter starting from an empty filter document: nothing to remove, but
// the new filter must be added and meta must reflect it.
TEST_F(VectorStorageTest, UpdateFilterFromEmptyCategory) {
    auto v1 = make_obj("v1", "");
    ASSERT_TRUE(storage->store_vectors_batch({{30, v1}}, {true}).ok());

    auto result = storage->updateFilter(30, R"({"color":"green"})");
    ASSERT_TRUE(result.ok()) << result.message;

    EXPECT_TRUE(filter_matches(json::array({{{"color", {{"$eq", "green"}}}}}), 30));
    EXPECT_EQ(storage->get_meta(30).filter, R"({"color":"green"})");
}

TEST_F(VectorStorageTest, UpdateFilterFromEmptyNumeric) {
    auto v1 = make_obj("v1", "");
    ASSERT_TRUE(storage->store_vectors_batch({{130, v1}}, {true}).ok());

    auto result = storage->updateFilter(130, R"({"score":42})");
    ASSERT_TRUE(result.ok()) << result.message;

    EXPECT_TRUE(filter_matches(json::array({{{"score", {{"$eq", 42}}}}}), 130));
    EXPECT_EQ(storage->get_meta(130).filter, R"({"score":42})");
}

// updateFilter to an empty document: the prior filter index entries must be
// removed and meta.filter must be cleared.
TEST_F(VectorStorageTest, UpdateFilterToEmptyCategory) {
    auto v1 = make_obj("v1", R"({"color":"yellow"})");
    ASSERT_TRUE(storage->store_vectors_batch({{31, v1}}, {true}).ok());
    ASSERT_TRUE(filter_matches(json::array({{{"color", {{"$eq", "yellow"}}}}}), 31));

    auto result = storage->updateFilter(31, "");
    ASSERT_TRUE(result.ok()) << result.message;

    EXPECT_FALSE(filter_matches(json::array({{{"color", {{"$eq", "yellow"}}}}}), 31))
            << "Old 'color=yellow' index entry survived updateFilter to empty";
    EXPECT_TRUE(storage->get_meta(31).filter.empty());
}

TEST_F(VectorStorageTest, UpdateFilterToEmptyNumeric) {
    auto v1 = make_obj("v1", R"({"score":55})");
    ASSERT_TRUE(storage->store_vectors_batch({{131, v1}}, {true}).ok());
    ASSERT_TRUE(filter_matches(json::array({{{"score", {{"$eq", 55}}}}}), 131));

    auto result = storage->updateFilter(131, "");
    ASSERT_TRUE(result.ok()) << result.message;

    EXPECT_FALSE(filter_matches(json::array({{{"score", {{"$eq", 55}}}}}), 131))
            << "Old 'score=55' index entry survived updateFilter to empty";
    EXPECT_TRUE(storage->get_meta(131).filter.empty());
}

