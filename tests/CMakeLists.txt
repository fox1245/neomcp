if(NOT TARGET GTest::gtest_main)
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG b514bdc898e2951020cbdca1304b75f5950d1f59)  # v1.15.2
    set(INSTALL_GTEST OFF)
    # MSVC: build googletest with the same runtime library as the tests (the default /MD).
    set(gtest_force_shared_crt ON)
    FetchContent_MakeAvailable(googletest)
endif()

add_executable(neomcp_profile_test profile_test.cpp)
find_package(Threads REQUIRED)
target_link_libraries(neomcp_profile_test PRIVATE neomcp::profile neojson::neojson GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_profile_test)

include(GoogleTest)
gtest_discover_tests(neomcp_profile_test DISCOVERY_TIMEOUT 120)

add_executable(neomcp_describe_test describe_test.cpp)
target_link_libraries(neomcp_describe_test PRIVATE neomcp::describe neojson::neojson GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_describe_test)
gtest_discover_tests(neomcp_describe_test DISCOVERY_TIMEOUT 120)

# ---- JSON Schema validator -------------------------------------------------------------------------------------------

# The official JSON Schema test suite, used as data only: the tests parse its JSON files and nothing of it is built.
include(FetchContent)
FetchContent_Declare(json_schema_test_suite
    GIT_REPOSITORY https://github.com/json-schema-org/JSON-Schema-Test-Suite.git
    GIT_TAG e2a68a15cd490750fd6b95e604e4054d7d978d1f
    SOURCE_SUBDIR no-cmake-project-here)
FetchContent_MakeAvailable(json_schema_test_suite)

add_executable(neomcp_json_schema_test json_schema_test.cpp)
target_link_libraries(neomcp_json_schema_test PRIVATE neomcp::schema neojson::neojson GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_json_schema_test)
gtest_discover_tests(neomcp_json_schema_test DISCOVERY_TIMEOUT 120)

add_executable(neomcp_json_schema_suite_test json_schema_suite_test.cpp)
target_link_libraries(neomcp_json_schema_suite_test PRIVATE neomcp::schema neojson::neojson GTest::gtest_main Threads::Threads)
target_compile_definitions(neomcp_json_schema_suite_test PRIVATE
    "NEOMCP_JSON_SCHEMA_SUITE_DIR=\"${json_schema_test_suite_SOURCE_DIR}/validation/tests\""
    "NEOMCP_JSON_SCHEMA_SKIPS_FILE=\"${CMAKE_CURRENT_SOURCE_DIR}/data/json_schema_suite_skips.json\"")
neomcp_configure_target(neomcp_json_schema_suite_test)
gtest_discover_tests(neomcp_json_schema_suite_test DISCOVERY_TIMEOUT 120)

# Large values must compare in linear time: a quadratic comparison does not finish within the time limit. The limit
# is set only with NEOMCP_TEST_TIME_BOUNDS: an AddressSanitizer build on 64-bit Arm runs the same linear code several
# hundred times slower than an uninstrumented one, so there the tests check the results only.
add_executable(neomcp_json_schema_scale_test json_schema_scale_test.cpp)
target_link_libraries(neomcp_json_schema_scale_test PRIVATE neomcp::schema neojson::neojson GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_json_schema_scale_test)
if(NEOMCP_TEST_TIME_BOUNDS)
    gtest_discover_tests(neomcp_json_schema_scale_test DISCOVERY_TIMEOUT 120 PROPERTIES TIMEOUT 2)
else()
    gtest_discover_tests(neomcp_json_schema_scale_test DISCOVERY_TIMEOUT 120)
endif()
