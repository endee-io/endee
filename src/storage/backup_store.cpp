#include <fstream>
#include <archive.h>
#include <archive_entry.h>

#include "backup_store.hpp"
#include "../core/ndd.hpp"
#include "utils/types.hpp"

// Construction

BackupStore::BackupStore(const std::string& data_dir) :
    data_dir_(data_dir) {
    std::filesystem::create_directories(data_dir + "/backups");
    cleanupTempDir();
}

// Core backup operations

void BackupStore::createBackup(const CreateBackupParams& params, std::stop_token st) {
    std::string index_id = params.index_id;
    std::string backup_name = params.backup_name;
    auto* index_manager = params.index_manager;

    std::string username;
    size_t upos = index_id.find('/');
    if (upos != std::string::npos) {
        username = index_id.substr(0, upos);
    }

    try {
        std::string index_name;
        if (upos != std::string::npos) {
            index_name = index_id.substr(upos + 1);
        } else {
            throw std::runtime_error("Invalid index ID format");
        }

        std::string user_backup_dir = getUserBackupDir(username);
        std::filesystem::create_directories(user_backup_dir);
        std::string user_temp_dir = getUserTempDir(username);
        std::filesystem::create_directories(user_temp_dir);
        std::string source_dir = data_dir_ + "/" + index_id;
        std::string backup_tar_final = user_backup_dir + "/" + backup_name + ".tar";
        std::string backup_tar_temp = user_temp_dir + "/.tmp_" + backup_name + ".tar";

        if(std::filesystem::exists(backup_tar_final)) {
            throw std::runtime_error("Backup already exists: " + backup_name);
        }

        size_t index_size = 0;
        for(const auto& file : std::filesystem::recursive_directory_iterator(source_dir)) {
            if(!std::filesystem::is_directory(file)) {
                index_size += std::filesystem::file_size(file);
            }
        }

        auto space_info = std::filesystem::space(user_backup_dir);
        if(space_info.available < index_size * 2) {
            throw std::runtime_error("Insufficient disk space: need " +
                std::to_string(index_size * 2 / MB) + " MB");
        }

        auto meta = index_manager->metadata_manager_->getMetadata(index_id);
        nlohmann::json metadata_json;
        if(meta) {
            metadata_json["original_index"] = index_name;
            metadata_json["timestamp"] = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            metadata_json["size_mb"] = index_size / MB;
            metadata_json["params"] = {{"M", meta->M},
                           {"ef_construction", meta->ef_con},
                           {"dim", meta->dimension},
                           {"sparse_model",
                            ndd::sparseScoringModelToString(meta->sparse_model)},
                           {"space_type", meta->space_type_str},
                           {"quant_level", static_cast<int>(meta->quant_level)},
                           {"total_elements", meta->total_elements},
                           {"checksum", meta->checksum}};
            LOG_DEBUG("Metadata prepared for backup: " << metadata_json.dump());
        } else {
            LOG_ERROR(2041, index_id, "Failed to get metadata for backup");
            throw std::runtime_error("Cannot create backup without index metadata");
        }

        // Check stop_token before expensive operations
        if (st.stop_requested()) {
            LOG_INFO(2056, index_id, "Backup cancelled before backup work started");
            clearActiveBackup(username);
            return;
        }

        auto entry_ptr = index_manager->getIndexEntry(index_id);
        auto& entry = *entry_ptr;
        std::string metadata_file_in_index = source_dir + "/metadata.json";
        {
            /**
             * NOTE: While making a backup is a reading operation on the index,
             * we are picking a writer's lock here because we have disabled reader's
             * locks on other instances of read in the system right now.
             *
             * This is to enable reads while writes are happening on the index.
             * Check other instances of shared_lock on operation_mutex.
             */
            std::unique_lock<std::shared_mutex> operation_lock(entry.operation_mutex);

            // Check again after acquiring lock (shutdown may have been requested while waiting)
            if (st.stop_requested()) {
                LOG_INFO(2057, index_id, "Backup cancelled");
                clearActiveBackup(username);
                return;
            }

            index_manager->saveIndexInternal(entry);

            if(!metadata_json.empty()) {
                std::ofstream meta_file(metadata_file_in_index, std::ios::binary);
                if(!meta_file) {
                    throw std::runtime_error("Failed to create metadata file: " + metadata_file_in_index);
                }
                meta_file << metadata_json.dump(4);
                meta_file.flush();
                meta_file.close();

                if(!std::filesystem::exists(metadata_file_in_index)) {
                    throw std::runtime_error("Metadata file was not created: " + metadata_file_in_index);
                }
                LOG_DEBUG("Metadata file created: " << metadata_file_in_index << " (size: " << std::filesystem::file_size(metadata_file_in_index) << " bytes)");
            }

            std::string error_msg;
            LOG_DEBUG("Creating tar archive from " << source_dir << " to " << backup_tar_temp);
            if(!createBackupTar(source_dir, backup_tar_temp, error_msg, st)) {
                if(std::filesystem::exists(metadata_file_in_index)) {
                    std::filesystem::remove(metadata_file_in_index);
                }
                throw std::runtime_error("Failed to create tar archive: " + error_msg);
            }

            if(!std::filesystem::exists(backup_tar_temp)) {
                throw std::runtime_error("Tar archive was not created: " + backup_tar_temp);
            }
            LOG_DEBUG("Tar archive created successfully: " << backup_tar_temp << " (size: " << std::filesystem::file_size(backup_tar_temp) << " bytes)");

            if(std::filesystem::exists(metadata_file_in_index)) {
                std::filesystem::remove(metadata_file_in_index);
            }
        }

        clearActiveBackup(username);

        LOG_INFO(2042, index_id, "Backup tar created; write operations resumed");

        std::filesystem::rename(backup_tar_temp, backup_tar_final);

        nlohmann::json backup_db = readBackupJson(username);
        backup_db[backup_name] = metadata_json;
        writeBackupJson(username, backup_db);

        LOG_INFO(2043, index_id, "Backup completed: " << backup_name << " -> " << backup_tar_final);

    } catch (const std::exception& e) {
        std::string user_backup_dir = getUserBackupDir(username);
        std::string user_temp_dir = getUserTempDir(username);
        std::string source_dir = data_dir_ + "/" + index_id;
        std::string backup_tar_final = user_backup_dir + "/" + backup_name + ".tar";
        std::string backup_tar_temp = user_temp_dir + "/.tmp_" + backup_name + ".tar";
        std::string metadata_file_in_index = source_dir + "/metadata.json";

        if(std::filesystem::exists(backup_tar_temp)) {
            std::filesystem::remove(backup_tar_temp);
        }
        if(std::filesystem::exists(backup_tar_final)) {
            std::filesystem::remove(backup_tar_final);
        }
        if(std::filesystem::exists(metadata_file_in_index)) {
            std::filesystem::remove(metadata_file_in_index);
        }

        clearActiveBackup(username);

        LOG_ERROR(2044, index_id, "Backup failed for " << backup_name << ": " << e.what());
    }
}

void BackupStore::restoreBackup(const RestoreBackupParams& params, std::stop_token st) {
    std::string username = params.username;
    std::string backup_name = params.backup_name;
    std::string target_index_name = params.target_index_name;
    auto* index_manager = params.index_manager;

    std::string backup_dir_root = getUserBackupDir(username);
    std::string backup_tar = backup_dir_root + "/" + backup_name + ".tar";
    std::string user_temp_dir = getUserTempDir(username);
    std::filesystem::create_directories(user_temp_dir);
    std::string backup_extract_dir = user_temp_dir + "/" + backup_name;
    std::string target_index_id = username + "/" + target_index_name;
    std::string target_dir = data_dir_ + "/" + target_index_id;

    try {
        std::string error_msg;
        if(!extractBackupTar(backup_tar, backup_extract_dir, error_msg)) {
            throw std::runtime_error("Failed to extract backup archive: " + error_msg);
        }

        std::vector<std::string> folders;
        for(const auto& entry : std::filesystem::directory_iterator(backup_extract_dir)) {
            if(entry.is_directory()) {
                folders.push_back(entry.path().string());
            }
        }

        if(folders.size() != 1) {
            std::filesystem::remove_all(backup_extract_dir);
            throw std::runtime_error("Backup extraction failed - directory not found");
        }

        std::string backup_dir = folders[0];

        std::ifstream f(backup_dir + "/metadata.json");
        if(!f.good()) {
            std::filesystem::remove_all(backup_extract_dir);
            throw std::runtime_error("Backup metadata missing");
        }
        nlohmann::json meta_json = nlohmann::json::parse(f);

        std::filesystem::create_directories(target_dir);
        std::filesystem::copy(backup_dir,
                              target_dir,
                              std::filesystem::copy_options::recursive
                                      | std::filesystem::copy_options::overwrite_existing);

        std::filesystem::remove(target_dir + "/metadata.json");

        IndexMetadata new_meta;
        new_meta.name = target_index_name;
        new_meta.dimension = meta_json["params"]["dim"];
        new_meta.M = meta_json["params"]["M"];
        new_meta.ef_con = meta_json["params"]["ef_construction"];
        new_meta.space_type_str = meta_json["params"]["space_type"];
        new_meta.quant_level = static_cast<ndd::quant::QuantizationLevel>(
                meta_json["params"]["quant_level"].get<int>());
        const auto sparse_model = ndd::sparseScoringModelFromString(
                meta_json["params"]["sparse_model"].get<std::string>());
        new_meta.sparse_model = *sparse_model;
        new_meta.created_at = std::chrono::system_clock::now();
        new_meta.total_elements = meta_json["params"].value("total_elements", 0ul);
        new_meta.checksum = meta_json["params"].value("checksum", -1);

        index_manager->metadata_manager_->storeMetadata(target_index_id, new_meta);

        std::filesystem::remove_all(backup_extract_dir);

        {
            std::unique_lock<std::shared_mutex> write_lock(index_manager->indices_mutex_);
            index_manager->loadIndex(target_index_id);
        }

        clearActiveBackup(username);

        LOG_INFO(2045, username, target_index_name, "Restored backup from " << backup_tar);
    } catch(const std::exception& e) {
        std::filesystem::remove_all(backup_extract_dir);
        clearActiveBackup(username);
        LOG_ERROR(2058,
                  backup_name,
                  "Restoration of backup failed for " << backup_name << ", index name "
                                                      << target_index_name << ": " << e.what());
    }
}

// Archive operations

bool BackupStore::createBackupTar(const std::filesystem::path& source_dir,
                                  const std::filesystem::path& archive_path,
                                  std::string& error_msg,
                                  std::stop_token st) {
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

bool BackupStore::extractBackupTar(const std::filesystem::path& archive_path,
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

// Backup listing & info

nlohmann::json BackupStore::listBackups(const std::string& username) {
    nlohmann::json backup_list_json = readBackupJson(username);
    return backup_list_json;
}

nlohmann::json BackupStore::getBackupInfo(const std::string& backup_name,
                                          const std::string& username) {
    nlohmann::json backup_db = readBackupJson(username);
    if(backup_db.contains(backup_name)) {
        return backup_db[backup_name];
    }
    return nlohmann::json();
}

// Backup name validation

std::pair<bool, std::string> BackupStore::validateBackupName(const std::string& backup_name) const {
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

// Backup deletion

std::pair<bool, std::string> BackupStore::deleteBackup(const std::string& backup_name,
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

// Active backup tracking

void BackupStore::setActiveBackup(const std::string& username,
                                  const std::string& backup_name,
                                  const BackupOperation& operation) {
    std::lock_guard<std::mutex> lock(active_user_backups_mutex_);
    active_user_backups_[username] = ActiveBackup{backup_name, operation, {}};
}

void BackupStore::attachBackupThread(const std::string& username, std::jthread&& thread) {
    std::lock_guard<std::mutex> lock(active_user_backups_mutex_);
    auto it = active_user_backups_.find(username);
    if(it != active_user_backups_.end()) {
        it->second.thread = std::move(thread);
    }
}

void BackupStore::clearActiveBackup(const std::string& username) {
    std::lock_guard<std::mutex> lock(active_user_backups_mutex_);
    auto it = active_user_backups_.find(username);
    if(it != active_user_backups_.end()) {
        // Called from within the thread itself — detach so erase doesn't try to join
        if(it->second.thread.joinable()) {
            it->second.thread.detach();
        }
        active_user_backups_.erase(it);
    }
}

bool BackupStore::hasActiveBackup(const std::string& username) const {
    std::lock_guard<std::mutex> lock(active_user_backups_mutex_);
    return active_user_backups_.count(username) > 0;
}

std::optional<std::pair<std::string, std::string>> BackupStore::getActiveBackup(const std::string& username) {
    std::lock_guard<std::mutex> lock(active_user_backups_mutex_);
    auto it = active_user_backups_.find(username);
    if(it != active_user_backups_.end()) {
        return std::make_pair(it->second.backup_name,
                              backupOperationToString(it->second.operation));
    }
    return std::nullopt;
}

void BackupStore::joinAllThreads() {
    std::vector<std::jthread> threads_to_join;
    {
        std::lock_guard<std::mutex> lock(active_user_backups_mutex_);
        for(auto& [username, backup] : active_user_backups_) {
            if(backup.thread.joinable()) {
                threads_to_join.push_back(std::move(backup.thread));
            }
        }
        active_user_backups_.clear();
    }
    // request_stop + join outside the lock
    for(auto& t : threads_to_join) {
        t.request_stop();  // signal stop_token — thread sees it inside createBackupTar
        if(t.joinable()) {
            t.join();
        }
    }
}

// Path helpers

std::string BackupStore::getUserBackupDir(const std::string& username) const {
    return data_dir_ + "/backups/" + username;
}

std::string BackupStore::getBackupJsonPath(const std::string& username) const {
    return getUserBackupDir(username) + "/backup.json";
}

std::string BackupStore::getUserTempDir(const std::string& username) const {
    return data_dir_ + "/backups/.tmp/" + username;
}

// Backup JSON helpers

void BackupStore::writeBackupJson(const std::string& username, const nlohmann::json& data) {
    std::string path = getBackupJsonPath(username);
    std::ofstream f(path);
    f << data.dump(2);
}

nlohmann::json BackupStore::readBackupJson(const std::string& username) {
    std::string path = getBackupJsonPath(username);
    if(!std::filesystem::exists(path)) {
        return nlohmann::json::object();
    }
    try {
        std::ifstream f(path);
        return nlohmann::json::parse(f);
    } catch(const std::exception& e) {
        LOG_WARN(1304,
                 username,
                 "Failed to parse backup metadata file " << path << ": " << e.what());
        return nlohmann::json::object();
    }
}

// Temp directory cleanup

void BackupStore::cleanupTempDir() {
    std::string temp_dir = data_dir_ + "/backups/.tmp";
    if(std::filesystem::exists(temp_dir)) {
        try {
            std::filesystem::remove_all(temp_dir);
            LOG_INFO(1301, "Cleaned up backup temp directory");
        } catch(const std::exception& e) {
            LOG_ERROR(1302, "Failed to clean up backup temp directory: " << e.what());
        }
    }
}
