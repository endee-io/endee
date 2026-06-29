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
