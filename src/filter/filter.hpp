#pragma once

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
    BitMapFilterFunctor(const ndd::RoaringBitmap& bitmap) :
        bitmap_(bitmap) {}

    bool operator()(ndd::idInt id) override { return bitmap_.contains(id); }
};

class Filter {
private:
    using SchemaCache = std::unordered_map<std::string, FieldType>;

    MDBX_env* env_;
    // Used for schema storage
    MDBX_dbi dbi_;
    std::string index_id_;
    std::string schema_dbi_name_;
    std::unique_ptr<ndd::filter::NumericIndex> numeric_index_;
    std::unique_ptr<ndd::filter::CategoryIndex> category_index_;

    static constexpr const char* SCHEMA_KEY = "__ndd_schema_v1__";
    SchemaCache schema_cache_;
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
    ndd::OperationResult<> save_schema_internal(MDBX_txn* txn);
    ndd::OperationResult<> save_schema_internal(MDBX_txn* txn,
                                                const SchemaCache& schema_cache);
    ndd::OperationResult<> save_schema_blob_internal(const std::string& schema_json);
    ndd::OperationResult<> save_schema_blob_internal(MDBX_txn* txn,
                                                     const std::string& schema_json);
    static std::string serialize_schema(const SchemaCache& schema_cache);

    static ndd::OperationResult<> stage_field_type(SchemaCache& schema_cache,
                                                   const std::string& field,
                                                   FieldType type,
                                                   bool* changed = nullptr);

    /*
     * Converts a JSON number into the current sortable numeric filter encoding.
     *
     * Return codes:
     * 0 = success
     * 2 = value is not numeric; caller should return HTTP 400
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
    static ndd::OperationResult<std::string> category_value_from_json(const nlohmann::json& value,
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
     * (e.g. $gt INT32_MAX, $lt the smallest float); callers must skip the lookup.
     *
     * Return codes:
     * 0 = success
     * 2 = value is not a finite number, or operator is not a numeric comparison;
     *     caller should return HTTP 400
     */
    static ndd::OperationResult<std::pair<uint32_t, uint32_t>>
    numeric_bound_from_comparison(const std::string& op, const nlohmann::json& val);

    void init_dbis();

public:
    Filter(MDBX_env* env,
           const std::string& index_id,
           const std::string& schema_dbi_name = "filter_schema");

    ~Filter();

    ndd::OperationResult<> reload_schema_cache() { return load_schema(); }

    /**
     * Computes the bitmap for an AND filter query, reading category and
     * numeric DBIs through the caller's MDBX read transaction so every
     * lookup in one search shares a single snapshot.
     *
     * Return codes:
     * 0 = success
     * 1 = invalid filter query shape; caller should return HTTP 400
     * 2 = invalid operator or value for field type; caller should return HTTP 400
     * 100-199 = propagated MDBX/storage failure from category or numeric index
     * 200-299 = propagated corruption/invariant failure from category or numeric index
     */
    ndd::OperationResult<ndd::RoaringBitmap>
    computeFilterBitmap(MDBX_txn* txn, const nlohmann::json& filter_array) const;

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
            MDBX_txn* txn,
            const std::vector<std::pair<ndd::idInt, std::string>>& id_filter_pairs,
            bool* schema_changed = nullptr);

    /*
     * Adds one filter JSON document into the numeric and category indexes.
     *
     * Return codes:
     * 0 = success
     * 1-99 = propagated filter validation failure from batch add
     * 100-199 = propagated MDBX/storage failure from batch add
     * 200-299 = propagated corruption/invariant failure from batch add
    */
    ndd::OperationResult<> add_filters_from_json(MDBX_txn* txn,
                                                     ndd::idInt numeric_id,
                                                     const std::string& filter_json,
                                                     bool* schema_changed = nullptr);

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
    ndd::OperationResult<> remove_filters_from_json(MDBX_txn* txn,
                                                        ndd::idInt numeric_id,
                                                        const std::string& filter_json);

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
