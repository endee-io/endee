#include <gtest/gtest.h>
#include <algorithm>
#include <climits>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>
#include "filter/filter.hpp"
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
