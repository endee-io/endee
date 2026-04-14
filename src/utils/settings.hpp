#pragma once

#include <cstdlib>
#include <string>
#include <sstream>
#include <algorithm>
#include <cstdint>

constexpr uint64_t KB = (1024ULL);
constexpr uint64_t MB = (1024ULL * KB);
constexpr uint64_t GB = (1024ULL * MB);
constexpr uint64_t TB = (1024ULL * GB);

namespace settings {
    // === Compile-time constants ===
    inline const std::string VERSION = "1.3.0";
    inline uint16_t INDEX_VERSION = 1;
    inline uint16_t SPARSE_ONDISK_VERSION = 1;
    inline const std::string DEFAULT_SPACE_TYPE = "cosine";

    constexpr size_t MIN_DIMENSION = 2;
    constexpr size_t MAX_DIMENSION = 16'384;
    constexpr size_t DEFAULT_M = 16;
    constexpr size_t MIN_M = 4;
    constexpr size_t MAX_M = 512;
    constexpr size_t DEFAULT_EF_CONSTRUCT = 128;
    constexpr size_t MIN_EF_CONSTRUCT = 8;
    constexpr size_t BACKFILL_BUFFER = 4; // Keep 4 slots free for high quality neighbors
    constexpr size_t MAX_EF_CONSTRUCT = 4096;
    constexpr size_t DEFAULT_EF_SEARCH = 128;
    constexpr size_t MIN_K = 1;
    constexpr size_t MAX_K = 4096;
    constexpr float DEFAULT_DENSE_RRF_WEIGHT = 0.5f;
    constexpr float DEFAULT_RRF_RANK_CONSTANT = 60.0f;
    constexpr size_t RANDOM_SEED = 100;
    constexpr size_t SAVE_EVERY_N_UPDATES = 10'000;
    constexpr size_t RECOVERY_BATCH_SIZE = 20'000;
    constexpr size_t SAVE_EVERY_N_MINUTES = 30;
    constexpr size_t AUTOSAVE_SLEEP_MINUTES = 5;
    // Number of threads for http server - 0 means it will default to hardware concurrency
    constexpr size_t DEFAULT_NUM_SERVER_THREADS = 0;

    // MDBX default map sizes. Growth step and initial size are the same for all databases.
    // System tables
    constexpr size_t DEFAULT_INDEX_META_MAP_SIZE_BITS = 21;      // 2 MiB
    constexpr size_t DEFAULT_INDEX_META_MAP_SIZE_MAX_BITS = 27;  // 128 MiB
    // Index-related tables
    constexpr size_t DEFAULT_ID_MAPPER_MAP_SIZE_BITS = 24;      // 16 MiB
    constexpr size_t DEFAULT_ID_MAPPER_MAP_SIZE_MAX_BITS = 33;  // 8 GiB
    constexpr size_t DEFAULT_FILTER_MAP_SIZE_BITS = 24;         // 16 MiB
    constexpr size_t DEFAULT_FILTER_MAP_SIZE_MAX_BITS = 36;     // 64 GiB
    constexpr size_t DEFAULT_METADATA_MAP_SIZE_BITS = 27;       // 128 MiB
    constexpr size_t DEFAULT_METADATA_MAP_SIZE_MAX_BITS = 39;   // 512 GiB
    constexpr size_t DEFAULT_VECTOR_MAP_SIZE_BITS = 30;         // 1 GiB
    constexpr size_t DEFAULT_VECTOR_MAP_SIZE_MAX_BITS = 40;     // 1 TiB
    // Sparse storage
    constexpr size_t DEFAULT_SPARSE_MAP_SIZE_MAX_BITS = 40;    // 1 TiB

    constexpr size_t MAX_LINK_LIST_LOCKS = 65536;

    // Sparse Index settings
    /*XXX: Should we make this a runtime configurable value ?*/
    constexpr size_t DEFAULT_INV_IDX_SEARCH_BATCH_SZ = 10'000;
    constexpr float NEAR_ZERO = 1e-9f;

    // Compact a posting list when the fraction of tombstoned entries
    // reaches this threshold (0.0 = compact every delete, 1.0 = never).
    constexpr float INV_IDX_COMPACTION_TOMBSTONE_RATIO = 0.1f;

    // Maximum number of elements in the index
    constexpr size_t MAX_VECTORS_ADMIN = 1'000'000'000;


    //minimum bytes in filesystem before triggering out of storage sequence
    constexpr size_t MINIMUM_REQUIRED_FS_BYTES = (10 * GB);

    // System sanity check thresholds
    constexpr size_t DEFAULT_MINIMUM_REQUIRED_DRAM_MB = (4 * 1024); //GB in MB
    constexpr size_t MINIMUM_OPEN_FILES = 5000;
    constexpr size_t DEFAULT_MINIMUM_CPU_CORES = 2;

    // Buffer for early exit in search base layer
    constexpr int EARLY_EXIT_BUFFER_INSERT = 16;
    constexpr int EARLY_EXIT_BUFFER_QUERY = 8;

    // Pre-filter threshold - use pre-filter when cardinality is below this value
    constexpr size_t PREFILTER_CARDINALITY_THRESHOLD = 10'000;
    constexpr size_t FILTER_BOOST_PERCENTAGE = 0;

    //DEFAULT VALUES
    constexpr size_t DEFAULT_NUM_PARALLEL_INSERTS = 4;
    constexpr size_t DEFAULT_NUM_RECOVERY_THREADS = 16;

    /**
     * Look at docs/memory_management.md
     * XXX: DO NOT CHANGE THIS
     * DEFAULT_MAX_LIVE_INDICES can only be upto 255
     * because of PTHREAD_KEYS_MAX limit
     */
    constexpr size_t DEFAULT_MAX_LIVE_INDICES = 100;
    // constexpr float MAX_ANON_MEM = 60; //A percentage of total memory

    constexpr bool DEFAULT_ENABLE_DEBUG_LOG = true;
    const std::string DEFAULT_AUTH_TOKEN = "";
    inline static std::string DEFAULT_USERNAME = "endee";
    constexpr size_t DEFAULT_SERVER_PORT = 8080;
    const std::string DEFAULT_SERVER_TYPE = "OSS";
    const std::string DEFAULT_DATA_DIR = "/mnt/data";
    const std::string DEFAULT_SUBINDEX = "default";
    constexpr size_t MAX_NR_SUBINDEX = 100; //Maximum number of subindexes
    constexpr size_t DEFAULT_MAX_ELEMENTS = 100'000;
    constexpr size_t DEFAULT_MAX_ELEMENTS_INCREMENT = 100'000;
    constexpr size_t DEFAULT_MAX_ELEMENTS_INCREMENT_TRIGGER = 50'000;
    constexpr size_t DEFAULT_VECTOR_CACHE_PERCENTAGE = 15;
    constexpr size_t DEFAULT_VECTOR_CACHE_MIN_BITS = 17; // Minimum 128K entries in cache
    const std::string DEFAULT_SERVER_ID = "unknown";

    //For Backups
    static const int MAX_BACKUP_NAME_LENGTH = 200;

    /*Lib tkn: ONLY PLACEHOLDER*/
    const std::string DEFAULT_LIB_TOKEN = "3a5f08c7d9e1b2a43a5f08c7d9e1b2a4";

    // === Runtime-configurable settings ===
    inline static std::string SERVER_ID = [] {
        const char* env = std::getenv("NDD_SERVER_ID");
        return env ? std::string(env) : DEFAULT_SERVER_ID;
    }();

    inline static size_t NUM_SERVER_THREADS = [] {
        const char* env = std::getenv("NDD_NUM_SERVER_THREADS");
        if (env) {
            return (size_t)std::stoull(env);
        }

        // If no env var, check if default is 0 (auto-detect)
        if (DEFAULT_NUM_SERVER_THREADS == 0) {
            unsigned int hw = std::thread::hardware_concurrency() * 2;
            return hw > 0 ? (size_t)hw : 1; // Fallback to 1 if hardware_concurrency returns 0
        }

        return (size_t)DEFAULT_NUM_SERVER_THREADS;
    }();

    inline static size_t INV_IDX_SEARCH_BATCH_SZ = [] {
        const char* env = std::getenv("NDD_INV_IDX_SEARCH_BATCH_SZ");
        return env ? std::stoull(env) : DEFAULT_INV_IDX_SEARCH_BATCH_SZ;
    }();

    inline static size_t SERVER_PORT = [] {
        const char* env = std::getenv("NDD_SERVER_PORT");
        return env ? std::stoull(env) : DEFAULT_SERVER_PORT;
    }();

    // Server type can be "SERVERLESS", "ON-PREM", "DEV"
    inline static std::string SERVER_TYPE = [] {
        const char* env = std::getenv("NDD_SERVER_TYPE");
        return env ? std::string(env) : DEFAULT_SERVER_TYPE;
    }();

    inline static std::string DATA_DIR = [] {
        const char* env = std::getenv("NDD_DATA_DIR");
        return env ? std::string(env) : DEFAULT_DATA_DIR;
    }();

    inline static size_t MAX_ELEMENTS = [] {
        const char* env = std::getenv("NDD_MAX_ELEMENTS");
        return env ? std::stoull(env) : DEFAULT_MAX_ELEMENTS;
    }();
    inline static size_t MAX_ELEMENTS_INCREMENT = [] {
        const char* env = std::getenv("NDD_MAX_ELEMENTS_INCREMENT");
        return env ? std::stoull(env) : DEFAULT_MAX_ELEMENTS_INCREMENT;
    }();
    inline static size_t MAX_ELEMENTS_INCREMENT_TRIGGER = [] {
        const char* env = std::getenv("NDD_MAX_INCREMENT_TRIGGER");
        return env ? std::stoull(env) : DEFAULT_MAX_ELEMENTS_INCREMENT_TRIGGER;
    }();

    inline static size_t VECTOR_CACHE_PERCENTAGE = [] {
        const char* env = std::getenv("NDD_VECTOR_CACHE_PERCENTAGE");
        return env ? std::min<size_t>(std::stoull(env), 100) : DEFAULT_VECTOR_CACHE_PERCENTAGE;
    }();

    inline static size_t VECTOR_CACHE_MIN_BITS = [] {
        const char* env = std::getenv("NDD_VECTOR_CACHE_MIN_BITS");
        return env ? std::stoull(env) : DEFAULT_VECTOR_CACHE_MIN_BITS;
    }();

    inline static size_t MAX_LIVE_INDICES = [] {
        const char* env = std::getenv("NDD_MAX_LIVE_INDICES");
        return env ? std::stoull(env) : DEFAULT_MAX_LIVE_INDICES;
    }();

    // Number of parallel inserts. It will use this many threads to insert data in parallel
    inline static size_t NUM_PARALLEL_INSERTS = [] {
        const char* env = std::getenv("NDD_NUM_PARALLEL_INSERTS");
        return env ? std::stoull(env) : DEFAULT_NUM_PARALLEL_INSERTS;
    }();
    inline static size_t NUM_RECOVERY_THREADS = [] {
        const char* env = std::getenv("NDD_NUM_RECOVERY_THREADS");
        return env ? std::stoull(env) : DEFAULT_NUM_RECOVERY_THREADS;
    }();

    // Minimum available DRAM in MB (configurable via NDD_MIN_DRAM_MB)
    inline static size_t MINIMUM_REQUIRED_DRAM_MB = [] {
        const char* env = std::getenv("NDD_MIN_DRAM_MB");
        return env ? std::stoull(env) : DEFAULT_MINIMUM_REQUIRED_DRAM_MB;
    }();

    inline static bool ENABLE_DEBUG_LOG = [] {
        const char* env = std::getenv("NDD_DEBUG_LOG");
        return env ? (std::string(env) == "1" || std::string(env) == "true")
                   : DEFAULT_ENABLE_DEBUG_LOG;
    }();

    // Authentication settings for open-source mode
    // If NDD_AUTH_TOKEN is set, authentication is required
    // If NDD_AUTH_TOKEN is empty/not set, all APIs work without authentication
    inline static std::string AUTH_TOKEN = [] {
        const char* env = std::getenv("NDD_AUTH_TOKEN");
        return env ? std::string(env) : DEFAULT_AUTH_TOKEN;
    }();

    inline static bool AUTH_ENABLED = !AUTH_TOKEN.empty();

    // MDBX map sizes (bit counts, used as 1ULL << N)
    inline static size_t INDEX_META_MAP_SIZE_BITS = [] {
        const char* env = std::getenv("NDD_INDEX_META_MAP_SIZE_BITS");
        return env ? std::stoull(env) : DEFAULT_INDEX_META_MAP_SIZE_BITS;
    }();
    inline static size_t INDEX_META_MAP_SIZE_MAX_BITS = [] {
        const char* env = std::getenv("NDD_INDEX_META_MAP_SIZE_MAX_BITS");
        return env ? std::stoull(env) : DEFAULT_INDEX_META_MAP_SIZE_MAX_BITS;
    }();
    inline static size_t ID_MAPPER_MAP_SIZE_BITS = [] {
        const char* env = std::getenv("NDD_ID_MAPPER_MAP_SIZE_BITS");
        return env ? std::stoull(env) : DEFAULT_ID_MAPPER_MAP_SIZE_BITS;
    }();
    inline static size_t ID_MAPPER_MAP_SIZE_MAX_BITS = [] {
        const char* env = std::getenv("NDD_ID_MAPPER_MAP_SIZE_MAX_BITS");
        return env ? std::stoull(env) : DEFAULT_ID_MAPPER_MAP_SIZE_MAX_BITS;
    }();
    inline static size_t FILTER_MAP_SIZE_BITS = [] {
        const char* env = std::getenv("NDD_FILTER_MAP_SIZE_BITS");
        return env ? std::stoull(env) : DEFAULT_FILTER_MAP_SIZE_BITS;
    }();
    inline static size_t FILTER_MAP_SIZE_MAX_BITS = [] {
        const char* env = std::getenv("NDD_FILTER_MAP_SIZE_MAX_BITS");
        return env ? std::stoull(env) : DEFAULT_FILTER_MAP_SIZE_MAX_BITS;
    }();
    inline static size_t METADATA_MAP_SIZE_BITS = [] {
        const char* env = std::getenv("NDD_METADATA_MAP_SIZE_BITS");
        return env ? std::stoull(env) : DEFAULT_METADATA_MAP_SIZE_BITS;
    }();
    inline static size_t METADATA_MAP_SIZE_MAX_BITS = [] {
        const char* env = std::getenv("NDD_METADATA_MAP_SIZE_MAX_BITS");
        return env ? std::stoull(env) : DEFAULT_METADATA_MAP_SIZE_MAX_BITS;
    }();
    inline static size_t VECTOR_MAP_SIZE_BITS = [] {
        const char* env = std::getenv("NDD_VECTOR_MAP_SIZE_BITS");
        return env ? std::stoull(env) : DEFAULT_VECTOR_MAP_SIZE_BITS;
    }();
    inline static size_t VECTOR_MAP_SIZE_MAX_BITS = [] {
        const char* env = std::getenv("NDD_VECTOR_MAP_SIZE_MAX_BITS");
        return env ? std::stoull(env) : DEFAULT_VECTOR_MAP_SIZE_MAX_BITS;
    }();
    inline static size_t SPARSE_MAP_SIZE_MAX_BITS = [] {
        const char* env = std::getenv("NDD_SPARSE_MAP_SIZE_MAX_BITS");
        return env ? std::stoull(env) : DEFAULT_SPARSE_MAP_SIZE_MAX_BITS;
    }();

    /**
     * All the startup settings will be checked here.
     * All violations will be returned as errors.
     */
    inline std::string validateSettings() {
        std::string error;

        auto appendComparisonError = [&error](const char* lhs_name,
                                              size_t lhs_value,
                                              const char* comparison_text,
                                              const char* rhs_name,
                                              size_t rhs_value)
        {
            error += "\n";
            error += lhs_name;
            error += "(";
            error += std::to_string(lhs_value);
            error += ") ";
            error += comparison_text;
            error += " ";
            error += rhs_name;
            error += " (";
            error += std::to_string(rhs_value);
            error += ")";
        };

        if(MAX_LIVE_INDICES > DEFAULT_MAX_LIVE_INDICES) {
            appendComparisonError("NDD_MAX_LIVE_INDICES",
                                  MAX_LIVE_INDICES,
                                  "exceeds",
                                  "DEFAULT_MAX_LIVE_INDICES",
                                  DEFAULT_MAX_LIVE_INDICES);
        }
        if(INDEX_META_MAP_SIZE_BITS >= INDEX_META_MAP_SIZE_MAX_BITS) {
            appendComparisonError("NDD_INDEX_META_MAP_SIZE_BITS",
                                  INDEX_META_MAP_SIZE_BITS,
                                  "must be less than",
                                  "NDD_INDEX_META_MAP_SIZE_MAX_BITS",
                                  INDEX_META_MAP_SIZE_MAX_BITS);
        }
        if(ID_MAPPER_MAP_SIZE_BITS >= ID_MAPPER_MAP_SIZE_MAX_BITS) {
            appendComparisonError("NDD_ID_MAPPER_MAP_SIZE_BITS",
                                  ID_MAPPER_MAP_SIZE_BITS,
                                  "must be less than",
                                  "NDD_ID_MAPPER_MAP_SIZE_MAX_BITS",
                                  ID_MAPPER_MAP_SIZE_MAX_BITS);
        }
        if(FILTER_MAP_SIZE_BITS >= FILTER_MAP_SIZE_MAX_BITS) {
            appendComparisonError("NDD_FILTER_MAP_SIZE_BITS",
                                  FILTER_MAP_SIZE_BITS,
                                  "must be less than",
                                  "NDD_FILTER_MAP_SIZE_MAX_BITS",
                                  FILTER_MAP_SIZE_MAX_BITS);
        }
        if(METADATA_MAP_SIZE_BITS >= METADATA_MAP_SIZE_MAX_BITS) {
            appendComparisonError("NDD_METADATA_MAP_SIZE_BITS",
                                  METADATA_MAP_SIZE_BITS,
                                  "must be less than",
                                  "NDD_METADATA_MAP_SIZE_MAX_BITS",
                                  METADATA_MAP_SIZE_MAX_BITS);
        }
        if(VECTOR_MAP_SIZE_BITS >= VECTOR_MAP_SIZE_MAX_BITS) {
            appendComparisonError("NDD_VECTOR_MAP_SIZE_BITS",
                                  VECTOR_MAP_SIZE_BITS,
                                  "must be less than",
                                  "NDD_VECTOR_MAP_SIZE_MAX_BITS",
                                  VECTOR_MAP_SIZE_MAX_BITS);
        }
        if(INDEX_META_MAP_SIZE_MAX_BITS > DEFAULT_INDEX_META_MAP_SIZE_MAX_BITS) {
            appendComparisonError("NDD_INDEX_META_MAP_SIZE_MAX_BITS",
                                  INDEX_META_MAP_SIZE_MAX_BITS,
                                  "exceeds",
                                  "DEFAULT_INDEX_META_MAP_SIZE_MAX_BITS",
                                  DEFAULT_INDEX_META_MAP_SIZE_MAX_BITS);
        }
        if(ID_MAPPER_MAP_SIZE_MAX_BITS > DEFAULT_ID_MAPPER_MAP_SIZE_MAX_BITS) {
            appendComparisonError("NDD_ID_MAPPER_MAP_SIZE_MAX_BITS",
                                  ID_MAPPER_MAP_SIZE_MAX_BITS,
                                  "exceeds",
                                  "DEFAULT_ID_MAPPER_MAP_SIZE_MAX_BITS",
                                  DEFAULT_ID_MAPPER_MAP_SIZE_MAX_BITS);
        }
        if(FILTER_MAP_SIZE_MAX_BITS > DEFAULT_FILTER_MAP_SIZE_MAX_BITS) {
            appendComparisonError("NDD_FILTER_MAP_SIZE_MAX_BITS",
                                  FILTER_MAP_SIZE_MAX_BITS,
                                  "exceeds",
                                  "DEFAULT_FILTER_MAP_SIZE_MAX_BITS",
                                  DEFAULT_FILTER_MAP_SIZE_MAX_BITS);
        }
        if(METADATA_MAP_SIZE_MAX_BITS > DEFAULT_METADATA_MAP_SIZE_MAX_BITS) {
            appendComparisonError("NDD_METADATA_MAP_SIZE_MAX_BITS",
                                  METADATA_MAP_SIZE_MAX_BITS,
                                  "exceeds",
                                  "DEFAULT_METADATA_MAP_SIZE_MAX_BITS",
                                  DEFAULT_METADATA_MAP_SIZE_MAX_BITS);
        }
        if(VECTOR_MAP_SIZE_MAX_BITS > DEFAULT_VECTOR_MAP_SIZE_MAX_BITS) {
            appendComparisonError("NDD_VECTOR_MAP_SIZE_MAX_BITS",
                                  VECTOR_MAP_SIZE_MAX_BITS,
                                  "exceeds",
                                  "DEFAULT_VECTOR_MAP_SIZE_MAX_BITS",
                                  DEFAULT_VECTOR_MAP_SIZE_MAX_BITS);
        }
        if(SPARSE_MAP_SIZE_MAX_BITS > DEFAULT_SPARSE_MAP_SIZE_MAX_BITS) {
            appendComparisonError("NDD_SPARSE_MAP_SIZE_MAX_BITS",
                                  SPARSE_MAP_SIZE_MAX_BITS,
                                  "exceeds",
                                  "DEFAULT_SPARSE_MAP_SIZE_MAX_BITS",
                                  DEFAULT_SPARSE_MAP_SIZE_MAX_BITS);
        }
        return error;
    }

    // Function to get all settings values as a multiline string
    inline std::string getAllSettingsAsString() {
        std::ostringstream oss;
        oss << "\n=== NDD Server ===\n";
        oss << "VERSION: " << VERSION << "\n";
        oss << "SERVER_ID: " << SERVER_ID << "\n";
        oss << "SERVER_PORT: " << SERVER_PORT << "\n";
        oss << "DATA_DIR: " << DATA_DIR << "\n";
        oss << "MAX_ELEMENTS: " << MAX_ELEMENTS << "\n";
        oss << "MAX_ELEMENTS_INCREMENT: " << MAX_ELEMENTS_INCREMENT << "\n";
        oss << "MAX_ELEMENTS_INCREMENT_TRIGGER: " << MAX_ELEMENTS_INCREMENT_TRIGGER << "\n";
        oss << "PREFILTER_CARDINALITY_THRESHOLD: " << PREFILTER_CARDINALITY_THRESHOLD << "\n";
        oss << "NUM_PARALLEL_INSERTS: " << NUM_PARALLEL_INSERTS << "\n";
        oss << "NUM_RECOVERY_THREADS: " << NUM_RECOVERY_THREADS << "\n";
        oss << "ENABLE_DEBUG_LOG: " << (ENABLE_DEBUG_LOG ? "true" : "false") << "\n";
        oss << "AUTH_ENABLED: " << (AUTH_ENABLED ? "true" : "false") << "\n";
        oss << "DEFAULT_USERNAME: " << DEFAULT_USERNAME << "\n";
        oss << "MINIMUM_REQUIRED_DRAM_MB: " << MINIMUM_REQUIRED_DRAM_MB << "\n";
        oss << "MINIMUM_OPEN_FILES: " << MINIMUM_OPEN_FILES << "\n";
        oss << "\n=== MDBX Map Sizes (bit shifts) ===\n";
        oss << "INDEX_META_MAP_SIZE_BITS: " << INDEX_META_MAP_SIZE_BITS << "\n";
        oss << "INDEX_META_MAP_SIZE_MAX_BITS: " << INDEX_META_MAP_SIZE_MAX_BITS << "\n";
        oss << "ID_MAPPER_MAP_SIZE_BITS: " << ID_MAPPER_MAP_SIZE_BITS << "\n";
        oss << "ID_MAPPER_MAP_SIZE_MAX_BITS: " << ID_MAPPER_MAP_SIZE_MAX_BITS << "\n";
        oss << "FILTER_MAP_SIZE_BITS: " << FILTER_MAP_SIZE_BITS << "\n";
        oss << "FILTER_MAP_SIZE_MAX_BITS: " << FILTER_MAP_SIZE_MAX_BITS << "\n";
        oss << "METADATA_MAP_SIZE_BITS: " << METADATA_MAP_SIZE_BITS << "\n";
        oss << "METADATA_MAP_SIZE_MAX_BITS: " << METADATA_MAP_SIZE_MAX_BITS << "\n";
        oss << "VECTOR_MAP_SIZE_BITS: " << VECTOR_MAP_SIZE_BITS << "\n";
        oss << "VECTOR_MAP_SIZE_MAX_BITS: " << VECTOR_MAP_SIZE_MAX_BITS << "\n";
        oss << "SPARSE_MAP_SIZE_MAX_BITS: " << SPARSE_MAP_SIZE_MAX_BITS << "\n";
        oss << "\n=== End Settings ===\n";
        return oss.str();
    }

}  //namespace settings
