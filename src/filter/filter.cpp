#include "filter.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <utility>

#include "../utils/log.hpp"
#include "../utils/settings.hpp"

BitMapFilterFunctor::BitMapFilterFunctor(const ndd::RoaringBitmap& bitmap) :
    bitmap_(bitmap) {}

bool BitMapFilterFunctor::operator()(ndd::idInt id) {
    return bitmap_.contains(id);
}

std::string Filter::format_filter_key(const std::string& field, const std::string& value) {
    return field + ":" + value;
}

// Rejects ':' because it is the MDBX key delimiter for category and numeric
// indexes (see format_filter_key, NumericIndex::make_*_key). Allowing ':' in
// user input causes byte-level key collisions across distinct (field, value)
// pairs.
ndd::OperationResult<>
Filter::validate_filter_key_component(const std::string& component,
                                      const std::string& context) {
    if(component.find(':') != std::string::npos) {
        return {1, context + " must not contain ':'"};
    }
    return {SUCCESS, ""};
}

/*
 * Converts a JSON number into the current sortable numeric filter encoding.
 *
 * Return codes:
 * 0 = success
 * 2 = value is not numeric; caller should return HTTP 400
 */
ndd::OperationResult<uint32_t> Filter::sortable_from_json(const nlohmann::json& value,
                                                          const std::string& context) {
    if(value.is_number_integer()) {
        return {SUCCESS, "", ndd::filter::int_to_sortable(value.get<int>())};
    }
    if(value.is_number()) {
        return {SUCCESS, "", ndd::filter::float_to_sortable(value.get<float>())};
    }
    return {2, context + " must be a number"};
}

/*
 * Converts a JSON scalar into the category key value representation.
 *
 * Return codes:
 * 0 = success
 * 2 = value is not a supported category scalar or is too long; caller should return HTTP 400
 */
ndd::OperationResult<std::string> Filter::category_value_from_json(
        const nlohmann::json& value,
        const std::string& context) {
    std::string str_val;
    if(value.is_string()) {
        str_val = value.get<std::string>();
    } else if(value.is_boolean()) {
        str_val = value.get<bool>() ? "1" : "0";
    } else if(value.is_number_integer()) {
        str_val = std::to_string(value.get<int>());
    } else {
        return {2, context + " must be string, integer, or boolean"};
    }

    if(str_val.size() > 255) {
        return {2, context + " is too long"};
    }
    auto delim_check = validate_filter_key_component(str_val, context);
    if(!delim_check.ok()) {
        return {delim_check.code, delim_check.message};
    }
    return {SUCCESS, "", std::move(str_val)};
}

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
ndd::OperationResult<std::pair<uint32_t, uint32_t>>
Filter::numeric_bound_from_comparison(const std::string& op, const nlohmann::json& val) {
    using Bound = std::pair<uint32_t, uint32_t>;
    constexpr uint32_t SORTABLE_MIN = 0x00000000u;
    constexpr uint32_t SORTABLE_MAX = 0xFFFFFFFFu;
    const Bound EMPTY{SORTABLE_MAX, SORTABLE_MIN};

    if(!val.is_number()) {
        return {2, op + " value must be a finite number"};
    }
    if(!val.is_number_integer() && !std::isfinite(val.get<float>())) {
        return {2, op + " value must be a finite number"};
    }

    if(op == "$gte") {
        auto sortable_result = sortable_from_json(val, op + " value");
        if(!sortable_result.ok()) {
            return {sortable_result.code, sortable_result.message};
        }
        return {SUCCESS, "", Bound{sortable_result.value_or_throw(), SORTABLE_MAX}};
    }
    if(op == "$lte") {
        auto sortable_result = sortable_from_json(val, op + " value");
        if(!sortable_result.ok()) {
            return {sortable_result.code, sortable_result.message};
        }
        return {SUCCESS, "", Bound{SORTABLE_MIN, sortable_result.value_or_throw()}};
    }
    if(op == "$gt") {
        if(val.is_number_integer()) {
            int32_t x = val.get<int>();
            if(x == std::numeric_limits<int32_t>::max()) {
                return {SUCCESS, "", EMPTY};
            }
            return {SUCCESS, "", Bound{ndd::filter::int_to_sortable(x + 1), SORTABLE_MAX}};
        }
        float x = val.get<float>();
        float next = std::nextafterf(x, std::numeric_limits<float>::infinity());
        if(!std::isfinite(next)) {
            return {SUCCESS, "", EMPTY};
        }
        return {SUCCESS, "", Bound{ndd::filter::float_to_sortable(next), SORTABLE_MAX}};
    }
    if(op == "$lt") {
        if(val.is_number_integer()) {
            int32_t x = val.get<int>();
            if(x == std::numeric_limits<int32_t>::min()) {
                return {SUCCESS, "", EMPTY};
            }
            return {SUCCESS, "", Bound{SORTABLE_MIN, ndd::filter::int_to_sortable(x - 1)}};
        }
        float x = val.get<float>();
        float next = std::nextafterf(x, -std::numeric_limits<float>::infinity());
        if(!std::isfinite(next)) {
            return {SUCCESS, "", EMPTY};
        }
        return {SUCCESS, "", Bound{SORTABLE_MIN, ndd::filter::float_to_sortable(next)}};
    }

    return {2, "Unsupported numeric comparison operator: " + op};
}

/*
 * Persists the current in-memory filter schema cache.
 *
 * Return codes:
 * 0 = success
 * 100 = MDBX transaction, write, or commit failure; caller should log ERROR and return HTTP 500
 */
ndd::OperationResult<> Filter::save_schema_internal() {
    nlohmann::json schema_json;
    for(const auto& [field, type] : schema_cache_) {
        schema_json[field] = static_cast<int>(type);
    }
    std::string json_str = schema_json.dump();

    MDBX_txn* txn = nullptr;
    int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
    if(rc != MDBX_SUCCESS) {
        return {100, "Failed to begin schema write transaction: "
                             + std::string(mdbx_strerror(rc))};
    }

    MDBX_val key{const_cast<char*>(SCHEMA_KEY), std::strlen(SCHEMA_KEY)};
    MDBX_val data{const_cast<char*>(json_str.c_str()), json_str.size()};

    rc = mdbx_put(txn, dbi_, &key, &data, MDBX_UPSERT);
    if(rc != MDBX_SUCCESS) {
        mdbx_txn_abort(txn);
        return {100, "Failed to persist filter schema: "
                             + std::string(mdbx_strerror(rc))};
    }

    rc = mdbx_txn_commit(txn);
    if(rc != MDBX_SUCCESS) {
        return {100, "Failed to commit filter schema update: "
                             + std::string(mdbx_strerror(rc))};
    }
    return {SUCCESS, ""};
}

/*
 * Loads the persisted filter schema into the in-memory schema cache.
 *
 * Return codes:
 * 0 = success
 * 100 = MDBX transaction or read failure; caller should log ERROR and return HTTP 500
 * 200 = corrupt schema JSON payload; caller should log ERROR and return HTTP 500
 */
ndd::OperationResult<> Filter::load_schema() {
    MDBX_txn* txn = nullptr;
    int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &txn);
    if(rc != MDBX_SUCCESS) {
        return {100, "Failed to begin schema read transaction: "
                             + std::string(mdbx_strerror(rc))};
    }

    MDBX_val key{const_cast<char*>(SCHEMA_KEY), std::strlen(SCHEMA_KEY)};
    MDBX_val data;
    rc = mdbx_get(txn, dbi_, &key, &data);

    if(rc == MDBX_NOTFOUND || (rc == MDBX_SUCCESS && data.iov_len == 0)) {
        mdbx_txn_abort(txn);
        return {SUCCESS, ""};
    }
    if(rc != MDBX_SUCCESS) {
        mdbx_txn_abort(txn);
        return {100, "Failed to read filter schema: " + std::string(mdbx_strerror(rc))};
    }

    try {
        std::string json_str(static_cast<const char*>(data.iov_base), data.iov_len);
        auto parsed = nlohmann::json::parse(json_str);
        std::lock_guard<std::mutex> lock(schema_mutex_);
        schema_cache_.clear();
        for(auto& [field, stored_type] : parsed.items()) {
            schema_cache_[field] = static_cast<FieldType>(stored_type.get<int>());
        }
    } catch(const std::exception& e) {
        mdbx_txn_abort(txn);
        return {200, "Failed to parse filter schema: " + std::string(e.what())};
    }

    mdbx_txn_abort(txn);
    return {SUCCESS, ""};
}

/*
 * Registers a field type in the filter schema if it is not already present.
 *
 * Return codes:
 * 0 = success
 * 3 = field type mismatch with existing schema; caller should return HTTP 400
 * 100-199 = propagated MDBX/storage failure from schema persistence
 */
ndd::OperationResult<> Filter::register_field_type(const std::string& field, FieldType type) {
    std::lock_guard<std::mutex> lock(schema_mutex_);
    auto it = schema_cache_.find(field);
    if(it != schema_cache_.end()) {
        if(it->second == type) {
            return {SUCCESS, ""};
        }
        return {3, "Filter field '" + field + "' has a different existing type"};
    }

    schema_cache_[field] = type;
    auto save_result = save_schema_internal();
    if(!save_result.ok()) {
        schema_cache_.erase(field);
        return save_result;
    }
    return {SUCCESS, ""};
}

void Filter::init_environment() {
    int rc = mdbx_env_create(&env_);
    if(rc != MDBX_SUCCESS) {
        throw std::runtime_error(std::string("Failed to create LMDB env for filters: ")
                                 + mdbx_strerror(rc));
    }

    // max DBs to allow multiple databases (main + schema + numeric_forward + numeric_inverted)
    rc = mdbx_env_set_maxdbs(env_, 10);
    if(rc != MDBX_SUCCESS) {
        throw std::runtime_error(std::string("Failed to configure max DBs for filters: ")
                                 + mdbx_strerror(rc));
    }

    // Set geometry for auto-grow using the filter map size settings
    rc = mdbx_env_set_geometry(env_,
                               -1,
                               1ULL << settings::FILTER_MAP_SIZE_BITS,
                               1ULL << settings::FILTER_MAP_SIZE_MAX_BITS,
                               1ULL << settings::FILTER_MAP_SIZE_BITS,
                               -1,
                               -1);
    if(rc != MDBX_SUCCESS) {
        throw std::runtime_error(std::string("Failed to set geometry for filters: ")
                                 + mdbx_strerror(rc));
    }

    rc = mdbx_env_open(env_,
                       path_.c_str(),
                       MDBX_WRITEMAP | MDBX_MAPASYNC | MDBX_NORDAHEAD,
                       0664);
    if(rc != MDBX_SUCCESS) {
        throw std::runtime_error(std::string("Failed to open filter environment: ")
                                 + mdbx_strerror(rc));
    }

    MDBX_txn* txn = nullptr;
    rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
    if(rc != MDBX_SUCCESS) {
        throw std::runtime_error(std::string("Failed to begin filter transaction: ")
                                 + mdbx_strerror(rc));
    }

    rc = mdbx_dbi_open(txn, nullptr, MDBX_CREATE, &dbi_);
    if(rc != MDBX_SUCCESS) {
        mdbx_txn_abort(txn);
        throw std::runtime_error(std::string("Failed to open filter database: ")
                                 + mdbx_strerror(rc));
    }

    rc = mdbx_txn_commit(txn);
    if(rc != MDBX_SUCCESS) {
        throw std::runtime_error(std::string("Failed to commit filter transaction: ")
                                 + mdbx_strerror(rc));
    }

    // Initialize Indices
    numeric_index_ = std::make_unique<ndd::filter::NumericIndex>(env_);
    category_index_ = std::make_unique<ndd::filter::CategoryIndex>(env_);

    auto schema_result = load_schema();
    if(!schema_result.ok()) {
        LOG_ERROR(1201, index_id_, schema_result.message);
        throw std::runtime_error(schema_result.message);
    }
}

Filter::Filter(const std::string& path, const std::string& index_id) :
    index_id_(index_id),
    path_(path) {
    std::filesystem::create_directories(path);
    init_environment();
}

Filter::Filter(const std::string& path) :
    Filter(path, "-/-") {}

Filter::~Filter() {
    mdbx_dbi_close(env_, dbi_);
    mdbx_env_close(env_);
}

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
Filter::computeFilterBitmap(const nlohmann::json& filter_array) const {
    if(!filter_array.is_array()) {
        return {1, "Filter must be an array"};
    }

    if(filter_array.empty()) {
        return {SUCCESS, "", ndd::RoaringBitmap()};
    }

    std::vector<ndd::RoaringBitmap> partial_results;
    partial_results.reserve(filter_array.size());

    for(const auto& condition : filter_array) {
        if(!condition.is_object() || condition.size() != 1) {
            return {1, "Each filter condition must be a single-field object"};
        }

        const auto& field = condition.begin().key();
        const auto& expr = condition.begin().value();
        if(field.empty()) {
            return {1, "Filter field name cannot be empty"};
        }
        auto field_check = validate_filter_key_component(field, "Filter field name");
        if(!field_check.ok()) {
            return {field_check.code, field_check.message};
        }
        if(!expr.is_object() || expr.size() != 1) {
            return {1, "Filter operator must be a single-field object"};
        }

        // Check schema for field type
        FieldType type = FieldType::Unknown;
        {
            std::lock_guard<std::mutex> lock(schema_mutex_);
            auto it = schema_cache_.find(field);
            if(it != schema_cache_.end()) {
                type = it->second;
            }
        }

        const std::string op = expr.begin().key();
        const auto& val = expr.begin().value();
        ndd::RoaringBitmap or_result;

        if(op == "$eq") {
            if(type == FieldType::Number) {
                auto sortable_result = sortable_from_json(val, "$eq value for numeric field");
                if(!sortable_result.ok()) {
                    return {sortable_result.code, sortable_result.message};
                }
                auto range_result =
                        numeric_index_->range(field, sortable_result.value_or_throw(), sortable_result.value_or_throw());
                if(!range_result.ok()) {
                    return {range_result.code, range_result.message};
                }
                or_result = std::move(range_result.value_or_throw());
            } else {
                auto value_result = category_value_from_json(val, "$eq value");
                if(!value_result.ok()) {
                    return {value_result.code, value_result.message};
                }
                auto bitmap_result = category_index_->get_bitmap_by_key(
                        format_filter_key(field, value_result.value_or_throw()));
                if(!bitmap_result.ok()) {
                    return {bitmap_result.code, bitmap_result.message};
                }
                or_result = std::move(bitmap_result.value_or_throw());
            }
        } else if(op == "$in") {
            if(!val.is_array()) {
                return {2, "$in must be an array"};
            }

            for(const auto& item : val) {
                if(type == FieldType::Number) {
                    auto sortable_result =
                            sortable_from_json(item, "$in value for numeric field");
                    if(!sortable_result.ok()) {
                        return {sortable_result.code, sortable_result.message};
                    }
                    auto range_result = numeric_index_->range(field,
                                                              sortable_result.value_or_throw(),
                                                              sortable_result.value_or_throw());
                    if(!range_result.ok()) {
                        return {range_result.code, range_result.message};
                    }
                    or_result |= range_result.value_or_throw();
                } else {
                    auto value_result = category_value_from_json(item, "$in value");
                    if(!value_result.ok()) {
                        return {value_result.code, value_result.message};
                    }
                    if(!value_result.value_or_throw().empty()) {
                        auto bitmap_result = category_index_->get_bitmap_by_key(
                                format_filter_key(field, value_result.value_or_throw()));
                        if(!bitmap_result.ok()) {
                            return {bitmap_result.code, bitmap_result.message};
                        }
                        or_result |= bitmap_result.value_or_throw();
                    }
                }
            }
        } else if(op == "$range") {
            if(!val.is_array() || val.size() != 2) {
                return {2, "$range must be [start, end] with exactly 2 values"};
            }
            if(type != FieldType::Number) {
                return {2, "$range operator is only supported for numeric fields"};
            }

            auto start_result = sortable_from_json(val[0], "Range start");
            if(!start_result.ok()) {
                return {start_result.code, start_result.message};
            }
            auto end_result = sortable_from_json(val[1], "Range end");
            if(!end_result.ok()) {
                return {end_result.code, end_result.message};
            }
            if(start_result.value_or_throw() > end_result.value_or_throw()) {
                return {2, "Invalid range: start > end"};
            }

            auto range_result =
                    numeric_index_->range(field, start_result.value_or_throw(), end_result.value_or_throw());
            if(!range_result.ok()) {
                return {range_result.code, range_result.message};
            }
            or_result = std::move(range_result.value_or_throw());
        } else if(op == "$lt" || op == "$lte" || op == "$gt" || op == "$gte") {
            if(type != FieldType::Number) {
                return {2, op + " operator is only supported for numeric fields"};
            }
            auto bound_result = numeric_bound_from_comparison(op, val);
            if(!bound_result.ok()) {
                return {bound_result.code, bound_result.message};
            }
            auto [min_val, max_val] = bound_result.value_or_throw();
            if(min_val <= max_val) {
                auto range_result = numeric_index_->range(field, min_val, max_val);
                if(!range_result.ok()) {
                    return {range_result.code, range_result.message};
                }
                or_result = std::move(range_result.value_or_throw());
            }
        } else {
            return {2, "Unsupported filter operator: " + op};
        }

        partial_results.push_back(std::move(or_result));
    }

    // Optimization: Sort by cardinality (smallest first)
    std::sort(partial_results.begin(),
              partial_results.end(),
              [](const ndd::RoaringBitmap& left, const ndd::RoaringBitmap& right) {
                  return left.cardinality() < right.cardinality();
              });

    if(partial_results.empty()) {
        return {SUCCESS, "", ndd::RoaringBitmap()};
    }

    ndd::RoaringBitmap final_result = partial_results[0];
    for(size_t i = 1; i < partial_results.size(); ++i) {
        final_result &= partial_results[i];

        // If result becomes empty, stop early
        if(final_result.isEmpty()) {
            return {SUCCESS, "", std::move(final_result)};
        }
    }

    return {SUCCESS, "", std::move(final_result)};
}

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
Filter::getIdsMatchingFilter(const nlohmann::json& filter_array) const {
    auto bitmap_result = computeFilterBitmap(filter_array);
    if(!bitmap_result.ok()) {
        return {bitmap_result.code, bitmap_result.message};
    }

    std::vector<ndd::idInt> ids;
    ids.reserve(bitmap_result.value_or_throw().cardinality());
    bitmap_result.value_or_throw().iterate(
            [](ndd::idInt val, void* ptr) {
                static_cast<std::vector<ndd::idInt>*>(ptr)->push_back(val);
                return true;
            },
            &ids);
    return {SUCCESS, "", std::move(ids)};
}

/*
 * Counts numeric ids matching a filter query.
 *
 * Return codes:
 * 0 = success
 * 1-99 = propagated filter validation failure from bitmap computation
 * 100-199 = propagated MDBX/storage failure from bitmap computation
 * 200-299 = propagated corruption/invariant failure from bitmap computation
 */
ndd::OperationResult<size_t> Filter::countIdsMatchingFilter(
        const nlohmann::json& filter_array) const {
    auto bitmap_result = computeFilterBitmap(filter_array);
    if(!bitmap_result.ok()) {
        return {bitmap_result.code, bitmap_result.message};
    }
    return {SUCCESS, "", bitmap_result.value_or_throw().cardinality()};
}

/*
 * Adds one id to a category filter.
 *
 * Return codes:
 * 0 = success
 * 100-199 = propagated MDBX/storage failure from category index
 * 200-299 = propagated corruption/invariant failure from category index
 */
ndd::OperationResult<>
Filter::add_to_filter(const std::string& field,
                      const std::string& value,
                      ndd::idInt numeric_id) {
    auto field_check = validate_filter_key_component(field, "Filter field name");
    if(!field_check.ok()) {
        return field_check;
    }
    auto value_check = validate_filter_key_component(value, "Filter value");
    if(!value_check.ok()) {
        return value_check;
    }
    return category_index_->add(field, value, numeric_id);
}

/*
 * Adds a batch of ids to one already formatted category filter key.
 *
 * Return codes:
 * 0 = success
 * 100-199 = propagated MDBX/storage failure from category index
 * 200-299 = propagated corruption/invariant failure from category index
 */
ndd::OperationResult<> Filter::add_to_filter_batch(
        const std::string& filter_key,
        const std::vector<ndd::idInt>& numeric_ids) {
    if(numeric_ids.empty()) {
        return {SUCCESS, ""};
    }
    return category_index_->add_batch_by_key(filter_key, numeric_ids);
}

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
ndd::OperationResult<> Filter::add_filters_from_json_batch(
        const std::vector<std::pair<ndd::idInt, std::string>>& id_filter_pairs) {
    if(id_filter_pairs.empty()) {
        return {SUCCESS, ""};
    }

    // Create a map to collect IDs for each label filter
    std::unordered_map<std::string, std::vector<ndd::idInt>> label_filter_to_ids;
    label_filter_to_ids.reserve(id_filter_pairs.size());
    std::vector<ndd::filter::NumericBatchEntry> numeric_filter_entries;
    numeric_filter_entries.reserve(id_filter_pairs.size());

    // Group IDs by filter
    for(const auto& [numeric_id, filter_json] : id_filter_pairs) {
        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(filter_json);
        } catch(const std::exception& e) {
            return {1, "Invalid filter JSON: " + std::string(e.what())};
        }

        if(!parsed.is_object()) {
            return {1, "Filter JSON document must be an object"};
        }

        for(const auto& [field, value] : parsed.items()) {
            if(field.empty()) {
                return {1, "Filter field name cannot be empty"};
            }
            auto field_check = validate_filter_key_component(field, "Filter field name");
            if(!field_check.ok()) {
                return {field_check.code, field_check.message};
            }

            FieldType type = FieldType::Unknown;
            if(value.is_boolean()) {
                type = FieldType::Bool;
            } else if(value.is_number()) {
                type = FieldType::Number;
            } else if(value.is_string()) {
                type = FieldType::String;
            }

            if(type == FieldType::Unknown) {
                return {2, "Unsupported filter type for field '" + field + "'"};
            }

            auto register_result = register_field_type(field, type);
            if(!register_result.ok()) {
                return register_result;
            }

            if(type == FieldType::String) {
                auto category_result = category_value_from_json(value, "Filter value");
                if(!category_result.ok()) {
                    return {category_result.code,
                            category_result.message + " for field '" + field + "'"};
                }
                label_filter_to_ids[format_filter_key(field, category_result.value_or_throw())]
                        .emplace_back(numeric_id);
            } else if(type == FieldType::Bool) {
                label_filter_to_ids[format_filter_key(field, value.get<bool>() ? "1" : "0")]
                        .emplace_back(numeric_id);
            } else if(type == FieldType::Number) {
                auto sortable_result = sortable_from_json(value, "Numeric filter value");
                if(!sortable_result.ok()) {
                    return {sortable_result.code,
                            sortable_result.message + " for field '" + field + "'"};
                }
                numeric_filter_entries.emplace_back(field, numeric_id, sortable_result.value_or_throw());
            }
        }
    }

    /**
     * XXX: For transactional correctness of filter adds, all the filters
     * should be added in a single transaction.
     * For now, they are being added in two different transactions.
     * one for numeric_index and other for labels.
     */

    if(!numeric_filter_entries.empty()) {
        auto numeric_result = numeric_index_->put_batch(numeric_filter_entries);
        if(!numeric_result.ok()) {
            return numeric_result;
        }
    }

    // Process each filter with its batch of IDs
    for(const auto& [filter_key, ids] : label_filter_to_ids) {
        auto add_result = add_to_filter_batch(filter_key, ids);
        if(!add_result.ok()) {
            return add_result;
        }
    }

    return {SUCCESS, ""};
}

/*
 * Removes one id from a category filter.
 *
 * Return codes:
 * 0 = success
 * 100-199 = propagated MDBX/storage failure from category index
 * 200-299 = propagated corruption/invariant failure from category index
 */
ndd::OperationResult<>
Filter::remove_from_filter(const std::string& field,
                           const std::string& value,
                           ndd::idInt numeric_id) {
    auto field_check = validate_filter_key_component(field, "Filter field name");
    if(!field_check.ok()) {
        return field_check;
    }
    auto value_check = validate_filter_key_component(value, "Filter value");
    if(!value_check.ok()) {
        return value_check;
    }
    return category_index_->remove(field, value, numeric_id);
}

/*
 * Checks whether one id is present in a category filter.
 *
 * Return codes:
 * 0 = success
 * 100-199 = propagated MDBX/storage failure from category index
 * 200-299 = propagated corruption/invariant failure from category index
 */
ndd::OperationResult<bool>
Filter::contains(const std::string& field,
                 const std::string& value,
                 ndd::idInt numeric_id) const {
    auto field_check = validate_filter_key_component(field, "Filter field name");
    if(!field_check.ok()) {
        return {field_check.code, field_check.message};
    }
    auto value_check = validate_filter_key_component(value, "Filter value");
    if(!value_check.ok()) {
        return {value_check.code, value_check.message};
    }
    return category_index_->contains(field, value, numeric_id);
}

/*
 * Adds one filter JSON document into the numeric and category indexes.
 *
 * Return codes:
 * 0 = success
 * 1-99 = propagated filter validation failure from batch add
 * 100-199 = propagated MDBX/storage failure from batch add
 * 200-299 = propagated corruption/invariant failure from batch add
 */
ndd::OperationResult<> Filter::add_filters_from_json(ndd::idInt numeric_id,
                                                     const std::string& filter_json) {
    return add_filters_from_json_batch({{numeric_id, filter_json}});
}

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
ndd::OperationResult<> Filter::remove_filters_from_json(ndd::idInt numeric_id,
                                                        const std::string& filter_json) {
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(filter_json);
    } catch(const std::exception& e) {
        return {1, "Invalid filter JSON while removing filters: " + std::string(e.what())};
    }

    if(!parsed.is_object()) {
        return {1, "Filter JSON document must be an object"};
    }

    for(const auto& [field, value] : parsed.items()) {
        if(field.empty()) {
            return {1, "Filter field name cannot be empty"};
        }
        auto field_check = validate_filter_key_component(field, "Filter field name");
        if(!field_check.ok()) {
            return {field_check.code, field_check.message};
        }

        ndd::OperationResult<> remove_result{SUCCESS, ""};
        if(value.is_string()) {
            auto category_result = category_value_from_json(value, "Filter value");
            if(!category_result.ok()) {
                return {category_result.code,
                        category_result.message + " for field '" + field + "'"};
            }
            remove_result = remove_from_filter(field, category_result.value_or_throw(), numeric_id);
        } else if(value.is_number()) {
            // Remove from Numeric Index
            remove_result = numeric_index_->remove(field, numeric_id);
        } else if(value.is_boolean()) {
            remove_result = remove_from_filter(field,
                                               value.get<bool>() ? "1" : "0",
                                               numeric_id);
        } else {
            return {2, "Unsupported filter type for field '" + field + "'"};
        }

        if(!remove_result.ok()) {
            return remove_result;
        }
    }

    return {SUCCESS, ""};
}

/*
 * Combines category filters with AND semantics.
 *
 * Return codes:
 * 0 = success
 * 100-199 = propagated MDBX/storage failure from category index
 * 200-299 = propagated corruption/invariant failure from category index
 */
ndd::OperationResult<ndd::RoaringBitmap> Filter::combine_filters_and(
        const std::vector<std::pair<std::string, std::string>>& filters) const {
    ndd::RoaringBitmap result;
    bool first = true;
    for(const auto& [field, value] : filters) {
        auto bitmap_result = category_index_->get_bitmap(field, value);
        if(!bitmap_result.ok()) {
            return {bitmap_result.code, bitmap_result.message};
        }
        if(first) {
            result = std::move(bitmap_result.value_or_throw());
            first = false;
        } else {
            result &= bitmap_result.value_or_throw();
        }
    }
    return {SUCCESS, "", std::move(result)};
}

/*
 * Combines category filters with OR semantics.
 *
 * Return codes:
 * 0 = success
 * 100-199 = propagated MDBX/storage failure from category index
 * 200-299 = propagated corruption/invariant failure from category index
 */
ndd::OperationResult<ndd::RoaringBitmap> Filter::combine_filters_or(
        const std::vector<std::pair<std::string, std::string>>& filters) const {
    ndd::RoaringBitmap result;
    for(const auto& [field, value] : filters) {
        auto bitmap_result = category_index_->get_bitmap(field, value);
        if(!bitmap_result.ok()) {
            return {bitmap_result.code, bitmap_result.message};
        }
        result |= bitmap_result.value_or_throw();
    }
    return {SUCCESS, "", std::move(result)};
}

/*
 * Checks whether one id satisfies one numeric filter expression.
 *
 * Return codes:
 * 0 = success
 * 2 = invalid numeric operator or value; caller should return HTTP 400
 * 100-199 = propagated MDBX/storage failure from numeric index
 * 200-299 = propagated corruption/invariant failure from numeric index
 */
ndd::OperationResult<bool> Filter::check_numeric(const std::string& field,
                                                 ndd::idInt id,
                                                 const std::string& op,
                                                 const nlohmann::json& val) const {
    if(op == "$eq") {
        auto sortable_result = sortable_from_json(val, "$eq value for numeric field");
        if(!sortable_result.ok()) {
            return {sortable_result.code, sortable_result.message};
        }
        return numeric_index_->check_range(field,
                                           id,
                                           sortable_result.value_or_throw(),
                                           sortable_result.value_or_throw());
    }

    if(op == "$in") {
        if(!val.is_array()) {
            return {2, "$in must be an array"};
        }
        for(const auto& item : val) {
            auto sortable_result = sortable_from_json(item, "$in value for numeric field");
            if(!sortable_result.ok()) {
                return {sortable_result.code, sortable_result.message};
            }

            auto check_result = numeric_index_->check_range(field,
                                                            id,
                                                            sortable_result.value_or_throw(),
                                                            sortable_result.value_or_throw());
            if(!check_result.ok()) {
                return check_result;
            }
            if(check_result.value_or_throw()) {
                return {SUCCESS, "", true};
            }
        }
        return {SUCCESS, "", false};
    }

    if(op == "$range") {
        if(!val.is_array() || val.size() != 2) {
            return {2, "$range must be [start, end] with exactly 2 values"};
        }

        auto start_result = sortable_from_json(val[0], "Range start");
        if(!start_result.ok()) {
            return {start_result.code, start_result.message};
        }
        auto end_result = sortable_from_json(val[1], "Range end");
        if(!end_result.ok()) {
            return {end_result.code, end_result.message};
        }
        if(start_result.value_or_throw() > end_result.value_or_throw()) {
            return {2, "Invalid range: start > end"};
        }

        return numeric_index_->check_range(field, id, start_result.value_or_throw(), end_result.value_or_throw());
    }

    if(op == "$lt" || op == "$lte" || op == "$gt" || op == "$gte") {
        auto bound_result = numeric_bound_from_comparison(op, val);
        if(!bound_result.ok()) {
            return {bound_result.code, bound_result.message};
        }
        auto [min_val, max_val] = bound_result.value_or_throw();
        if(min_val > max_val) {
            return {SUCCESS, "", false};
        }
        return numeric_index_->check_range(field, id, min_val, max_val);
    }

    return {2, "Unsupported numeric operator: " + op};
}
