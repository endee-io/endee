#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <optional>
#include <filesystem>
#include <mutex>
#include <memory>
#include <unordered_map>
#include "log.hpp"
#include "settings.hpp"
#include "db_backend.hpp"
#include "quant/common.hpp"

struct IndexMetadata {
    std::string name;  // Just the index name, not the full path
    size_t dimension;
    size_t sparse_dim = 0;  // Added sparse dimension
    std::string space_type_str;
    ndd::quant::QuantizationLevel quant_level =
            ndd::quant::QuantizationLevel::INT8;  // Selected quantization level
    int32_t checksum;
    size_t total_elements;
    size_t M;
    size_t ef_con;
    std::chrono::system_clock::time_point created_at;

    nlohmann::json to_json() const {
        return {{"name", name},
                {"dimension", dimension},
                {"sparse_dim", sparse_dim},
                {"space_type_str", space_type_str},
                {"quant_level", static_cast<uint8_t>(quant_level)},
                {"checksum", checksum},
                {"total_elements", total_elements},
                {"M", M},
                {"ef_con", ef_con},
                {"created_at", std::chrono::system_clock::to_time_t(created_at)}};
    }

    static IndexMetadata from_json(const nlohmann::json& j) {
        IndexMetadata meta;
        meta.name = j["name"].get<std::string>();
        meta.dimension = j["dimension"].get<size_t>();
        if(j.contains("sparse_dim")) {
            meta.sparse_dim = j["sparse_dim"].get<size_t>();
        } else {
            meta.sparse_dim = 0;
        }
        meta.space_type_str = j["space_type_str"].get<std::string>();
        meta.quant_level =
                static_cast<ndd::quant::QuantizationLevel>(j["quant_level"].get<uint8_t>());
        meta.checksum = j["checksum"].get<int32_t>();
        meta.total_elements = j["total_elements"].get<size_t>();
        meta.M = j["M"].get<size_t>();
        meta.ef_con = j["ef_con"].get<size_t>();
        meta.created_at = std::chrono::system_clock::from_time_t(j["created_at"].get<time_t>());
        return meta;
    }
};

class MetadataManager {
private:
    MDBX_env* metadata_env_;
    MDBX_dbi metadata_dbi_;
    std::string metadata_dir_;
    ndd::db::EnvResizeConfig map_config_;

public:
    MetadataManager(const std::string& base_dir) :
        metadata_dir_(base_dir + "/meta"),
        map_config_("index-metadata", settings::INDEX_META_MAP_SIZE_MAX_BITS, settings::INDEX_META_MAP_GROWTH_BITS) {
        std::filesystem::create_directories(metadata_dir_);
        initEnvironment();
    }

    ~MetadataManager() {
        mdbx_dbi_close(metadata_env_, metadata_dbi_);
        mdbx_env_close(metadata_env_);
    }

    // Store metadata for an index
    bool storeMetadata(const std::string& index_id, const IndexMetadata& metadata) {
        std::string key = index_id;

        try {
            ndd::db::with_write_txn_retry(metadata_env_, map_config_, index_id, [&](MDBX_txn* txn) {
                auto json_str = metadata.to_json().dump();
                MDBX_val db_key{(void*)key.c_str(), key.size()};
                MDBX_val data{(void*)json_str.c_str(), json_str.size()};

                const int rc = mdbx_put(txn, metadata_dbi_, &db_key, &data, MDBX_UPSERT);
                ndd::db::throw_if_error(rc, "Failed to store metadata");
            });
            return true;
        } catch(const std::exception& e) {
            LOG_ERROR(1504, index_id, "Exception while storing metadata: " << e.what());
            return false;
        }
    }

    // Update element count in index metadata
    bool updateElementCount(const std::string& index_id, size_t count) {
        auto metadata = getMetadata(index_id);
        if(!metadata) {
            LOG_WARN(1505, index_id, "Cannot update element count because metadata was not found");
            return false;
        }
        metadata->total_elements = count;
        return storeMetadata(index_id, *metadata);
    }

    // Retrieve metadata for an index
    std::optional<IndexMetadata> getMetadata(const std::string& index_id) {
        std::string key = index_id;

        MDBX_txn* txn;
        int rc = mdbx_txn_begin(metadata_env_, nullptr, MDBX_TXN_RDONLY, &txn);
        if(rc != 0) {
            LOG_ERROR(
                    1506, index_id, "Failed to begin metadata read transaction: " << mdbx_strerror(rc));
            return std::nullopt;
        }

        try {
            MDBX_val db_key{(void*)key.c_str(), key.size()};
            MDBX_val data;

            rc = mdbx_get(txn, metadata_dbi_, &db_key, &data);
            if(rc != 0) {
                mdbx_txn_abort(txn);
                if(rc != MDBX_NOTFOUND) {
                    LOG_ERROR(
                            1507, index_id, "Failed to retrieve metadata: " << mdbx_strerror(rc));
                }
                return std::nullopt;
            }

            std::string json_str(static_cast<char*>(data.iov_base), data.iov_len);
            mdbx_txn_abort(txn);

            return IndexMetadata::from_json(nlohmann::json::parse(json_str));
        } catch(const std::exception& e) {
            mdbx_txn_abort(txn);
            LOG_ERROR(1508, index_id, "Exception while retrieving metadata: " << e.what());
            return std::nullopt;
        }
    }

    // Delete metadata for an index
    bool deleteMetadata(const std::string& index_id) {
        std::string key = index_id;

        try {
            return ndd::db::with_write_txn_retry(metadata_env_, map_config_, index_id, [&](MDBX_txn* txn) {
                MDBX_val db_key{(void*)key.c_str(), key.size()};

                const int rc = mdbx_del(txn, metadata_dbi_, &db_key, nullptr);
                if(rc != MDBX_SUCCESS && rc != MDBX_NOTFOUND) {
                    LOG_ERROR(
                            1510, index_id, "Failed to delete metadata: " << mdbx_strerror(rc));
                    return false;
                }

                return true;
            });
        } catch(const std::exception& e) {
            LOG_ERROR(1512, index_id, "Exception while deleting metadata: " << e.what());
            return false;
        }
    }

    // List all indexes metadata
    std::vector<std::pair<std::string, IndexMetadata>> listAllMetadata() {
        std::vector<std::pair<std::string, IndexMetadata>> result;

        MDBX_txn* txn;
        int rc = mdbx_txn_begin(metadata_env_, nullptr, MDBX_TXN_RDONLY, &txn);
        if(rc != 0) {
            LOG_ERROR(
                    1513, "Failed to begin list-all metadata transaction: " << mdbx_strerror(rc));
            return result;
        }

        MDBX_cursor* cursor;
        rc = mdbx_cursor_open(txn, metadata_dbi_, &cursor);
        if(rc != 0) {
            mdbx_txn_abort(txn);
            LOG_ERROR(1514, "Failed to open metadata cursor: " << mdbx_strerror(rc));
            return result;
        }

        MDBX_val key, data;
        while(mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT) == 0) {
            try {
                std::string key_str(static_cast<char*>(key.iov_base), key.iov_len);
                std::string json_str(static_cast<char*>(data.iov_base), data.iov_len);
                result.push_back(
                        {key_str, IndexMetadata::from_json(nlohmann::json::parse(json_str))});
            } catch(const std::exception& e) {
                LOG_ERROR(1515, "Failed to parse metadata while listing all metadata: " << e.what());
                // Skip invalid entries
            }
        }

        mdbx_cursor_close(cursor);
        mdbx_txn_abort(txn);
        return result;
    }

    // List indexes with metadata for a specific user
    std::vector<std::pair<std::string, IndexMetadata>>
    listUserIndexes(const std::string& username) {
        std::vector<std::pair<std::string, IndexMetadata>> indexes;
        std::string prefix = username + "/";

        MDBX_txn* txn;
        int rc = mdbx_txn_begin(metadata_env_, nullptr, MDBX_TXN_RDONLY, &txn);
        if(rc != 0) {
            LOG_ERROR(
                    1516, username, "Failed to begin list-user metadata transaction: " << mdbx_strerror(rc));
            return indexes;
        }

        MDBX_cursor* cursor;
        rc = mdbx_cursor_open(txn, metadata_dbi_, &cursor);
        if(rc != 0) {
            mdbx_txn_abort(txn);
            LOG_ERROR(1517, username, "Failed to open metadata cursor: " << mdbx_strerror(rc));
            return indexes;
        }

        MDBX_val key, data;
        while(mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT) == 0) {
            std::string key_str(static_cast<char*>(key.iov_base), key.iov_len);

            // Check if key starts with the username prefix
            if(key_str.substr(0, prefix.length()) == prefix) {
                try {
                    // Parse the metadata
                    std::string json_str(static_cast<char*>(data.iov_base), data.iov_len);
                    IndexMetadata metadata =
                            IndexMetadata::from_json(nlohmann::json::parse(json_str));

                    // Extract just the index name part (without username prefix)
                    std::string index_name = key_str.substr(prefix.length());

                    // Add to result
                    indexes.emplace_back(index_name, std::move(metadata));
                } catch(const std::exception& e) {
                    LOG_ERROR(
                            1518, key_str, "Failed to parse metadata for index: " << e.what());
                    // Skip invalid entries
                }
            }
        }

        mdbx_cursor_close(cursor);
        mdbx_txn_abort(txn);
        return indexes;
    }

    std::vector<std::pair<std::string, IndexMetadata>> listAllIndexes() {
        std::vector<std::pair<std::string, IndexMetadata>> result;

        MDBX_txn* txn;
        int rc = mdbx_txn_begin(metadata_env_, nullptr, MDBX_TXN_RDONLY, &txn);
        if(rc != 0) {
            LOG_ERROR(
                    1519, "Failed to begin list-all indexes transaction: " << mdbx_strerror(rc));
            return result;
        }

        MDBX_cursor* cursor;
        rc = mdbx_cursor_open(txn, metadata_dbi_, &cursor);
        if(rc != 0) {
            mdbx_txn_abort(txn);
            LOG_ERROR(1520, "Failed to open list-all indexes cursor: " << mdbx_strerror(rc));
            return result;
        }

        MDBX_val key, data;
        while(mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT) == 0) {
            try {
                std::string key_str(static_cast<char*>(key.iov_base), key.iov_len);
                std::string json_str(static_cast<char*>(data.iov_base), data.iov_len);
                IndexMetadata metadata = IndexMetadata::from_json(nlohmann::json::parse(json_str));
                result.emplace_back(key_str, std::move(metadata));
            } catch(const std::exception& e) {
                LOG_ERROR(1521, "Failed to parse metadata while listing all indexes: " << e.what());
                // skip bad record
            }
        }

        mdbx_cursor_close(cursor);
        mdbx_txn_abort(txn);
        return result;
    }

private:
    void initEnvironment() {
        int rc = mdbx_env_create(&metadata_env_);
        if(rc != 0) {
            throw std::runtime_error(std::string("Failed to create metadata LMDB env: ")
                                     + mdbx_strerror(rc));
        }

        rc = ndd::db::configure_env_mapsize(metadata_env_, map_config_);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error(std::string("Failed to set geometry: ") + mdbx_strerror(rc));
        }

        rc = mdbx_env_open(metadata_env_,
                           metadata_dir_.c_str(),
                           MDBX_WRITEMAP | MDBX_MAPASYNC | MDBX_NORDAHEAD,
                           0664);
        if(rc != 0) {
            throw std::runtime_error(std::string("Failed to open metadata environment: ")
                                     + mdbx_strerror(rc));
        }

        ndd::db::with_write_txn_retry(metadata_env_, map_config_, metadata_dir_, [&](MDBX_txn* txn) {
            const int open_rc = mdbx_dbi_open(txn, nullptr, MDBX_CREATE, &metadata_dbi_);
            ndd::db::throw_if_error(open_rc, "Failed to open metadata database");
        });
    }
};
