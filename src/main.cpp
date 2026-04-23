// System includes
#include <iostream>
#include <filesystem>

// ASIO/Crow related
#include "crow/query_string.h"
#include "crow/http_parser_merged.h"
#include "crow/ci_map.h"
#include "crow/TinySHA1.hpp"
#include "crow/settings.h"
#include "crow/socket_adaptors.h"
#include "crow/json.h"
#include "crow/mustache.h"
#include "crow/logging.h"
#include "crow/task_timer.h"
#include "crow/utility.h"
#include "crow/common.h"
#include "crow/http_request.h"
#include "crow/websocket.h"
#include "crow/parser.h"
#include "crow/http_response.h"
#include "crow/multipart.h"
#include "crow/multipart_view.h"
#include "crow/routing.h"
#include "crow/middleware.h"
#include "crow/middleware_context.h"
#include "crow/compression.h"
#include "crow/http_connection.h"
#include "crow/http_server.h"
#include "crow/app.h"

// local includes
#include "settings.hpp"
#include "mdbx/mdbx.h"
#include "json/nlohmann_json.hpp"
#include "sparse/inverted_index.hpp"
#include "core/ndd.hpp"
#include "auth.hpp"
#include "quant/common.hpp"
#include "system_sanity/system_sanity.hpp"

using ndd::quant::quantLevelToString;
using ndd::quant::stringToQuantLevel;

// Authentication middleware for open-source mode
// If NDD_AUTH_TOKEN is set: token is required
// If NDD_AUTH_TOKEN is not set: all requests are allowed
struct AuthMiddleware : crow::ILocalMiddleware {
    AuthManager& auth_manager;

    AuthMiddleware(AuthManager& am) :
        auth_manager(am) {}

    struct context {
        std::string username;
    };

    void before_handle(crow::request& req, crow::response& res, context& ctx) {
        ctx.username = settings::DEFAULT_USERNAME;  // Single configured username in OSS mode

        if(!settings::AUTH_ENABLED) {
            return;  // No auth required - open mode
        }

        // Auth is enabled - token is REQUIRED
        auto auth_header = req.get_header_value("Authorization");
        if(auth_header.empty()) {
            LOG_WARN(1001, ctx.username, "Rejected request without Authorization header");
            res.code = 401;
            res.write("Authorization header required");
            res.end();
            return;
        }

        if(auth_header != settings::AUTH_TOKEN) {
            LOG_WARN(1002, ctx.username, "Rejected request with invalid Authorization header");
            res.code = 401;
            res.write("Invalid token");
            res.end();
            return;
        }
    }

    void after_handle(crow::request&, crow::response&, context&) {}
};
// Helper function to send error messages in JSON format
inline crow::response json_error(int code, const std::string& message) {
    crow::json::wvalue err_json({{"error", message}});
    return crow::response(code, err_json.dump());
}
// Special helper function to log and send error messages in JSON format for 500 errors
inline crow::response json_error_500(const std::string& username,
                                     const std::string& index_name,
                                     const std::string& path,
                                     const std::string& message) {
    LOG_ERROR(1003, username, index_name, "500 error on " << path << ": " << message);

    crow::json::wvalue err_json({{"error", message}});
    return crow::response(500, err_json.dump());
}

inline crow::response
json_error_500(const std::string& username, const std::string& path, const std::string& message) {
    return json_error_500(username, "-", path, message);
}

inline crow::response json_response(const nlohmann::ordered_json& payload, int code = 200) {
    crow::response res(code);
    res.set_header("Content-Type", "application/json");
    res.body = payload.dump();
    return res;
}

inline nlohmann::ordered_json make_index_list_item(const std::string& index_name,
                                                   const IndexMetadata& metadata) {
    nlohmann::ordered_json item = nlohmann::ordered_json::object();
    item["name"] = index_name;
    item["total_elements"] = static_cast<int64_t>(metadata.total_elements);
    item["dimension"] = static_cast<int64_t>(metadata.dimension);
    item["sparse_model"] = ndd::sparseScoringModelToString(metadata.sparse_model);
    item["space_type"] = metadata.space_type_str;
    item["precision"] = quantLevelToString(metadata.quant_level);
    item["checksum"] = metadata.checksum;
    item["M"] = static_cast<int64_t>(metadata.M);
    item["created_at"] =
            static_cast<int64_t>(std::chrono::system_clock::to_time_t(metadata.created_at));
    return item;
}

inline nlohmann::ordered_json make_index_info_payload(const IndexInfo& info) {
    nlohmann::ordered_json payload = nlohmann::ordered_json::object();
    payload["total_elements"] = static_cast<int64_t>(info.total_elements);
    payload["dimension"] = static_cast<int64_t>(info.dimension);
    payload["sparse_model"] = ndd::sparseScoringModelToString(info.sparse_model);
    payload["space_type"] = info.space_type_str;
    payload["precision"] = quantLevelToString(info.quant_level);
    payload["checksum"] = info.checksum;
    payload["M"] = static_cast<int64_t>(info.M);
    payload["ef_con"] = static_cast<int64_t>(info.ef_con);
    payload["lib_token"] = settings::DEFAULT_LIB_TOKEN;
    return payload;
}

// Read file contents
std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if(!file.is_open()) {
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Get MIME type based on file extension
std::string get_mime_type(const std::string& path) {
    if(path.ends_with(".html")) {
        return "text/html";
    }
    if(path.ends_with(".css")) {
        return "text/css";
    }
    if(path.ends_with(".js")) {
        return "application/javascript";
    }
    if(path.ends_with(".json")) {
        return "application/json";
    }
    if(path.ends_with(".png")) {
        return "image/png";
    }
    if(path.ends_with(".jpg") || path.ends_with(".jpeg")) {
        return "image/jpeg";
    }
    if(path.ends_with(".svg")) {
        return "image/svg+xml";
    }
    if(path.ends_with(".ico")) {
        return "image/x-icon";
    }
    if(path.ends_with(".woff")) {
        return "font/woff";
    }
    if(path.ends_with(".woff2")) {
        return "font/woff2";
    }
    if(path.ends_with(".ttf")) {
        return "font/ttf";
    }
    if(path.ends_with(".map")) {
        return "application/json";
    }
    return "application/octet-stream";
}

// Check if file exists
bool file_exists(const std::string& path) {
    return std::filesystem::exists(path) && std::filesystem::is_regular_file(path);
}

int main(int argc, char** argv) {

    const std::string settings_error = settings::validateSettings();
    if(!settings_error.empty()) {
        LOG_ERROR(1065, "Server startup aborted: " << settings_error);
        return 1;
    }

    if(!run_startup_sanity_checks()) {
        LOG_ERROR(1799, "Server startup aborted due to failed sanity checks");
        return 1;
    }

    LOG_INFO("SERVER_ID: " << settings::SERVER_ID);
    LOG_INFO("SERVER_PORT: " << settings::SERVER_PORT);
    LOG_INFO("DATA_DIR: " << settings::DATA_DIR);
    LOG_INFO("NUM_PARALLEL_INSERTS: " << settings::NUM_PARALLEL_INSERTS);
    LOG_INFO("NUM_RECOVERY_THREADS: " << settings::NUM_RECOVERY_THREADS);
    LOG_INFO("ENABLE_DEBUG_LOG: " << settings::ENABLE_DEBUG_LOG);
    LOG_INFO("AUTH_TOKEN: " << settings::AUTH_TOKEN);
    LOG_INFO("AUTH_ENABLED: " << settings::AUTH_ENABLED);
    LOG_INFO("DEFAULT_USERNAME: " << settings::DEFAULT_USERNAME);
    LOG_INFO("DEFAULT_SERVER_TYPE: " << settings::DEFAULT_SERVER_TYPE);
    LOG_INFO("DEFAULT_DATA_DIR: " << settings::DEFAULT_DATA_DIR);
    LOG_INFO("DEFAULT_MAX_ELEMENTS: " << settings::DEFAULT_MAX_ELEMENTS);
    LOG_INFO("DEFAULT_MAX_ELEMENTS_INCREMENT: " << settings::DEFAULT_MAX_ELEMENTS_INCREMENT);
    LOG_INFO("DEFAULT_MAX_ELEMENTS_INCREMENT_TRIGGER: "
                << settings::DEFAULT_MAX_ELEMENTS_INCREMENT_TRIGGER);
    LOG_INFO("MINIMUM_REQUIRED_DRAM_MB: " << settings::MINIMUM_REQUIRED_DRAM_MB);

    // Path to React build directory
    // Get the executable's directory and resolve frontend/dist relative to it
    // Get executable directory using argv[0] (cross-platform)
    std::filesystem::path exe_path = std::filesystem::canonical(argv[0]).parent_path();
    const std::string BUILD_DIR = (exe_path / "../frontend/dist").string();
    const std::string INDEX_PATH = BUILD_DIR + "/index.html";

    // Initialize index manager with persistence config
    std::string data_dir = settings::DATA_DIR;

    PersistenceConfig persistence_config{
            settings::SAVE_EVERY_N_UPDATES,                        // Save every n updates
            std::chrono::minutes(settings::SAVE_EVERY_N_MINUTES),  // Save every n minutes
            true                                                   // Save on shutdown
    };
    // Initialize auth manager and user manager
    LOG_INFO(1005, "Starting the server");
    AuthManager auth_manager(data_dir);
    LOG_INFO(1006, "Created auth manager");
    IndexManager index_manager(data_dir, persistence_config);
    LOG_INFO(1007, "Created index manager");

    // Initialize the app
    crow::App<AuthMiddleware> app{AuthMiddleware(auth_manager)};

    // ========== GENERAL ==========
    // Health check endpoint (no auth required)
    // CROW_ROUTE(app, "/api/v1/health").methods("GET"_method)([](const crow::request& req) {
    CROW_ROUTE(app, "/api/v1/health").methods("GET"_method)([]() {
        crow::json::wvalue response(
                {{"status", "ok"},
                {"timestamp", (std::int64_t)std::chrono::system_clock::now().time_since_epoch().count()}});
        PRINT_LOG_TIME();
        ndd::printSparseSearchDebugStats();
        ndd::printSparseUpdateDebugStats();
        print_mdbx_stats();
        return crow::response(200, response.dump());
    });

    // ========= USER ENDPOINTS ==========
    // Get user info for the configured single user
    CROW_ROUTE(app, "/api/v1/users/<string>/info")
            .CROW_MIDDLEWARES(app, AuthMiddleware)
            .methods("GET"_method)([&auth_manager, &app](const crow::request& req,
                                                         const std::string& target_username) {
                auto& ctx = app.get_context<AuthMiddleware>(req);

                try {
                    auto user_info = auth_manager.getUserInfo(ctx.username, target_username);
                    if(!user_info) {
                        return json_error(404, "User not found");
                    }

                    return crow::response(200, user_info->dump());
                } catch(const std::exception& e) {
                    return json_error_500(ctx.username, req.url, std::string("Error: ") + e.what());
                }
            });

    // Get user type - always returns Admin in open-source mode
    CROW_ROUTE(app, "/api/v1/users/<string>/type")
            .CROW_MIDDLEWARES(app, AuthMiddleware)
            .methods("GET"_method)([&auth_manager, &app](const crow::request& req,
                                                         const std::string& target_username) {
                auto& ctx = app.get_context<AuthMiddleware>(req);

                try {
                    auto user_type = auth_manager.getUserType(target_username);
                    if(!user_type) {
                        return json_error(404, "User not found");
                    }

                    crow::json::wvalue response(
                            {{"username", settings::DEFAULT_USERNAME}, {"user_type", "Admin"}});

                    return crow::response(200, response.dump());
                } catch(const std::exception& e) {
                    return json_error_500(ctx.username, req.url, std::string("Error: ") + e.what());
                }
            });

    CROW_ROUTE(app, "/api/v1/stats").methods("GET"_method)([](const crow::request& req) {
        crow::json::wvalue response(
                {{"version", settings::VERSION}, {"uptime", 0}, {"total_requests", 0}});
        return crow::response(200, response.dump());
    });

    // Create index
    CROW_ROUTE(app, "/api/v1/index/create")
            .CROW_MIDDLEWARES(app, AuthMiddleware)
            .methods("POST"_method)([&index_manager, &app](const crow::request& req) {
                auto& ctx = app.get_context<AuthMiddleware>(req);

                LOG_DEBUG(" Request Body: " << req.body);

                auto body = crow::json::load(req.body);

                if(!body) {
                    LOG_WARN(1011, ctx.username, "Create-index request contained invalid JSON");
                    return json_error(400, "Invalid JSON");
                }

                if(!body.has("index_name") || !body.has("dim") || !body.has("space_type")) {
                    LOG_WARN(1012, ctx.username, "Create-index request is missing required parameters");
                    return json_error(400, "Missing required parameters");
                }

                // Format index_id as username/index_name
                std::string index_id = ctx.username + "/" + std::string(body["index_name"].s());

                // Get checksum (optional, for queryable encryption)
                int32_t checksum = body.has("checksum") ? body["checksum"].i() : -1;
                LOG_DEBUG("Checksum: " << checksum);

                size_t dim = (size_t)body["dim"].i();
                // Validate dimension
                if(dim < settings::MIN_DIMENSION || dim > settings::MAX_DIMENSION) {
                    LOG_WARN(1013, index_id, "Invalid dimension: " << dim);
                    return json_error(400,
                                      "Dimension must be between "
                                              + std::to_string(settings::MIN_DIMENSION) + " and "
                                              + std::to_string(settings::MAX_DIMENSION));
                }

                // Validate M
                size_t m = body.has("M") ? (size_t)body["M"].i() : settings::DEFAULT_M;
                if(m < settings::MIN_M || m > settings::MAX_M) {
                    LOG_WARN(1014, index_id, "Invalid M: " << m);
                    return json_error(400,
                                      "M must be between " + std::to_string(settings::MIN_M)
                                              + " and " + std::to_string(settings::MAX_M));
                }

                // Validate ef_con
                size_t ef_con = body.has("ef_con") ? (size_t)body["ef_con"].i()
                                                   : settings::DEFAULT_EF_CONSTRUCT;
                if(ef_con < settings::MIN_EF_CONSTRUCT || ef_con > settings::MAX_EF_CONSTRUCT) {
                    LOG_WARN(1015, index_id, "Invalid ef_construction: " << ef_con);
                    return json_error(400,
                                      "ef_con must be between "
                                              + std::to_string(settings::MIN_EF_CONSTRUCT) + " and "
                                              + std::to_string(settings::MAX_EF_CONSTRUCT));
                }

                // Get quantization level (default to INT16)
                std::string precision = body.has("precision") ? std::string(body["precision"].s()) : "int16";

                if(precision == "int8d") {
                    precision = "int8";
                } else if(precision == "int16d") {
                    precision = "int16";
                }

                ndd::quant::QuantizationLevel quant_level = stringToQuantLevel(precision);

                // Validate quantization level
                if(quant_level == ndd::quant::QuantizationLevel::UNKNOWN) {
                    LOG_WARN(1016, index_id, "Invalid precision: " << body["precision"].s());
                    std::vector<std::string> names = ndd::quant::getAvailableQuantizationNames();
                    std::string names_str;
                    for(size_t i = 0; i < names.size(); ++i) {
                        names_str += names[i];
                        if(i < names.size() - 1) {
                            names_str += ", ";
                        }
                    }
                    return json_error(400, "Invalid precision. Must be one of: " + names_str);
                }

                // Get custom size in millions (optional)
                size_t size_in_millions = 0;
                if(body.has("size_in_millions")) {
                    size_in_millions = static_cast<size_t>(body["size_in_millions"].i());
                    if(size_in_millions == 0 || size_in_millions > 10000) {  // Cap at 10B vectors
                        LOG_WARN(1017,
                                       index_id,
                                       "Invalid custom size_in_millions: " << size_in_millions);
                        return json_error(400, "size_in_millions must be between 1 and 10000");
                    }
                    LOG_INFO(1018, index_id, "Creating index with custom size: " << size_in_millions << "M vectors");
                }

                if(body.has("sparse_dim")) {
                    LOG_WARN(1019,
                             index_id,
                             "Create-index request used legacy sparse fields");
                    return json_error(
                        400,
                        "Legacy sparse fields are not supported. Use sparse_model with one of: "
                        "None, default, endee_bm25");
                }

                const std::string sparse_model_str =
                        body.has("sparse_model") ? std::string(body["sparse_model"].s()) : "None";
                const auto sparse_model = ndd::sparseScoringModelFromString(sparse_model_str);
                if(!sparse_model.has_value()) {
                    LOG_WARN(1025, index_id, "Invalid sparse_model: " << sparse_model_str);
                    return json_error(
                        400,
                        "Invalid sparse_model. Must be one of: None, default, endee_bm25");
                }

                IndexConfig config{dim,
                                   *sparse_model,
                                   settings::MAX_ELEMENTS,  // max elements
                                   body["space_type"].s(),
                                   m,
                                   ef_con,
                                   quant_level,
                                   checksum};

                try {
                    // Pass the full index_id to index_manager using the Admin user type
                    index_manager.createIndex(index_id, config, UserType::Admin, size_in_millions);
                    return crow::response(200, "Index created successfully");
                } catch(const std::runtime_error& e) {
                    LOG_WARN(1026, index_id, "Create-index request failed: " << e.what());
                    return json_error(409, e.what());
                } catch(const std::exception& e) {
                    return json_error_500(
                            ctx.username, body["index_name"].s(), req.url, std::string("Error: ") + e.what());
                }
            });

    // Create Backup
    CROW_ROUTE(app, "/api/v1/index/<string>/backup")
            .CROW_MIDDLEWARES(app, AuthMiddleware)
            .methods("POST"_method)([&index_manager, &app](const crow::request& req,
                                                           const std::string& index_name) {
                auto& ctx = app.get_context<AuthMiddleware>(req);
                auto body = crow::json::load(req.body);

                if(!body || !body.has("name")) {
                    LOG_WARN(1020, ctx.username, index_name, "Create-backup request missing backup name");
                    return json_error(400, "Missing backup name");
                }

                std::string backup_name = body["name"].s();
                std::string index_id = ctx.username + "/" + index_name;

                try {
                    std::pair<bool, std::string> result =
                            index_manager.createBackupAsync(index_id, backup_name);
                    if(!result.first) {
                        LOG_WARN(1021, ctx.username, index_name, "Create-backup request rejected: " << result.second);
                        return json_error(400, result.second);
                    }

                    // Return 202 Accepted with backup_name as job_id
                    crow::json::wvalue response;
                    response["backup_name"] = result.second;
                    response["status"] = "in_progress";
                    return crow::response(202, response.dump());
                } catch(const std::exception& e) {
                    return json_error_500(ctx.username, index_name, req.url, e.what());
                }
            });

    // List Backups
    CROW_ROUTE(app, "/api/v1/backups")
            .CROW_MIDDLEWARES(app, AuthMiddleware)
            .methods("GET"_method)([&index_manager, &app](const crow::request& req) {
                auto& ctx = app.get_context<AuthMiddleware>(req);
                try {
                    auto backups = index_manager.listBackups(ctx.username);
                    nlohmann::json result_json = backups;
                    crow::response res;
                    res.code = 200;
                    res.set_header("Content-Type", "application/json");
                    res.body = result_json.dump();
                    return res;
                } catch(const std::exception& e) {
                    return json_error_500(ctx.username, req.url, e.what());
                }
            });

    // Restore Backup
    CROW_ROUTE(app, "/api/v1/backups/<string>/restore")
            .CROW_MIDDLEWARES(app, AuthMiddleware)
            .methods("POST"_method)([&index_manager, &app](const crow::request& req,
                                                           const std::string& backup_name) {
                auto& ctx = app.get_context<AuthMiddleware>(req);
                auto body = crow::json::load(req.body);

                if(!body || !body.has("target_index_name")) {
                    LOG_WARN(1022, ctx.username, "Restore-backup request missing target index name");
                    return json_error(400, "Missing target_index_name");
                }

                std::string target_index_name = body["target_index_name"].s();

                try {
                    std::pair<bool, std::string> result =
                            index_manager.restoreBackup(backup_name, target_index_name, ctx.username);
                    if(!result.first) {
                        LOG_WARN(1023, ctx.username, target_index_name, "Restore-backup request rejected: " << result.second);
                        return json_error(400, result.second);
                    }
                    return crow::response(201, "Backup restored successfully");
                } catch(const std::exception& e) {
                    return json_error_500(ctx.username, target_index_name, req.url, e.what());
                }
            });

    // Delete Backup
    CROW_ROUTE(app, "/api/v1/backups/<string>")
            .CROW_MIDDLEWARES(app, AuthMiddleware)
            .methods("DELETE"_method)([&index_manager, &app](const crow::request& req,
                                                             const std::string& backup_name) {
                auto& ctx = app.get_context<AuthMiddleware>(req);
                try {
                    std::pair<bool, std::string> result = index_manager.deleteBackup(backup_name, ctx.username);
                    if(!result.first) {
                        LOG_WARN(1024, ctx.username, "Delete-backup request rejected: " << result.second);
                        return json_error(400, result.second);
                    }
                    return crow::response(204, "Backup deleted successfully");
                } catch(const std::exception& e) {
                    return json_error_500(ctx.username, req.url, e.what());
                }
            });

    // Download Backup - accepts auth token via query param or works without auth if disabled
    CROW_ROUTE(app, "/api/v1/backups/<string>/download")
            .methods("GET"_method)([&](const crow::request& req, const std::string& backup_name) {
                try {
                    if(settings::AUTH_ENABLED) {
                        std::string token =
                                req.url_params.get("token") ? req.url_params.get("token") : "";
                        if(token != settings::AUTH_TOKEN) {
                            LOG_WARN(1057, "Rejected backup download request with invalid token");
                            return json_error(401, "Unauthorized");
                        }
                    }

                    std::string backup_file =
                            settings::DATA_DIR + "/backups/" + settings::DEFAULT_USERNAME + "/" + backup_name + ".tar";

                    if(!std::filesystem::exists(backup_file)) {
                        LOG_WARN(1058, settings::DEFAULT_USERNAME, "Backup download requested for missing backup " << backup_name);
                        return json_error(404, "Backup not found");
                    }


                    crow::response response;
                    response.set_static_file_info_unsafe(backup_file);
                    response.set_header("Content-Type", "application/x-tar");
                    response.set_header("Content-Disposition",
                                        "attachment; filename=\"" + backup_name + ".tar\"");
                    response.set_header("Cache-Control", "no-cache");
                    return response;
                } catch(const std::exception& e) {
                    return json_error_500(settings::DEFAULT_USERNAME, req.url, e.what());
                }
            });

    // Upload Backup
    CROW_ROUTE(app, "/api/v1/backups/upload")
            .CROW_MIDDLEWARES(app, AuthMiddleware)
            .methods("POST"_method)([&index_manager, &app](const crow::request& req) {
                auto& ctx = app.get_context<AuthMiddleware>(req);
                try {
                    // Parse multipart message
                    crow::multipart::message msg(req);

                    // Find the file part
                    std::string backup_name;
                    std::string file_content;

                    for(const auto& part : msg.parts) {
                        auto content_disposition = part.get_header_object("Content-Disposition");
                        std::string name = content_disposition.params.count("name")
                                                   ? content_disposition.params.at("name")
                                                   : "";

                        if(name == "backup") {
                            // Get filename from Content-Disposition
                            if(content_disposition.params.count("filename")) {
                                backup_name = content_disposition.params.at("filename");
                                // check if backup name ends with .tar
                                if(backup_name.ends_with(".tar")) {
                                    backup_name = backup_name.substr(0, backup_name.size() - 4);
                                } else {
                                    LOG_WARN(1059, ctx.username, "Backup upload used invalid file extension");
                                    return json_error(400, "Invalid backup file extension. Expected .tar file");
                                }
                            }
                            file_content = part.body;
                            break;
                        }
                    }

                    if(backup_name.empty()) {
                        LOG_WARN(1060, ctx.username, "Backup upload request missing backup name");
                        return json_error(400, "Missing backup name or filename");
                    }

                    if(file_content.empty()) {
                        LOG_WARN(1061, ctx.username, "Backup upload request missing backup file content");
                        return json_error(400, "Missing backup file content");
                    }

                    // Validate backup name
                    std::pair<bool, std::string> result =
                            index_manager.validateBackupName(backup_name);
                    if(!result.first) {
                        LOG_WARN(1062, ctx.username, "Backup upload request rejected: " << result.second);
                        return json_error(400, result.second);
                    }

                    std::pair<bool, std::string> uploadStatus = index_manager.uploadBackup(backup_name, ctx.username, file_content);
                    if(!uploadStatus.first) {
                        return json_error(500, uploadStatus.second);
                    }

                    return crow::response(201, "Backup uploaded successfully");
                } catch(const std::exception& e) {
                    return json_error_500(ctx.username, req.url, e.what());
                }
            });

    // Get active backup status for current user
    CROW_ROUTE(app, "/api/v1/backups/active")
            .CROW_MIDDLEWARES(app, AuthMiddleware)
            .methods("GET"_method)([&index_manager, &app](const crow::request& req) {
                auto& ctx = app.get_context<AuthMiddleware>(req);
                try {
                    auto active = index_manager.getActiveBackup(ctx.username);
                    crow::json::wvalue response;
                    if (active) {
                        response["active"] = true;
                        response["backup_name"] = active->second;
                        response["index_id"] = active->first;
                    } else {
                        response["active"] = false;
                    }
                    return crow::response(200, response.dump());
                } catch(const std::exception& e) {
                    return json_error_500(ctx.username, req.url, e.what());
                }
            });

    // Get backup info
    CROW_ROUTE(app, "/api/v1/backups/<string>/info")
            .CROW_MIDDLEWARES(app, AuthMiddleware)
            .methods("GET"_method)([&index_manager, &app](const crow::request& req,
                                                          const std::string& backup_name) {
                auto& ctx = app.get_context<AuthMiddleware>(req);
                try {
                    auto info = index_manager.getBackupInfo(backup_name, ctx.username);
                    if (info.empty()) {
                        LOG_WARN(1064, ctx.username, "Backup-info request for missing backup " << backup_name);
                        return json_error(404, "Backup not found or metadata missing");
                    }
                    crow::response res;
                    res.code = 200;
                    res.set_header("Content-Type", "application/json");
                    res.body = info.dump();
                    return res;
                } catch(const std::exception& e) {
                    return json_error_500(ctx.username, req.url, e.what());
                }
            });

    // List indexes for current user
    CROW_ROUTE(app, "/api/v1/index/list")
            .CROW_MIDDLEWARES(app, AuthMiddleware)
            .methods("GET"_method)([&index_manager, &app](const crow::request& req) {
                auto& ctx = app.get_context<AuthMiddleware>(req);

                // Use the method to get user indexes with metadata
                auto indexes_with_metadata = index_manager.listUserIndexes(ctx.username);

                // Build a detailed response with array of index objects
                nlohmann::ordered_json index_list = nlohmann::ordered_json::array();
                for(const auto& [index_name, metadata] : indexes_with_metadata) {
                    index_list.push_back(make_index_list_item(index_name, metadata));
                }

                nlohmann::ordered_json response = nlohmann::ordered_json::object();
                response["indexes"] = std::move(index_list);
                return json_response(response);
            });

    // Delete index
    CROW_ROUTE(app, "/api/v1/index/<string>/delete")
            .CROW_MIDDLEWARES(app, AuthMiddleware)
            .methods("DELETE"_method)([&index_manager, &app](const crow::request& req,
                                                             std::string index_name) {
                auto& ctx = app.get_context<AuthMiddleware>(req);

                // Format full index_id
                std::string index_id = ctx.username + "/" + index_name;

                try {
                    if(index_manager.deleteIndex(index_id)) {
                        return crow::response(200, "Index deleted successfully");
                    } else {
                        LOG_WARN(1030, ctx.username, index_name, "Delete-index request for missing index");
                        return json_error(404, "Index not found");
                    }
                } catch(const std::runtime_error& e) {
                    LOG_WARN(1031, ctx.username, index_name, "Delete-index request rejected: " << e.what());
                    return json_error(400, e.what());
                } catch(const std::exception& e) {
                    return json_error_500(ctx.username,
                                          index_name,
                                          req.url,
                                          std::string("Failed to delete index: ") + e.what());
                }
            });

    // Search
    CROW_ROUTE(app, "/api/v1/index/<string>/search")
            .CROW_MIDDLEWARES(app, AuthMiddleware)
            .methods("POST"_method)([&index_manager, &app](const crow::request& req,
                                                           std::string index_name) {
                auto& ctx = app.get_context<AuthMiddleware>(req);
                // Format full index_id
                std::string index_id = ctx.username + "/" + index_name;

                auto body = crow::json::load(req.body);
                if(!body || !body.has("k")) {
                    LOG_WARN(1032, ctx.username, index_name, "Search request missing parameter k or has invalid JSON");
                    return json_error(400, "Missing required parameters: k");
                }

                if(!body.has("vector") && !body.has("sparse_indices")) {
                    LOG_WARN(1033, ctx.username, index_name, "Search request missing dense and sparse query vectors");
                    return json_error(400, "Missing query vector (dense or sparse)");
                }

                std::vector<float> query;
                if(body.has("vector")) {
                    for(const auto& elem : body["vector"]) {
                        query.push_back((float)elem.d());
                    }
                }

                std::vector<uint32_t> sparse_indices;
                std::vector<float> sparse_values;

                if(body.has("sparse_indices")) {
                    for(const auto& elem : body["sparse_indices"]) {
                        sparse_indices.push_back((uint32_t)elem.i());
                    }
                }

                if(body.has("sparse_values")) {
                    for(const auto& elem : body["sparse_values"]) {
                        sparse_values.push_back((float)elem.d());
                    }
                }

                if(sparse_indices.size() != sparse_values.size()) {
                    LOG_WARN(1034,
                                 ctx.username,
                                 index_name,
                                 "Search request has mismatched sparse_indices and sparse_values");
                    return json_error(400,
                                      "Mismatch between sparse_indices and sparse_values size");
                }

                size_t k = (size_t)body["k"].i();
                if(k < settings::MIN_K || k > settings::MAX_K) {
                    LOG_WARN(1035, ctx.username, index_name, "Invalid k: " << k);
                    return json_error(400,
                                      "k must be between " + std::to_string(settings::MIN_K)
                                              + " and " + std::to_string(settings::MAX_K));
                }
                size_t ef = body.has("ef") ? (size_t)body["ef"].i() : settings::DEFAULT_EF_SEARCH;
                bool include_vectors =
                        body.has("include_vectors") ? body["include_vectors"].b() : false;
                nlohmann::json filter_array = nlohmann::json::array();  // default: empty filter

                if(body.has("filter")) {
                    try {
                        auto raw_filter = nlohmann::json::parse(body["filter"].s());
                        // Expect new array-based filter format
                        if(!raw_filter.is_array()) {
                            LOG_WARN(1036, ctx.username, index_name, "Search request used invalid filter format");
                            return json_error(400,
                                              "Filter must be an array. Please use format: "
                                              "[{\"field\":{\"$op\":value}}]");
                        }
                        filter_array = raw_filter;
                    } catch(const std::exception& e) {
                        LOG_WARN(1037, ctx.username, index_name, "Search request filter JSON parsing failed: " << e.what());
                        return json_error(400, std::string("Invalid filter JSON: ") + e.what());
                    }
                }

                // Extract filter parameters (Option B from chat plan)
                ndd::FilterParams filter_params;
                if (body.has("filter_params")) {
                     auto fp = body["filter_params"];
                     if (fp.has("prefilter_threshold")) {
                         filter_params.prefilter_threshold = static_cast<size_t>(fp["prefilter_threshold"].i());
                     }
                     if (fp.has("boost_percentage")) {
                         filter_params.boost_percentage = static_cast<size_t>(fp["boost_percentage"].i());
                     }
                }

                float dense_rrf_weight = body.has("dense_rrf_weight") ? (float)body["dense_rrf_weight"].d() : settings::DEFAULT_DENSE_RRF_WEIGHT;
                if (dense_rrf_weight < 0.0f || dense_rrf_weight > 1.0f) {
                    return json_error(400, "dense_rrf_weight must be between 0 and 1");
                }
                float rrf_rank_constant = body.has("rrf_rank_constant") ? (float)body["rrf_rank_constant"].d() : settings::DEFAULT_RRF_RANK_CONSTANT;
                if(rrf_rank_constant<=0.0f){
                    return json_error(400, "rrf_rank_constant must be greater than 0");
                }
                LOG_DEBUG("Filter: " << filter_array.dump());
                try {
                    auto search_response = index_manager.searchKNN(index_id,
                                                                    query,
                                                                    sparse_indices,
                                                                    sparse_values,
                                                                    k,
                                                                    filter_array,
                                                                    filter_params,
                                                                    include_vectors,
                                                                    ef,
                                                                    dense_rrf_weight,
                                                                    rrf_rank_constant);

                    if(!search_response) {
                        LOG_WARN(1038, ctx.username, index_name, "Search request returned no results because the index is missing or search failed");
                        return json_error(404, "Index not found or search failed");
                    }

                    // Serialize the ResultSet using MessagePack
                    msgpack::sbuffer sbuf;
                    msgpack::pack(sbuf, search_response.value());
                    crow::response resp(200, std::string(sbuf.data(), sbuf.size()));
                    resp.add_header("Content-Type", "application/msgpack");
                    return resp;
                } catch(const std::runtime_error& e) {
                    LOG_WARN(1039, ctx.username, index_name, "Search request rejected: " << e.what());
                    return json_error(400, e.what());
                } catch(const std::exception& e) {
                    LOG_DEBUG("Search failed: " << e.what());
                    return json_error_500(
                            ctx.username,
                            index_name,
                            req.url,
                            std::string("Search failed: ") + e.what());
                }
            });

    //  Insert a list of vectors
    CROW_ROUTE(app, "/api/v1/index/<string>/vector/insert")
            .CROW_MIDDLEWARES(app, AuthMiddleware)
            .methods("POST"_method)([&index_manager, &app](const crow::request& req,
                                                           std::string index_name) {
                auto& ctx = app.get_context<AuthMiddleware>(req);
                std::string index_id = ctx.username + "/" + index_name;

                // Verify content type is application/msgpack or application/json
                auto content_type = req.get_header_value("Content-Type");

                if(is_disk_full()){
                    return json_error(400, "Batch insertion aborted: Not enough storage space");
                }

                if(content_type == "application/json") {
                    auto body = crow::json::load(req.body);
                    if(!body) {
                        LOG_WARN(1040, ctx.username, index_name, "Insert request contained invalid JSON");
                        return json_error(400, "Invalid JSON");
                    }

                    std::vector<ndd::HybridVectorObject> vectors;

                    // Helper to parse single object
                    auto parse_obj = [](const crow::json::rvalue& item) -> ndd::HybridVectorObject {
                        ndd::HybridVectorObject vec;
                        if(item.has("id")) {
                            if(item["id"].t() == crow::json::type::Number) {
                                vec.id = std::to_string(item["id"].i());
                            } else {
                                vec.id = item["id"].s();
                            }
                        }

                        if(item.has("meta")) {
                            auto meta_str = std::string(item["meta"].s());
                            vec.meta.assign(meta_str.begin(), meta_str.end());
                        }

                        if(item.has("filter")) {
                            vec.filter = std::string(item["filter"].s());
                        }

                        if(item.has("norm")) {
                            vec.norm = static_cast<float>(item["norm"].d());
                        }

                        if(item.has("vector")) {
                            for(const auto& v : item["vector"]) {
                                vec.vector.push_back(static_cast<float>(v.d()));
                            }
                        }

                        if(item.has("sparse_indices")) {
                            for(const auto& v : item["sparse_indices"]) {
                                vec.sparse_ids.push_back(static_cast<uint32_t>(v.i()));
                            }
                        }

                        if(item.has("sparse_values")) {
                            for(const auto& v : item["sparse_values"]) {
                                vec.sparse_values.push_back(static_cast<float>(v.d()));
                            }
                        }
                        return vec;
                    };

                    if(body.t() == crow::json::type::List) {
                        for(const auto& item : body) {
                            vectors.push_back(parse_obj(item));
                        }
                    } else {
                        vectors.push_back(parse_obj(body));
                    }

                    try {
                        bool success = index_manager.addVectors(index_id, vectors);
                        if(!success) {
                            LOG_WARN(1066,
                                     ctx.username,
                                     index_name,
                                     "Insert request failed without detailed error from addVectors");
                            return json_error(400, "Batch insertion failed");
                        }
                        return crow::response(200);
                    } catch(const std::runtime_error& e) {
                        LOG_WARN(1041, ctx.username, index_name, "Insert request rejected: " << e.what());
                        return json_error(400, e.what());
                    } catch(const std::exception& e) {
                        return json_error_500(ctx.username, index_name, req.url, e.what());
                    }
                } else if(content_type == "application/msgpack") {
                    // Deserialize MsgPack batch
                    try {
                        auto oh = msgpack::unpack(req.body.data(), req.body.size());
                        auto obj = oh.get();

                        try {
                            // Try HybridVectorObject first
                            auto vectors = obj.as<std::vector<ndd::HybridVectorObject>>();
                            LOG_DEBUG("Batch size (Hybrid): " << vectors.size());
                            bool success = index_manager.addVectors(index_id, vectors);
                            if(!success) {
                                LOG_WARN(1067,
                                         ctx.username,
                                         index_name,
                                         "Insert request failed without detailed error from addVectors");
                                return json_error(400, "Batch insertion failed");
                            }
                            return crow::response(200);
                        } catch(...) {
                            // Fallback to VectorObject
                            auto vectors = obj.as<std::vector<ndd::VectorObject>>();
                            LOG_DEBUG("Batch size (Dense): " << vectors.size());
                            bool success = index_manager.addVectors(index_id, vectors);
                            if(!success) {
                                LOG_WARN(1068,
                                         ctx.username,
                                         index_name,
                                         "Insert request failed without detailed error from addVectors");
                                return json_error(400, "Batch insertion failed");
                            }
                            return crow::response(200);
                        }
                    } catch(const std::runtime_error& e) {
                        LOG_WARN(1042, ctx.username, index_name, "Insert request rejected: " << e.what());
                        return json_error(400, e.what());
                    } catch(const std::exception& e) {
                        LOG_DEBUG("Batch insertion failed: " << e.what());
                        return json_error_500(ctx.username, index_name, req.url, e.what());
                    }
                } else {
                    LOG_WARN(1043, ctx.username, index_name, "Insert request used unsupported Content-Type: " << content_type);
                    return crow::response(
                            400, "Content-Type must be application/msgpack or application/json");
                }
            });

    // Get a single vector
    CROW_ROUTE(app, "/api/v1/index/<string>/vector/get")
            .CROW_MIDDLEWARES(app, AuthMiddleware)
            .methods("POST"_method)(
                    [&index_manager, &app](const crow::request& req, std::string index_name) {
                        auto& ctx = app.get_context<AuthMiddleware>(req);
                        std::string index_id = ctx.username + "/" + index_name;

                        // Read vector ID from JSON input (still using JSON for ID here)
                        auto body = crow::json::load(req.body);
                        if(!body || !body.has("id")) {
                            LOG_WARN(1044, ctx.username, index_name, "Get-vector request missing vector id");
                            return json_error(400, "Missing required parameter 'id'");
                        }
                        std::string vector_id = body["id"].s();
                        try {
                            auto vector = index_manager.getVector(index_id, vector_id);
                            if(!vector) {
                                LOG_WARN(1045, ctx.username, index_name, "Get-vector request for missing vector id " << vector_id);
                                return json_error(404, "Vector with the given ID does not exist");
                            }
                            // Serialize hybrid vector as MsgPack
                            msgpack::sbuffer sbuf;
                            msgpack::pack(sbuf, vector.value());
                            // Return as MessagePack
                            crow::response resp(200, std::string(sbuf.data(), sbuf.size()));
                            resp.add_header("Content-Type", "application/msgpack");
                            return resp;
                        } catch(const std::exception& e) {
                            LOG_DEBUG("Failed to get vector: " << e.what());
                            return json_error_500(ctx.username,
                                                  index_name,
                                                  req.url,
                                                  std::string("Failed to get vector: ") + e.what());
                        }
                    });

    // Delete a vector
    CROW_ROUTE(app, "/api/v1/index/<string>/vector/<string>/delete")
            .CROW_MIDDLEWARES(app, AuthMiddleware)
            .methods("DELETE"_method)([&index_manager, &app](const crow::request& req,
                                                             std::string index_name,
                                                             std::string vector_id) {
                auto& ctx = app.get_context<AuthMiddleware>(req);
                std::string index_id = ctx.username + "/" + index_name;

                LOG_DEBUG("Deleting vector " << vector_id << " from index " << index_id);

                try {
                    if(index_manager.deleteVector(index_id, vector_id)) {
                        return crow::response(200, "Vector deleted successfully");
                    } else {
                        LOG_WARN(1046, ctx.username, index_name, "Delete-vector request for missing vector id " << vector_id);
                        return json_error(404, "Vector with the given ID does not exist");
                    }
                } catch(const std::runtime_error& e) {
                    LOG_WARN(1047, ctx.username, index_name, "Delete-vector request rejected: " << e.what());
                    return json_error(400, e.what());
                } catch(const std::exception& e) {
                    LOG_DEBUG("Failed to delete vector: " << e.what());
                    return json_error_500(ctx.username,
                                          index_name,
                                          req.url,
                                          std::string("Failed to delete vector: ") + e.what());
                }
            });

    // Delete vectors by filter
    CROW_ROUTE(app, "/api/v1/index/<string>/vectors/delete")
            .CROW_MIDDLEWARES(app, AuthMiddleware)
            .methods("DELETE"_method)([&index_manager, &app](const crow::request& req,
                                                             std::string index_name) {
                auto& ctx = app.get_context<AuthMiddleware>(req);
                std::string index_id = ctx.username + "/" + index_name;

                nlohmann::json body;
                try {
                    body = nlohmann::json::parse(req.body);
                } catch(const std::exception& e) {
                    LOG_WARN(1048, ctx.username, index_name, "Delete-by-filter request contained invalid JSON");
                    return json_error(400, "Invalid JSON body");
                }
                if(!body.contains("filter")) {
                    LOG_WARN(1049, ctx.username, index_name, "Delete-by-filter request is missing filter");
                    return json_error(400, "Invalid request body - missing filter");
                }
                try {
                    nlohmann::json filter_array = body["filter"];
                    // Expect new array-based filter format
                    if(!filter_array.is_array()) {
                        LOG_WARN(1050, ctx.username, index_name, "Delete-by-filter request used invalid filter format");
                        return json_error(400,
                                          "Filter must be an array. Please use format: "
                                          "[{\"field\":{\"$op\":value}}]");
                    }
                    size_t deleted_count =
                            index_manager.deleteVectorsByFilter(index_id, filter_array);

                    return crow::response(200, std::to_string(deleted_count) + " vectors deleted");
                } catch(const std::runtime_error& e) {
                    LOG_WARN(1051, ctx.username, index_name, "Delete-by-filter request rejected: " << e.what());
                    return json_error(400, e.what());
                } catch(const std::exception& e) {
                    return json_error_500(ctx.username,
                                          index_name,
                                          req.url,
                                          std::string("Failed to delete vectors: ") + e.what());
                }
            });

    // Update filters for vectors
    CROW_ROUTE(app, "/api/v1/index/<string>/filters/update")
            .CROW_MIDDLEWARES(app, AuthMiddleware)
            .methods("POST"_method)([&index_manager, &app](const crow::request& req,
                                                           std::string index_name) {
                auto& ctx = app.get_context<AuthMiddleware>(req);
                std::string index_id = ctx.username + "/" + index_name;

                nlohmann::json body;
                try {
                    body = nlohmann::json::parse(req.body);
                } catch(const std::exception& e) {
                    LOG_WARN(1052, ctx.username, index_name, "Update-filters request contained invalid JSON");
                    return json_error(400, "Invalid JSON body");
                }

                if(!body.contains("updates") || !body["updates"].is_array()) {
                    LOG_WARN(1053, ctx.username, index_name, "Update-filters request missing valid updates array");
                    return json_error(400,
                                      "Missing or invalid 'updates' field. Must be a list of {id, "
                                      "filter} objects.");
                }

                std::vector<std::pair<std::string, std::string>> updates;
                try {
                    for(const auto& item : body["updates"]) {
                        if(!item.contains("id") || !item.contains("filter")) {
                            continue;  // Skip invalid items
                        }
                        std::string id = item["id"].get<std::string>();
                        // Convert filter object to string
                        std::string filter = item["filter"].dump();
                        updates.emplace_back(id, filter);
                    }

                    size_t count = index_manager.updateFilters(index_id, updates);
                    return crow::response(200, std::to_string(count) + " filters updated");

                } catch(const std::runtime_error& e) {
                    LOG_WARN(1054, ctx.username, index_name, "Update-filters request rejected: " << e.what());
                    return json_error(400, e.what());
                } catch(const std::exception& e) {
                    return json_error_500(ctx.username,
                                          index_name,
                                          req.url,
                                          std::string("Failed to update filters: ") + e.what());
                }
            });

    CROW_ROUTE(app, "/api/v1/index/<string>/info")
            .CROW_MIDDLEWARES(app, AuthMiddleware)
            .methods("GET"_method)([&index_manager, &app](const crow::request& req,
                                                          std::string index_name) {
                auto& ctx = app.get_context<AuthMiddleware>(req);
                std::string index_id = ctx.username + "/" + index_name;
                try {
                    auto info = index_manager.getIndexInfo(index_id);
                    if(!info) {
                        LOG_WARN(1055, ctx.username, index_name, "Index-info request for missing index");
                        return json_error(404, "Index does not exist");
                    }
                    return json_response(make_index_info_payload(*info));
                } catch(const std::runtime_error& e) {
                    LOG_WARN(1056, ctx.username, index_name, "Index-info request failed: " << e.what());
                    return json_error(404, std::string("Error: ") + e.what());
                } catch(const std::exception& e) {
                    return json_error_500(ctx.username,
                                          index_name,
                                          req.url,
                                          std::string("Error: ") + e.what());
                }
            });

    // ============================================================
    // REACT SPA SERVING WITH CLIENT-SIDE ROUTING SUPPORT
    // ============================================================

    // Serve static assets from React build (JS, CSS, images, fonts, etc.)
    // This catches all requests to /assets/* which is Vite's default output structure
    CROW_ROUTE(app, "/assets/<path>")
    ([BUILD_DIR](const crow::request&, std::string path) {
        std::string file_path = BUILD_DIR + "/assets/" + path;

        if(!file_exists(file_path)) {
            return crow::response(404, "Asset not found");
        }

        std::string content = read_file(file_path);
        if(content.empty()) {
            return crow::response(404, "Asset not found");
        }

        auto response = crow::response(content);
        response.set_header("Content-Type", get_mime_type(path));
        // Cache static assets for 1 year (they are content-hashed)
        response.set_header("Cache-Control", "public, max-age=31536000, immutable");
        return response;
    });

    // Serve other static files (favicon, manifest, etc.)
    CROW_ROUTE(app, "/<path>")
    ([BUILD_DIR, INDEX_PATH](const crow::request&, std::string path) {
        // Try to serve the specific file first
        std::string file_path = BUILD_DIR + "/" + path;

        // If file exists and is not a directory, serve it
        if(file_exists(file_path)) {
            std::string content = read_file(file_path);
            if(!content.empty()) {
                auto response = crow::response(content);
                response.set_header("Content-Type", get_mime_type(path));

                // Cache static files
                if(path.ends_with(".ico") || path.ends_with(".png") || path.ends_with(".svg")
                   || path.ends_with(".json")) {
                    response.set_header("Cache-Control", "public, max-age=3600");
                }

                return response;
            }
        }

        // SPA Fallback: For any route that doesn't match a file,
        // serve index.html to let React Router handle it
        // This enables client-side routing for paths like:
        // /, /dashboard, /settings, /items/123, etc.
        std::string index_content = read_file(INDEX_PATH);
        if(index_content.empty()) {
            return crow::response(404, "App not found");
        }

        auto response = crow::response(index_content);
        response.set_header("Content-Type", "text/html");
        response.set_header("Cache-Control", "no-cache");
        return response;
    });

    // Root route - serve index.html
    CROW_ROUTE(app, "/")
    ([INDEX_PATH] {
        std::string content = read_file(INDEX_PATH);
        if(content.empty()) {
            return crow::response(404, "App not found");
        }

        auto response = crow::response(content);
        response.set_header("Content-Type", "text/html");
        response.set_header("Cache-Control", "no-cache");
        return response;
    });

    LOG_INFO(1008, "Using: " << settings::NUM_SERVER_THREADS << " server threads.");
    app.port(settings::SERVER_PORT).concurrency(settings::NUM_SERVER_THREADS).run();
    return 0;
}
