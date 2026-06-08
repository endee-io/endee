/**
 * Standalone tool that rewrites layout-v0 (legacy) indexes into the current
 * layout-v2 shared-MDBX format. The server has been stripped of legacy
 * migration code; this binary is the only place that knows how to do it.
 *
 * Two subcommands:
 *   in-place    Rewrite a live index folder in place. Catalog row is bumped
 *               to layout_version=2 so the server loads it natively on next
 *               start. Requires the server to be stopped.
 *   from-backup Extract a legacy backup tar, migrate it into an output dir,
 *               optionally re-tar the result into a new-layout backup tar.
 *               Does not touch any catalog - the operator chooses where to
 *               drop the resulting folder/tar.
 */

#include <archive.h>
#include <archive_entry.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "json/nlohmann_json.hpp"
#include "storage/index_meta.hpp"
#include "tools/index_layout_migrator_v0_to_v2.hpp"
#include "utils/settings.hpp"

namespace fs = std::filesystem;

namespace {

void print_usage() {
    std::cerr
            << "usage:\n"
            << "  ndd-migrate-v0-to-v2 in-place --data-dir <root> --index-id <user>/<name>\n"
            << "  ndd-migrate-v0-to-v2 from-backup --backup <tar>\n"
            << "      ( --out-dir <dir> [--out-tar <new.tar>] | --replace-original [--out-dir <dir>] )\n"
            << "\n"
            << "  --replace-original  Rename the input tar to v0_<basename> in the same\n"
            << "                      directory and write the migrated tar in its place,\n"
            << "                      so the backup keeps its name in backup.json. The\n"
            << "                      unpacked dir is scratch and auto-managed unless\n"
            << "                      --out-dir is given.\n";
}

bool has_flag(const std::vector<std::string>& args, const std::string& flag) {
    for(const auto& a : args) {
        if(a == flag) {
            return true;
        }
    }
    return false;
}

std::string require_arg(const std::vector<std::string>& args,
                        const std::string& flag) {
    for(size_t i = 0; i + 1 < args.size(); ++i) {
        if(args[i] == flag) {
            return args[i + 1];
        }
    }
    throw std::runtime_error("missing required argument: " + flag);
}

std::string optional_arg(const std::vector<std::string>& args,
                         const std::string& flag,
                         const std::string& fallback = {}) {
    for(size_t i = 0; i + 1 < args.size(); ++i) {
        if(args[i] == flag) {
            return args[i + 1];
        }
    }
    return fallback;
}

void extract_tar(const fs::path& archive_path, const fs::path& dest_dir) {
    fs::create_directories(dest_dir);

    struct archive* a = archive_read_new();
    struct archive* ext = archive_write_disk_new();

    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);
    archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM);
    archive_write_disk_set_standard_lookup(ext);

    if(archive_read_open_filename(a, archive_path.string().c_str(), 10240) != ARCHIVE_OK) {
        const std::string err = archive_error_string(a);
        archive_read_free(a);
        archive_write_free(ext);
        throw std::runtime_error("failed to open backup tar: " + err);
    }

    struct archive_entry* entry;
    while(archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        fs::path full_path = dest_dir / archive_entry_pathname(entry);
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
}

/**
 * Read metadata.json out of a backup tar without extracting any other entry.
 * Walks headers and skips each entry's payload with archive_read_data_skip(),
 * so the cost is O(entries) of seek/skim rather than O(payload bytes) - a
 * backup tar can be tens of GB. Returns an empty (null) json if the entry is
 * missing or unparseable; callers treat that as "no usable metadata". Mirrors
 * BackupStore::readMetadataJsonFromTar - replicated here to keep this offline
 * tool free of server backup-store machinery, just as extract_tar/write_tar
 * already locally re-implement the BackupStore archive helpers.
 */
nlohmann::json read_metadata_json_from_tar(const fs::path& archive_path) {
    nlohmann::json result;
    struct archive* a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    if(archive_read_open_filename(a, archive_path.string().c_str(), 10240) == ARCHIVE_OK) {
        struct archive_entry* entry;
        while(archive_read_next_header(a, &entry) == ARCHIVE_OK) {
            std::string_view path = archive_entry_pathname(entry);
            if(path.ends_with("metadata.json")) {
                std::string content;
                char buf[4096];
                la_ssize_t n;
                while((n = archive_read_data(a, buf, sizeof(buf))) > 0) {
                    content.append(buf, static_cast<size_t>(n));
                }
                try {
                    result = nlohmann::json::parse(content);
                } catch(...) {
                }
                break;
            }
            archive_read_data_skip(a);
        }
    }
    archive_read_free(a);
    return result;
}

void write_tar(const fs::path& source_dir,
               const fs::path& archive_path,
               const std::string& inner_name = "") {
    struct archive* a = archive_write_new();
    archive_write_set_format_pax_restricted(a);

    if(archive_write_open_filename(a, archive_path.string().c_str()) != ARCHIVE_OK) {
        const std::string err = archive_error_string(a);
        archive_write_free(a);
        throw std::runtime_error("failed to open output tar: " + err);
    }

    /**
     * Master-produced tars put files under `<index_name>/...`. The server's
     * restoreBackup tolerates any single top-level dir name, but operators
     * who untar manually see this, so honor an explicit inner name when
     * given. Otherwise fall back to source_dir's basename (computed via
     * parent_path() relative).
     */
    const fs::path tar_top = inner_name.empty() ? source_dir.filename()
                                                : fs::path(inner_name);

    for(const auto& entry : fs::recursive_directory_iterator(source_dir)) {
        if(!entry.is_regular_file()) {
            continue;
        }
        struct archive_entry* e = archive_entry_new();
        fs::path rel_inside = fs::relative(entry.path(), source_dir);
        fs::path rel_path = tar_top / rel_inside;
        archive_entry_set_pathname(e, rel_path.string().c_str());
        archive_entry_set_size(e, fs::file_size(entry.path()));
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_perm(e, 0644);

        if(archive_write_header(a, e) != ARCHIVE_OK) {
            const std::string err = archive_error_string(a);
            archive_entry_free(e);
            archive_write_close(a);
            archive_write_free(a);
            throw std::runtime_error("failed to write tar header: " + err);
        }

        std::ifstream file(entry.path(), std::ios::binary);
        char buffer[8192];
        while(file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
            archive_write_data(a, buffer, file.gcount());
        }
        archive_entry_free(e);
    }

    archive_write_close(a);
    archive_write_free(a);
}

int do_in_place(const std::vector<std::string>& args) {
    const std::string data_dir = require_arg(args, "--data-dir");
    const std::string index_id = require_arg(args, "--index-id");

    const fs::path index_dir = fs::path(data_dir) / index_id;
    if(!fs::exists(index_dir)) {
        throw std::runtime_error("index folder does not exist: " + index_dir.string());
    }

    // Read the catalog up front so an already-migrated index is refused before
    // any disk is touched, and reuse the row for the post-migration bump.
    // MetadataManager opens <data_dir>/meta/ directly; it does not coordinate
    // with a running server, so the server must be stopped.
    MetadataManager catalog(data_dir);
    auto meta = catalog.getMetadata(index_id);
    if(meta && meta->layout_version >= settings::INDEX_LAYOUT_VERSION) {
        throw std::runtime_error(
                "catalog reports index_id=" + index_id + " is already layout_version="
                + std::to_string(meta->layout_version) + " (current is "
                + std::to_string(settings::INDEX_LAYOUT_VERSION)
                + "); nothing to migrate.");
    }

    std::cout << "in-place migrating " << index_dir << " ...\n";
    ndd::tools::IndexLayoutMigratorV0toV2::migrateInPlaceV0toV2(index_dir);

    // Bump the catalog row so the server's loadIndex accepts this index on
    // next start.
    if(!meta) {
        throw std::runtime_error(
                "migration rewrote disk layout but catalog has no row for index_id="
                + index_id + " - the server cannot load this index until you re-register it.");
    }
    meta->layout_version = settings::INDEX_LAYOUT_VERSION;
    if(!catalog.storeMetadata(index_id, *meta)) {
        throw std::runtime_error("failed to update catalog layout_version for " + index_id);
    }

    std::cout << "done. catalog row for " << index_id
              << " bumped to layout_version=" << settings::INDEX_LAYOUT_VERSION << "\n";
    return 0;
}

int do_from_backup(const std::vector<std::string>& args) {
    const fs::path backup_tar = require_arg(args, "--backup");
    const std::string out_dir_arg = optional_arg(args, "--out-dir");
    const std::string out_tar = optional_arg(args, "--out-tar");
    const bool replace_original = has_flag(args, "--replace-original");

    if(replace_original && !out_tar.empty()) {
        throw std::runtime_error("--replace-original and --out-tar are mutually exclusive");
    }
    if(!replace_original && out_dir_arg.empty()) {
        throw std::runtime_error("--out-dir is required unless --replace-original is set");
    }
    if(!fs::exists(backup_tar)) {
        throw std::runtime_error("backup tar does not exist: " + backup_tar.string());
    }

    /**
     * Scan-first: peek metadata.json straight out of the tar stream and refuse
     * an already-current backup before extracting anything. A backup tar can be
     * tens of GB, so the old order (extract -> read metadata -> reject) wrote
     * the whole payload to scratch just to throw it away. Mirrors the server's
     * restoreBackup pre-extraction layout_version gate.
     */
    {
        const nlohmann::json scanned = read_metadata_json_from_tar(backup_tar);
        const uint32_t scanned_layout =
                scanned.is_object() && scanned.contains("params")
                        ? scanned["params"].value("layout_version",
                                                  settings::LEGACY_INDEX_LAYOUT_VERSION)
                        : settings::LEGACY_INDEX_LAYOUT_VERSION;
        if(scanned_layout >= settings::INDEX_LAYOUT_VERSION) {
            throw std::runtime_error(
                    "Backup is already layout_version=" + std::to_string(scanned_layout)
                    + "; this tool only upgrades legacy layout_version="
                    + std::to_string(settings::LEGACY_INDEX_LAYOUT_VERSION)
                    + " backups to layout_version="
                    + std::to_string(settings::INDEX_LAYOUT_VERSION)
                    + ". Nothing to migrate - restore this backup as-is.");
        }
    }

    /**
     * With --replace-original the unpacked dir is just scratch space the tool
     * uses internally, not an artifact the caller wants. If --out-dir wasn't
     * passed, derive an absolute dot-prefixed sibling of the tar so it stays
     * self-contained and we can clean it up at the end. Absolute is required
     * because write_tar uses parent_path() to compute archive entry names and
     * a relative path with no parent would yield empty entry names.
     */
    const bool out_dir_is_scratch = replace_original && out_dir_arg.empty();
    const fs::path out_dir = out_dir_is_scratch
            ? fs::absolute(backup_tar).parent_path()
                    / ("." + backup_tar.filename().string() + ".migrating")
            : fs::path(out_dir_arg);

    fs::path v0_archive_path;
    fs::path staging_tar_path;
    if(replace_original) {
        v0_archive_path = backup_tar.parent_path() /
                          ("v0_" + backup_tar.filename().string());
        if(fs::exists(v0_archive_path)) {
            throw std::runtime_error(
                    "refusing to overwrite existing safety copy: " + v0_archive_path.string()
                    + " - rename or remove it before retrying");
        }
        staging_tar_path = backup_tar;
        staging_tar_path += ".v2.partial";
        fs::remove(staging_tar_path);
        if(out_dir_is_scratch) {
            fs::remove_all(out_dir);
        }
    }

    const fs::path staging = out_dir.parent_path() / (out_dir.filename().string() + ".extracted");
    fs::remove_all(staging);
    std::cout << "extracting " << backup_tar << " ...\n";
    extract_tar(backup_tar, staging);

    /**
     * The tar's top-level is `<backup_name>/...`. Find that single child dir
     * and feed it to the migrator.
     */
    fs::path backup_inner;
    for(const auto& entry : fs::directory_iterator(staging)) {
        if(entry.is_directory()) {
            if(!backup_inner.empty()) {
                throw std::runtime_error("backup tar has multiple top-level directories");
            }
            backup_inner = entry.path();
        }
    }
    if(backup_inner.empty()) {
        throw std::runtime_error("backup tar contains no top-level directory");
    }

    std::cout << "migrating into " << out_dir << " ...\n";
    ndd::tools::IndexLayoutMigratorV0toV2::migrateBackupV0toV2(backup_inner, out_dir);

    fs::remove_all(staging);

    if(!out_tar.empty()) {
        std::cout << "writing new-layout tar to " << out_tar << " ...\n";
        write_tar(out_dir, out_tar);
    } else if(replace_original) {
        /**
         * Write to a sibling .v2.partial first so the original is only ever
         * lost atomically by rename. Order: write partial -> rename original
         * to v0_<name> -> rename partial to <name>. A crash between the two
         * renames leaves the original safe at v0_<name>.tar plus a leftover
         * .v2.partial, both recoverable.
         *
         * The tar's top-level dir is set to the tar's stem (e.g. "b1" for
         * "b1.tar") so operators who untar manually see a clean name, not
         * the internal scratch dir.
         */
        std::cout << "writing new-layout tar to " << staging_tar_path << " ...\n";
        write_tar(out_dir, staging_tar_path, backup_tar.stem().string());
        std::cout << "preserving original as " << v0_archive_path << " ...\n";
        fs::rename(backup_tar, v0_archive_path);
        fs::rename(staging_tar_path, backup_tar);
        if(out_dir_is_scratch) {
            fs::remove_all(out_dir);
        }
    }

    std::cout << "done.\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if(argc < 2) {
        print_usage();
        return 2;
    }

    const std::string subcommand = argv[1];
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc - 2));
    for(int i = 2; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    try {
        if(subcommand == "in-place") {
            return do_in_place(args);
        }
        if(subcommand == "from-backup") {
            return do_from_backup(args);
        }
    } catch(const std::exception& e) {
        std::cerr << "ndd-migrate-v0-to-v2: " << e.what() << "\n";
        return 1;
    }

    print_usage();
    return 2;
}
