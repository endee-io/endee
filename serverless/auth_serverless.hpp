#pragma once
#include "settings_serverless.hpp"
#include "auth_token_cache.hpp"
#include "../third_party/mdbx/mdbx.h"
#include "../third_party/json/nlohmann_json.hpp"
#include <shared_mutex>
#include <chrono>
#include <vector>
#include <optional>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <algorithm>

// Serverless mode: Extended UserType enum (4 tiers)
// OSS "endee" admin user maps to Admin tier in serverless
enum class UserType {
    Starter,    // 1M vectors, 2K dimensions, 3 indices
    Pro,        // 10M vectors, 4K dimensions, 10 indices
    Scale,      // 100M vectors, 8K dimensions, Unlimited indices
    Admin       // Unlimited (same as OSS)
};

// Tier-aware helper functions (replace OSS versions)
inline std::string userTypeToString(UserType type) {
    switch(type) {
        case UserType::Starter:    return "Starter";
        case UserType::Pro:        return "Pro";
        case UserType::Scale:      return "Scale";
        case UserType::Admin:      return "Admin";
    }
    return "Admin";  // fallback
}

inline UserType userTypeFromString(const std::string& type) {
    // Case-insensitive matching
    std::string lower = type;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if(lower == "starter")    return UserType::Starter;
    if(lower == "pro")        return UserType::Pro;
    if(lower == "scale")      return UserType::Scale;
    return UserType::Admin;  // default to admin
}

inline size_t getMaxVectorsPerIndex(UserType type) {
    using namespace settings::serverless;
    switch(type) {
        case UserType::Starter:    return MAX_VECTORS_STARTER;
        case UserType::Pro:        return MAX_VECTORS_PRO;
        case UserType::Scale:      return MAX_VECTORS_SCALE;
        case UserType::Admin:      return MAX_VECTORS_ADMIN;
    }
    return MAX_VECTORS_ADMIN;
}

inline int getMaxAllowedIndices(UserType type) {
    using namespace settings::serverless;
    switch(type) {
        case UserType::Starter:    return STARTER_MAX_INDICES;
        case UserType::Pro:        return PRO_MAX_INDICES;
        case UserType::Scale:      return -1;  // Unlimited
        case UserType::Admin:      return -1;  // Unlimited
    }
    return -1;
}

inline size_t getBloomFilterBits(UserType type) {
    using namespace settings::serverless;
    switch(type) {
        case UserType::Starter:    return BLOOM_FILTER_BITS_STARTER;
        case UserType::Pro:        return BLOOM_FILTER_BITS_PRO;
        case UserType::Scale:      return BLOOM_FILTER_BITS_SCALE;
        case UserType::Admin:      return BLOOM_FILTER_BITS_SCALE;
    }
    return BLOOM_FILTER_BITS_SCALE;
}

inline bool isPrecisionAllowed(UserType type, ndd::quant::QuantizationLevel level) {
    using namespace settings::serverless;
    const std::vector<ndd::quant::QuantizationLevel>* allowed;
    switch(type) {
        case UserType::Starter:    allowed = &ALLOWED_PRECISIONS_STARTER; break;
        case UserType::Pro:        allowed = &ALLOWED_PRECISIONS_PRO; break;
        case UserType::Scale:      allowed = &ALLOWED_PRECISIONS_SCALE; break;
        case UserType::Admin:      return true;
    }
    return std::find(allowed->begin(), allowed->end(), level) != allowed->end();
}

inline std::string getAllowedPrecisionNames(UserType type) {
    using namespace settings::serverless;
    const std::vector<ndd::quant::QuantizationLevel>* allowed;
    switch(type) {
        case UserType::Starter:    allowed = &ALLOWED_PRECISIONS_STARTER; break;
        case UserType::Pro:        allowed = &ALLOWED_PRECISIONS_PRO; break;
        case UserType::Scale:      allowed = &ALLOWED_PRECISIONS_SCALE; break;
        case UserType::Admin:      return "all";
    }
    std::string result;
    for(size_t i = 0; i < allowed->size(); i++) {
        if(i > 0) result += ", ";
        result += ndd::quant::quantLevelToString((*allowed)[i]);
    }
    return result;
}

// Extended User struct
struct User {
    std::string username;
    bool is_active;
    UserType user_type;
    std::chrono::system_clock::time_point created_at;

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["username"] = username;
        j["is_active"] = is_active;
        j["user_type"] = userTypeToString(user_type);
        j["created_at"] = std::chrono::duration_cast<std::chrono::seconds>(
            created_at.time_since_epoch()).count();
        return j;
    }

    static User from_json(const nlohmann::json& j) {
        User u;
        u.username = j["username"];
        u.is_active = j["is_active"];
        u.user_type = userTypeFromString(j["user_type"]);
        auto seconds = j["created_at"].get<int64_t>();
        u.created_at = std::chrono::system_clock::time_point(
            std::chrono::seconds(seconds));
        return u;
    }
};

// Token struct — raw tokens are NEVER stored, only SHA-256 hashes
struct Token {
    std::string hashed_token;  // SHA-256 hash of raw token
    std::string token_name;
    std::string username;
    std::chrono::system_clock::time_point created_at;

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["hashed_token"] = hashed_token;
        j["name"] = token_name;
        j["username"] = username;
        j["created_at"] = std::chrono::duration_cast<std::chrono::seconds>(
            created_at.time_since_epoch()).count();
        return j;
    }

    static Token from_json(const nlohmann::json& j) {
        Token t;
        t.hashed_token = j["hashed_token"];
        t.token_name = j.contains("name") ? j["name"].get<std::string>() :
                       (j.contains("token_name") ? j["token_name"].get<std::string>() : "default");
        t.username = j["username"];
        auto seconds = j["created_at"].get<int64_t>();
        t.created_at = std::chrono::system_clock::time_point(
            std::chrono::seconds(seconds));
        return t;
    }
};

// Extended AuthManager with MDBX storage and SHA-256 token hashing
// Uses 2 DBIs: users (username -> User JSON) and tokens (username:hashed_token -> Token JSON)
class AuthManager {
    std::string base_dir_;
    MDBX_env* env_;
    MDBX_dbi users_dbi_;        // username -> User JSON
    MDBX_dbi tokens_dbi_;       // username:hashed_token -> Token JSON
    TokenCache token_cache_;
    std::string root_token_;
    mutable std::shared_mutex mutex_;

    static const inline std::vector<std::string> RESERVED_KEYWORDS = {
        "user", "index", "auth", "meta", "admin", "root", "api", "system"
    };

    void initMDBX() {
        namespace fs = std::filesystem;
        fs::path auth_path = fs::path(base_dir_) / "auth";
        fs::create_directories(auth_path);

        int rc = mdbx_env_create(&env_);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error("Failed to create MDBX environment: " +
                std::string(mdbx_strerror(rc)));
        }

        rc = mdbx_env_set_maxdbs(env_, 2);
        if(rc != MDBX_SUCCESS) {
            mdbx_env_close(env_);
            throw std::runtime_error("Failed to set max DBIs: " +
                std::string(mdbx_strerror(rc)));
        }

        using namespace settings::serverless;
        size_t lower = 1ULL << AUTH_MAP_SIZE_BITS;
        size_t upper = 1ULL << AUTH_MAP_SIZE_MAX_BITS;
        rc = mdbx_env_set_geometry(env_, lower, lower, upper, -1, -1, -1);
        if(rc != MDBX_SUCCESS) {
            mdbx_env_close(env_);
            throw std::runtime_error("Failed to set geometry: " +
                std::string(mdbx_strerror(rc)));
        }

        rc = mdbx_env_open(env_, auth_path.c_str(),
            MDBX_NOSUBDIR | MDBX_COALESCE | MDBX_LIFORECLAIM, 0664);
        if(rc != MDBX_SUCCESS) {
            mdbx_env_close(env_);
            throw std::runtime_error("Failed to open MDBX environment: " +
                std::string(mdbx_strerror(rc)));
        }

        MDBX_txn* txn;
        rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error("Failed to begin transaction: " +
                std::string(mdbx_strerror(rc)));
        }

        rc = mdbx_dbi_open(txn, "users", MDBX_CREATE, &users_dbi_);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            throw std::runtime_error("Failed to open users DBI");
        }

        rc = mdbx_dbi_open(txn, "tokens", MDBX_CREATE, &tokens_dbi_);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            throw std::runtime_error("Failed to open tokens DBI");
        }

        rc = mdbx_txn_commit(txn);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error("Failed to commit transaction");
        }
    }

    // SHA-256 hash using OpenSSL EVP API
    std::string calculateHash(const std::string& str) {
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        const EVP_MD* md = EVP_sha256();
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int hash_len;

        EVP_DigestInit_ex(ctx, md, nullptr);
        EVP_DigestUpdate(ctx, str.c_str(), str.size());
        EVP_DigestFinal_ex(ctx, hash, &hash_len);
        EVP_MD_CTX_free(ctx);

        std::stringstream ss;
        for(unsigned int i = 0; i < hash_len; i++) {
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
        }
        return ss.str();
    }

    // Generate cryptographically secure random alphanumeric string
    std::string generateRandomString(size_t length) {
        static const char alphanum[] =
            "0123456789"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz";

        std::vector<unsigned char> buffer(length);
        if(RAND_bytes(buffer.data(), static_cast<int>(length)) != 1) {
            throw std::runtime_error("Failed to generate random bytes");
        }

        std::string result;
        result.reserve(length);
        for(size_t i = 0; i < length; i++) {
            result += alphanum[buffer[i] % (sizeof(alphanum) - 1)];
        }
        return result;
    }

    // Delete all tokens for a user (called within an existing write transaction)
    void deleteAllUserTokens(MDBX_txn* txn, const std::string& username) {
        MDBX_cursor* cursor;
        int rc = mdbx_cursor_open(txn, tokens_dbi_, &cursor);
        if(rc != MDBX_SUCCESS) return;

        std::string prefix = username + ":";

        // Collect keys to delete first (avoid cursor invalidation)
        std::vector<std::string> keys_to_delete;
        std::vector<std::string> hashes;

        MDBX_val key, data;
        key.iov_base = (void*)prefix.c_str();
        key.iov_len = prefix.size();

        rc = mdbx_cursor_get(cursor, &key, &data, MDBX_SET_RANGE);
        while(rc == MDBX_SUCCESS) {
            std::string key_str((char*)key.iov_base, key.iov_len);
            if(key_str.substr(0, prefix.size()) != prefix) break;

            keys_to_delete.push_back(key_str);
            hashes.push_back(key_str.substr(prefix.size()));

            rc = mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT);
        }
        mdbx_cursor_close(cursor);

        // Delete from tokens_dbi
        for(const auto& k : keys_to_delete) {
            key.iov_base = (void*)k.c_str();
            key.iov_len = k.size();
            mdbx_del(txn, tokens_dbi_, &key, nullptr);
        }

        // Invalidate cache
        for(const auto& h : hashes) {
            token_cache_.invalidate(h);
        }
    }

    bool isReservedKeyword(const std::string& username) const {
        std::string lower = username;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        return std::find(RESERVED_KEYWORDS.begin(), RESERVED_KEYWORDS.end(), lower)
               != RESERVED_KEYWORDS.end();
    }

    void createUserDirectory(const std::string& username) {
        if(username == "root") return;
        std::filesystem::path user_dir = std::filesystem::path(base_dir_) / username;
        std::filesystem::create_directories(user_dir);
    }

public:
    AuthManager(const std::string& base_dir)
        : base_dir_(base_dir)
        , env_(nullptr)
        , token_cache_(settings::serverless::MAX_TOKENS_IN_CACHE)
        , root_token_(settings::AUTH_TOKEN)
    {
        initMDBX();
        std::cerr << "Enterprise AuthManager initialized at " << base_dir << "/auth/" << std::endl;
        std::cerr << "Root token configured (first " << std::min(8ul, root_token_.size())
                  << " chars): " << root_token_.substr(0, std::min(8ul, root_token_.size()))
                  << "..." << std::endl;
    }

    ~AuthManager() {
        if(env_) {
            mdbx_env_close(env_);
        }
    }

    // ==================== TOKEN VALIDATION (HOT PATH) ====================
    // Client sends raw token "username:random" -> we hash it -> look up hash
    std::string validateToken(const std::string& raw_token) {
        // 1. Check root token first (admin access, not hashed)
        if(raw_token == root_token_) {
            return "root";
        }

        // 2. Parse username from raw token (format: username:random)
        size_t pos = raw_token.find(':');
        if(pos == std::string::npos) {
            return "";
        }

        std::string username = raw_token.substr(0, pos);
        std::string hashed_token = calculateHash(raw_token);

        // 3. Check cache (keyed by hashed token)
        auto cached = token_cache_.get(hashed_token);
        if(cached) {
            return *cached;
        }

        // 4. Check MDBX (key = username:hashed_token)
        std::string db_key = username + ":" + hashed_token;

        std::shared_lock lock(mutex_);
        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &txn);
        if(rc != MDBX_SUCCESS) {
            return "";
        }

        MDBX_val key, data;
        key.iov_base = (void*)db_key.c_str();
        key.iov_len = db_key.size();

        rc = mdbx_get(txn, tokens_dbi_, &key, &data);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            return "";
        }

        mdbx_txn_abort(txn);

        // Token found — cache it
        token_cache_.put(hashed_token, username);
        return username;
    }

    // ==================== USER MANAGEMENT ====================

    bool createUser(const std::string& username, UserType type) {
        // Validate username
        if(username.empty()) return false;
        if(username.length() < 3 || username.length() > 32) return false;

        for(char c : username) {
            if(!std::isalnum(c) && c != '_') return false;
        }

        if(isReservedKeyword(username)) return false;

        std::unique_lock lock(mutex_);
        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
        if(rc != MDBX_SUCCESS) {
            return false;
        }

        // Check if user already exists
        MDBX_val key, data;
        key.iov_base = (void*)username.c_str();
        key.iov_len = username.size();
        rc = mdbx_get(txn, users_dbi_, &key, &data);
        if(rc == MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            return false;  // User already exists
        }

        // Create new user
        User user;
        user.username = username;
        user.is_active = true;
        user.user_type = type;
        user.created_at = std::chrono::system_clock::now();

        std::string json_str = user.to_json().dump();
        data.iov_base = (void*)json_str.c_str();
        data.iov_len = json_str.size();

        rc = mdbx_put(txn, users_dbi_, &key, &data, MDBX_UPSERT);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            return false;
        }

        rc = mdbx_txn_commit(txn);
        if(rc != MDBX_SUCCESS) {
            return false;
        }

        // Create user data directory
        try {
            createUserDirectory(username);
        } catch(const std::exception& e) {
            std::cerr << "Warning: Failed to create user directory: " << e.what() << std::endl;
        }

        std::cerr << "User created: " << username << " (type: "
                  << userTypeToString(type) << ")" << std::endl;
        return true;
    }

    bool deleteUser(const std::string& username) {
        std::unique_lock lock(mutex_);
        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
        if(rc != MDBX_SUCCESS) {
            return false;
        }

        // Check user exists
        MDBX_val key;
        key.iov_base = (void*)username.c_str();
        key.iov_len = username.size();
        MDBX_val data;
        rc = mdbx_get(txn, users_dbi_, &key, &data);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            return false;
        }

        // Delete all user tokens first
        deleteAllUserTokens(txn, username);

        // Delete user record
        rc = mdbx_del(txn, users_dbi_, &key, nullptr);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            return false;
        }

        rc = mdbx_txn_commit(txn);
        if(rc == MDBX_SUCCESS) {
            std::cerr << "User deleted: " << username << std::endl;
        }
        return rc == MDBX_SUCCESS;
    }

    bool deactivateUser(const std::string& username) {
        auto user_opt = getUser(username);
        if(!user_opt) {
            return false;
        }

        std::unique_lock lock(mutex_);
        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
        if(rc != MDBX_SUCCESS) {
            return false;
        }

        // Delete all tokens (deactivated user can't authenticate)
        deleteAllUserTokens(txn, username);

        // Set user inactive
        user_opt->is_active = false;
        std::string json_str = user_opt->to_json().dump();

        MDBX_val key, data;
        key.iov_base = (void*)username.c_str();
        key.iov_len = username.size();
        data.iov_base = (void*)json_str.c_str();
        data.iov_len = json_str.size();

        rc = mdbx_put(txn, users_dbi_, &key, &data, MDBX_UPSERT);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            return false;
        }

        rc = mdbx_txn_commit(txn);
        return rc == MDBX_SUCCESS;
    }

    bool activateUser(const std::string& username) {
        auto user_opt = getUser(username);
        if(!user_opt) {
            return false;
        }

        std::unique_lock lock(mutex_);
        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
        if(rc != MDBX_SUCCESS) {
            return false;
        }

        user_opt->is_active = true;
        std::string json_str = user_opt->to_json().dump();

        MDBX_val key, data;
        key.iov_base = (void*)username.c_str();
        key.iov_len = username.size();
        data.iov_base = (void*)json_str.c_str();
        data.iov_len = json_str.size();

        rc = mdbx_put(txn, users_dbi_, &key, &data, MDBX_UPSERT);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            return false;
        }

        rc = mdbx_txn_commit(txn);
        return rc == MDBX_SUCCESS;
    }

    bool setUserType(const std::string& username, UserType type) {
        auto user_opt = getUser(username);
        if(!user_opt) {
            return false;
        }

        std::unique_lock lock(mutex_);
        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
        if(rc != MDBX_SUCCESS) {
            return false;
        }

        user_opt->user_type = type;
        std::string json_str = user_opt->to_json().dump();

        MDBX_val key, data;
        key.iov_base = (void*)username.c_str();
        key.iov_len = username.size();
        data.iov_base = (void*)json_str.c_str();
        data.iov_len = json_str.size();

        rc = mdbx_put(txn, users_dbi_, &key, &data, MDBX_UPSERT);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            return false;
        }

        rc = mdbx_txn_commit(txn);
        return rc == MDBX_SUCCESS;
    }

    std::optional<User> getUser(const std::string& username) {
        std::shared_lock lock(mutex_);
        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &txn);
        if(rc != MDBX_SUCCESS) {
            return std::nullopt;
        }

        MDBX_val key, data;
        key.iov_base = (void*)username.c_str();
        key.iov_len = username.size();

        rc = mdbx_get(txn, users_dbi_, &key, &data);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            return std::nullopt;
        }

        std::string json_str((char*)data.iov_base, data.iov_len);
        mdbx_txn_abort(txn);

        auto j = nlohmann::json::parse(json_str);
        return User::from_json(j);
    }

    std::optional<UserType> getUserType(const std::string& username) {
        if(username == "root") {
            return UserType::Admin;
        }

        auto user = getUser(username);
        if(!user || !user->is_active) {
            return std::nullopt;
        }
        return user->user_type;
    }

    std::vector<User> listUsers() {
        std::shared_lock lock(mutex_);
        std::vector<User> users;

        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &txn);
        if(rc != MDBX_SUCCESS) {
            return users;
        }

        MDBX_cursor* cursor;
        rc = mdbx_cursor_open(txn, users_dbi_, &cursor);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            return users;
        }

        MDBX_val key, data;
        while(mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT) == MDBX_SUCCESS) {
            try {
                std::string json_str((char*)data.iov_base, data.iov_len);
                auto j = nlohmann::json::parse(json_str);
                users.push_back(User::from_json(j));
            } catch(const std::exception& e) {
                std::cerr << "Error parsing user data: " << e.what() << std::endl;
            }
        }

        mdbx_cursor_close(cursor);
        mdbx_txn_abort(txn);
        return users;
    }

    std::optional<nlohmann::json> getUserInfo(const std::string& req_user,
                                              const std::string& target_user) {
        if(req_user != target_user && !isAdmin(req_user)) {
            return std::nullopt;
        }

        auto user = getUser(target_user);
        if(!user) {
            return std::nullopt;
        }

        nlohmann::json info;
        info["username"] = user->username;
        info["user_type"] = userTypeToString(user->user_type);
        info["is_active"] = user->is_active;
        info["created_at"] = std::chrono::duration_cast<std::chrono::seconds>(
            user->created_at.time_since_epoch()).count();

        auto tokens = listUserTokens(user->username);
        info["token_count"] = tokens.size();

        return info;
    }

    // ==================== TOKEN MANAGEMENT ====================
    // SHA-256 hashing: raw token never stored.
    // Single DBI: tokens_dbi_ (key = username:hashed_token -> Token JSON)
    // Name-based operations (list, delete by name, duplicate check) scan with prefix.

    std::string generateToken(const std::string& username, const std::string& token_name = "default") {
        // Verify user exists
        auto user = getUser(username);
        if(!user) {
            return "";
        }

        // Check if token_name already exists by scanning tokens_dbi
        {
            std::shared_lock rlock(mutex_);
            MDBX_txn* rtxn;
            int rrc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &rtxn);
            if(rrc == MDBX_SUCCESS) {
                MDBX_cursor* cursor;
                rrc = mdbx_cursor_open(rtxn, tokens_dbi_, &cursor);
                if(rrc == MDBX_SUCCESS) {
                    std::string prefix = username + ":";
                    MDBX_val k, d;
                    k.iov_base = (void*)prefix.c_str();
                    k.iov_len = prefix.size();
                    rrc = mdbx_cursor_get(cursor, &k, &d, MDBX_SET_RANGE);
                    while(rrc == MDBX_SUCCESS) {
                        std::string key_str((char*)k.iov_base, k.iov_len);
                        if(key_str.substr(0, prefix.size()) != prefix) break;
                        try {
                            std::string json_str((char*)d.iov_base, d.iov_len);
                            auto j = nlohmann::json::parse(json_str);
                            Token t = Token::from_json(j);
                            if(t.token_name == token_name) {
                                mdbx_cursor_close(cursor);
                                mdbx_txn_abort(rtxn);
                                return "";  // Token name already exists
                            }
                        } catch(...) {}
                        rrc = mdbx_cursor_get(cursor, &k, &d, MDBX_NEXT);
                    }
                    mdbx_cursor_close(cursor);
                }
                mdbx_txn_abort(rtxn);
            }
        }

        // Generate random string and construct raw token
        std::string random = generateRandomString(settings::serverless::TOKEN_LENGTH);
        std::string raw_token = username + ":" + random;
        std::string hashed_token = calculateHash(raw_token);

        // DB key for tokens_dbi: username:hashed_token
        std::string db_key = username + ":" + hashed_token;

        // Create Token object (stores hash, never raw)
        Token token;
        token.hashed_token = hashed_token;
        token.token_name = token_name;
        token.username = username;
        token.created_at = std::chrono::system_clock::now();

        std::string json_str = token.to_json().dump();

        std::unique_lock lock(mutex_);
        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
        if(rc != MDBX_SUCCESS) {
            return "";
        }

        // Store in tokens_dbi: key = username:hashed_token -> Token JSON
        MDBX_val key, data;
        key.iov_base = (void*)db_key.c_str();
        key.iov_len = db_key.size();
        data.iov_base = (void*)json_str.c_str();
        data.iov_len = json_str.size();

        rc = mdbx_put(txn, tokens_dbi_, &key, &data, MDBX_UPSERT);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            return "";
        }

        rc = mdbx_txn_commit(txn);
        if(rc != MDBX_SUCCESS) {
            return "";
        }

        // Cache: hashed_token -> username
        token_cache_.put(hashed_token, username);

        // Return raw token to client (only time it's visible)
        return raw_token;
    }

    bool deleteToken(const std::string& username, const std::string& token_name) {
        std::string target_key;
        std::string target_hash;

        // Find the token by scanning tokens_dbi for matching name
        {
            std::shared_lock rlock(mutex_);
            MDBX_txn* rtxn;
            int rrc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &rtxn);
            if(rrc != MDBX_SUCCESS) {
                return false;
            }

            MDBX_cursor* cursor;
            rrc = mdbx_cursor_open(rtxn, tokens_dbi_, &cursor);
            if(rrc != MDBX_SUCCESS) {
                mdbx_txn_abort(rtxn);
                return false;
            }

            std::string prefix = username + ":";
            MDBX_val k, d;
            k.iov_base = (void*)prefix.c_str();
            k.iov_len = prefix.size();
            rrc = mdbx_cursor_get(cursor, &k, &d, MDBX_SET_RANGE);
            while(rrc == MDBX_SUCCESS) {
                std::string key_str((char*)k.iov_base, k.iov_len);
                if(key_str.substr(0, prefix.size()) != prefix) break;
                try {
                    std::string json_str((char*)d.iov_base, d.iov_len);
                    auto j = nlohmann::json::parse(json_str);
                    Token t = Token::from_json(j);
                    if(t.token_name == token_name) {
                        target_key = key_str;
                        target_hash = t.hashed_token;
                        break;
                    }
                } catch(...) {}
                rrc = mdbx_cursor_get(cursor, &k, &d, MDBX_NEXT);
            }
            mdbx_cursor_close(cursor);
            mdbx_txn_abort(rtxn);
        }

        if(target_key.empty()) {
            return false;
        }

        std::unique_lock lock(mutex_);
        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
        if(rc != MDBX_SUCCESS) {
            return false;
        }

        MDBX_val key;
        key.iov_base = (void*)target_key.c_str();
        key.iov_len = target_key.size();
        mdbx_del(txn, tokens_dbi_, &key, nullptr);

        token_cache_.invalidate(target_hash);

        rc = mdbx_txn_commit(txn);
        return rc == MDBX_SUCCESS;
    }

    std::vector<Token> listUserTokens(const std::string& username) {
        std::shared_lock lock(mutex_);
        std::vector<Token> tokens;

        MDBX_txn* txn;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &txn);
        if(rc != MDBX_SUCCESS) {
            return tokens;
        }

        MDBX_cursor* cursor;
        rc = mdbx_cursor_open(txn, tokens_dbi_, &cursor);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            return tokens;
        }

        std::string prefix = username + ":";
        MDBX_val key, data;
        key.iov_base = (void*)prefix.c_str();
        key.iov_len = prefix.size();

        rc = mdbx_cursor_get(cursor, &key, &data, MDBX_SET_RANGE);
        while(rc == MDBX_SUCCESS) {
            std::string key_str((char*)key.iov_base, key.iov_len);
            if(key_str.substr(0, prefix.size()) != prefix) {
                break;
            }

            try {
                std::string json_str((char*)data.iov_base, data.iov_len);
                auto j = nlohmann::json::parse(json_str);
                tokens.push_back(Token::from_json(j));
            } catch(const std::exception& e) {
                std::cerr << "Error parsing token data: " << e.what() << std::endl;
            }

            rc = mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT);
        }

        mdbx_cursor_close(cursor);
        mdbx_txn_abort(txn);
        return tokens;
    }

    // ==================== ADMIN CHECK ====================

    bool isAdmin(const std::string& username) {
        if(username == "root") {
            return true;
        }
        auto user_type = getUserType(username);
        return user_type && *user_type == UserType::Admin;
    }
};
