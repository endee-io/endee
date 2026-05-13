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

# Vector storage tests: bring up the integration layer (vector + meta + filter
# stores) so we can verify upsert cleanup and deleteFilter meta sync end-to-end.
add_executable(ndd_vector_storage_test vector_storage_test.cpp
    ${LMDB_SOURCES}
    ${ROARING_SOURCE})

target_link_libraries(ndd_vector_storage_test GTest::gtest_main)

target_include_directories(ndd_vector_storage_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/filter
    ${CMAKE_SOURCE_DIR}/src/storage
    ${CMAKE_SOURCE_DIR}/src/quant
    ${CMAKE_SOURCE_DIR}/src/utils
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/json
    ${CMAKE_SOURCE_DIR}/third_party/msgpack/include
)

target_compile_definitions(ndd_vector_storage_test PRIVATE MDB_MAXKEYSIZE=512)
if(USE_AVX512)
    target_compile_definitions(ndd_vector_storage_test PRIVATE USE_AVX512)
    target_compile_options(ndd_vector_storage_test PRIVATE
        -mavx512f -mavx512bw -mavx512vnni -mavx512fp16 -mavx512vpopcntdq)
elseif(USE_AVX2)
    target_compile_definitions(ndd_vector_storage_test PRIVATE USE_AVX2)
    target_compile_options(ndd_vector_storage_test PRIVATE -mavx2 -mfma -mf16c)
elseif(USE_SVE2)
    target_compile_definitions(ndd_vector_storage_test PRIVATE USE_SVE2)
    target_compile_options(ndd_vector_storage_test PRIVATE -march=armv8.6-a+sve2+fp16+dotprod)
elseif(USE_NEON)
    target_compile_definitions(ndd_vector_storage_test PRIVATE USE_NEON)
endif()

add_executable(ndd_request_validation_test request_validation_test.cpp)

target_link_libraries(ndd_request_validation_test GTest::gtest_main)

target_include_directories(ndd_request_validation_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/server
    ${CMAKE_SOURCE_DIR}/src/utils
    ${CROW_INCLUDE_DIR}
)

# Numeric index stress test: random churn + drain phase, asserts the
# forward<->inverted invariant across the bitmap-only / split paths.
add_executable(ndd_numeric_index_stress_test numeric_index_stress_test.cpp
    ${LMDB_SOURCES}
    ${ROARING_SOURCE})

target_link_libraries(ndd_numeric_index_stress_test GTest::gtest_main)

target_include_directories(ndd_numeric_index_stress_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/filter
    ${CMAKE_SOURCE_DIR}/src/utils
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/json
)

target_compile_definitions(ndd_numeric_index_stress_test PRIVATE MDB_MAXKEYSIZE=512)

include(GoogleTest)
gtest_discover_tests(ndd_filter_test)
gtest_discover_tests(ndd_vector_storage_test)
gtest_discover_tests(ndd_request_validation_test)
gtest_discover_tests(ndd_numeric_index_stress_test)
