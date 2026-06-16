#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "filter/filter.hpp"
#include "filter/numeric_index.hpp"
#include "json/nlohmann_json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

/*
 * Stress test for the numeric inverted index invariant:
 *   for every (id, value) in the forward index, an $eq(value) query
 *   must return id; and the set of ids returned for value V must equal
 *   exactly the set of ids whose forward-mapped value is V.
 *
 * Two complementary phases:
 *
 *   A. SaturatedDuplicatesThenSplit -- builds the pathological state
 *      from the verified plan deterministically. For each anchor value
 *      A, insert MAX_SIZE + EXTRA ids at A so the bucket saturates
 *      with delta-0 entries and pushes EXTRA into the summary_bitmap
 *      via Bucket::add's saturated-duplicate short-circuit. Then
 *      insert a few ids at A's float-next-up neighbor (which lands in
 *      the same bucket: float spacing near A puts that delta well
 *      under MAX_DELTA). The first such insert hits the slide-split
 *      fallthrough; the next triggers a real slide-split. Verifies
 *      $eq(A) returns all surviving ids -- pre-fix the split rebuilds
 *      the left bitmap from ids[] only and EXTRA ids vanish.
 *
 *   B. RandomChurnPlusDrain -- mixed insert / value-update / remove
 *      workload, then drain every id still bound to an anchor. The
 *      drain order is randomized so removes interleave bitmap-only
 *      and ids[] entries; the model is the ground truth for the
 *      invariant check after each phase.
 *
 * The Filter API is used end-to-end so the test exercises put_internal
 * (remove-old + add-new) on updates and remove_from_buckets on removes.
 */

namespace {

static void expect_ok(const ndd::OperationResult<>& r) {
    ASSERT_TRUE(r.ok()) << r.message;
}

template <typename T>
static T unwrap(ndd::OperationResult<T> r) {
    EXPECT_TRUE(r.ok()) << r.message;
    EXPECT_TRUE(r.value.has_value());
    return std::move(*r.value);
}

/*
 * Floats are the on-the-wire value type for numeric filters. Encode
 * them with full precision so the JSON round-trip lands on exactly the
 * same float (and therefore exactly the same sortable bucket key) as
 * the C++ float we hold here.
 */
static std::string score_payload(float v) {
    std::ostringstream os;
    os.precision(std::numeric_limits<float>::max_digits10);
    os << R"({"score": )" << v << "}";
    return os.str();
}

struct Model {
    std::unordered_map<ndd::idInt, float> id_to_value;
    // Float keys here are exact: we only ever store values that came
    // out of Model::set, which received them as floats with no further
    // arithmetic, so equality and hashing on the bit pattern are safe.
    std::unordered_map<float, std::unordered_set<ndd::idInt>> value_to_ids;

    void set(ndd::idInt id, float value) {
        auto it = id_to_value.find(id);
        if(it != id_to_value.end()) {
            auto& old_set = value_to_ids[it->second];
            old_set.erase(id);
            if(old_set.empty()) {
                value_to_ids.erase(it->second);
            }
            it->second = value;
        } else {
            id_to_value.emplace(id, value);
        }
        value_to_ids[value].insert(id);
    }

    void erase(ndd::idInt id) {
        auto it = id_to_value.find(id);
        if(it == id_to_value.end()) {
            return;
        }
        auto& set = value_to_ids[it->second];
        set.erase(id);
        if(set.empty()) {
            value_to_ids.erase(it->second);
        }
        id_to_value.erase(it);
    }
};

static void verify_eq_consistency(Filter& filter, const Model& model,
                                  const std::string& tag) {
    SCOPED_TRACE("verify_eq_consistency: " + tag);
    for(const auto& [value, expected_ids] : model.value_to_ids) {
        json q = json::array({ { {"score", { {"$eq", value} } } } });
        auto ids = unwrap(filter.getIdsMatchingFilter(q));
        std::unordered_set<ndd::idInt> got(ids.begin(), ids.end());
        ASSERT_EQ(got.size(), expected_ids.size())
            << "value=" << value
            << " expected_count=" << expected_ids.size()
            << " got_count=" << got.size();
        for(ndd::idInt eid : expected_ids) {
            ASSERT_TRUE(got.count(eid))
                << "value=" << value << " missing id=" << eid;
        }
    }
}

class NumericStressFixture : public ::testing::Test {
protected:
    std::string db_path;
    std::unique_ptr<Filter> filter;

    void SetUp() override {
        db_path = "./stress_db_" + std::to_string(::rand());
        if(fs::exists(db_path)) {
            fs::remove_all(db_path);
        }
        filter = std::make_unique<Filter>(db_path);
    }

    void TearDown() override {
        filter.reset();
        if(fs::exists(db_path)) {
            fs::remove_all(db_path);
        }
    }
};

}  // namespace

TEST_F(NumericStressFixture, SaturatedDuplicatesThenSplit) {
    /*
     * Pick a handful of unrelated anchor values. They must be far
     * apart in sortable space so they live in different buckets and
     * don't accidentally help each other split.
     */
    const std::vector<float> anchors = { 100.0f, 5'000.0f, 250'000.0f };
    constexpr ndd::idInt EXTRA = 200;

    Model model;
    ndd::idInt next_id = 1;

    for(float A : anchors) {
        /*
         * Phase A.1 -- saturate the bucket purely with delta-0
         * duplicates at A. The first MAX_SIZE go into ids[]/deltas[];
         * the trailing EXTRA fall through Bucket::add's saturated
         * short-circuit into summary_bitmap only.
         */
        for(ndd::idInt i = 0; i < ndd::filter::Bucket::MAX_SIZE + EXTRA; ++i) {
            ndd::idInt id = next_id++;
            expect_ok(filter->add_filters_from_json(id, score_payload(A)));
            model.set(id, A);
        }

        /*
         * Phase A.2 -- introduce a value boundary in the same bucket.
         * std::nextafterf(A, +inf) is one ULP above A: it lands in the
         * same bucket (sortable delta == 1, well under MAX_DELTA). The
         * first such insert hits the slide-split fallthrough (all
         * existing deltas zero), placing a non-zero delta into ids[].
         * The second insert (a duplicate of A here) now finds a value
         * boundary and triggers a real slide-split. Pre-fix that
         * rebuild drops the EXTRA bitmap-only ids.
         */
        const float A_next = std::nextafterf(A, std::numeric_limits<float>::infinity());
        ASSERT_NE(A, A_next);

        ndd::idInt boundary_id = next_id++;
        expect_ok(filter->add_filters_from_json(boundary_id, score_payload(A_next)));
        model.set(boundary_id, A_next);

        ndd::idInt split_trigger_id = next_id++;
        expect_ok(filter->add_filters_from_json(split_trigger_id, score_payload(A)));
        model.set(split_trigger_id, A);
    }

    verify_eq_consistency(*filter, model, "after saturate+split per anchor");

    /*
     * A few more inserts and a final consistency sweep, to catch a
     * regression where a damaged left bitmap survives the first
     * verification but corrupts a subsequent operation.
     */
    for(float A : anchors) {
        for(int extra_pass = 0; extra_pass < 50; ++extra_pass) {
            ndd::idInt id = next_id++;
            expect_ok(filter->add_filters_from_json(id, score_payload(A)));
            model.set(id, A);
        }
    }

    verify_eq_consistency(*filter, model, "after follow-up inserts");
}

TEST_F(NumericStressFixture, RandomChurnPlusDrain) {
    /*
     * Hot values: dense duplicates. Each lives in its own bucket
     * because they are far apart in sortable space, so saturation
     * happens per-bucket.
     */
    const std::vector<float> hot_values = { 1'000.0f, 50'000.0f, 1'000'000.0f };

    /*
     * Warm offsets are one ULP up from a hot value: same bucket as the
     * hot anchor, non-zero delta. Used sparingly so a hot bucket can
     * still saturate before any warm enters.
     */
    auto warm_for = [](float hot) {
        return std::nextafterf(hot, std::numeric_limits<float>::infinity());
    };

    std::mt19937 rng(0xDEADBEEFu);  // fixed seed for reproducibility
    std::uniform_real_distribution<double> coin(0.0, 1.0);
    std::uniform_real_distribution<float> cold_dist(50.0f, 500'000.0f);

    Model model;
    ndd::idInt next_id = 1;

    /*
     * Warm-up: drive each hot value past MAX_SIZE so its bucket holds
     * bitmap-only entries before random churn starts. Without this
     * priming the random workload would interleave warm inserts
     * before saturation and the slide-split fallthrough path would
     * never form.
     */
    constexpr ndd::idInt PRIME_EXTRA = 100;
    for(float h : hot_values) {
        for(ndd::idInt i = 0; i < ndd::filter::Bucket::MAX_SIZE + PRIME_EXTRA; ++i) {
            ndd::idInt id = next_id++;
            expect_ok(filter->add_filters_from_json(id, score_payload(h)));
            model.set(id, h);
        }
    }
    verify_eq_consistency(*filter, model, "after priming");

    auto pick_value = [&]() -> float {
        const double v = coin(rng);
        if(v < 0.60) {
            std::uniform_int_distribution<size_t> p(0, hot_values.size() - 1);
            return hot_values[p(rng)];
        }
        if(v < 0.80) {
            std::uniform_int_distribution<size_t> p(0, hot_values.size() - 1);
            return warm_for(hot_values[p(rng)]);
        }
        return cold_dist(rng);
    };

    auto pick_existing_id = [&]() -> ndd::idInt {
        std::uniform_int_distribution<size_t> p(0, model.id_to_value.size() - 1);
        auto it = model.id_to_value.begin();
        std::advance(it, p(rng));
        return it->first;
    };

    constexpr int ROUNDS = 6000;
    constexpr int CHECK_EVERY = 1500;

    for(int round = 0; round < ROUNDS; ++round) {
        const double action_dice = coin(rng);
        const bool can_touch_existing = !model.id_to_value.empty();

        if(action_dice < 0.55 || !can_touch_existing) {
            ndd::idInt id = next_id++;
            float v = pick_value();
            expect_ok(filter->add_filters_from_json(id, score_payload(v)));
            model.set(id, v);
        } else if(action_dice < 0.85) {
            ndd::idInt id = pick_existing_id();
            float v = pick_value();
            expect_ok(filter->add_filters_from_json(id, score_payload(v)));
            model.set(id, v);
        } else {
            ndd::idInt id = pick_existing_id();
            float old_v = model.id_to_value[id];
            expect_ok(filter->remove_filters_from_json(id, score_payload(old_v)));
            model.erase(id);
        }

        if(round != 0 && round % CHECK_EVERY == 0) {
            verify_eq_consistency(*filter, model,
                                  "mid-churn round=" + std::to_string(round));
        }
    }

    verify_eq_consistency(*filter, model, "post-churn");

    /*
     * Drain every id still bound to a hot value, in shuffled order so
     * removes mix ids[] entries and bitmap-only entries. Verify after
     * each batch so a buggy is_empty() (deleting a bucket while
     * bitmap-only entries remain) is caught before subsequent removes
     * silently reconcile state.
     */
    constexpr size_t DRAIN_BATCH = 25;
    for(float h : hot_values) {
        auto it = model.value_to_ids.find(h);
        if(it == model.value_to_ids.end()) {
            continue;
        }
        std::vector<ndd::idInt> ids_at_hot(it->second.begin(), it->second.end());
        std::shuffle(ids_at_hot.begin(), ids_at_hot.end(), rng);

        for(size_t i = 0; i < ids_at_hot.size(); i += DRAIN_BATCH) {
            const size_t end = std::min(i + DRAIN_BATCH, ids_at_hot.size());
            for(size_t j = i; j < end; ++j) {
                expect_ok(filter->remove_filters_from_json(
                    ids_at_hot[j], score_payload(h)));
                model.erase(ids_at_hot[j]);
            }
            verify_eq_consistency(*filter, model,
                                  "drain h=" + std::to_string(h)
                                      + " batch_end=" + std::to_string(end));
        }
    }

    verify_eq_consistency(*filter, model, "post-drain");

    /*
     * Range-query cross-check across the union of all anchors. Result
     * must equal the model union for values inside the band. Exercises
     * the range slow path on every bucket whose extent is not fully
     * covered.
     */
    constexpr float RANGE_LO = 900.0f;
    constexpr float RANGE_HI = 1'500'000.0f;
    json range_q =
        json::array({ { {"score", { {"$range", { RANGE_LO, RANGE_HI }} } } } });
    auto range_ids = unwrap(filter->getIdsMatchingFilter(range_q));
    std::unordered_set<ndd::idInt> got_range(range_ids.begin(), range_ids.end());

    std::unordered_set<ndd::idInt> expected_range;
    for(const auto& [value, ids_set] : model.value_to_ids) {
        if(value >= RANGE_LO && value <= RANGE_HI) {
            expected_range.insert(ids_set.begin(), ids_set.end());
        }
    }
    ASSERT_EQ(got_range.size(), expected_range.size())
        << "range query cardinality mismatch";
    for(ndd::idInt eid : expected_range) {
        ASSERT_TRUE(got_range.count(eid))
            << "range query missing id=" << eid
            << " (forward value=" << model.id_to_value[eid] << ")";
    }
}
