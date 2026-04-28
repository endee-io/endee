# Tests

Unit tests for Endee. Currently three test suites: filter, rebuild, and backup.

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

## ndd_backup_test

Unit and integration tests for the backup subsystem (`BackupStore` + `IndexManager` backup methods).

Build and run individually:

    cmake --build build --target ndd_backup_test
    ./build/tests/ndd_backup_test

Test cases:

BackupStore state management (no IndexManager):
- BackupStoreStateTest/ValidateName_AlphanumericUnderscore_Passes
- BackupStoreStateTest/ValidateName_WithHyphen_Passes
- BackupStoreStateTest/ValidateName_Empty_Fails
- BackupStoreStateTest/ValidateName_TooLong_Fails
- BackupStoreStateTest/ValidateName_Slash_Fails
- BackupStoreStateTest/ValidateName_Space_Fails
- BackupStoreStateTest/ValidateName_Dot_Fails
- BackupStoreStateTest/NoActive_HasActiveIsFalse
- BackupStoreStateTest/SetActive_HasActiveIsTrue
- BackupStoreStateTest/SetActive_GetActiveReturnsNameAndOperation
- BackupStoreStateTest/SetActive_Restoration_OperationString
- BackupStoreStateTest/ClearActive_HasActiveIsFalse
- BackupStoreStateTest/ClearActive_GetActiveReturnsNullopt
- BackupStoreStateTest/ClearNonExistent_NoOp
- BackupStoreStateTest/TwoUsers_IndependentState
- BackupStoreStateTest/ReadBackupJson_MissingFile_ReturnsEmptyObject
- BackupStoreStateTest/WriteAndReadBackupJson_RoundTrip
- BackupStoreStateTest/ListBackups_EmptyWhenNoneExist
- BackupStoreStateTest/ListBackups_ReturnsAllWrittenEntries
- BackupStoreStateTest/GetBackupInfo_ExistingEntry
- BackupStoreStateTest/GetBackupInfo_NonExistent_ReturnsNull
- BackupStoreStateTest/DeleteBackup_NonExistent_ReturnsFalse
- BackupStoreStateTest/DeleteBackup_InvalidName_ReturnsFalse
- BackupStoreStateTest/DeleteBackup_RemovesTarAndJsonEntry

Archive (tar) operations:
- BackupArchiveTest/CreateBackupTar_ProducesNonEmptyFile
- BackupArchiveTest/ExtractBackupTar_FilesRoundTrip
- BackupArchiveTest/ExtractBackupTar_ContentPreserved
- BackupArchiveTest/ExtractBackupTar_NonExistentArchive_Fails
- BackupArchiveTest/CreateBackupTar_PreCancelledStopToken_ReturnsFalse

End-to-end backup and restore via IndexManager:
- BackupIntegrationTest/CreateBackupAsync_ReturnsTrueAndBackupName
- BackupIntegrationTest/CreateBackup_SetsActiveBackupDuringRun
- BackupIntegrationTest/CreateBackup_ProducesTarFile
- BackupIntegrationTest/CreateBackup_AppearsInListBackups
- BackupIntegrationTest/CreateBackup_MetadataHasExpectedFields
- BackupIntegrationTest/CreateBackup_WhileInProgress_ReturnsFalse
- BackupIntegrationTest/CreateBackup_DuplicateName_ReturnsFalse
- BackupIntegrationTest/CreateBackup_InvalidName_ReturnsFalse
- BackupIntegrationTest/DeleteBackup_RemovesTarAndJsonEntry
- BackupIntegrationTest/DeleteBackup_NonExistent_ReturnsFalse
- BackupIntegrationTest/RestoreBackupAsync_ReturnsTrueAndTargetName
- BackupIntegrationTest/RestoreBackup_CreatesIndexWithCorrectMetadata
- BackupIntegrationTest/RestoreBackup_PreservesVectorCount
- BackupIntegrationTest/RestoreBackup_NonExistentBackup_ReturnsFalse
- BackupIntegrationTest/RestoreBackup_TargetIndexAlreadyExists_ReturnsFalse
- BackupIntegrationTest/RestoreBackup_WhileCreateInProgress_ReturnsFalse

## Notes

- Tests use real file I/O and real MDBX databases — no mocking.
- Each test creates its own temp directory and removes it on teardown.
- The `tests/build/` directory is ignored by git.
