#pragma once

#include <string>
#include <filesystem>
#include <unordered_map>
#include <optional>
#include <thread>
#include <mutex>

#include <archive.h>
#include <archive_entry.h>

#include "json/nlohmann_json.hpp"

class IndexManager;

enum class BackupOperation { Creation, Restoration };

inline std::string backupOperationToString(BackupOperation op) {
    switch(op) {
        case BackupOperation::Creation:    return "creation";
        case BackupOperation::Restoration: return "restoration";
    }
    return "";
}

struct CreateBackupParams {
    std::string index_id;
    std::string backup_name;
    IndexManager* index_manager;
};

struct RestoreBackupParams {
    std::string backup_name;
    std::string target_index_name;
    std::string username;
    IndexManager* index_manager;
};

struct ActiveBackup {
    std::string backup_name;
    BackupOperation operation;
    std::jthread thread;  // jthread: built-in stop_token + auto-join on destruction
};

class BackupStore {
private:
    std::string data_dir_;
    std::unordered_map<std::string, ActiveBackup> active_user_backups_;
    mutable std::mutex active_user_backups_mutex_;

public:
    BackupStore(const std::string& data_dir);

    // Core backup operations

    void createBackup(const CreateBackupParams& params, std::stop_token st);

    void restoreBackup(const RestoreBackupParams& params, std::stop_token st);

    // Archive operations

    bool createBackupTar(const std::filesystem::path& source_dir,
                         const std::filesystem::path& archive_path,
                         std::string& error_msg,
                         std::stop_token st = {});

    bool extractBackupTar(const std::filesystem::path& archive_path,
                          const std::filesystem::path& dest_dir,
                          std::string& error_msg);

    // Backup listing & info

    nlohmann::json listBackups(const std::string& username);

    nlohmann::json getBackupInfo(const std::string& backup_name, const std::string& username);

    // Backup name validation

    std::pair<bool, std::string> validateBackupName(const std::string& backup_name) const;

    // Backup deletion

    std::pair<bool, std::string> deleteBackup(const std::string& backup_name,
                                              const std::string& username);

    // Active backup tracking

    void setActiveBackup(const std::string& username,
                         const std::string& backup_name,
                         const BackupOperation& operation);

    void attachBackupThread(const std::string& username, std::jthread&& thread);

    void clearActiveBackup(const std::string& username);

    bool hasActiveBackup(const std::string& username) const;

    std::optional<std::pair<std::string, std::string>> getActiveBackup(const std::string& username);

    // Join all background backup threads before destroying IndexManager members.
    // Moves threads out under lock, then request_stop + join outside lock to avoid
    // deadlock (finishing threads call clearActiveBackup which also locks
    // active_user_backups_mutex_).
    void joinAllThreads();

    // Path helpers

    std::string getUserBackupDir(const std::string& username) const;

    std::string getBackupJsonPath(const std::string& username) const;

    std::string getUserTempDir(const std::string& username) const;

    // Backup JSON helpers

    nlohmann::json readBackupJson(const std::string& username);

    void writeBackupJson(const std::string& username, const nlohmann::json& data);

    // Temp directory cleanup

    void cleanupTempDir();
};
