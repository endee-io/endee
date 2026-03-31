#pragma once
#include "auth_serverless.hpp"
#include "../third_party/crow/include/crow.h"

// Serverless Auth Middleware
// This middleware authenticates all requests using the serverless AuthManager
// and provides context with username and user_type for downstream handlers.
// Supports root impersonation: token format "root/<target_user>:<root_token>"
// allows root to act as any user for index/vector operations.
struct ServerlessAuthMiddleware : crow::ILocalMiddleware {
    AuthManager& auth_manager;

    struct context {
        std::string username;
        UserType user_type;
        bool is_impersonated = false; // true when root is impersonating a user
    };

    ServerlessAuthMiddleware(AuthManager& am) : auth_manager(am) {}

    void before_handle(crow::request& req, crow::response& res, context& ctx) {
        // Get Authorization header (raw token, same as OSS pattern)
        std::string auth_header = req.get_header_value("Authorization");

        // Check for root impersonation format: "root/<target_username>:<root_token>"
        // This allows root to perform index/vector operations on behalf of any user.
        if(auth_header.size() > 5 && auth_header.substr(0, 5) == "root/") {
            size_t colon_pos = auth_header.find(':', 5);
            if(colon_pos != std::string::npos && colon_pos > 5) {
                std::string target_username = auth_header.substr(5, colon_pos - 5);
                std::string provided_token = auth_header.substr(colon_pos + 1);

                // Validate the root token portion
                if(provided_token == auth_manager.getRootToken()) {
                    // Verify target user exists and is active
                    auto user = auth_manager.getUser(target_username);
                    if(!user) {
                        res.code = 403;
                        crow::json::wvalue err_json({{"error", "Impersonation target user not found"}});
                        res.write(err_json.dump());
                        res.end();
                        return;
                    }
                    if(!user->is_active) {
                        res.code = 403;
                        crow::json::wvalue err_json({{"error", "Impersonation target user is deactivated"}});
                        res.write(err_json.dump());
                        res.end();
                        return;
                    }

                    ctx.username = target_username;
                    ctx.user_type = user->user_type;
                    ctx.is_impersonated = true;
                    LOG_INFO("root acting as '" << target_username << "' for " << req.url);
                    return;
                }
                // If root token doesn't match, fall through to normal validation (will 401)
            }
        }

        // Normal auth flow: validate token → get username
        std::string username = auth_manager.validateToken(auth_header);
        if(username.empty()) {
            res.code = 401;
            crow::json::wvalue err_json({{"error", "Invalid or missing token"}});
            res.write(err_json.dump());
            res.end();
            return;
        }

        // Get user type
        auto user_type_opt = auth_manager.getUserType(username);
        if(!user_type_opt) {
            res.code = 403;
            crow::json::wvalue err_json({{"error", "User not found or deactivated"}});
            res.write(err_json.dump());
            res.end();
            return;
        }

        // Block root from index/vector operations — root must impersonate a user
        // via "root/<username>:<root_token>" format for data plane operations.
        // Only admin routes (/api/v1/admin/), health, stats, and user info are allowed.
        if(username == "root") {
            const std::string& url = req.url;
            bool is_data_route = url.find("/api/v1/index/") != std::string::npos
                              || url.find("/api/v1/tokens") != std::string::npos
                              || url.find("/api/v1/backups") != std::string::npos;
            if(is_data_route) {
                res.code = 403;
                crow::json::wvalue err_json({{"error",
                    "Root cannot perform index/vector operations."}});
                res.write(err_json.dump());
                res.end();
                return;
            }
        }

        ctx.username = username;
        ctx.user_type = *user_type_opt;
    }

    void after_handle(crow::request& req, crow::response& res, context& ctx) {
        if(ctx.is_impersonated) {
            LOG_INFO("root impersonated '" << ctx.username << "': "
                     << req.url << " -> " << res.code);
        }
    }
};
