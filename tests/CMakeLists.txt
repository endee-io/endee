include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

# Common sources needed for tests
file(GLOB LMDB_SOURCES ${CMAKE_SOURCE_DIR}/third_party/mdbx/*.c)
set(ROARING_SOURCE ${CMAKE_SOURCE_DIR}/third_party/roaring_bitmap/roaring.c)

# Create the test executable
add_executable(ndd_filter_test filter_test.cpp ${LMDB_SOURCES} ${ROARING_SOURCE})

# Link against GTest
target_link_libraries(ndd_filter_test GTest::gtest_main)

# Set MDBX compile flags (same as main project)
set_source_files_properties(${LMDB_SOURCES} PROPERTIES
    COMPILE_FLAGS "-DMDBX_BUILD_SHARED_LIBRARY=0 -DMDBX_BUILD_FLAGS=\\\"NDD_EMBEDDED\\\""
)

# Allow tests to include private headers from src/
target_include_directories(ndd_filter_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/filter
    ${CMAKE_SOURCE_DIR}/src/utils
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/json
)
# Add other necessary definitions
target_compile_definitions(ndd_filter_test PRIVATE MDB_MAXKEYSIZE=512)

include(GoogleTest)
gtest_discover_tests(ndd_filter_test)

# --- ndd_rebuild_test ---
add_executable(ndd_rebuild_test rebuild_test.cpp ${LMDB_SOURCES} ${ROARING_SOURCE})

set_source_files_properties(${LMDB_SOURCES} PROPERTIES
    COMPILE_FLAGS "-DMDBX_BUILD_SHARED_LIBRARY=0 -DMDBX_BUILD_FLAGS=\\\"NDD_EMBEDDED\\\""
)

target_include_directories(ndd_rebuild_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/utils
    ${CMAKE_SOURCE_DIR}/src/server
    ${CMAKE_SOURCE_DIR}/src/storage
    ${CMAKE_SOURCE_DIR}/src/filter
    ${CMAKE_SOURCE_DIR}/src/sparse
    ${CMAKE_SOURCE_DIR}/src/hnsw
    ${CMAKE_SOURCE_DIR}/src/quant
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/json
    ${CMAKE_SOURCE_DIR}/third_party/msgpack/include
    ${LIBARCHIVE_INCLUDE_DIR}
)

target_link_libraries(ndd_rebuild_test
    PRIVATE
    ndd_core
    GTest::gtest_main
)

target_compile_definitions(ndd_rebuild_test PRIVATE MDB_MAXKEYSIZE=512)

gtest_discover_tests(ndd_rebuild_test)
