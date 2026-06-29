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

#include <optional>
#include <string>
#include <variant>

// Generic operation result returned by async and sync operations.
// Each function documents its return codes in comments above its declaration.
// Code 0 always means success. Non-zero codes are operation-specific.
// Codes can be conglomerated into ENUMs per operation as the codebase matures.
namespace ndd {

template <typename T = std::monostate>
struct OperationResult {
    unsigned int code = 0;
    std::string message;
    std::optional<T> value;

    bool ok() const { return code == 0; }
};

}  // namespace ndd
