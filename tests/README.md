# Tests

Unit tests for Endee. Currently two test suites: filter and rebuild.

## Build & Run All Tests

    cmake -S . -B build -DENABLE_TESTING=ON -DUSE_NEON=ON   # Apple Silicon
    cmake -S . -B build -DENABLE_TESTING=ON -DUSE_AVX2=ON   # Intel/AMD
    cmake --build build
    ctest --test-dir build --output-on-failure

## ndd_filter_test

Tests for the filter subsystem (categorical, numeric, boolean filtering).

Build and run individually:

    cmake --build build --target ndd_filter_test
    ./build/tests/ndd_filter_test

Test cases:
- BucketTest: bucket serialization and deserialization
- FilterTest/CategoryFilterBasics: string category filter add and query
- FilterTest/BooleanFilterBasics: boolean filter via JSON input
- FilterTest/NumericFilterBasics: integer range queries
- FilterTest/FloatNumericFilter: float range queries
- FilterTest/MixedAndLogic: AND logic across multiple fields
- FilterTest/InOperator: $in operator with multiple values
- FilterTest/DeleteFilter: removal of categorical filters
- FilterTest/NumericDelete: removal of numeric filters

## ndd_rebuild_test

Unit and integration tests for the rebuild subsystem.

Build and run individually:

    cmake --build build --target ndd_rebuild_test
    ./build/tests/ndd_rebuild_test

Test cases:

State management (Rebuild class in isolation):
- RebuildStateTest/NoRebuild_HasActiveIsFalse
- RebuildStateTest/NoRebuild_GetProgressIsIdle
- RebuildStateTest/SetActive_HasActiveIsTrue
- RebuildStateTest/SetActive_GetProgressShowsInProgress
- RebuildStateTest/UpdateProgress_ReflectedInGetProgress
- RebuildStateTest/PercentComplete_CalculatedCorrectly
- RebuildStateTest/PercentComplete_ZeroTotal_IsZero
- RebuildStateTest/Complete_StatusIsCompleted
- RebuildStateTest/Complete_HasActiveIsFalse
- RebuildStateTest/Complete_CompletedAtPresent
- RebuildStateTest/Fail_StatusIsFailed
- RebuildStateTest/Fail_HasActiveIsFalse
- RebuildStateTest/Fail_ErrorMessagePresent
- RebuildStateTest/Fail_CompletedAtPresent
- RebuildStateTest/TwoUsers_IndependentState
- RebuildStateTest/GetProgress_WrongIndex_ReturnsIdle
- RebuildStateTest/SetActive_OverwritesPreviousCompleted

Temp file cleanup and path helpers:
- RebuildCleanupTest/CleanupTempFiles_NonExistentDir_NoOp
- RebuildCleanupTest/CleanupTempFiles_RemovesTempFile
- RebuildCleanupTest/CleanupTempFiles_RemovesTimestampedFile
- RebuildCleanupTest/CleanupTempFiles_LeavesCanonicalIndex
- RebuildCleanupTest/CleanupTempFiles_EmptyDir_NoOp
- RebuildPathTest/GetTempPath_Format
- RebuildPathTest/GetTimestampedPath_HasTimestamp

End-to-end rebuild via IndexManager:
- RebuildIntegrationTest/RebuildAsync_ReturnSuccessCode
- RebuildIntegrationTest/RebuildCompletes_ConfigUpdated
- RebuildIntegrationTest/RebuildCompletes_VectorCountPreserved
- RebuildIntegrationTest/RebuildWhileInProgress_Returns409Code
- RebuildIntegrationTest/RebuildNonExistentIndex_Returns404Code
- RebuildIntegrationTest/RebuildNoChange_Returns400Code

## Notes

- Tests use real file I/O and real MDBX databases — no mocking.
- Each test creates its own temp directory and removes it on teardown.
- The `tests/build/` directory is ignored by git.
