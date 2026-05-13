#include <gtest/gtest.h>
#include <string>

#include "core/types.hpp"
#include "crow/json.h"
#include "server/request_validation.hpp"
#include "settings.hpp"

namespace {

const crow::json::rvalue& field_from_json(const std::string& json_body, const char* field_name) {
    static crow::json::rvalue body;
    body = crow::json::load(json_body);
    return body[field_name];
}

}  // namespace

TEST(RequestValidationTest, RejectsNegativePrefilterThreshold) {
    auto result = ndd::server::parse_bounded_size(field_from_json(R"({"v": -1})", "v"),
                                                 "filter_params.prefilter_threshold",
                                                 0,
                                                 settings::MAX_VECTORS_ADMIN);

    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.message.find("must be between"), std::string::npos);
}

TEST(RequestValidationTest, RejectsNegativeBoostPercentage) {
    auto result = ndd::server::parse_bounded_size(field_from_json(R"({"v": -1})", "v"),
                                                 "filter_params.boost_percentage",
                                                 0,
                                                 100);

    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.message.find("must be between"), std::string::npos);
}

TEST(RequestValidationTest, AcceptsValidBounds) {
    auto zero = ndd::server::parse_bounded_size(field_from_json(R"({"v": 0})", "v"),
                                               "filter_params.prefilter_threshold",
                                               0,
                                               settings::MAX_VECTORS_ADMIN);
    ASSERT_TRUE(zero.ok()) << zero.message;
    ASSERT_TRUE(zero.value.has_value());
    EXPECT_EQ(*zero.value, 0u);

    auto max_prefilter = ndd::server::parse_bounded_size(
            field_from_json(R"({"v": 1000000000})", "v"),
            "filter_params.prefilter_threshold",
            0,
            settings::MAX_VECTORS_ADMIN);
    ASSERT_TRUE(max_prefilter.ok()) << max_prefilter.message;
    ASSERT_TRUE(max_prefilter.value.has_value());
    EXPECT_EQ(*max_prefilter.value, settings::MAX_VECTORS_ADMIN);

    auto max_boost = ndd::server::parse_bounded_size(field_from_json(R"({"v": 100})", "v"),
                                                    "filter_params.boost_percentage",
                                                    0,
                                                    100);
    ASSERT_TRUE(max_boost.ok()) << max_boost.message;
    ASSERT_TRUE(max_boost.value.has_value());
    EXPECT_EQ(*max_boost.value, 100u);
}

TEST(RequestValidationTest, FilterParamsDefaultsRemainUnchangedWhenAbsent) {
    ndd::FilterParams filter_params;

    EXPECT_EQ(filter_params.prefilter_threshold, settings::PREFILTER_CARDINALITY_THRESHOLD);
    EXPECT_EQ(filter_params.boost_percentage, settings::FILTER_BOOST_PERCENTAGE);
}

TEST(RequestValidationTest, RejectsOutOfRangeValues) {
    auto prefilter = ndd::server::parse_bounded_size(
            field_from_json(R"({"v": 1000000001})", "v"),
            "filter_params.prefilter_threshold",
            0,
            settings::MAX_VECTORS_ADMIN);
    EXPECT_FALSE(prefilter.ok());

    auto boost = ndd::server::parse_bounded_size(field_from_json(R"({"v": 101})", "v"),
                                                "filter_params.boost_percentage",
                                                0,
                                                100);
    EXPECT_FALSE(boost.ok());
}

TEST(RequestValidationTest, RejectsNonIntegerValues) {
    auto floating = ndd::server::parse_bounded_size(field_from_json(R"({"v": 1.5})", "v"),
                                                   "filter_params.boost_percentage",
                                                   0,
                                                   100);
    EXPECT_FALSE(floating.ok());
    EXPECT_NE(floating.message.find("must be an integer"), std::string::npos);

    auto string_value = ndd::server::parse_bounded_size(field_from_json(R"({"v": "5"})", "v"),
                                                       "filter_params.boost_percentage",
                                                       0,
                                                       100);
    EXPECT_FALSE(string_value.ok());
    EXPECT_NE(string_value.message.find("must be an integer"), std::string::npos);
}
