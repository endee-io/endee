#pragma once

bool is_disk_full();

bool check_cpu_compatibility();
bool check_data_dir_permissions();
bool check_disk_space();
bool check_available_memory();
bool check_open_files_limit();
bool check_total_physical_memory();
bool check_cpu_cores();

// Runs all startup sanity checks. Returns false if any critical check fails.
bool run_startup_sanity_checks();
