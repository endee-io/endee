#include <iostream>
#include <fstream>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits.h>
#include <filesystem>
#include <thread>

#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach.h>
#endif

#include "utils/settings.hpp"
#include "utils/log.hpp"
#include "utils/cpu_compat_check/check_avx_compat.hpp"
#include "utils/cpu_compat_check/check_arm_compat.hpp"
#include "utils/system_sanity/system_sanity.hpp"

static bool is_cpu_compatible() {
    bool ret = true;

#if defined(USE_AVX2) && (defined(__x86_64__) || defined(_M_X64))
    ret &= is_avx2_compatible();
#endif  //AVX2 checks

#if defined(USE_AVX512) && (defined(__x86_64__) || defined(_M_X64))
    ret &= is_avx512_compatible();
#endif  //AVX512 checks

#if defined(USE_NEON)
    ret &= is_neon_compatible();
#endif

#if defined(USE_SVE2)
    ret &= is_sve2_compatible();
#endif

    return ret;
}

bool check_cpu_compatibility() {
    if(!is_cpu_compatible()) {
        LOG_ERROR(1739, "CPU is not compatible with required instruction sets");
        return false;
    }
    LOG_INFO(1738, "CPU compatibility check passed");
    return true;
}

// ============================================================================
// Disk space
// ============================================================================

static size_t get_remaining_storage(const char *folder_path) {
    struct statvfs vfs;

    if (!folder_path || statvfs(folder_path, &vfs) != 0) {
        LOG_ERROR(1747, "statvfs failed for " << (folder_path ? folder_path : "(null)"));
        return SIZE_MAX;  // error sentinel
    }

    return (size_t)vfs.f_bavail * (size_t)vfs.f_frsize;
}

bool is_disk_full(){
    size_t remaining_size = get_remaining_storage(settings::DATA_DIR.c_str());

    if(remaining_size < settings::MINIMUM_REQUIRED_FS_BYTES){
        LOG_INFO("Remaining storage in " + settings::DATA_DIR + " is : " + std::to_string(remaining_size/MB) + " MB");
        return true;
    }
    return false;
}

bool check_disk_space() {
    if(is_disk_full()) {
        LOG_ERROR(1746, "Insufficient disk space in " << settings::DATA_DIR
                  << "; need at least " << settings::MINIMUM_REQUIRED_FS_BYTES / GB << " GB");
        return false;
    }
    size_t remaining = get_remaining_storage(settings::DATA_DIR.c_str());
    LOG_INFO(1745, "Disk space check passed: " << remaining / MB << " MB available in " << settings::DATA_DIR);
    return true;
}

// ============================================================================
// DATA_DIR permissions
// ============================================================================

bool check_data_dir_permissions() {
    if(!std::filesystem::exists(settings::DATA_DIR)) {
        std::error_code ec;
        std::filesystem::create_directories(settings::DATA_DIR, ec);
        if(ec) {
            LOG_ERROR(1743, "Cannot create DATA_DIR: " << settings::DATA_DIR
                      << " (" << ec.message() << ")");
            return false;
        }
        LOG_INFO(1740, "Created DATA_DIR: " << settings::DATA_DIR);
    }

    if(!std::filesystem::is_directory(settings::DATA_DIR)) {
        LOG_ERROR(1741, "DATA_DIR is not a directory: " << settings::DATA_DIR);
        return false;
    }

    if(access(settings::DATA_DIR.c_str(), R_OK | W_OK) != 0) {
        LOG_ERROR(1742, "DATA_DIR is not readable/writable: " << settings::DATA_DIR);
        return false;
    }

    LOG_INFO(1744, "DATA_DIR permissions check passed: " << settings::DATA_DIR);
    return true;
}

// ============================================================================
// Available memory (cgroup-aware)
// ============================================================================

/**
 * Try to read a single numeric value from a file.
 * Returns 0 on failure (file doesn't exist, can't parse, or value is "max").
 */
static size_t read_cgroup_value(const char* path) {
    std::ifstream f(path);
    if(!f.is_open()) return 0;

    std::string line;
    if(!std::getline(f, line)) return 0;

    // cgroup v2 memory.max can be "max" meaning unlimited
    if(line == "max") return 0;

    try {
        return std::stoull(line);
    } catch(...) {
        return 0;
    }
}

static bool parse_cgroup_line(const std::string& line,
                              std::string& controllers,
                              std::string& cgroup_path) {
    const size_t first_colon = line.find(':');
    if(first_colon == std::string::npos) return false;

    const size_t second_colon = line.find(':', first_colon + 1);
    if(second_colon == std::string::npos) return false;

    controllers = line.substr(first_colon + 1, second_colon - first_colon - 1);
    cgroup_path = line.substr(second_colon + 1);
    return !cgroup_path.empty();
}

static bool controller_list_contains(const std::string& controllers, const char* controller) {
    size_t start = 0;
    while(start < controllers.size()) {
        size_t end = controllers.find(',', start);
        if(end == std::string::npos) {
            end = controllers.size();
        }
        if(controllers.compare(start, end - start, controller) == 0) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

static bool read_current_cgroup_v2_path(std::string& cgroup_path) {
    std::ifstream cgroup_file("/proc/self/cgroup");
    if(!cgroup_file.is_open()) return false;

    std::string line;
    while(std::getline(cgroup_file, line)) {
        std::string controllers;
        std::string candidate_path;
        if(parse_cgroup_line(line, controllers, candidate_path) && controllers.empty()) {
            cgroup_path = candidate_path;
            return true;
        }
    }
    return false;
}

static bool read_current_cgroup_v1_path(const char* controller, std::string& cgroup_path) {
    std::ifstream cgroup_file("/proc/self/cgroup");
    if(!cgroup_file.is_open()) return false;

    std::string line;
    while(std::getline(cgroup_file, line)) {
        std::string controllers;
        std::string candidate_path;
        if(parse_cgroup_line(line, controllers, candidate_path)
           && controller_list_contains(controllers, controller)) {
            cgroup_path = candidate_path;
            return true;
        }
    }
    return false;
}

static std::string build_cgroup_file_path(const char* mount_root,
                                          const std::string& cgroup_path,
                                          const char* file_name) {
    if(cgroup_path.empty() || cgroup_path == "/") {
        return std::string(mount_root) + "/" + file_name;
    }
    if(cgroup_path.front() == '/') {
        return std::string(mount_root) + cgroup_path + "/" + file_name;
    }
    return std::string(mount_root) + "/" + cgroup_path + "/" + file_name;
}

/**
 * Get available memory in bytes, respecting cgroup limits.
 *
 * Priority:
 *   1. current-process cgroup v2 (memory.max - memory.current)
 *   2. current-process cgroup v1 (memory.limit_in_bytes - memory.usage_in_bytes)
 *   3. Host memory (macOS: host_statistics64, Linux: /proc/meminfo MemAvailable)
 */
static size_t get_available_memory_bytes(std::string& source) {
    // Try the current process's cgroup v2 path.
    std::string cg2_path;
    if(read_current_cgroup_v2_path(cg2_path)) {
        const std::string cg2_limit_path =
                build_cgroup_file_path("/sys/fs/cgroup", cg2_path, "memory.max");
        size_t cg2_limit = read_cgroup_value(cg2_limit_path.c_str());
        if(cg2_limit > 0) {
            const std::string cg2_usage_path =
                    build_cgroup_file_path("/sys/fs/cgroup", cg2_path, "memory.current");
            size_t cg2_usage = read_cgroup_value(cg2_usage_path.c_str());
            source = "cgroup v2 (" + cg2_path + ")";
            return (cg2_limit > cg2_usage) ? (cg2_limit - cg2_usage) : 0;
        }
    }

    // Try the current process's cgroup v1 memory controller path.
    std::string cg1_path;
    if(read_current_cgroup_v1_path("memory", cg1_path)) {
        const std::string cg1_limit_path = build_cgroup_file_path(
                "/sys/fs/cgroup/memory", cg1_path, "memory.limit_in_bytes");
        size_t cg1_limit = read_cgroup_value(cg1_limit_path.c_str());
        // cgroup v1 sets limit to a very large number when unlimited
        // (typically PAGE_COUNTER_MAX * PAGE_SIZE, around 2^63).
        // Treat anything above 1 PB as "unlimited".
        constexpr size_t CGROUP_V1_UNLIMITED_THRESHOLD = 1024ULL * TB;
        if(cg1_limit > 0 && cg1_limit < CGROUP_V1_UNLIMITED_THRESHOLD) {
            const std::string cg1_usage_path = build_cgroup_file_path(
                    "/sys/fs/cgroup/memory", cg1_path, "memory.usage_in_bytes");
            size_t cg1_usage = read_cgroup_value(cg1_usage_path.c_str());
            source = "cgroup v1 (" + cg1_path + ")";
            return (cg1_limit > cg1_usage) ? (cg1_limit - cg1_usage) : 0;
        }
    }

    // Fallback to host memory
#ifdef __APPLE__
    vm_statistics64_data_t vm_stat;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    kern_return_t kr = host_statistics64(mach_host_self(), HOST_VM_INFO64,
                                          (host_info64_t)&vm_stat, &count);
    if(kr != KERN_SUCCESS) {
        LOG_ERROR(1751, "Failed to get macOS VM statistics");
        return 0;
    }
    source = "host (macOS vm_stat)";
    return ((size_t)vm_stat.free_count + (size_t)vm_stat.inactive_count) * vm_page_size;
#else
    // Linux: parse MemAvailable from /proc/meminfo
    std::ifstream meminfo("/proc/meminfo");
    if(!meminfo.is_open()) {
        LOG_ERROR(1752, "Failed to open /proc/meminfo");
        return 0;
    }
    std::string line;
    while(std::getline(meminfo, line)) {
        if(line.rfind("MemAvailable:", 0) == 0) {
            size_t kb = 0;
            if(sscanf(line.c_str(), "MemAvailable: %zu kB", &kb) == 1) {
                source = "host (/proc/meminfo)";
                return kb * 1024;
            }
        }
    }
    LOG_ERROR(1753, "MemAvailable not found in /proc/meminfo");
    return 0;
#endif
}

bool check_available_memory() {
    std::string source;
    size_t available = get_available_memory_bytes(source);

    if(available == 0) {
        LOG_ERROR(1750, "Could not determine available memory");
        return false;
    }

    size_t required_bytes = settings::MINIMUM_REQUIRED_DRAM_MB * MB;
    if(available < required_bytes) {
        LOG_ERROR(1749, "Insufficient available memory: "
                  << available / MB << " MB available (source: " << source
                  << "), need at least " << settings::MINIMUM_REQUIRED_DRAM_MB << " MB");
        return false;
    }

    LOG_INFO(1748, "Memory check passed: " << available / MB
             << " MB available (source: " << source << ")");
    return true;
}

// ============================================================================
// Open files limit (ulimit -n)
// ============================================================================

bool check_open_files_limit() {
    struct rlimit rlim;
    if(getrlimit(RLIMIT_NOFILE, &rlim) != 0) {
        LOG_ERROR(1756, "Failed to get RLIMIT_NOFILE");
        return false;
    }

    LOG_INFO(1754, "Open files limit: soft=" << rlim.rlim_cur
             << " hard=" << rlim.rlim_max);

    if(rlim.rlim_cur < settings::MINIMUM_OPEN_FILES) {
        LOG_ERROR(1755, "Open files soft limit (" << rlim.rlim_cur
                  << ") is below minimum (" << settings::MINIMUM_OPEN_FILES
                  << "). Increase with: ulimit -n " << settings::MINIMUM_OPEN_FILES);
        return false;
    }
    return true;
}

// ============================================================================
// Warning-level checks (informational, don't block startup)
// ============================================================================

bool check_total_physical_memory() {
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    if(pages <= 0 || page_size <= 0) {
        LOG_WARN(1759, "Could not determine total physical memory");
        return true;
    }
    size_t total = (size_t)pages * (size_t)page_size;
    LOG_INFO(1757, "Total physical memory: " << total / MB << " MB");
    if(total < 16 * GB) {
        LOG_WARN(1758, "Total physical memory (" << total / MB
                 << " MB) is low for a vector database workload");
    }
    return true;
}

bool check_cpu_cores() {
    unsigned int cores = std::thread::hardware_concurrency();
    LOG_INFO(1760, "CPU cores detected: " << cores);
    if(cores < settings::DEFAULT_MINIMUM_CPU_CORES) {
        LOG_WARN(1761, "CPU core count (" << cores
                 << ") is below recommended minimum (" << settings::DEFAULT_MINIMUM_CPU_CORES << ")");
    }
    return true;
}

// ============================================================================
// Orchestrator
// ============================================================================

bool run_startup_sanity_checks() {
    LOG_INFO("=== Running system sanity checks ===");
    bool critical_pass = true;

    // Ensure the host CPU supports the SIMD/instruction-set features required by this build.
    critical_pass &= check_cpu_compatibility();
    // Ensure DATA_DIR exists or can be created and is usable for database reads and writes.
    critical_pass &= check_data_dir_permissions();
    // Ensure the configured data directory has enough free space for startup to proceed safely.
    critical_pass &= check_disk_space();
    // Ensure available memory, including cgroup-limited environments, meets the minimum requirement.
    critical_pass &= check_available_memory();
    // Ensure the process can keep enough files open for expected database and index usage.
    critical_pass &= check_open_files_limit();

    // Warning checks - logged but don't block startup
    // check_total_physical_memory();
    // Advisory only: warn when the machine has fewer CPU cores than the recommended minimum.
    check_cpu_cores();

    if(critical_pass) {
        LOG_INFO("=== All system sanity checks passed ===");
    } else {
        LOG_ERROR(1799, "System sanity checks FAILED");
    }
    return critical_pass;
}
