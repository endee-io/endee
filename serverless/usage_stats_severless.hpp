#pragma once
#include "../src/core/ndd.hpp"
#include "settings_serverless.hpp"
#include <curl/curl.h>

namespace serverless {

// Send usage stats payload to remote endpoint via HTTP POST
inline bool sendUsageStatsToServer(const nlohmann::json& usage_data) {
    std::string url = settings::serverless::USAGE_STATS_URL;
    std::string payload = usage_data.dump();

    if(url.empty()) {
        LOG_DEBUG("Usage stats URL empty, logging payload: " << payload);
        return false;
    }

    CURL* curl = curl_easy_init();
    if(!curl) {
        LOG_DEBUG("Failed to init curl for usage stats");
        return false;
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if(res != CURLE_OK) {
        LOG_DEBUG("Usage stats send failed: " << curl_easy_strerror(res));
        return false;
    }

    return true;
}

// Collect usage stats from all indices and send to remote endpoint
// Called from IndexManager::checkAndSaveIndices() every 5 minutes
// Only sends if SERVER_TYPE == "SERVERLESS" and searchCount > 0
inline void collectAndSendUsageStats(std::unordered_map<std::string, std::shared_ptr<CacheEntry>>& indices) {
    nlohmann::json usage_array = nlohmann::json::array();

    auto now = std::chrono::system_clock::now();
    auto ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    for(auto& [index_id, entry] : indices) {
        if(entry && entry->searchCount > 0) {
            usage_array.push_back({
                {"server_id", settings::SERVER_ID},
                {"index_id", index_id},
                {"query_count", entry->searchCount},
                {"timestamp", ts}
            });
            entry->resetSearchCount();
        }
    }

    if(!usage_array.empty() && settings::SERVER_TYPE == "SERVERLESS") {
        sendUsageStatsToServer(usage_array);
    }
}

}  // namespace serverless
