include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

# Common sources needed for tests
file(GLOB LMDB_SOURCES ${CMAKE_SOURCE_DIR}/third_party/mdbx/*.c)
set(ROARING_SOURCE ${CMAKE_SOURCE_DIR}/third_party/roaring_bitmap/roaring.c)

# Create the test executable
add_executable(ndd_filter_test
    filter_test.cpp
    ${NDD_FILTER_SOURCES}
    ${LMDB_SOURCES}
    ${ROARING_SOURCE}
)

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

add_executable(ndd_request_validation_test request_validation_test.cpp)

target_link_libraries(ndd_request_validation_test GTest::gtest_main)

target_include_directories(ndd_request_validation_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/server
    ${CMAKE_SOURCE_DIR}/src/utils
    ${CROW_INCLUDE_DIR}
)

add_executable(ndd_layout_migrator_test
    layout_migrator_test.cpp
    ${LMDB_SOURCES}
)

target_link_libraries(ndd_layout_migrator_test GTest::gtest_main)

target_include_directories(ndd_layout_migrator_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/storage
    ${CMAKE_SOURCE_DIR}/src/tools
    ${CMAKE_SOURCE_DIR}/src/utils
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/json
)

target_compile_definitions(ndd_layout_migrator_test PRIVATE MDB_MAXKEYSIZE=512)

add_executable(ndd_acid_regression_test
    acid_regression_test.cpp
    ${CMAKE_SOURCE_DIR}/src/sparse/inverted_index.cpp
    ${LMDB_SOURCES}
    ${ROARING_SOURCE}
)

target_link_libraries(ndd_acid_regression_test GTest::gtest_main)

target_include_directories(ndd_acid_regression_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/filter
    ${CMAKE_SOURCE_DIR}/src/sparse
    ${CMAKE_SOURCE_DIR}/src/storage
    ${CMAKE_SOURCE_DIR}/src/utils
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/json
)

target_compile_definitions(ndd_acid_regression_test PRIVATE MDB_MAXKEYSIZE=512)

include(GoogleTest)
gtest_discover_tests(ndd_filter_test)
gtest_discover_tests(ndd_request_validation_test)
gtest_discover_tests(ndd_layout_migrator_test)
gtest_discover_tests(ndd_acid_regression_test)
