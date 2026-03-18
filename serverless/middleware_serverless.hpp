#pragma once
#include "auth_serverless.hpp"
#include "../third_party/crow/include/crow.h"

// Serverless Auth Middleware
// This middleware authenticates all requests using the serverless AuthManager
// and provides context with username and user_type for downstream handlers
struct ServerlessAuthMiddleware : crow::ILocalMiddleware {
    AuthManager& auth_manager;

    struct context {
        std::string username;
        UserType user_type;
    };

    ServerlessAuthMiddleware(AuthManager& am) : auth_manager(am) {}

    void before_handle(crow::request& req, crow::response& res, context& ctx) {
        // Get Authorization header (raw token, same as OSS pattern)
        std::string auth_header = req.get_header_value("Authorization");

        // Validate token → get username
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

        ctx.username = username;
        ctx.user_type = *user_type_opt;
    }

    void after_handle(crow::request&, crow::response&, context&) {}
};
