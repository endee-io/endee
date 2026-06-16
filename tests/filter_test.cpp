#include <gtest/gtest.h>
#include <algorithm>
#include <climits>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>
#include "filter/filter.hpp"
#include "filter/category_index.hpp"
#include "json/nlohmann_json.hpp"
#include "filter/numeric_index.hpp" // For Bucket test

namespace fs = std::filesystem;
using json = nlohmann::json;

static void expect_ok(const ndd::OperationResult<>& result) {
    EXPECT_TRUE(result.ok()) << result.message;
}

template <typename T>
static T unwrap_ok(ndd::OperationResult<T> result) {
    EXPECT_TRUE(result.ok()) << result.message;
    EXPECT_TRUE(result.value.has_value());
    return std::move(*result.value);
}

TEST(BucketTest, Serialization) {
    ndd::filter::Bucket b;
    b.base_value = 100;
    b.add(105, 1);
    b.add(110, 2);
    
    auto bytes = b.serialize();
    EXPECT_GT(bytes.size(), 6);
    
    auto b2 = ndd::filter::Bucket::deserialize(bytes.data(), bytes.size(), 100);
    EXPECT_EQ(b2.ids.size(), 2);
    EXPECT_EQ(b2.ids[0], 1);
    EXPECT_EQ(b2.ids[1], 2);
}

class FilterTest : public ::testing::Test {
protected:
    std::string db_path;
    std::unique_ptr<Filter> filter;

    void SetUp() override {
        // Create a unique temporary directory for each test
        db_path = "./test_db_" + std::to_string(rand());
        if (fs::exists(db_path)) {
            fs::remove_all(db_path);
        }
        
        // Initialize Filter
        filter = std::make_unique<Filter>(db_path);
    }

    void TearDown() override {
        // Clean up
        filter.reset(); // Close DB environment first
        if (fs::exists(db_path)) {
            fs::remove_all(db_path);
        }
    }
};

TEST_F(FilterTest, CategoryFilterBasics) {
    // Add simple category filters
    // ID 1: City=Paris
    // ID 2: City=London
    // ID 3: City=Paris
    
    expect_ok(filter->add_to_filter("city", "Paris", 1));
    expect_ok(filter->add_to_filter("city", "London", 2));
    expect_ok(filter->add_to_filter("city", "Paris", 3));

    // Query for City=Paris
    json query = json::array({
        {{"city", {{"$eq", "Paris"}}}}
    });

    std::vector<ndd::idInt> ids = unwrap_ok(filter->getIdsMatchingFilter(query));
    
    // Should find 1 and 3
    EXPECT_EQ(ids.size(), 2);
    EXPECT_NE(std::find(ids.begin(), ids.end(), 1), ids.end());
    EXPECT_NE(std::find(ids.begin(), ids.end(), 3), ids.end());
    EXPECT_EQ(std::find(ids.begin(), ids.end(), 2), ids.end());
}

class CategoryIndexCorruptionTest : public ::testing::Test {
protected:
    std::string db_path;
    MDBX_env* env = nullptr;
    std::unique_ptr<ndd::filter::CategoryIndex> category_index;

    void SetUp() override {
        db_path = "./category_corrupt_db_" + std::to_string(rand());
        if(fs::exists(db_path)) {
            fs::remove_all(db_path);
        }
        fs::create_directories(db_path);

        int rc = mdbx_env_create(&env);
        ASSERT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);

        rc = mdbx_env_set_maxdbs(env, 10);
        ASSERT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);

        rc = mdbx_env_set_geometry(env,
                                   -1,
                                   1ULL << settings::FILTER_MAP_SIZE_BITS,
                                   1ULL << settings::FILTER_MAP_SIZE_MAX_BITS,
                                   1ULL << settings::FILTER_MAP_SIZE_BITS,
                                   -1,
                                   -1);
        ASSERT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);

        rc = mdbx_env_open(env,
                           db_path.c_str(),
                           MDBX_WRITEMAP | MDBX_MAPASYNC | MDBX_NORDAHEAD,
                           0664);
        ASSERT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);

        category_index = std::make_unique<ndd::filter::CategoryIndex>(env);
    }

    void TearDown() override {
        category_index.reset();
        if(env != nullptr) {
            mdbx_env_close(env);
            env = nullptr;
        }
        if(fs::exists(db_path)) {
            fs::remove_all(db_path);
        }
    }

    void put_raw_payload(const std::string& key_string, std::vector<char>& payload) {
        MDBX_txn* txn = nullptr;
        int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
        ASSERT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);

        MDBX_val key{const_cast<char*>(key_string.data()), key_string.size()};
        MDBX_val data{payload.data(), payload.size()};
        rc = mdbx_put(txn, category_index->get_dbi(), &key, &data, MDBX_UPSERT);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            ASSERT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);
        }

        rc = mdbx_txn_commit(txn);
        ASSERT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);
    }
};

TEST_F(CategoryIndexCorruptionTest, RejectsTruncatedBitmapPayload) {
    ndd::RoaringBitmap bitmap;
    bitmap.add(1);
    bitmap.add(3);

    std::vector<char> payload(bitmap.getSizeInBytes());
    bitmap.write(payload.data(), true);
    ASSERT_GT(payload.size(), 1u);
    payload.pop_back();

    put_raw_payload(ndd::filter::CategoryIndex::make_key("city", "Paris"), payload);

    auto result = category_index->get_bitmap("city", "Paris");
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.code, 200u);
}

TEST_F(CategoryIndexCorruptionTest, ReadsValidRawBitmapPayload) {
    ndd::RoaringBitmap bitmap;
    bitmap.add(11);
    bitmap.add(29);
    bitmap.runOptimize();

    std::vector<char> payload(bitmap.getSizeInBytes());
    bitmap.write(payload.data(), true);

    put_raw_payload(ndd::filter::CategoryIndex::make_key("city", "Berlin"), payload);

    auto result = category_index->get_bitmap("city", "Berlin");
    ASSERT_TRUE(result.ok()) << result.message;
    ASSERT_TRUE(result.value.has_value());
    EXPECT_TRUE(result.value->contains(11));
    EXPECT_TRUE(result.value->contains(29));
    EXPECT_FALSE(result.value->contains(30));
}

TEST_F(CategoryIndexCorruptionTest, RejectsGarbageBitmapPayload) {
    std::vector<char> payload{0, 0, 0, 0, 1, 2, 3, 4};

    put_raw_payload(ndd::filter::CategoryIndex::make_key("city", "Rome"), payload);

    auto result = category_index->get_bitmap("city", "Rome");
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.code, 200u);
    EXPECT_NE(result.message.find("invalid or truncated bitmap payload"),
              std::string::npos);
}

TEST_F(CategoryIndexCorruptionTest, RejectsTrailingBytesAfterBitmapPayload) {
    ndd::RoaringBitmap bitmap;
    bitmap.add(5);

    std::vector<char> payload(bitmap.getSizeInBytes());
    bitmap.write(payload.data(), true);
    payload.push_back('\0');

    put_raw_payload(ndd::filter::CategoryIndex::make_key("city", "London"), payload);

    auto result = category_index->get_bitmap("city", "London");
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.code, 200u);
}

TEST_F(FilterTest, BooleanFilterBasics) {
    // Boolean is just a special category "0" or "1"
    // ID 10: Active=true
    // ID 11: Active=false
    
    // Using JSON add interface for variety
    expect_ok(filter->add_filters_from_json(10, R"({"is_active": true})"));
    expect_ok(filter->add_filters_from_json(11, R"({"is_active": false})"));

    // Query Active=true
    json query_true = json::array({
        {{"is_active", {{"$eq", true}}}}
    });
    
    auto ids_true = unwrap_ok(filter->getIdsMatchingFilter(query_true));
    EXPECT_EQ(ids_true.size(), 1);
    EXPECT_EQ(ids_true[0], 10);

    // Query Active=false
    json query_false = json::array({
        {{"is_active", {{"$eq", false}}}}
    });
    
    auto ids_false = unwrap_ok(filter->getIdsMatchingFilter(query_false));
    EXPECT_EQ(ids_false.size(), 1);
    EXPECT_EQ(ids_false[0], 11);
}

TEST_F(FilterTest, NumericFilterBasics) {
    // ID 100: Age=25
    // ID 101: Age=30
    // ID 102: Age=35
    
    expect_ok(filter->add_filters_from_json(100, R"({"age": 25})"));
    expect_ok(filter->add_filters_from_json(101, R"({"age": 30})"));
    expect_ok(filter->add_filters_from_json(102, R"({"age": 35})"));

    // Range Query: 20 <= Age <= 32
    json query_range = json::array({
        {{"age", {{"$range", {20, 32}}}}}
    });

    auto ids = unwrap_ok(filter->getIdsMatchingFilter(query_range));
    
    // Should match 100 (25) and 101 (30)
    EXPECT_EQ(ids.size(), 2);
    bool found100 = false, found101 = false;
    for(auto id : ids) {
        if(id == 100) found100 = true;
        if(id == 101) found101 = true;
    }
    EXPECT_TRUE(found100);
    EXPECT_TRUE(found101);
}

TEST_F(FilterTest, FloatNumericFilter) {
    // ID 1: Price=10.5
    // ID 2: Price=20.0
    
    expect_ok(filter->add_filters_from_json(1, R"({"price": 10.5})"));
    expect_ok(filter->add_filters_from_json(2, R"({"price": 20.0})"));

    json query = json::array({
        {{"price", {{"$range", {10.0, 15.0}}}}}
    });

    auto ids = unwrap_ok(filter->getIdsMatchingFilter(query));
    EXPECT_EQ(ids.size(), 1);
    EXPECT_EQ(ids[0], 1);
}

TEST_F(FilterTest, MixedAndLogic) {
    // ID 1: City=NY, Age=30 (Match)
    // ID 2: City=NY, Age=40 (Age fail)
    // ID 3: City=LA, Age=30 (City fail)
    
    expect_ok(filter->add_filters_from_json(1, R"({"city": "NY", "age": 30})"));
    expect_ok(filter->add_filters_from_json(2, R"({"city": "NY", "age": 40})"));
    expect_ok(filter->add_filters_from_json(3, R"({"city": "LA", "age": 30})"));

    // Filter: City=NY AND Age < 35
    json query = json::array({
        {{"city", {{"$eq", "NY"}}}},
        {{"age", {{"$range", {0, 35}}}}}
    });

    auto ids = unwrap_ok(filter->getIdsMatchingFilter(query));
    EXPECT_EQ(ids.size(), 1);
    EXPECT_EQ(ids[0], 1);
}

TEST_F(FilterTest, InOperator) {
    // ID 1: Color=Red
    // ID 2: Color=Blue
    // ID 3: Color=Green
    
    expect_ok(filter->add_to_filter("color", "Red", 1));
    expect_ok(filter->add_to_filter("color", "Blue", 2));
    expect_ok(filter->add_to_filter("color", "Green", 3));

    // Query: Color IN [Red, Green]
    json query = json::array({
        {{"color", {{"$in", {"Red", "Green"}}}}}
    });

    auto ids = unwrap_ok(filter->getIdsMatchingFilter(query));
    EXPECT_EQ(ids.size(), 2); // 1 and 3
}

TEST_F(FilterTest, DeleteFilter) {
    // ID 1: Tag=A
    expect_ok(filter->add_to_filter("tag", "A", 1));
    
    json query = json::array({
        {{"tag", {{"$eq", "A"}}}}
    });
    
    EXPECT_EQ(unwrap_ok(filter->countIdsMatchingFilter(query)), 1);
    
    // Remove functionality test
    // Usually removal requires us to know what to remove or we remove entire ID?
    // The Filter class has: remove_from_filter(field, value, id)
    
    expect_ok(filter->remove_from_filter("tag", "A", 1));
    
    EXPECT_EQ(unwrap_ok(filter->countIdsMatchingFilter(query)), 0);
}

TEST_F(FilterTest, NumericDelete) {
    // ID 1: Score=100
    expect_ok(filter->add_filters_from_json(1, R"({"score": 100})"));
    
    // Check it exists
    json query = json::array({
        {{"score", {{"$eq", 100}}}}
    });
    EXPECT_EQ(unwrap_ok(filter->countIdsMatchingFilter(query)), 1);
    
    // Remove
    // remove_filters_from_json uses the whole object
    expect_ok(filter->remove_filters_from_json(1, R"({"score": 100})"));
    
    EXPECT_EQ(unwrap_ok(filter->countIdsMatchingFilter(query)), 0);
}

TEST_F(FilterTest, RejectsMalformedFilterJson) {
    auto result = filter->add_filters_from_json(1, R"({"city": "Paris")");

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.code, 1);
}

TEST_F(FilterTest, RejectsUnsupportedFilterType) {
    auto result = filter->add_filters_from_json(1, R"({"tags": ["a", "b"]})");

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.code, 2);
}

TEST_F(FilterTest, RejectsSchemaTypeMismatch) {
    expect_ok(filter->add_filters_from_json(1, R"({"age": 30})"));

    auto result = filter->add_filters_from_json(2, R"({"age": "thirty"})");

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.code, 3);
}

TEST_F(FilterTest, RejectsInvalidOperator) {
    json query = json::array({
        {{"city", {{"$contains", "Paris"}}}}
    });

    auto result = filter->getIdsMatchingFilter(query);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.code, 2);
}

TEST_F(FilterTest, RejectsInvalidRange) {
    expect_ok(filter->add_filters_from_json(1, R"({"score": 100})"));
    json query = json::array({
        {{"score", {{"$range", {200, 100}}}}}
    });

    auto result = filter->getIdsMatchingFilter(query);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.code, 2);
}

TEST_F(FilterTest, RejectsColonInFieldNameOnInsert) {
    auto result = filter->add_filters_from_json(1, R"({"user:id": "x"})");

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.code, 1);
}

TEST_F(FilterTest, RejectsColonInValueOnInsert) {
    auto result = filter->add_filters_from_json(1, R"({"city": "Paris:France"})");

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.code, 1);
}

TEST_F(FilterTest, RejectsColonInFieldNameOnQuery) {
    json query = json::array({
        {{"user:id", {{"$eq", "x"}}}}
    });

    auto result = filter->getIdsMatchingFilter(query);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.code, 1);
}

TEST_F(FilterTest, RejectsColonInValueOnQuery) {
    expect_ok(filter->add_filters_from_json(1, R"({"city": "Paris"})"));
    json query = json::array({
        {{"city", {{"$eq", "Paris:France"}}}}
    });

    auto result = filter->getIdsMatchingFilter(query);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.code, 1);
}

TEST_F(FilterTest, RejectsColonInLowLevelAddToFilter) {
    auto field_result = filter->add_to_filter("user:id", "x", 1);
    EXPECT_FALSE(field_result.ok());
    EXPECT_EQ(field_result.code, 1);

    auto value_result = filter->add_to_filter("user", "x:y", 1);
    EXPECT_FALSE(value_result.ok());
    EXPECT_EQ(value_result.code, 1);
}

namespace {

std::vector<ndd::idInt> sorted_ids(std::vector<ndd::idInt> ids) {
    std::sort(ids.begin(), ids.end());
    return ids;
}

}  // namespace

TEST_F(FilterTest, ComparisonOperatorsInteger) {
    expect_ok(filter->add_filters_from_json(1, R"({"age": 25})"));
    expect_ok(filter->add_filters_from_json(2, R"({"age": 30})"));
    expect_ok(filter->add_filters_from_json(3, R"({"age": 35})"));

    auto run = [&](const json& expr) {
        json query = json::array({{{"age", expr}}});
        return sorted_ids(unwrap_ok(filter->getIdsMatchingFilter(query)));
    };

    EXPECT_EQ(run({{"$lt", 30}}),  (std::vector<ndd::idInt>{1}));
    EXPECT_EQ(run({{"$lte", 30}}), (std::vector<ndd::idInt>{1, 2}));
    EXPECT_EQ(run({{"$gt", 30}}),  (std::vector<ndd::idInt>{3}));
    EXPECT_EQ(run({{"$gte", 30}}), (std::vector<ndd::idInt>{2, 3}));
}

TEST_F(FilterTest, ComparisonOperatorsFloat) {
    expect_ok(filter->add_filters_from_json(1, R"({"price": 10.5})"));
    expect_ok(filter->add_filters_from_json(2, R"({"price": 20.0})"));
    expect_ok(filter->add_filters_from_json(3, R"({"price": 20.5})"));

    auto run = [&](const json& expr) {
        json query = json::array({{{"price", expr}}});
        return sorted_ids(unwrap_ok(filter->getIdsMatchingFilter(query)));
    };

    EXPECT_EQ(run({{"$lt", 20.0}}),  (std::vector<ndd::idInt>{1}));
    EXPECT_EQ(run({{"$lte", 20.0}}), (std::vector<ndd::idInt>{1, 2}));
    EXPECT_EQ(run({{"$gt", 20.0}}),  (std::vector<ndd::idInt>{3}));
    EXPECT_EQ(run({{"$gte", 20.0}}), (std::vector<ndd::idInt>{2, 3}));
}

TEST_F(FilterTest, ComparisonOperatorsNegativeAndZero) {
    expect_ok(filter->add_filters_from_json(1, R"({"temp": -5})"));
    expect_ok(filter->add_filters_from_json(2, R"({"temp": 0})"));
    expect_ok(filter->add_filters_from_json(3, R"({"temp": 5})"));

    auto run = [&](const json& expr) {
        json query = json::array({{{"temp", expr}}});
        return sorted_ids(unwrap_ok(filter->getIdsMatchingFilter(query)));
    };

    EXPECT_EQ(run({{"$lt", 0}}),    (std::vector<ndd::idInt>{1}));
    EXPECT_EQ(run({{"$gte", 0}}),   (std::vector<ndd::idInt>{2, 3}));
    EXPECT_EQ(run({{"$lt", -5}}),   (std::vector<ndd::idInt>{}));
    EXPECT_EQ(run({{"$lte", -5}}),  (std::vector<ndd::idInt>{1}));
}

TEST_F(FilterTest, ComparisonAndCombination) {
    expect_ok(filter->add_filters_from_json(1, R"({"city": "NY", "age": 25})"));
    expect_ok(filter->add_filters_from_json(2, R"({"city": "NY", "age": 30})"));
    expect_ok(filter->add_filters_from_json(3, R"({"city": "NY", "age": 35})"));
    expect_ok(filter->add_filters_from_json(4, R"({"city": "LA", "age": 30})"));

    json query = json::array({
        {{"city", {{"$eq", "NY"}}}},
        {{"age",  {{"$gte", 25}}}},
        {{"age",  {{"$lt", 35}}}}
    });

    auto ids = sorted_ids(unwrap_ok(filter->getIdsMatchingFilter(query)));
    EXPECT_EQ(ids, (std::vector<ndd::idInt>{1, 2}));
}

TEST_F(FilterTest, ComparisonInteractionWithIn) {
    expect_ok(filter->add_filters_from_json(1, R"({"score": 1})"));
    expect_ok(filter->add_filters_from_json(2, R"({"score": 5})"));
    expect_ok(filter->add_filters_from_json(3, R"({"score": 10})"));

    json query = json::array({
        {{"score", {{"$in",  {1, 5, 10}}}}},
        {{"score", {{"$gte", 5}}}}
    });

    auto ids = sorted_ids(unwrap_ok(filter->getIdsMatchingFilter(query)));
    EXPECT_EQ(ids, (std::vector<ndd::idInt>{2, 3}));
}

TEST_F(FilterTest, ComparisonInteractionWithRange) {
    for(int i = 10; i <= 50; i += 10) {
        std::string body = R"({"v": )" + std::to_string(i) + "}";
        expect_ok(filter->add_filters_from_json(i, body));
    }

    json query = json::array({
        {{"v", {{"$range", {10, 50}}}}},
        {{"v", {{"$gt", 20}}}},
        {{"v", {{"$lte", 30}}}}
    });

    auto ids = sorted_ids(unwrap_ok(filter->getIdsMatchingFilter(query)));
    EXPECT_EQ(ids, (std::vector<ndd::idInt>{30}));
}

TEST_F(FilterTest, ComparisonRejectsNonNumericValue) {
    expect_ok(filter->add_filters_from_json(1, R"({"age": 25})"));
    json query = json::array({
        {{"age", {{"$gt", "old"}}}}
    });

    auto result = filter->getIdsMatchingFilter(query);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.code, 2);
}

TEST_F(FilterTest, ComparisonRejectsOnNonNumericField) {
    expect_ok(filter->add_filters_from_json(1, R"({"city": "NY"})"));
    json query = json::array({
        {{"city", {{"$gt", 5}}}}
    });

    auto result = filter->getIdsMatchingFilter(query);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.code, 2);
}

TEST_F(FilterTest, ComparisonEmptyRangeAtIntegerBoundary) {
    expect_ok(filter->add_filters_from_json(1, R"({"x": 0})"));

    json q_lt_min = json::array({
        {{"x", {{"$lt", INT_MIN}}}}
    });
    auto r1 = filter->getIdsMatchingFilter(q_lt_min);
    EXPECT_TRUE(r1.ok()) << r1.message;
    EXPECT_EQ(r1.value_or_throw().size(), 0u);

    json q_gt_max = json::array({
        {{"x", {{"$gt", INT_MAX}}}}
    });
    auto r2 = filter->getIdsMatchingFilter(q_gt_max);
    EXPECT_TRUE(r2.ok()) << r2.message;
    EXPECT_EQ(r2.value_or_throw().size(), 0u);
}

// =================================================================
// Hypothesis tests for the dirty numeric_index.hpp range() perf work.
//
// These tests do NOT modify production code; they probe internal
// invariants of Bucket and end-to-end behavior of Filter to confirm
// or refute the claims made about the post_filter_new regression.
//
// Naming convention: HypothesisN_*  where N matches the analysis.
// A failing assertion in these tests means the corresponding
// hypothesis is correct (the claimed unwanted behavior is observable).
// =================================================================

// --- Hypothesis 1 ----------------------------------------------------
// Claim: For VectorDBBench-style packed int data, bucket values are
// densely packed in a much narrower extent than 65536, so the new
// "Coarse full-coverage fast path" predicate
//     bucket_base >= min_val
//     && bucket_base + Bucket::MAX_DELTA <= max_val
// is FALSE on the typical bucket -- even when the OLD post-deserialize
// predicate (bucket_min >= min_val && bucket_max <= max_val) is TRUE.
// Implication: the fast path does not actually fire on the workload
// it was meant to optimize, and we keep paying the deserialize cost.
//
// PASS = both predicates evaluated below match the hypothesis values.
// FAIL = the predicates disagree with the hypothesis (analysis is wrong).
TEST(Hypothesis1, FastPathPredicateMissesPackedBucket) {
    constexpr uint32_t base = 0x80000000u;  // sortable encoding of int 0
    constexpr size_t   N = ndd::filter::Bucket::MAX_SIZE;
    constexpr uint32_t spread = 1023;       // values densely packed in [base, base+1023]

    ndd::filter::Bucket bucket;
    bucket.base_value = base;
    for (size_t i = 0; i < N; ++i) {
        bucket.add(base + static_cast<uint32_t>(i % (spread + 1)),
                   static_cast<ndd::idInt>(i + 1));
    }
    ASSERT_EQ(bucket.ids.size(), N);

    const uint32_t bucket_min = bucket.get_value(0);
    const uint32_t bucket_max = bucket.get_value(bucket.ids.size() - 1);
    EXPECT_EQ(bucket_min, base);
    EXPECT_EQ(bucket_max, base + spread);

    // Query covers exactly the bucket's actual extent.
    const uint32_t min_val = bucket_min;
    const uint32_t max_val = bucket_max;

    const bool old_full_overlap = bucket_min >= min_val && bucket_max <= max_val;
    const bool new_fast_path =
        bucket.base_value >= min_val
        && static_cast<uint64_t>(bucket.base_value)
                   + ndd::filter::Bucket::MAX_DELTA <= max_val;

    EXPECT_TRUE(old_full_overlap)
        << "OLD code's full-overlap branch would fire on this bucket";
    EXPECT_FALSE(new_fast_path)
        << "NEW fast path requires the entire 65536-wide extent to fit "
           "inside [min,max], so it MISSES on packed buckets";
}

// Counter-test: when bucket values DO span the full delta range and
// the query is wide enough, the new fast path predicate is TRUE.
TEST(Hypothesis1, FastPathFiresOnWidelySpreadBucket) {
    constexpr uint32_t base = 100'000;
    ndd::filter::Bucket bucket;
    bucket.base_value = base;
    for (size_t i = 0; i < 1024; ++i) {
        const uint32_t val = base
            + static_cast<uint32_t>(
                  (i * static_cast<uint64_t>(ndd::filter::Bucket::MAX_DELTA))
                  / 1023);
        bucket.add(val, static_cast<ndd::idInt>(i + 1));
    }

    const uint32_t min_val = base;
    const uint32_t max_val = base + ndd::filter::Bucket::MAX_DELTA;
    const bool new_fast_path =
        bucket.base_value >= min_val
        && static_cast<uint64_t>(bucket.base_value)
                   + ndd::filter::Bucket::MAX_DELTA <= max_val;
    EXPECT_TRUE(new_fast_path);
}

// --- Hypothesis 2 ----------------------------------------------------
// Claim: After bucket saturation with duplicates (the dirty Bucket::add
// caps deltas/ids at MAX_SIZE for delta_32 == 0 inserts but keeps
// adding to summary_bitmap), the bucket has cardinality > ids.size,
// and the new bitmap-only-inclusion branch in range() returns ids
// that the OLD code would never have surfaced.
TEST(Hypothesis2, SaturationCreatesBitmapOnlyEntries) {
    GTEST_SKIP() << "Part 2 alarm: Bucket::add saturated-duplicate routing "
                    "to summary_bitmap is introduced by 546430d. See "
                    "docs/filter_part2_followups.md item 1. Remove this "
                    "GTEST_SKIP when Part 2 lands.";

    constexpr uint32_t base = 0;
    constexpr ndd::idInt N_TOTAL = ndd::filter::Bucket::MAX_SIZE + 500;

    ndd::filter::Bucket bucket;
    bucket.base_value = base;
    for (ndd::idInt i = 1; i <= N_TOTAL; ++i) {
        bucket.add(base, i);  // all duplicates of base_value
    }

    EXPECT_EQ(bucket.ids.size(), ndd::filter::Bucket::MAX_SIZE);
    EXPECT_EQ(bucket.summary_bitmap.cardinality(), N_TOTAL);
    EXPECT_GT(bucket.summary_bitmap.cardinality(), bucket.ids.size())
        << "bitmap-only branch in range() will fire iff cardinality > ids.size";
}

// End-to-end check through the Filter API: when we insert MAX_SIZE+K
// rows that all share a numeric value, an $eq query should return all
// MAX_SIZE+K ids. If saturation drops K of them, this test fails -- but
// then the recall bump observed in the chart cannot be explained by
// this branch and we should look elsewhere.
TEST_F(FilterTest, Hypothesis2_RangeReturnsAllSaturatedDuplicates) {
    constexpr int VALUE = 42;
    constexpr ndd::idInt EXTRA = 500;
    constexpr ndd::idInt N = ndd::filter::Bucket::MAX_SIZE + EXTRA;

    const std::string filter_payload =
        std::string(R"({"score": )") + std::to_string(VALUE) + "}";
    for (ndd::idInt i = 1; i <= N; ++i) {
        expect_ok(filter->add_filters_from_json(i, filter_payload));
    }

    json query = json::array({{ {"score", {{"$eq", VALUE}}} }});
    auto ids = unwrap_ok(filter->getIdsMatchingFilter(query));
    EXPECT_EQ(ids.size(), N)
        << "If saturation logic is dropping ids, recall would actually go DOWN, "
           "not up, contradicting the chart.";
}

// --- Hypothesis 3 ----------------------------------------------------
// Claim: When a slide-split fires on a saturated bucket, the LEFT
// bucket's summary_bitmap is rebuilt from `ids` only (see
// add_to_buckets at numeric_index.hpp:614-617):
//     bucket.summary_bitmap = ndd::RoaringBitmap();
//     for (auto bucket_id : bucket.ids) bucket.summary_bitmap.add(bucket_id);
// Any bitmap-only entries (excess saturated duplicates) that lived on
// the LEFT side of the split are silently dropped.
//
// We reproduce the rebuild step inline because the slide-split lives
// inside NumericIndex::add_to_buckets (a private path with no test
// hook). If H3 holds, the data loss is observable on the local Bucket.
TEST(Hypothesis3, SlideSplitRebuildLosesBitmapOnlyEntries) {
    constexpr uint32_t base = 0;
    ndd::filter::Bucket bucket;
    bucket.base_value = base;

    // Fill with MAX_SIZE unique-delta entries so a real split is possible.
    for (uint32_t v = 0; v < ndd::filter::Bucket::MAX_SIZE; ++v) {
        bucket.add(v, static_cast<ndd::idInt>(v + 1));
    }
    ASSERT_EQ(bucket.ids.size(), ndd::filter::Bucket::MAX_SIZE);

    // Simulate the saturated-duplicate path: bitmap gains an id but
    // ids/deltas do not (because Bucket::add returns early for
    // delta_32 == 0 once ids.size() >= MAX_SIZE).
    constexpr ndd::idInt BITMAP_ONLY_ID_A = 100'000;
    constexpr ndd::idInt BITMAP_ONLY_ID_B = 100'001;
    bucket.summary_bitmap.add(BITMAP_ONLY_ID_A);
    bucket.summary_bitmap.add(BITMAP_ONLY_ID_B);
    ASSERT_EQ(bucket.summary_bitmap.cardinality(), bucket.ids.size() + 2);

    // Reproduce the slide-split LEFT-side rebuild.
    const size_t mid_idx = bucket.ids.size() / 2;
    bucket.deltas.resize(mid_idx);
    bucket.ids.resize(mid_idx);
    bucket.summary_bitmap = ndd::RoaringBitmap();
    for (auto id : bucket.ids) {
        bucket.summary_bitmap.add(id);
    }

    EXPECT_FALSE(bucket.summary_bitmap.contains(BITMAP_ONLY_ID_A));
    EXPECT_FALSE(bucket.summary_bitmap.contains(BITMAP_ONLY_ID_B));
    EXPECT_EQ(bucket.summary_bitmap.cardinality(), bucket.ids.size());
}

// --- Hypothesis 4 ----------------------------------------------------
// Claim: accepting the OLD on-disk format (legacy uint16_t count
// between bitmap and arrays) recovers cliff-corrupted bitmap ids and
// can grow the range result candidate set. The production reader now
// rejects that payload shape instead of trying to salvage it.
TEST(Hypothesis4, DeserializeRejectsLegacyCountFormat) {
    GTEST_SKIP() << "Part 2 alarm: legacy count-bearing layout is still "
                    "the on-disk format in Part 1, so Bucket::deserialize "
                    "accepts it. Part 2 commit 546430d drops the count "
                    "field; the residual-bytes-not-aligned check then "
                    "rejects the legacy shape. See "
                    "docs/filter_part2_followups.md item 1. Remove this "
                    "GTEST_SKIP when Part 2 lands.";

    // Manually craft an OLD-format payload:
    //   [u32 bm_size] [bitmap bytes] [u16 count=0]
    // i.e. cliff-truncated count, but bitmap retained the lost ids.

    constexpr ndd::idInt LOST_ID_A = 7;
    constexpr ndd::idInt LOST_ID_B = 9;
    ndd::RoaringBitmap original;
    original.add(LOST_ID_A);
    original.add(LOST_ID_B);
    original.runOptimize();

    const size_t bm_size = original.getSizeInBytes();
    std::vector<uint8_t> buffer(sizeof(uint32_t) + bm_size + sizeof(uint16_t), 0);
    uint8_t* ptr = buffer.data();

    const uint32_t bm_size_32 = static_cast<uint32_t>(bm_size);
    std::memcpy(ptr, &bm_size_32, sizeof(uint32_t));
    ptr += sizeof(uint32_t);
    original.write(reinterpret_cast<char*>(ptr));
    ptr += bm_size;
    const uint16_t legacy_count = 0;
    std::memcpy(ptr, &legacy_count, sizeof(uint16_t));

    EXPECT_THROW(
        (void)ndd::filter::Bucket::deserialize(
                buffer.data(), buffer.size(), /*base_val=*/100),
        std::runtime_error);
}

// Companion check on the read_summary_bitmap fast-path helper: it must
// reject the same legacy-format payloads as the full deserializer, so
// the fast path cannot silently reintroduce compatibility.
TEST(Hypothesis4, ReadSummaryBitmapRejectsLegacyCountFormat) {
    GTEST_SKIP() << "Part 2 alarm: read_summary_bitmap intentionally "
                    "ignores the count-bearing trailer in Part 1 (see "
                    "the comment block on read_summary_bitmap in "
                    "numeric_index.hpp). Part 2 commit 546430d drops the "
                    "count field and the alignment check then catches "
                    "the legacy shape. See docs/filter_part2_followups.md "
                    "item 1 and carry 2. Remove this GTEST_SKIP when "
                    "Part 2 lands.";

    ndd::RoaringBitmap original;
    for (ndd::idInt i = 0; i < 50; ++i) original.add(i * 3);
    original.runOptimize();

    const size_t bm_size = original.getSizeInBytes();
    std::vector<uint8_t> buffer(sizeof(uint32_t) + bm_size + sizeof(uint16_t), 0);
    uint8_t* ptr = buffer.data();
    const uint32_t bm_size_32 = static_cast<uint32_t>(bm_size);
    std::memcpy(ptr, &bm_size_32, sizeof(uint32_t));
    ptr += sizeof(uint32_t);
    original.write(reinterpret_cast<char*>(ptr));
    ptr += bm_size;
    const uint16_t legacy_count = 0;
    std::memcpy(ptr, &legacy_count, sizeof(uint16_t));

    EXPECT_THROW(
        (void)ndd::filter::Bucket::deserialize(buffer.data(), buffer.size(), 0),
        std::runtime_error);
    EXPECT_THROW(
        (void)ndd::filter::Bucket::read_summary_bitmap(
                buffer.data(), buffer.size()),
        std::runtime_error);
}

TEST(NumericBucketCorruptionTest, RejectsExtraBytesInsideDeclaredBitmapPayload) {
    ndd::RoaringBitmap original;
    for(ndd::idInt i = 0; i < 50; ++i) {
        original.add(i * 5);
    }
    original.runOptimize();

    const size_t bm_size = original.getSizeInBytes();
    const uint32_t declared_bm_size = static_cast<uint32_t>(bm_size + 1);
    std::vector<uint8_t> buffer(sizeof(uint32_t) + declared_bm_size, 0);
    uint8_t* ptr = buffer.data();

    std::memcpy(ptr, &declared_bm_size, sizeof(uint32_t));
    ptr += sizeof(uint32_t);
    original.write(reinterpret_cast<char*>(ptr));

    EXPECT_THROW(
        (void)ndd::filter::Bucket::deserialize(buffer.data(), buffer.size(), 0),
        std::runtime_error);
    EXPECT_THROW(
        (void)ndd::filter::Bucket::read_summary_bitmap(
                buffer.data(), buffer.size()),
        std::runtime_error);
}

TEST(NumericBucketCorruptionTest, ReadBitmapPayloadReturnsOperationResultOnSuccess) {
    ndd::RoaringBitmap original;
    original.add(101);
    original.add(202);
    original.runOptimize();

    std::vector<uint8_t> payload(original.getSizeInBytes());
    original.write(reinterpret_cast<char*>(payload.data()));

    auto result = ndd::filter::Bucket::read_bitmap_payload(payload.data(),
                                                           payload.size());

    ASSERT_TRUE(result.ok()) << result.message;
    ASSERT_TRUE(result.value.has_value());
    EXPECT_TRUE(result.value->contains(101));
    EXPECT_TRUE(result.value->contains(202));
    EXPECT_FALSE(result.value->contains(303));
}

TEST(NumericBucketCorruptionTest, ReadBitmapPayloadRejectsGarbageWithoutThrowing) {
    std::vector<uint8_t> payload{0, 0, 0, 0, 7, 8, 9, 10};

    auto result = ndd::filter::Bucket::read_bitmap_payload(payload.data(),
                                                           payload.size());

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.code, 200u);
    EXPECT_FALSE(result.value.has_value());
    EXPECT_NE(result.message.find("invalid or truncated bitmap payload"),
              std::string::npos);
}

TEST(NumericBucketCorruptionTest, DeserializesValidBucketAfterPayloadValidation) {
    ndd::filter::Bucket bucket;
    bucket.base_value = 1000;
    bucket.add(1000, 42);
    bucket.add(1007, 43);

    auto bytes = bucket.serialize();
    auto decoded = ndd::filter::Bucket::deserialize(bytes.data(),
                                                    bytes.size(),
                                                    bucket.base_value);
    auto bitmap_only = ndd::filter::Bucket::read_summary_bitmap(bytes.data(),
                                                                bytes.size());

    EXPECT_EQ(decoded.base_value, bucket.base_value);
    EXPECT_EQ(decoded.ids.size(), 2u);
    EXPECT_TRUE(decoded.summary_bitmap.contains(42));
    EXPECT_TRUE(decoded.summary_bitmap.contains(43));
    EXPECT_TRUE(bitmap_only.contains(42));
    EXPECT_TRUE(bitmap_only.contains(43));
}

TEST(NumericBucketCorruptionTest, RejectsGarbageInsideDeclaredBitmapPayload) {
    const uint32_t declared_bm_size = 8;
    std::vector<uint8_t> buffer(sizeof(uint32_t) + declared_bm_size, 0);
    std::memcpy(buffer.data(), &declared_bm_size, sizeof(uint32_t));

    EXPECT_THROW(
        (void)ndd::filter::Bucket::deserialize(buffer.data(), buffer.size(), 0),
        std::runtime_error);
    EXPECT_THROW(
        (void)ndd::filter::Bucket::read_summary_bitmap(
                buffer.data(), buffer.size()),
        std::runtime_error);
}

// End-to-end recall check through the Filter API: insert N records
// with a wide spread of numeric values, run a wide range query, and
// compare the returned id set against a brute-force enumeration of
// the same JSON payload. If H4 is the regression cause, the chart's
// recall bump corresponds to results that match brute force more
// closely on the dirty branch -- but on a freshly built DB (no cliff
// state) this test must pass exactly. Mismatch here would mean the
// dirty range() over-includes even on clean data, which would shift
// the diagnosis.
TEST_F(FilterTest, Hypothesis4_RangeMatchesBruteForceOnCleanDb) {
    constexpr ndd::idInt N = 5000;
    // Spread values across more than one bucket extent (MAX_DELTA = 65535)
    // so we exercise both the fast path and the per-bucket scan.
    auto value_for = [](ndd::idInt i) -> int {
        return static_cast<int>((i * 37) % 200000);
    };

    for (ndd::idInt i = 1; i <= N; ++i) {
        const std::string payload =
            std::string(R"({"score": )") + std::to_string(value_for(i)) + "}";
        expect_ok(filter->add_filters_from_json(i, payload));
    }

    constexpr int LO = 50000;
    constexpr int HI = 120000;
    json query = json::array({
        {{"score", {{"$range", json::array({LO, HI})}}}}
    });
    auto got = unwrap_ok(filter->getIdsMatchingFilter(query));
    std::sort(got.begin(), got.end());

    std::vector<ndd::idInt> expected;
    for (ndd::idInt i = 1; i <= N; ++i) {
        const int v = value_for(i);
        if (v >= LO && v <= HI) expected.push_back(i);
    }
    std::sort(expected.begin(), expected.end());

    EXPECT_EQ(got, expected);
}

// =====================================================================
// NumericRangeBench: targeted microbench against an EXISTING filter MDBX
// directory. Runs Filter::computeFilterBitmap (which calls
// NumericIndex::range) repeatedly for a few canned filter_rates and
// prints per-call wall time. Compare two builds (dirty vs stashed)
// against the SAME db path, with no concurrency, no HNSW, no HTTP.
//
// Activation: set ENDEE_BENCH_DB to a directory containing mdbx.dat.
// Optional: ENDEE_BENCH_FIELD (default "id"), ENDEE_BENCH_ITERS (default 200).
//
// Caveat: Filter::init_environment opens the env with MDBX_WRITEMAP, so
// no other process may hold the DB while the bench runs (stop the
// endee server first). The bench itself only issues read queries.
// =====================================================================
namespace {
struct BenchPoint {
    const char* label;
    int lo;
    int hi;
};

void run_bench_point(Filter& filter,
                     const std::string& field,
                     const BenchPoint& pt,
                     int iters) {
    json query = json::array({
        {{field, {{"$range", json::array({pt.lo, pt.hi})}}}}
    });

    // Warmup -- prime page cache, schema cache, allocator state.
    for (int i = 0; i < 3; ++i) {
        auto r = filter.computeFilterBitmap(query);
        ASSERT_TRUE(r.ok()) << r.message;
    }

    size_t result_card = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        auto r = filter.computeFilterBitmap(query);
        ASSERT_TRUE(r.ok()) << r.message;
        result_card = r.value_or_throw().cardinality();
    }
    auto t1 = std::chrono::steady_clock::now();

    const double total_ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double per_call_ms = total_ms / iters;

    std::printf("  %-12s [% 8d,% 8d]  iters=%d  per_call=%.3f ms  card=%zu\n",
                pt.label, pt.lo, pt.hi, iters, per_call_ms, result_card);
}
}  // namespace

// Dumps internal structure of the bitmap that range() returns. Compare
// the output between clean and dirty builds to see whether the dirty
// path is producing a structurally different (and possibly slower to
// query) bitmap. Also bench-times a tight contains() loop on that
// bitmap to mirror what BitMapFilterFunctor does inside HNSW search.
TEST(NumericRangeBench, BitmapStructureAndContainsCost) {
    const char* db_path = std::getenv("ENDEE_BENCH_DB");
    if (!db_path || !*db_path) GTEST_SKIP() << "Set ENDEE_BENCH_DB";
    const char* field_env = std::getenv("ENDEE_BENCH_FIELD");
    const std::string field = (field_env && *field_env) ? field_env : "id";

    Filter filter(db_path);

    struct Point { const char* label; long long lo; long long hi; };
    const Point points[] = {
        {"rate~0.99", 0, 9'900'000},
        {"rate~0.80", 0, 8'000'000},
        {"rate~0.50", 0, 5'000'000},
        {"rate~0.01", 0,   100'000},
    };

    for (const auto& p : points) {
        json q = json::array({{ {field, {{"$range", json::array({p.lo, p.hi})}}} }});
        auto r = filter.computeFilterBitmap(q);
        ASSERT_TRUE(r.ok()) << r.message;
        auto& bm = r.value_or_throw();
        const uint64_t card = bm.cardinality();

        // Force-serialize to see the structural cost of the bitmap.
        // The size after runOptimize is the most honest "structural cost"
        // because OLD writes always runOptimize before persisting.
        bm.runOptimize();
        const size_t opt_bytes = bm.getSizeInBytes();
        // Probe contains() cost on a fixed, stride-based set of ids inside
        // the range. 1M lookups -- about the same order as HNSW filtered
        // search visit count at moderate ef.
        constexpr int N_PROBES = 1'000'000;
        const long long stride = std::max<long long>(1, (p.hi - p.lo) / N_PROBES);
        volatile uint64_t sink = 0;
        auto t0 = std::chrono::steady_clock::now();
        for (long long v = p.lo; v < p.hi && v < p.lo + (long long)N_PROBES * stride; v += stride) {
            sink += bm.contains(static_cast<uint32_t>(v)) ? 1 : 0;
        }
        auto t1 = std::chrono::steady_clock::now();
        const double total_us =
            std::chrono::duration<double, std::micro>(t1 - t0).count();
        const long long probes_done = (p.hi - p.lo) / stride;

        std::printf("  %-10s card=%llu  bytes_after_runOpt=%zu  "
                    "contains(%lld probes)=%.1f us  (%.1f ns/probe, hits=%llu)\n",
                    p.label, (unsigned long long)card, opt_bytes,
                    probes_done, total_us,
                    total_us * 1000.0 / std::max<long long>(1, probes_done),
                    (unsigned long long)sink);
    }
}

TEST(NumericRangeBench, ProbeValueDistribution) {
    const char* db_path = std::getenv("ENDEE_BENCH_DB");
    if (!db_path || !*db_path) GTEST_SKIP() << "Set ENDEE_BENCH_DB";
    const char* field_env = std::getenv("ENDEE_BENCH_FIELD");
    const std::string field = (field_env && *field_env) ? field_env : "id";
    Filter f(db_path);
    auto probe = [&](long long lo, long long hi) {
        json q = json::array({{ {field, {{"$range", json::array({lo, hi})}}} }});
        auto r = f.computeFilterBitmap(q);
        ASSERT_TRUE(r.ok()) << r.message;
        std::printf("  range[% 12lld, % 12lld]  card=%llu\n",
                    lo, hi, (unsigned long long)r.value_or_throw().cardinality());
    };
    probe(-2147483647LL, 2147483647LL);
    probe(0, 10000000);
    probe(0, 5000000);
    probe(2500000, 7500000);
    probe(-32768, 32767);
    probe(0, 100000);
}

TEST(NumericRangeBench, RangeQueryWallClock) {
    const char* db_path = std::getenv("ENDEE_BENCH_DB");
    if (!db_path || !*db_path) {
        GTEST_SKIP() << "Set ENDEE_BENCH_DB to a filter directory to run";
    }
    const char* field_env = std::getenv("ENDEE_BENCH_FIELD");
    const std::string field = (field_env && *field_env) ? field_env : "id";
    const char* iters_env = std::getenv("ENDEE_BENCH_ITERS");
    const int iters = (iters_env && *iters_env) ? std::atoi(iters_env) : 200;
    ASSERT_GT(iters, 0);

    std::printf("NumericRangeBench: db=%s  field=%s  iters=%d\n",
                db_path, field.c_str(), iters);

    Filter filter(db_path);

    // Chart-aligned filter_rate buckets. The benchmark DB has uint32
    // values in [0, 10_000_000] with exactly one id per value (probed
    // via ProbeValueDistribution), so filter_rate ~= (hi - lo) / 1e7.
    const BenchPoint points[] = {
        {"rate~0.99", 0, 9'900'000},
        {"rate~0.80", 0, 8'000'000},
        {"rate~0.50", 0, 5'000'000},
        {"rate~0.01", 0,   100'000},
    };

    for (const auto& pt : points) {
        run_bench_point(filter, field, pt, iters);
    }
}

// =====================================================================
// NumericRangeBench_MT: same as above, but with N threads hammering
// range() concurrently against ONE shared Filter. Each thread issues
// computeFilterBitmap in a tight loop for a fixed wall-clock window.
//
// What this tells us: if dirty range() regresses here vs the clean
// build but the single-threaded NumericRangeBench above does not,
// the cost is concurrency-related (heap / allocator / cache-line
// contention triggered by the dirty in-memory layout), not per-call
// algorithmic.
//
// Activation: same env vars as the single-threaded bench, plus:
//   ENDEE_BENCH_THREADS  (default 16)
//   ENDEE_BENCH_SECONDS  (default 8)
// =====================================================================
namespace {
struct MtResult {
    uint64_t total_ops = 0;
    uint64_t result_card_sample = 0;
};

void run_bench_point_mt(Filter& filter,
                        const std::string& field,
                        const BenchPoint& pt,
                        int threads,
                        double seconds) {
    json query = json::array({
        {{field, {{"$range", json::array({pt.lo, pt.hi})}}}}
    });

    // Warmup serially -- prime page cache + schema cache.
    for (int i = 0; i < 3; ++i) {
        auto r = filter.computeFilterBitmap(query);
        ASSERT_TRUE(r.ok()) << r.message;
    }

    std::atomic<bool> start{false};
    std::atomic<bool> stop{false};
    std::vector<MtResult> per_thread(threads);

    auto worker = [&](int tid) {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        uint64_t ops = 0;
        uint64_t card_sample = 0;
        while (!stop.load(std::memory_order_acquire)) {
            auto r = filter.computeFilterBitmap(query);
            if (!r.ok()) {
                std::fprintf(stderr, "thread %d: %s\n", tid, r.message.c_str());
                return;
            }
            if ((ops & 0xFFF) == 0) {
                card_sample = r.value_or_throw().cardinality();
            }
            ++ops;
        }
        per_thread[tid].total_ops = ops;
        per_thread[tid].result_card_sample = card_sample;
    };

    std::vector<std::thread> ts;
    ts.reserve(threads);
    for (int i = 0; i < threads; ++i) ts.emplace_back(worker, i);

    auto t0 = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);

    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    stop.store(true, std::memory_order_release);

    for (auto& t : ts) t.join();
    auto t1 = std::chrono::steady_clock::now();

    uint64_t total_ops = 0;
    uint64_t card = 0;
    for (const auto& r : per_thread) {
        total_ops += r.total_ops;
        if (r.result_card_sample) card = r.result_card_sample;
    }
    const double elapsed_s =
        std::chrono::duration<double>(t1 - t0).count();
    const double qps = total_ops / elapsed_s;
    const double per_call_ms = (elapsed_s * 1000.0 * threads) / total_ops;

    std::printf("  %-12s [% 8d,% 8d]  threads=%d  ops=%llu  qps=%.1f  "
                "per_call_avg=%.3f ms  card=%llu\n",
                pt.label, pt.lo, pt.hi, threads,
                (unsigned long long)total_ops, qps, per_call_ms,
                (unsigned long long)card);
}
}  // namespace

TEST(NumericRangeBench, RangeQueryMultiThreaded) {
    const char* db_path = std::getenv("ENDEE_BENCH_DB");
    if (!db_path || !*db_path) {
        GTEST_SKIP() << "Set ENDEE_BENCH_DB to a filter directory to run";
    }
    const char* field_env = std::getenv("ENDEE_BENCH_FIELD");
    const std::string field = (field_env && *field_env) ? field_env : "id";
    const char* threads_env = std::getenv("ENDEE_BENCH_THREADS");
    const int threads = (threads_env && *threads_env) ? std::atoi(threads_env) : 16;
    const char* seconds_env = std::getenv("ENDEE_BENCH_SECONDS");
    const double seconds =
        (seconds_env && *seconds_env) ? std::atof(seconds_env) : 8.0;
    ASSERT_GT(threads, 0);
    ASSERT_GT(seconds, 0.0);

    std::printf("NumericRangeBench_MT: db=%s  field=%s  threads=%d  seconds=%.1f\n",
                db_path, field.c_str(), threads, seconds);

    Filter filter(db_path);

    const BenchPoint points[] = {
        {"rate~0.99", 0, 9'900'000},
        {"rate~0.80", 0, 8'000'000},
        {"rate~0.50", 0, 5'000'000},
        {"rate~0.01", 0,   100'000},
    };

    for (const auto& pt : points) {
        run_bench_point_mt(filter, field, pt, threads, seconds);
    }
}
