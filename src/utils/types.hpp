#pragma once
#include <string>

// Generic operation result returned by async and sync operations.
// Each function documents its return codes in comments above its declaration.
// Code 0 always means success. Non-zero codes are operation-specific.
// Codes can be conglomerated into ENUMs per operation as the codebase matures.
struct OperationResult {
    unsigned char code;  // 0 = success, non-zero = error (operation-specific)
    std::string message;
};
