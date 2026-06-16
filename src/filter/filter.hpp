#pragma once

// System includes
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "json/nlohmann_json.hpp"
#include "mdbx/mdbx.h"
#include "../core/types.hpp"
#include "../hnsw/hnswlib.h"
#include "../utils/types.hpp"

#include "category_index.hpp"
#include "numeric_index.hpp"

enum class FieldType : uint8_t {
    Unknown = 0,
    String = 1,
    Number = 2, // Unified Integer and Float
    Bool = 4
};

// Filter Functor for HNSW
class BitMapFilterFunctor : public hnswlib::BaseFilterFunctor {
    const ndd::RoaringBitmap& bitmap_;

public:
    BitMapFilterFunctor(const ndd::RoaringBitmap& bitmap);

    bool operator()(ndd::idInt id) override;
};

class Filter {
private:
    MDBX_env* env_;
    // Used for schema storage
    MDBX_dbi dbi_;
    std::string index_id_;
    std::string path_;
    std::unique_ptr<ndd::filter::NumericIndex> numeric_index_;
    std::unique_ptr<ndd::filter::CategoryIndex> category_index_;

    static constexpr const char* SCHEMA_KEY = "__ndd_schema_v1__";
    std::unordered_map<std::string, FieldType> schema_cache_;
    mutable std::mutex schema_mutex_;

    /*
     * Loads the persisted filter schema into the in-memory schema cache.
     *
     * Return codes:
     * 0 = success
     * 100 = MDBX transaction or read failure; caller should log ERROR and return HTTP 500
     * 200 = corrupt schema JSON payload; caller should log ERROR and return HTTP 500
     */
    ndd::OperationResult<> load_schema();

    /*
     * Persists the current in-memory filter schema cache.
     *
     * Return codes:
     * 0 = success
     * 100 = MDBX transaction, write, or commit failure; caller should log ERROR and return HTTP 500
     */
    ndd::OperationResult<> save_schema_internal();

    /*
     * Registers a field type in the filter schema if it is not already present.
     *
     * Return codes:
     * 0 = success
     * 3 = field type mismatch with existing schema; caller should return HTTP 400
     * 100-199 = propagated MDBX/storage failure from schema persistence
     */
    ndd::OperationResult<> register_field_type(const std::string& field, FieldType type);

    /**
     * Converts a JSON number into the current sortable numeric filter encoding.
     * All numeric filter values use one float32 sortable domain, including JSON
     * integers, so 2 and 2.0 compare equal. Limitations: values are rounded to
     * float precision before indexing/querying, distinct large integers can
     * collapse to the same key, and strict comparisons use the next float32
     * representable value around the rounded query bound. float32 has 24 bits
     * of integer precision (23 stored mantissa bits plus the hidden bit), so it
     * represents every integer only up to 2^24 = 16,777,216; above that, not all
     * consecutive integers are representable. Consecutive integers are also less
     * dense in the float sortable bit domain than under int_to_sortable, so
     * integer-heavy fields can create more numeric buckets and make wide range
     * scans walk more bucket entries. Existing filter DBs that indexed integers
     * with int_to_sortable must be rebuilt.
     *
     * Return codes:
     * 0 = success
     * 2 = value is not numeric or not finite in float32; caller should return HTTP 400
     */
    static ndd::OperationResult<uint32_t> sortable_from_json(const nlohmann::json& value,
                                                            const std::string& context);

    /*
     * Converts a JSON scalar into the category key value representation.
     *
     * Return codes:
     * 0 = success
     * 2 = value is not a supported category scalar or is too long; caller should return HTTP 400
     */
    static ndd::OperationResult<std::string> category_value_from_json(
            const nlohmann::json& value,
            const std::string& context);

    // Rejects ':' because it is the MDBX key delimiter for category and numeric
    // indexes (see format_filter_key, NumericIndex::make_*_key). Allowing ':' in
    // user input causes byte-level key collisions across distinct (field, value)
    // pairs.
    static ndd::OperationResult<>
    validate_filter_key_component(const std::string& component,
                                  const std::string& context);

    static std::string format_filter_key(const std::string& field, const std::string& value);

    /*
     * Resolves [$lt | $lte | $gt | $gte] on a JSON numeric value into a
     * sortable [min, max] range usable against NumericIndex::range / check_range.
     * A returned pair with min > max signals a provably-empty range
     * (e.g. $gt the largest finite float32); callers must skip the lookup.
     *
     * Return codes:
     * 0 = success
     * 2 = value is not a finite number, or operator is not a numeric comparison;
     *     caller should return HTTP 400
     */
    static ndd::OperationResult<std::pair<uint32_t, uint32_t>>
    numeric_bound_from_comparison(const std::string& op, const nlohmann::json& val);

    void init_environment();

public:
    Filter(const std::string& path, const std::string& index_id);

    Filter(const std::string& path);

    ~Filter();

    /*
     * Computes the bitmap for an AND filter query.
     *
     * Return codes:
     * 0 = success
     * 1 = invalid filter query shape; caller should return HTTP 400
     * 2 = invalid operator or value for field type; caller should return HTTP 400
     * 100-199 = propagated MDBX/storage failure from category or numeric index
     * 200-299 = propagated corruption/invariant failure from category or numeric index
     */
    ndd::OperationResult<ndd::RoaringBitmap>
    computeFilterBitmap(const nlohmann::json& filter_array) const;

    /**
     * Returns numeric ids matching a filter query based on the provided JSON filter array
     *
     * Return codes:
     * 0 = success
     * 1-99 = propagated filter validation failure from bitmap computation
     * 100-199 = propagated MDBX/storage failure from bitmap computation
     * 200-299 = propagated corruption/invariant failure from bitmap computation
     */
    ndd::OperationResult<std::vector<ndd::idInt>>
    getIdsMatchingFilter(const nlohmann::json& filter_array) const;

    /*
     * Counts numeric ids matching a filter query.
     *
     * Return codes:
     * 0 = success
     * 1-99 = propagated filter validation failure from bitmap computation
     * 100-199 = propagated MDBX/storage failure from bitmap computation
     * 200-299 = propagated corruption/invariant failure from bitmap computation
     */
    ndd::OperationResult<size_t> countIdsMatchingFilter(
            const nlohmann::json& filter_array) const;

    /*
     * Adds one id to a category filter.
     *
     * Return codes:
     * 0 = success
     * 100-199 = propagated MDBX/storage failure from category index
     * 200-299 = propagated corruption/invariant failure from category index
     */
    ndd::OperationResult<>
    add_to_filter(const std::string& field, const std::string& value, ndd::idInt numeric_id);

    /*
     * Adds a batch of ids to one already formatted category filter key.
     *
     * Return codes:
     * 0 = success
     * 100-199 = propagated MDBX/storage failure from category index
     * 200-299 = propagated corruption/invariant failure from category index
     */
    ndd::OperationResult<> add_to_filter_batch(const std::string& filter_key,
                                               const std::vector<ndd::idInt>& numeric_ids);

    /*
     * Adds one batch of filter JSON documents into the numeric and category indexes.
     *
     * Return codes:
     * 0 = success
     * 1 = invalid filter JSON or field shape; caller should return HTTP 400
     * 2 = unsupported filter field type or category value too long; caller should return HTTP 400
     * 3 = field type mismatch with existing schema; caller should return HTTP 400
     * 100-199 = propagated MDBX/storage failure from schema, numeric, or category writes
     * 200-299 = propagated corruption/invariant failure from numeric or category writes
     */
    ndd::OperationResult<> add_filters_from_json_batch(
            const std::vector<std::pair<ndd::idInt, std::string>>& id_filter_pairs);

    /*
     * Removes one id from a category filter.
     *
     * Return codes:
     * 0 = success
     * 100-199 = propagated MDBX/storage failure from category index
     * 200-299 = propagated corruption/invariant failure from category index
     */
    ndd::OperationResult<>
    remove_from_filter(const std::string& field,
                       const std::string& value,
                       ndd::idInt numeric_id);

    /*
     * Checks whether one id is present in a category filter.
     *
     * Return codes:
     * 0 = success
     * 100-199 = propagated MDBX/storage failure from category index
     * 200-299 = propagated corruption/invariant failure from category index
     */
    ndd::OperationResult<bool>
    contains(const std::string& field, const std::string& value, ndd::idInt numeric_id) const;

    /*
     * Adds one filter JSON document into the numeric and category indexes.
     *
     * Return codes:
     * 0 = success
     * 1-99 = propagated filter validation failure from batch add
     * 100-199 = propagated MDBX/storage failure from batch add
     * 200-299 = propagated corruption/invariant failure from batch add
     */
    ndd::OperationResult<> add_filters_from_json(ndd::idInt numeric_id,
                                                 const std::string& filter_json);

    /*
     * Removes one filter JSON document from the numeric and category indexes.
     *
     * Return codes:
     * 0 = success
     * 1 = invalid filter JSON or field shape; caller should return HTTP 400
     * 2 = unsupported filter field type; caller should return HTTP 400
     * 100-199 = propagated MDBX/storage failure from numeric or category index
     * 200-299 = propagated corruption/invariant failure from numeric or category index
     */
    ndd::OperationResult<> remove_filters_from_json(ndd::idInt numeric_id,
                                                    const std::string& filter_json);

    /*
     * Combines category filters with AND semantics.
     *
     * Return codes:
     * 0 = success
     * 100-199 = propagated MDBX/storage failure from category index
     * 200-299 = propagated corruption/invariant failure from category index
     */
    ndd::OperationResult<ndd::RoaringBitmap> combine_filters_and(
            const std::vector<std::pair<std::string, std::string>>& filters) const;

    /*
     * Combines category filters with OR semantics.
     *
     * Return codes:
     * 0 = success
     * 100-199 = propagated MDBX/storage failure from category index
     * 200-299 = propagated corruption/invariant failure from category index
     */
    ndd::OperationResult<ndd::RoaringBitmap> combine_filters_or(
            const std::vector<std::pair<std::string, std::string>>& filters) const;

    /*
     * Checks whether one id satisfies one numeric filter expression.
     *
     * Return codes:
     * 0 = success
     * 2 = invalid numeric operator or value; caller should return HTTP 400
     * 100-199 = propagated MDBX/storage failure from numeric index
     * 200-299 = propagated corruption/invariant failure from numeric index
     */
    ndd::OperationResult<bool> check_numeric(const std::string& field,
                                             ndd::idInt id,
                                             const std::string& op,
                                             const nlohmann::json& val) const;
};
