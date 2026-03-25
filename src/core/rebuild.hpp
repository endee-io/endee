#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <mutex>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>

#include "settings.hpp"
#include "log.hpp"

struct ActiveRebuild {
    std::string index_id;
    std::string status{"in_progress"};  // "in_progress", "completed", "failed"
    std::string error_message;
    std::atomic<size_t> vectors_processed{0};
    std::atomic<size_t> total_vectors{0};
    std::chrono::system_clock::time_point started_at;
    std::chrono::system_clock::time_point completed_at;
};

class Rebuild {
private:
    // Keyed by username — one rebuild per user at a time
    std::unordered_map<std::string, std::shared_ptr<ActiveRebuild>> active_rebuilds_;
    mutable std::mutex rebuild_state_mutex_;

    static std::string timeToISO8601(std::chrono::system_clock::time_point tp) {
        auto time_t_val = std::chrono::system_clock::to_time_t(tp);
        std::tm tm_val{};
        gmtime_r(&time_t_val, &tm_val);
        std::ostringstream oss;
        oss << std::put_time(&tm_val, "%Y-%m-%dT%H:%M:%SZ");
        return oss.str();
    }

public:
    Rebuild() = default;

    // Lifecycle — cleanup temp files from interrupted rebuilds on startup
    void cleanupTempFiles(const std::string& data_dir) {
        if (!std::filesystem::exists(data_dir)) {
            return;
        }
        try {
            std::string temp_filename = std::string(settings::DEFAULT_SUBINDEX) + ".idx.temp";
            for (const auto& entry : std::filesystem::recursive_directory_iterator(data_dir)) {
                if (entry.is_regular_file() &&
                    entry.path().filename().string() == temp_filename) {
                    std::filesystem::remove(entry.path());
                }
            }
        } catch (const std::exception&) {
            // Silently ignore cleanup errors on startup
        }
    }

    // State tracking — per user

    void setActiveRebuild(const std::string& username, const std::string& index_id,
                          size_t total_vectors) {
        std::lock_guard<std::mutex> lock(rebuild_state_mutex_);
        auto state = std::make_shared<ActiveRebuild>();
        state->index_id = index_id;
        state->status = "in_progress";
        state->total_vectors.store(total_vectors);
        state->vectors_processed.store(0);
        state->started_at = std::chrono::system_clock::now();
        active_rebuilds_[username] = state;
    }

    void completeActiveRebuild(const std::string& username) {
        std::lock_guard<std::mutex> lock(rebuild_state_mutex_);
        auto it = active_rebuilds_.find(username);
        if (it != active_rebuilds_.end()) {
            it->second->status = "completed";
            it->second->completed_at = std::chrono::system_clock::now();
        }
    }

    void failActiveRebuild(const std::string& username, const std::string& error) {
        std::lock_guard<std::mutex> lock(rebuild_state_mutex_);
        auto it = active_rebuilds_.find(username);
        if (it != active_rebuilds_.end()) {
            it->second->status = "failed";
            it->second->error_message = error;
            it->second->completed_at = std::chrono::system_clock::now();
        }
    }

    bool hasActiveRebuild(const std::string& username) const {
        std::lock_guard<std::mutex> lock(rebuild_state_mutex_);
        auto it = active_rebuilds_.find(username);
        // Only "in_progress" blocks a new rebuild
        return it != active_rebuilds_.end() && it->second->status == "in_progress";
    }

    std::shared_ptr<ActiveRebuild> getActiveRebuild(const std::string& username) const {
        std::lock_guard<std::mutex> lock(rebuild_state_mutex_);
        auto it = active_rebuilds_.find(username);
        if (it != active_rebuilds_.end()) {
            return it->second;
        }
        return nullptr;
    }

    // Format state as JSON fields
    static std::string formatTime(std::chrono::system_clock::time_point tp) {
        return timeToISO8601(tp);
    }

    // Path helpers

    static std::string getTempPath(const std::string& index_dir) {
        return index_dir + "/vectors/" + settings::DEFAULT_SUBINDEX + ".idx.temp";
    }

    static std::string getTimestampedPath(const std::string& index_dir) {
        auto ts = std::to_string(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count()
        );
        return index_dir + "/vectors/" + settings::DEFAULT_SUBINDEX + ".idx." + ts;
    }
};
