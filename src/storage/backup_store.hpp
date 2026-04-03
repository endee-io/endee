#pragma once

#include <string>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <optional>

#include <thread>
#include <chrono>
#include <regex>
#include <mutex>

#include <archive.h>
#include <archive_entry.h>

#include "json/nlohmann_json.hpp"
#include "index_meta.hpp"
#include "settings.hpp"
#include "log.hpp"

struct ActiveBackup {
    std::string index_id;
    std::string backup_name;
    std::jthread thread;       // jthread: built-in stop_token + auto-join on destruction
};

class BackupStore {
private:
    std::string data_dir_;
    std::unordered_map<std::string, ActiveBackup> active_user_backups_;
    mutable std::mutex active_user_backups_mutex_;

public:
    BackupStore(const std::string& data_dir)
        : data_dir_(data_dir) {
        std::filesystem::create_directories(data_dir + "/backups");
        cleanupTempDir();
    }

    // Archive methods

    bool createBackupTar(const std::filesystem::path& source_dir,
                         const std::filesystem::path& archive_path,
                         std::string& error_msg,
                         std::stop_token st = {}) {
        struct archive* a = archive_write_new();
        archive_write_set_format_pax_restricted(a);

        if(archive_write_open_filename(a, archive_path.string().c_str()) != ARCHIVE_OK) {
            error_msg = archive_error_string(a);
            archive_write_free(a);
            return false;
        }

        for(const auto& entry : std::filesystem::recursive_directory_iterator(source_dir)) {
            // Check stop_token per-file so shutdown doesn't block on large tar operations
            if(st.stop_requested()) {
                archive_write_close(a);
                archive_write_free(a);
                error_msg = "Backup cancelled";
                return false;
            }
            if(entry.is_regular_file()) {
                struct archive_entry* e = archive_entry_new();

                std::filesystem::path rel_path =
                        std::filesystem::relative(entry.path(), source_dir.parent_path());
                archive_entry_set_pathname(e, rel_path.string().c_str());
                archive_entry_set_size(e, std::filesystem::file_size(entry.path()));
                archive_entry_set_filetype(e, AE_IFREG);
                archive_entry_set_perm(e, 0644);

                if(archive_write_header(a, e) != ARCHIVE_OK) {
                    error_msg = archive_error_string(a);
                    archive_entry_free(e);
                    archive_write_close(a);
                    archive_write_free(a);
                    return false;
                }

                std::ifstream file(entry.path(), std::ios::binary);
                char buffer[8192];
                while(file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
                    archive_write_data(a, buffer, file.gcount());
                }
                file.close();
                archive_entry_free(e);
            }
        }

        archive_write_close(a);
        archive_write_free(a);
        return true;
    }

    bool extractBackupTar(const std::filesystem::path& archive_path,
                          const std::filesystem::path& dest_dir,
                          std::string& error_msg) {
        struct archive* a = archive_read_new();
        struct archive* ext = archive_write_disk_new();
        struct archive_entry* entry;

        archive_read_support_format_all(a);
        archive_read_support_filter_all(a);
        archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM);
        archive_write_disk_set_standard_lookup(ext);

        if(archive_read_open_filename(a, archive_path.string().c_str(), 10240) != ARCHIVE_OK) {
            error_msg = archive_error_string(a);
            archive_read_free(a);
            archive_write_free(ext);
            return false;
        }

        while(archive_read_next_header(a, &entry) == ARCHIVE_OK) {
            std::filesystem::path full_path = dest_dir / archive_entry_pathname(entry);
            archive_entry_set_pathname(entry, full_path.string().c_str());

            if(archive_write_header(ext, entry) == ARCHIVE_OK) {
                const void* buff;
                size_t size;
                la_int64_t offset;

                while(archive_read_data_block(a, &buff, &size, &offset) == ARCHIVE_OK) {
                    archive_write_data_block(ext, buff, size, offset);
                }
            }
            archive_write_finish_entry(ext);
        }

        archive_read_close(a);
        archive_read_free(a);
        archive_write_close(ext);
        archive_write_free(ext);
        return true;
    }

    // Path helpers

    std::string getUserBackupDir(const std::string& username) const {
        return data_dir_ + "/backups/" + username;
    }

    std::string getBackupJsonPath(const std::string& username) const {
        return getUserBackupDir(username) + "/backup.json";
    }

    std::string getUserTempDir(const std::string& username) const {
        return data_dir_ + "/backups/.tmp/" + username;
    }

    // Backup JSON helpers

    nlohmann::json readBackupJson(const std::string& username) {
        std::string path = getBackupJsonPath(username);
        if (!std::filesystem::exists(path)) return nlohmann::json::object();
        try {
            std::ifstream f(path);
            return nlohmann::json::parse(f);
        } catch (const std::exception& e) {
            LOG_WARN(1304,
                          username,
                          "Failed to parse backup metadata file " << path << ": " << e.what());
            return nlohmann::json::object();
        }
    }

    void writeBackupJson(const std::string& username, const nlohmann::json& data) {
        std::string path = getBackupJsonPath(username);
        std::ofstream f(path);
        f << data.dump(2);
    }

    // Temp directory cleanup

    void cleanupTempDir() {
        std::string temp_dir = data_dir_ + "/backups/.tmp";
        if (std::filesystem::exists(temp_dir)) {
            try {
                std::filesystem::remove_all(temp_dir);
                LOG_INFO(1301, "Cleaned up backup temp directory");
            } catch (const std::exception& e) {
                LOG_ERROR(1302, "Failed to clean up backup temp directory: " << e.what());
            }
        }
    }

    // Active backup tracking

    void setActiveBackup(const std::string& username, const std::string& index_id,
                         const std::string& backup_name, std::jthread&& thread) {
        std::lock_guard<std::mutex> lock(active_user_backups_mutex_);
        active_user_backups_[username] = {index_id, backup_name, std::move(thread)};
    }

    void clearActiveBackup(const std::string& username) {
        std::lock_guard<std::mutex> lock(active_user_backups_mutex_);
        auto it = active_user_backups_.find(username);
        if (it != active_user_backups_.end()) {
            // Called from within the thread itself — detach so erase doesn't try to join
            if (it->second.thread.joinable()) {
                it->second.thread.detach();
            }
            active_user_backups_.erase(it);
        }
    }

    bool hasActiveBackup(const std::string& username) const {
        std::lock_guard<std::mutex> lock(active_user_backups_mutex_);
        return active_user_backups_.count(username) > 0;
    }

    // Join all background backup threads before destroying IndexManager members.
    // Moves threads out under lock, then request_stop + join outside lock to avoid
    // deadlock (finishing threads call clearActiveBackup which also locks active_user_backups_mutex_).
    void joinAllThreads() {
        std::vector<std::jthread> threads_to_join;
        {
            std::lock_guard<std::mutex> lock(active_user_backups_mutex_);
            for (auto& [username, backup] : active_user_backups_) {
                if (backup.thread.joinable()) {
                    threads_to_join.push_back(std::move(backup.thread));
                }
            }
            active_user_backups_.clear();
        }
        // request_stop + join outside the lock
        for (auto& t : threads_to_join) {
            t.request_stop();   // signal stop_token — thread sees it inside createBackupTar
            if (t.joinable()) {
                t.join();
            }
        }
    }

    // Backup name validation

    std::pair<bool, std::string> validateBackupName(const std::string& backup_name) const {
        if(backup_name.empty()) {
            return std::make_pair(false, "Backup name cannot be empty");
        }

        if(backup_name.length() > settings::MAX_BACKUP_NAME_LENGTH) {
            return std::make_pair(false,
                                  "Backup name too long (max "
                                          + std::to_string(settings::MAX_BACKUP_NAME_LENGTH)
                                          + " characters)");
        }

        static const std::regex backup_name_regex("^[a-zA-Z0-9_-]+$");
        if(!std::regex_match(backup_name, backup_name_regex)) {
            return std::make_pair(false,
                                  "Invalid backup name: only alphanumeric, underscores, "
                                  "and hyphens allowed");
        }

        return std::make_pair(true, "");
    }

    // Backup listing

    nlohmann::json listBackups(const std::string& username) {
        nlohmann::json backup_list_json = readBackupJson(username);
        return backup_list_json;
    }

    // Backup deletion

    std::pair<bool, std::string> deleteBackup(const std::string& backup_name,
                                               const std::string& username) {
        std::pair<bool, std::string> result = validateBackupName(backup_name);
        if(!result.first) {
            return result;
        }

        std::string backup_tar = getUserBackupDir(username) + "/" + backup_name + ".tar";

        if(std::filesystem::exists(backup_tar)) {
            std::filesystem::remove(backup_tar);

            nlohmann::json backup_db = readBackupJson(username);
            backup_db.erase(backup_name);
            writeBackupJson(username, backup_db);

            LOG_INFO(1303, username, "Deleted backup " << backup_tar);
            return {true, ""};
        } else {
            return {false, "Backup not found"};
        }
    }

    // Active backup query

    std::optional<std::pair<std::string, std::string>> getActiveBackup(const std::string& username) {
        std::lock_guard<std::mutex> lock(active_user_backups_mutex_);
        auto it = active_user_backups_.find(username);
        if (it != active_user_backups_.end()) {
            return std::make_pair(it->second.index_id, it->second.backup_name);
        }
        return std::nullopt;
    }

    // Backup info

    nlohmann::json getBackupInfo(const std::string& backup_name, const std::string& username) {
        nlohmann::json backup_db = readBackupJson(username);
        if (backup_db.contains(backup_name)) {
            return backup_db[backup_name];
        }
        return nlohmann::json();
    }
};
