// Endee — high-performance vector database
// Copyright (C) 2026 Endee Labs
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

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

#if defined(__unix__) || defined(__APPLE__)
#  include <fcntl.h>
#  include <unistd.h>
#  include <sys/stat.h>
#  include <cerrno>
#  include <cstring>
#endif

#include "json/nlohmann_json.hpp"
#include "index_meta.hpp"
#include "settings.hpp"
#include "log.hpp"
#include "../utils/errors.hpp"

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

    // Writes a single file into an open libarchive PAX writer with sparse awareness.
    //
    // For truly sparse files (physical blocks < apparent size): scan for non-zero
    // data regions in 4096-byte pages, register them as PAX sparse extents, then
    // feed bytes to the archive writer sequentially — real data for regions, zero-fill
    // for hole gaps. The zero-fill is required because the PAX writer maintains a
    // sequential byte cursor and must consume hole bytes (which it discards) to stay
    // in sync; skipping them causes data bytes for the next region to be lost.
    //
    // For dense files (e.g. the HNSW .idx file): plain sequential copy. Dense files
    // may contain legitimate zero-valued bytes that must not be treated as holes.
    //
    // IMPORTANT: caller must call archive_entry_set_size() with the apparent file
    // size before calling this function.
    bool writeSparseFileToArchive(struct archive* a,
                                   struct archive_entry* e,
                                   const std::filesystem::path& file_path,
                                   std::string& error_msg) {
#if defined(__unix__) || defined(__APPLE__)
        int fd = ::open(file_path.string().c_str(), O_RDONLY | O_CLOEXEC);
        if(fd < 0) {
            error_msg = "open() failed for " + file_path.string()
                        + ": " + std::strerror(errno);
            return false;
        }

        struct stat st;
        if(::fstat(fd, &st) < 0) {
            error_msg = std::string("fstat() failed: ") + std::strerror(errno);
            ::close(fd);
            return false;
        }
        const off_t file_size = st.st_size;

        if(file_size == 0) {
            if(archive_write_header(a, e) != ARCHIVE_OK) {
                error_msg = archive_error_string(a);
                ::close(fd);
                return false;
            }
            ::close(fd);
            return true;
        }

        // Only scan for sparse regions when the OS reports fewer physical blocks
        // than the apparent size. Dense files (physical == apparent) are copied
        // sequentially to avoid misidentifying real zero bytes as holes.
        const bool is_sparse = ((off_t)st.st_blocks * 512 < file_size);

        constexpr size_t IO_BUF    = 65536;
        constexpr size_t SCAN_PAGE = 4096;  // MDBX page size

        char buf[IO_BUF];

        if(is_sparse) {
            // Pass 1: scan file in SCAN_PAGE blocks to find non-zero data regions.
            static const char kZeroPage[SCAN_PAGE] = {};
            struct SparseRegion { off_t offset; off_t length; };
            std::vector<SparseRegion> regions;

            {
                char page[SCAN_PAGE];
                off_t off = 0, region_start = -1;
                while(off < file_size) {
                    ssize_t n = ::read(fd, page,
                        (size_t)std::min((off_t)SCAN_PAGE, file_size - off));
                    if(n <= 0) break;
                    if(memcmp(page, kZeroPage, (size_t)n) != 0) {
                        if(region_start < 0) region_start = off;
                    } else if(region_start >= 0) {
                        regions.push_back({region_start, off - region_start});
                        region_start = -1;
                    }
                    off += n;
                }
                if(region_start >= 0)
                    regions.push_back({region_start, file_size - region_start});
            }

            // Register data extents on the archive entry. The PAX header stores
            // both the apparent size and the sparse map.
            if(regions.empty())
                archive_entry_sparse_add_entry(e, 0, 0); // all-hole file marker
            else
                for(const auto& r : regions)
                    archive_entry_sparse_add_entry(e,
                        (la_int64_t)r.offset, (la_int64_t)r.length);

            if(archive_write_header(a, e) != ARCHIVE_OK) {
                error_msg = archive_error_string(a);
                ::close(fd);
                return false;
            }

            // Pass 2: feed bytes to the archive in sparse-list order.
            // Send zero-fill for each hole gap (PAX writer consumes but discards them),
            // then actual data bytes for each region.
            static const char kZeroBuf[IO_BUF] = {};
            off_t cursor = 0;

            for(const auto& r : regions) {
                // Feed zeros for the hole between cursor and this region.
                for(off_t rem = r.offset - cursor; rem > 0; rem -= IO_BUF) {
                    size_t n = (size_t)std::min(rem, (off_t)IO_BUF);
                    if(archive_write_data(a, kZeroBuf, n) < 0) {
                        error_msg = archive_error_string(a);
                        ::close(fd);
                        return false;
                    }
                }
                cursor = r.offset;

                // Feed actual data bytes for this region.
                if(::lseek(fd, r.offset, SEEK_SET) < 0) {
                    error_msg = std::string("lseek failed: ") + std::strerror(errno);
                    ::close(fd);
                    return false;
                }
                for(off_t rem = r.length; rem > 0; ) {
                    ssize_t n = ::read(fd, buf,
                        (size_t)std::min(rem, (off_t)IO_BUF));
                    if(n < 0) {
                        error_msg = std::string("read() failed: ") + std::strerror(errno);
                        ::close(fd);
                        return false;
                    }
                    if(n == 0) break;
                    if(archive_write_data(a, buf, (size_t)n) < 0) {
                        error_msg = archive_error_string(a);
                        ::close(fd);
                        return false;
                    }
                    rem -= n;
                }
                cursor += r.length;
            }
        } else {
            // Dense file: write header then copy all bytes sequentially.
            if(archive_write_header(a, e) != ARCHIVE_OK) {
                error_msg = archive_error_string(a);
                ::close(fd);
                return false;
            }
            while(true) {
                ssize_t n = ::read(fd, buf, IO_BUF);
                if(n < 0) {
                    error_msg = std::string("read() failed: ") + std::strerror(errno);
                    ::close(fd);
                    return false;
                }
                if(n == 0) break;
                if(archive_write_data(a, buf, (size_t)n) < 0) {
                    error_msg = archive_error_string(a);
                    ::close(fd);
                    return false;
                }
            }
        }

        ::close(fd);
        return true;
#endif
        // Non-POSIX fallback (Windows): plain sequential copy, no sparse support.
        if(archive_write_header(a, e) != ARCHIVE_OK) {
            error_msg = archive_error_string(a);
            return false;
        }
        std::ifstream file(file_path, std::ios::binary);
        char buffer[8192];
        while(file.read(buffer, sizeof(buffer)) || file.gcount() > 0)
            archive_write_data(a, buffer, file.gcount());
        return true;
    }

    // Copies a single file preserving sparseness: scans for non-zero data regions,
    // copies only those (seeking dst to the correct offset), then ftruncate restores
    // the full apparent size. Dense files are copied sequentially.
    //
    // ftruncate is always called: for sparse files the last data region ends before
    // file_size, and without it MDBX would see the wrong apparent size on mmap.
    bool sparseCopyFile(const std::filesystem::path& src,
                        const std::filesystem::path& dst,
                        std::string& error_msg) {
#if defined(__unix__) || defined(__APPLE__)
        int src_fd = ::open(src.string().c_str(), O_RDONLY | O_CLOEXEC);
        if(src_fd < 0) {
            error_msg = "open(src) failed for " + src.string()
                        + ": " + std::strerror(errno);
            return false;
        }

        struct stat st;
        if(::fstat(src_fd, &st) < 0) {
            error_msg = std::string("fstat() failed: ") + std::strerror(errno);
            ::close(src_fd);
            return false;
        }
        const off_t file_size = st.st_size;

        int dst_fd = ::open(dst.string().c_str(),
                            O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if(dst_fd < 0) {
            error_msg = "open(dst) failed for " + dst.string()
                        + ": " + std::strerror(errno);
            ::close(src_fd);
            return false;
        }

        if(file_size == 0) {
            ::close(src_fd);
            ::close(dst_fd);
            return true;
        }

        const bool is_sparse = ((off_t)st.st_blocks * 512 < file_size);

        constexpr size_t IO_BUF    = 65536;
        constexpr size_t SCAN_PAGE = 4096;

        char buf[IO_BUF];

        if(is_sparse) {
            static const char kZeroPage[SCAN_PAGE] = {};
            struct SparseRegion { off_t offset; off_t length; };
            std::vector<SparseRegion> regions;

            {
                char page[SCAN_PAGE];
                off_t off = 0, region_start = -1;
                while(off < file_size) {
                    ssize_t n = ::read(src_fd, page,
                        (size_t)std::min((off_t)SCAN_PAGE, file_size - off));
                    if(n <= 0) break;
                    if(memcmp(page, kZeroPage, (size_t)n) != 0) {
                        if(region_start < 0) region_start = off;
                    } else if(region_start >= 0) {
                        regions.push_back({region_start, off - region_start});
                        region_start = -1;
                    }
                    off += n;
                }
                if(region_start >= 0)
                    regions.push_back({region_start, file_size - region_start});
            }

            for(const auto& r : regions) {
                if(::lseek(src_fd, r.offset, SEEK_SET) < 0 ||
                   ::lseek(dst_fd,  r.offset, SEEK_SET) < 0) {
                    error_msg = std::string("lseek failed: ") + std::strerror(errno);
                    ::close(src_fd); ::close(dst_fd);
                    return false;
                }
                for(off_t rem = r.length; rem > 0; ) {
                    ssize_t n = ::read(src_fd, buf,
                        (size_t)std::min(rem, (off_t)IO_BUF));
                    if(n < 0) {
                        error_msg = std::string("read() failed: ") + std::strerror(errno);
                        ::close(src_fd); ::close(dst_fd);
                        return false;
                    }
                    if(n == 0) break;
                    if(::write(dst_fd, buf, (size_t)n) != n) {
                        error_msg = std::string("write() failed: ") + std::strerror(errno);
                        ::close(src_fd); ::close(dst_fd);
                        return false;
                    }
                    rem -= n;
                }
            }
        } else {
            // Dense file: sequential copy.
            while(true) {
                ssize_t n = ::read(src_fd, buf, IO_BUF);
                if(n < 0) {
                    error_msg = std::string("read() failed: ") + std::strerror(errno);
                    ::close(src_fd); ::close(dst_fd);
                    return false;
                }
                if(n == 0) break;
                if(::write(dst_fd, buf, (size_t)n) != n) {
                    error_msg = std::string("write() failed: ") + std::strerror(errno);
                    ::close(src_fd); ::close(dst_fd);
                    return false;
                }
            }
        }

        if(::ftruncate(dst_fd, file_size) < 0) {
            error_msg = std::string("ftruncate() failed: ") + std::strerror(errno);
            ::close(src_fd); ::close(dst_fd);
            return false;
        }

        ::close(src_fd);
        ::close(dst_fd);
        return true;

#else
        // Non-POSIX fallback: delegate to std::filesystem.
        std::error_code ec;
        std::filesystem::copy_file(src, dst,
            std::filesystem::copy_options::overwrite_existing, ec);
        if(ec) {
            error_msg = "copy_file failed: " + ec.message();
            return false;
        }
        return true;
#endif
    }

public:
    BackupStore(const std::string& data_dir)
        : data_dir_(data_dir) {
        std::filesystem::create_directories(data_dir + "/backups");
        cleanupTempDir();
    }

    // Recursively copies src_dir → dst_dir preserving sparseness on each file.
    // Replaces std::filesystem::copy in the restore path to avoid materialising
    // the full apparent size of sparse MDBX files as physical disk blocks.
    bool sparseCopyDirectory(const std::filesystem::path& src_dir,
                              const std::filesystem::path& dst_dir,
                              std::string& error_msg) {
        for(const auto& entry : std::filesystem::recursive_directory_iterator(src_dir)) {
            std::filesystem::path rel = std::filesystem::relative(entry.path(), src_dir);
            std::filesystem::path dst_path = dst_dir / rel;

            if(entry.is_directory()) {
                std::filesystem::create_directories(dst_path);
            } else if(entry.is_regular_file()) {
                std::filesystem::create_directories(dst_path.parent_path());
                if(!sparseCopyFile(entry.path(), dst_path, error_msg)) {
                    return false;
                }
            }
        }
        return true;
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
                archive_entry_set_size(e, (la_int64_t)std::filesystem::file_size(entry.path()));
                archive_entry_set_filetype(e, AE_IFREG);
                archive_entry_set_perm(e, 0644);

                if(!writeSparseFileToArchive(a, e, entry.path(), error_msg)) {
                    archive_entry_free(e);
                    archive_write_close(a);
                    archive_write_free(a);
                    return false;
                }
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
        // ARCHIVE_EXTRACT_SPARSE: activates sparse file creation on disk — zero-valued
        // data blocks are written as lseek operations instead of actual writes.
        // archive_write_finish_entry then calls ftruncate to restore the apparent size.
        archive_write_disk_set_options(ext,
            ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_SPARSE);
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

    void validateBackupName(const std::string& backup_name) const {
        if(backup_name.empty()) {
            throw ndd::ApiError(400, "Backup name cannot be empty");
        }

        if(backup_name.length() > settings::MAX_BACKUP_NAME_LENGTH) {
            throw ndd::ApiError(400, "Backup name too long (max "
                                     + std::to_string(settings::MAX_BACKUP_NAME_LENGTH)
                                     + " characters)");
        }

        static const std::regex backup_name_regex("^[a-zA-Z0-9_-]+$");
        if(!std::regex_match(backup_name, backup_name_regex)) {
            throw ndd::ApiError(400, "Invalid backup name: only alphanumeric, underscores, "
                                     "and hyphens allowed");
        }
    }

    // Backup listing

    nlohmann::json listBackups(const std::string& username) {
        nlohmann::json backup_list_json = readBackupJson(username);
        return backup_list_json;
    }

    // Backup deletion

    void deleteBackup(const std::string& backup_name,
                      const std::string& username) {
        validateBackupName(backup_name);

        std::string backup_tar = getUserBackupDir(username) + "/" + backup_name + ".tar";

        if(std::filesystem::exists(backup_tar)) {
            std::filesystem::remove(backup_tar);

            nlohmann::json backup_db = readBackupJson(username);
            backup_db.erase(backup_name);
            writeBackupJson(username, backup_db);

            LOG_INFO(1303, username, "Deleted backup " << backup_tar);
        } else {
            throw ndd::ApiError(404, "Backup not found");
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
