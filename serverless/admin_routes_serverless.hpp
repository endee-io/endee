#pragma once
#include "auth_serverless.hpp"
#include "middleware_serverless.hpp"
#include "../core/ndd.hpp"
#include "../quant/common.hpp"
#include "../third_party/crow/include/crow.h"
#include <map>

// Helper to construct JSON error response
inline crow::response admin_json_error(int code, const std::string& message) {
    crow::json::wvalue err_json({{"error", message}});
    return crow::response(code, err_json.dump());
}

// Register all 18 admin endpoints for enterprise mode
// Template allows working with any Crow app type
template <typename App>
void registerAdminRoutes(App& app,
                         AuthManager& auth_manager,
                         IndexManager& index_manager) {

    // ===== ADMIN-ONLY ENDPOINTS (1-14) =====

    // 1. POST /api/v1/admin/users - Create user (admin only)
    CROW_ROUTE(app, "/api/v1/admin/users")
        .CROW_MIDDLEWARES(app, ServerlessAuthMiddleware)
        .methods("POST"_method)([&auth_manager, &app](const crow::request& req) {
            auto& ctx = app.template get_context<ServerlessAuthMiddleware>(req);

            if(!auth_manager.isAdmin(ctx.username)) {
                return admin_json_error(403, "Admin access required");
            }

            auto body = crow::json::load(req.body);
            if(!body) {
                return admin_json_error(400, "Invalid JSON");
            }

            std::string username = body["username"].s();
            std::string user_type_str = body["user_type"].s();
            UserType user_type = userTypeFromString(user_type_str);

            if(auth_manager.createUser(username, user_type)) {
                // Auto-generate a "default" token for the new user
                std::string token = auth_manager.generateToken(username, "default");
                if(token.empty()) {
                    return admin_json_error(500, "User created but failed to generate token");
                }

                crow::json::wvalue response;
                response["message"] = "User created successfully";
                response["username"] = username;
                response["user_type"] = userTypeToString(user_type);
                response["token"] = token;
                response["name"] = "default";
                return crow::response(201, response.dump());
            }

            return admin_json_error(400, "Failed to create user (may already exist)");
        });

    // 3. GET /api/v1/admin/users - List all users (admin only)
    CROW_ROUTE(app, "/api/v1/admin/users")
        .CROW_MIDDLEWARES(app, ServerlessAuthMiddleware)
        .methods("GET"_method)([&auth_manager, &app](const crow::request& req) {
            auto& ctx = app.template get_context<ServerlessAuthMiddleware>(req);

            if(!auth_manager.isAdmin(ctx.username)) {
                return admin_json_error(403, "Admin access required");
            }

            auto users = auth_manager.listUsers();
            std::vector<crow::json::wvalue> users_list;

            for(const auto& user : users) {
                crow::json::wvalue user_json;
                user_json["username"] = user.username;
                user_json["user_type"] = userTypeToString(user.user_type);
                user_json["is_active"] = user.is_active;
                user_json["created_at"] = std::chrono::duration_cast<std::chrono::seconds>(
                    user.created_at.time_since_epoch()).count();
                users_list.push_back(std::move(user_json));
            }

            crow::json::wvalue response;
            response["users"] = std::move(users_list);
            return crow::response(200, response.dump());
        });

    // 4. DELETE /api/v1/admin/users/<username> - Delete user (admin only)
    CROW_ROUTE(app, "/api/v1/admin/users/<string>")
        .CROW_MIDDLEWARES(app, ServerlessAuthMiddleware)
        .methods("DELETE"_method)([&auth_manager, &app](const crow::request& req, std::string username) {
            auto& ctx = app.template get_context<ServerlessAuthMiddleware>(req);

            if(!auth_manager.isAdmin(ctx.username)) {
                return admin_json_error(403, "Admin access required");
            }

            if(auth_manager.deleteUser(username)) {
                crow::json::wvalue response;
                response["message"] = "User deleted successfully";
                return crow::response(200, response.dump());
            }

            return admin_json_error(404, "User not found");
        });

    // 5. POST /api/v1/admin/users/<username>/deactivate - Deactivate user (admin only)
    CROW_ROUTE(app, "/api/v1/admin/users/<string>/deactivate")
        .CROW_MIDDLEWARES(app, ServerlessAuthMiddleware)
        .methods("POST"_method)([&auth_manager, &app](const crow::request& req, std::string username) {
            auto& ctx = app.template get_context<ServerlessAuthMiddleware>(req);

            if(!auth_manager.isAdmin(ctx.username)) {
                return admin_json_error(403, "Admin access required");
            }

            if(auth_manager.deactivateUser(username)) {
                crow::json::wvalue response;
                response["message"] = "User deactivated successfully";
                return crow::response(200, response.dump());
            }

            return admin_json_error(404, "User not found");
        });

    // 5b. POST /api/v1/admin/users/<username>/activate - Activate user (admin only)
    CROW_ROUTE(app, "/api/v1/admin/users/<string>/activate")
        .CROW_MIDDLEWARES(app, ServerlessAuthMiddleware)
        .methods("POST"_method)([&auth_manager, &app](const crow::request& req, std::string username) {
            auto& ctx = app.template get_context<ServerlessAuthMiddleware>(req);

            if(!auth_manager.isAdmin(ctx.username)) {
                return admin_json_error(403, "Admin access required");
            }

            if(auth_manager.activateUser(username)) {
                crow::json::wvalue response;
                response["message"] = "User activated successfully";
                return crow::response(200, response.dump());
            }

            return admin_json_error(404, "User not found");
        });

    // 6. PUT /api/v1/admin/users/<username>/type - Change user tier (admin only)
    CROW_ROUTE(app, "/api/v1/admin/users/<string>/type")
        .CROW_MIDDLEWARES(app, ServerlessAuthMiddleware)
        .methods("PUT"_method)([&auth_manager, &app](const crow::request& req, std::string username) {
            auto& ctx = app.template get_context<ServerlessAuthMiddleware>(req);

            if(!auth_manager.isAdmin(ctx.username)) {
                return admin_json_error(403, "Admin access required");
            }

            auto body = crow::json::load(req.body);
            if(!body) {
                return admin_json_error(400, "Invalid JSON");
            }

            std::string user_type_str = body["user_type"].s();
            UserType user_type = userTypeFromString(user_type_str);

            if(auth_manager.setUserType(username, user_type)) {
                crow::json::wvalue response;
                response["message"] = "User type updated successfully";
                response["username"] = username;
                response["user_type"] = userTypeToString(user_type);
                return crow::response(200, response.dump());
            }

            return admin_json_error(404, "User not found");
        });

    // 7. GET /api/v1/admin/users/<username>/indices - List user indices (admin only)
    CROW_ROUTE(app, "/api/v1/admin/users/<string>/indices")
        .CROW_MIDDLEWARES(app, ServerlessAuthMiddleware)
        .methods("GET"_method)([&index_manager, &auth_manager, &app](const crow::request& req, std::string username) {
            auto& ctx = app.template get_context<ServerlessAuthMiddleware>(req);

            if(!auth_manager.isAdmin(ctx.username)) {
                return admin_json_error(403, "Admin access required");
            }

            auto indices_with_metadata = index_manager.listUserIndexes(username);
            std::vector<std::string> indices_list;
            for(const auto& [index_name, metadata] : indices_with_metadata) {
                indices_list.push_back(index_name);
            }

            crow::json::wvalue response;
            response["indices"] = std::move(indices_list);
            response["count"] = indices_list.size();

            return crow::response(200, response.dump());
        });

    // 8. GET /api/v1/admin/indexes - List all indexes (admin only)
    CROW_ROUTE(app, "/api/v1/admin/indexes")
        .CROW_MIDDLEWARES(app, ServerlessAuthMiddleware)
        .methods("GET"_method)([&index_manager, &auth_manager, &app](const crow::request& req) {
            auto& ctx = app.template get_context<ServerlessAuthMiddleware>(req);

            if(!auth_manager.isAdmin(ctx.username)) {
                return admin_json_error(403, "Admin access required");
            }

            auto all_indices_with_metadata = index_manager.listAllIndexes();

            // Group indices by username (extract from index_id format: username/index_name)
            std::map<std::string, std::vector<std::string>> indices_by_user_map;
            for(const auto& [index_id, metadata] : all_indices_with_metadata) {
                size_t slash_pos = index_id.find('/');
                if(slash_pos != std::string::npos) {
                    std::string username = index_id.substr(0, slash_pos);
                    std::string index_name = index_id.substr(slash_pos + 1);
                    indices_by_user_map[username].push_back(index_name);
                }
            }

            std::vector<crow::json::wvalue> indices_by_user_list;
            for(const auto& [username, indices] : indices_by_user_map) {
                std::vector<std::string> indices_vec(indices.begin(), indices.end());
                crow::json::wvalue user_indices;
                user_indices["username"] = username;
                user_indices["indices"] = std::move(indices_vec);
                indices_by_user_list.push_back(std::move(user_indices));
            }

            crow::json::wvalue response;
            response["indices"] = std::move(indices_by_user_list);
            return crow::response(200, response.dump());
        });

    // 9. DELETE /api/v1/admin/users/<username>/indexes/<index_id> - Delete user index (admin only)
    CROW_ROUTE(app, "/api/v1/admin/users/<string>/indexes/<string>")
        .CROW_MIDDLEWARES(app, ServerlessAuthMiddleware)
        .methods("DELETE"_method)([&index_manager, &auth_manager, &app](
                const crow::request& req, std::string username, std::string index_id) {
            auto& ctx = app.template get_context<ServerlessAuthMiddleware>(req);

            if(!auth_manager.isAdmin(ctx.username)) {
                return admin_json_error(403, "Admin access required");
            }

            // Construct full index_id (username/index_name format)
            std::string full_index_id = username + "/" + index_id;
            if(index_manager.deleteIndex(full_index_id)) {
                crow::json::wvalue response;
                response["message"] = "Index deleted successfully";
                return crow::response(200, response.dump());
            }

            return admin_json_error(404, "Index not found");
        });

    // 10. POST /api/v1/admin/users/<username>/tokens - Create token for user (admin only)
    //     Body: {"name": "my-api-key"}
    CROW_ROUTE(app, "/api/v1/admin/users/<string>/tokens")
        .CROW_MIDDLEWARES(app, ServerlessAuthMiddleware)
        .methods("POST"_method)([&auth_manager, &app](const crow::request& req, std::string username) {
            auto& ctx = app.template get_context<ServerlessAuthMiddleware>(req);

            if(!auth_manager.isAdmin(ctx.username)) {
                return admin_json_error(403, "Admin access required");
            }

            auto body = crow::json::load(req.body);
            if(!body || !body.has("name")) {
                return admin_json_error(400, "name is required");
            }

            std::string token_name = body["name"].s();
            std::string token = auth_manager.generateToken(username, token_name);
            if(token.empty()) {
                return admin_json_error(400, "User not found or token name already exists");
            }

            crow::json::wvalue response;
            response["token"] = token;
            response["name"] = token_name;
            response["username"] = username;
            return crow::response(201, response.dump());
        });

    // 11. GET /api/v1/admin/users/<username>/tokens - List user tokens (admin only)
    CROW_ROUTE(app, "/api/v1/admin/users/<string>/tokens")
        .CROW_MIDDLEWARES(app, ServerlessAuthMiddleware)
        .methods("GET"_method)([&auth_manager, &app](const crow::request& req, std::string username) {
            auto& ctx = app.template get_context<ServerlessAuthMiddleware>(req);

            if(!auth_manager.isAdmin(ctx.username)) {
                return admin_json_error(403, "Admin access required");
            }

            auto tokens = auth_manager.listUserTokens(username);
            std::vector<crow::json::wvalue> tokens_list;

            for(const auto& token : tokens) {
                crow::json::wvalue token_json;
                token_json["name"] = token.token_name;
                token_json["username"] = token.username;
                token_json["created_at"] = std::chrono::duration_cast<std::chrono::seconds>(
                    token.created_at.time_since_epoch()).count();
                tokens_list.push_back(std::move(token_json));
            }

            crow::json::wvalue response;
            response["tokens"] = std::move(tokens_list);
            response["count"] = tokens.size();
            return crow::response(200, response.dump());
        });

    // 12. DELETE /api/v1/admin/users/<username>/tokens/<token_name> - Delete user token by name (admin only)
    CROW_ROUTE(app, "/api/v1/admin/users/<string>/tokens/<string>")
        .CROW_MIDDLEWARES(app, ServerlessAuthMiddleware)
        .methods("DELETE"_method)([&auth_manager, &app](
                const crow::request& req, std::string username, std::string token_name) {
            auto& ctx = app.template get_context<ServerlessAuthMiddleware>(req);

            if(!auth_manager.isAdmin(ctx.username)) {
                return admin_json_error(403, "Admin access required");
            }

            if(auth_manager.deleteToken(username, token_name)) {
                crow::json::wvalue response;
                response["message"] = "Token '" + token_name + "' deleted successfully";
                return crow::response(200, response.dump());
            }

            return admin_json_error(404, "Token name '" + token_name + "' not found");
        });

    // 13. POST /api/v1/admin/users/<username>/indices/<index_id>/reset - Reset index (admin only)
    //     Body: {"dim": 1536, "space_type": "cosine", "M": 16, "ef_con": 200, "quant_level": 8, "checksum": -1}
    CROW_ROUTE(app, "/api/v1/admin/users/<string>/indices/<string>/reset")
        .CROW_MIDDLEWARES(app, ServerlessAuthMiddleware)
        .methods("POST"_method)([&index_manager, &auth_manager, &app](
                const crow::request& req, std::string username, std::string index_id) {
            auto& ctx = app.template get_context<ServerlessAuthMiddleware>(req);

            if(!auth_manager.isAdmin(ctx.username)) {
                return admin_json_error(403, "Admin access required");
            }

            // Construct full index_id (username/index_name format)
            std::string full_index_id = username + "/" + index_id;

            auto body = crow::json::load(req.body);
            if(!body) {
                return admin_json_error(400, "Invalid JSON body");
            }

            // Build config from body params
            size_t dim = body.has("dim") ? static_cast<size_t>(body["dim"].i()) : 0;
            size_t sparse_dim = body.has("sparse_dim") ? static_cast<size_t>(body["sparse_dim"].i()) : 0;
            std::string space_type = body.has("space_type") ? std::string(body["space_type"].s()) : "cosine";
            size_t M = body.has("M") ? static_cast<size_t>(body["M"].i()) : 16;
            size_t ef_con = body.has("ef_con") ? static_cast<size_t>(body["ef_con"].i()) : 200;
            int quant_level_int = body.has("quant_level") ? static_cast<int>(body["quant_level"].i()) : 8;
            int checksum = body.has("checksum") ? static_cast<int>(body["checksum"].i()) : -1;

            if(dim == 0) {
                return admin_json_error(400, "dim is required");
            }

            auto quant_level = static_cast<ndd::quant::QuantizationLevel>(quant_level_int);

            IndexConfig config{
                dim,
                sparse_dim,
                settings::MAX_ELEMENTS,
                space_type,
                M,
                ef_con,
                quant_level,
                checksum
            };

            if(index_manager.resetIndex(full_index_id, config)) {
                crow::json::wvalue response;
                response["message"] = "Index reset successfully";
                return crow::response(200, response.dump());
            }

            return admin_json_error(500, "Failed to reset index");
        });

    // 14. POST /api/v1/admin/users/<username>/indices/<index_id>/recover - Recover index (admin only)
    CROW_ROUTE(app, "/api/v1/admin/users/<string>/indices/<string>/recover")
        .CROW_MIDDLEWARES(app, ServerlessAuthMiddleware)
        .methods("POST"_method)([&index_manager, &auth_manager, &app](
                const crow::request& req, std::string username, std::string index_id) {
            auto& ctx = app.template get_context<ServerlessAuthMiddleware>(req);

            if(!auth_manager.isAdmin(ctx.username)) {
                return admin_json_error(403, "Admin access required");
            }

            // Construct full index_id (username/index_name format)
            std::string full_index_id = username + "/" + index_id;

            if(index_manager.recoverIndex(full_index_id)) {
                crow::json::wvalue response;
                response["message"] = "Index recovered successfully";
                return crow::response(200, response.dump());
            }

            return admin_json_error(404, "Index not found or recovery failed");
        });

    // ===== SELF-SERVICE ENDPOINTS (15-18) =====

    // 15a. GET /api/v1/users/<username>/info - Get user info (self or admin)
    CROW_ROUTE(app, "/api/v1/users/<string>/info")
        .CROW_MIDDLEWARES(app, ServerlessAuthMiddleware)
        .methods("GET"_method)([&auth_manager, &app](const crow::request& req, std::string username) {
            auto& ctx = app.template get_context<ServerlessAuthMiddleware>(req);

            auto info = auth_manager.getUserInfo(ctx.username, username);
            if(!info) {
                return admin_json_error(403, "Access denied or user not found");
            }

            // Convert nlohmann::json to crow response
            crow::json::wvalue response;
            response["username"] = info->at("username").template get<std::string>();
            response["user_type"] = info->at("user_type").template get<std::string>();
            response["is_active"] = info->at("is_active").template get<bool>();
            response["created_at"] = info->at("created_at").template get<int64_t>();
            response["token_count"] = info->at("token_count").template get<size_t>();
            return crow::response(200, response.dump());
        });

    // 15b. GET /api/v1/users/<username>/type - Get user type (self or admin)
    // 15c. PUT /api/v1/users/<username>/type - Set user type (admin only)
    CROW_ROUTE(app, "/api/v1/users/<string>/type")
        .CROW_MIDDLEWARES(app, ServerlessAuthMiddleware)
        .methods("GET"_method, "PUT"_method)([&auth_manager, &app](const crow::request& req, std::string username) {
            auto& ctx = app.template get_context<ServerlessAuthMiddleware>(req);

            if(req.method == crow::HTTPMethod::PUT) {
                // PUT: Set user type (admin only)
                if(!auth_manager.isAdmin(ctx.username)) {
                    return admin_json_error(403, "Admin access required");
                }

                auto body = crow::json::load(req.body);
                if(!body || !body.has("user_type")) {
                    return admin_json_error(400, "user_type is required");
                }

                std::string user_type_str = body["user_type"].s();
                UserType user_type = userTypeFromString(user_type_str);

                if(auth_manager.setUserType(username, user_type)) {
                    crow::json::wvalue response;
                    response["message"] = "User type updated successfully";
                    response["username"] = username;
                    response["user_type"] = userTypeToString(user_type);
                    return crow::response(200, response.dump());
                }

                return admin_json_error(404, "User not found");
            }

            // GET: Get user type (self or admin)
            if(ctx.username != username && !auth_manager.isAdmin(ctx.username)) {
                return admin_json_error(403, "Access denied");
            }

            auto user = auth_manager.getUser(username);
            if(!user) {
                return admin_json_error(404, "User not found");
            }

            crow::json::wvalue response;
            response["username"] = username;
            response["user_type"] = userTypeToString(user->user_type);
            return crow::response(200, response.dump());
        });

    // 16. POST /api/v1/tokens - Create own token (self-service)
    //     Body: {"name": "my-api-key"}
    CROW_ROUTE(app, "/api/v1/tokens")
        .CROW_MIDDLEWARES(app, ServerlessAuthMiddleware)
        .methods("POST"_method)([&auth_manager, &app](const crow::request& req) {
            auto& ctx = app.template get_context<ServerlessAuthMiddleware>(req);

            auto body = crow::json::load(req.body);
            if(!body || !body.has("name")) {
                return admin_json_error(400, "name is required");
            }

            std::string token_name = body["name"].s();
            std::string token = auth_manager.generateToken(ctx.username, token_name);
            if(token.empty()) {
                return admin_json_error(400, "Failed to generate token or name already exists");
            }

            crow::json::wvalue response;
            response["token"] = token;
            response["name"] = token_name;
            return crow::response(201, response.dump());
        });

    // 17. GET /api/v1/tokens - List own tokens (self-service)
    CROW_ROUTE(app, "/api/v1/tokens")
        .CROW_MIDDLEWARES(app, ServerlessAuthMiddleware)
        .methods("GET"_method)([&auth_manager, &app](const crow::request& req) {
            auto& ctx = app.template get_context<ServerlessAuthMiddleware>(req);

            auto tokens = auth_manager.listUserTokens(ctx.username);
            std::vector<crow::json::wvalue> tokens_list;

            for(const auto& token : tokens) {
                crow::json::wvalue token_json;
                token_json["name"] = token.token_name;
                token_json["username"] = token.username;
                token_json["created_at"] = std::chrono::duration_cast<std::chrono::seconds>(
                    token.created_at.time_since_epoch()).count();
                tokens_list.push_back(std::move(token_json));
            }

            crow::json::wvalue response;
            response["tokens"] = std::move(tokens_list);
            response["count"] = tokens.size();
            return crow::response(200, response.dump());
        });

    // 18. DELETE /api/v1/tokens/<token_name> - Delete own token by name (self-service)
    CROW_ROUTE(app, "/api/v1/tokens/<string>")
        .CROW_MIDDLEWARES(app, ServerlessAuthMiddleware)
        .methods("DELETE"_method)([&auth_manager, &app](const crow::request& req, std::string token_name) {
            auto& ctx = app.template get_context<ServerlessAuthMiddleware>(req);

            if(auth_manager.deleteToken(ctx.username, token_name)) {
                crow::json::wvalue response;
                response["message"] = "Token '" + token_name + "' deleted successfully";
                return crow::response(200, response.dump());
            }

            return admin_json_error(404, "Token name '" + token_name + "' not found");
        });
}
