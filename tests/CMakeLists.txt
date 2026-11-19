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

# Descriptions whose names are not unique must not compile when they are converted. Each case is an object library
# that is not part of the build; a test builds it. The control case must build, the others must not.
function(neomcp_compile_case case expect_failure)
    set(target neomcp_compile_check_${case})
    add_library(${target} OBJECT EXCLUDE_FROM_ALL compile_check/describe_names.cpp)
    target_compile_definitions(${target} PRIVATE NEOMCP_CASE=${case})
    target_link_libraries(${target} PRIVATE neomcp::describe neojson::neojson)
    neomcp_configure_target(${target})
    add_test(NAME CompileCheck.DescribeNames${case}
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG> --target ${target})
    if(expect_failure)
        set_tests_properties(CompileCheck.DescribeNames${case} PROPERTIES WILL_FAIL TRUE)
    endif()
endfunction()
neomcp_compile_case(0 FALSE)
neomcp_compile_case(1 TRUE)
neomcp_compile_case(2 TRUE)
neomcp_compile_case(3 TRUE)
neomcp_compile_case(4 TRUE)

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

# SHA-256, HMAC-SHA-256 and the random source; the test reaches the internal headers of the primitives.
# The time a `pattern` takes on short strings, for patterns whose repeated group can match one text in many ways.
add_executable(neomcp_json_schema_pattern_time_test json_schema_pattern_time_test.cpp)
target_link_libraries(neomcp_json_schema_pattern_time_test PRIVATE neomcp::schema neojson::neojson GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_json_schema_pattern_time_test)
gtest_discover_tests(neomcp_json_schema_pattern_time_test DISCOVERY_TIMEOUT 120)

# What a pattern means in the dialect of JSON Schema, what is refused, and the bound on the memory of a pattern.
add_executable(neomcp_json_schema_pattern_test json_schema_pattern_test.cpp)
target_link_libraries(neomcp_json_schema_pattern_test PRIVATE neomcp::schema neojson::neojson GTest::gtest_main Threads::Threads)
target_compile_definitions(neomcp_json_schema_pattern_test PRIVATE
    "NEOMCP_PATTERN_CASES_FILE=\"${CMAKE_CURRENT_SOURCE_DIR}/data/pattern_cases.json\"")
neomcp_configure_target(neomcp_json_schema_pattern_test)
gtest_discover_tests(neomcp_json_schema_pattern_test DISCOVERY_TIMEOUT 120)

# The translation of a pattern to the syntax of the regular expression engine; the test reaches the internal header.
add_executable(neomcp_json_schema_pattern_translate_test json_schema_pattern_translate_test.cpp)
target_link_libraries(neomcp_json_schema_pattern_translate_test PRIVATE neomcp::schema GTest::gtest_main Threads::Threads)
target_include_directories(neomcp_json_schema_pattern_translate_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
neomcp_configure_target(neomcp_json_schema_pattern_translate_test)
gtest_discover_tests(neomcp_json_schema_pattern_translate_test DISCOVERY_TIMEOUT 120)

# The time of a check grows in proportion to the string. Like the other tests of how cost grows with size, it is
# registered only with NEOMCP_TEST_TIME_BOUNDS.
if(NEOMCP_TEST_TIME_BOUNDS)
    add_executable(neomcp_json_schema_pattern_linear_test json_schema_pattern_linear_test.cpp)
    target_link_libraries(neomcp_json_schema_pattern_linear_test PRIVATE neomcp::schema neojson::neojson GTest::gtest_main Threads::Threads)
    neomcp_configure_target(neomcp_json_schema_pattern_linear_test)
    gtest_discover_tests(neomcp_json_schema_pattern_linear_test DISCOVERY_TIMEOUT 120 PROPERTIES TIMEOUT 300)
endif()

add_executable(neomcp_crypto_test crypto_test.cpp)
target_link_libraries(neomcp_crypto_test PRIVATE neomcp::crypto GTest::gtest_main Threads::Threads)
target_include_directories(neomcp_crypto_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
neomcp_configure_target(neomcp_crypto_test)
gtest_discover_tests(neomcp_crypto_test DISCOVERY_TIMEOUT 120)

add_executable(neomcp_core_test core_test.cpp)
target_link_libraries(neomcp_core_test PRIVATE neomcp::core GTest::gtest_main Threads::Threads)
# The test reaches internal pieces of the core (the Base64 decoder).
target_include_directories(neomcp_core_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
neomcp_configure_target(neomcp_core_test)
gtest_discover_tests(neomcp_core_test DISCOVERY_TIMEOUT 120)

# Input requests in the session engine, under both mechanisms; the test reaches the sealing of request state.
add_executable(neomcp_input_test input_test.cpp)
target_link_libraries(neomcp_input_test PRIVATE neomcp::core GTest::gtest_main Threads::Threads)
target_include_directories(neomcp_input_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
neomcp_configure_target(neomcp_input_test)
gtest_discover_tests(neomcp_input_test DISCOVERY_TIMEOUT 120)

# The change feed in the session engine, under both mechanisms.
add_executable(neomcp_feed_test feed_test.cpp)
target_link_libraries(neomcp_feed_test PRIVATE neomcp::core GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_feed_test)
gtest_discover_tests(neomcp_feed_test DISCOVERY_TIMEOUT 120)

add_executable(neomcp_uri_template_test uri_template_test.cpp)
target_link_libraries(neomcp_uri_template_test PRIVATE neomcp::core GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_uri_template_test)
gtest_discover_tests(neomcp_uri_template_test DISCOVERY_TIMEOUT 120)

add_executable(neomcp_server_test server_test.cpp)
target_link_libraries(neomcp_server_test PRIVATE neomcp::server GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_server_test)
gtest_discover_tests(neomcp_server_test DISCOVERY_TIMEOUT 120)

# Argument headers: the marks of a tool's input schema, their rules at registration, and the check of a call's headers.
add_executable(neomcp_argument_headers_test argument_headers_test.cpp)
target_link_libraries(neomcp_argument_headers_test PRIVATE neomcp::server GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_argument_headers_test)
gtest_discover_tests(neomcp_argument_headers_test DISCOVERY_TIMEOUT 120)

# The id of an error answer to a message whose id could not be read, in every revision.
add_executable(neomcp_error_id_test error_id_test.cpp)
target_link_libraries(neomcp_error_id_test PRIVATE neomcp::core GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_error_id_test)
gtest_discover_tests(neomcp_error_id_test DISCOVERY_TIMEOUT 120)

add_executable(neomcp_server_features_test server_features_test.cpp)
target_link_libraries(neomcp_server_features_test PRIVATE neomcp::server GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_server_features_test)
gtest_discover_tests(neomcp_server_features_test DISCOVERY_TIMEOUT 120)

# Items switched while the server serves, updated resources, and what the sessions of each revision tell.
add_executable(neomcp_server_feed_test server_feed_test.cpp)
target_link_libraries(neomcp_server_feed_test PRIVATE neomcp::server GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_server_feed_test)
gtest_discover_tests(neomcp_server_feed_test DISCOVERY_TIMEOUT 120)

add_executable(neomcp_server_input_test server_input_test.cpp)
target_link_libraries(neomcp_server_input_test PRIVATE neomcp::server GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_server_input_test)
gtest_discover_tests(neomcp_server_input_test DISCOVERY_TIMEOUT 120)

# ---- The stdio transport ---------------------------------------------------------------------------------------------

add_executable(neomcp_stdio_test stdio_test.cpp)
target_link_libraries(neomcp_stdio_test PRIVATE neomcp::stdio GTest::gtest_main Threads::Threads)
# The test reaches the transport's internal pieces (the line splitter and the descriptor streams).
target_include_directories(neomcp_stdio_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
neomcp_configure_target(neomcp_stdio_test)
gtest_discover_tests(neomcp_stdio_test DISCOVERY_TIMEOUT 120)

# A real program that serves a few tools over stdio. The interoperability test drives it; it is neither installed nor
# an example.
add_executable(neomcp_stdio_fixture fixtures/stdio_server.cpp)
target_link_libraries(neomcp_stdio_fixture PRIVATE neomcp::stdio Threads::Threads)
neomcp_configure_target(neomcp_stdio_fixture)

if(NEOMCP_INTEROP_TESTS)
    if(NEOMCP_INTEROP_PYTHON STREQUAL "")
        message(FATAL_ERROR "neomcp: NEOMCP_INTEROP_TESTS needs NEOMCP_INTEROP_PYTHON, the Python interpreter that has tests/interop/requirements.txt installed")
    endif()
    # Isolated mode (-I): the interpreter reads neither the environment nor the current directory; the packages come
    # from the interpreter's own site-packages.
    add_test(NAME interop_stdio_python_sdk
        COMMAND "${NEOMCP_INTEROP_PYTHON}" -I "${CMAKE_CURRENT_SOURCE_DIR}/interop/test_stdio_interop.py")
    set_tests_properties(interop_stdio_python_sdk PROPERTIES
        ENVIRONMENT "NEOMCP_STDIO_FIXTURE=$<TARGET_FILE:neomcp_stdio_fixture>"
        TIMEOUT 300)
    # The same client over Streamable HTTP, in both eras against one server process.
    add_test(NAME interop_http_python_sdk
        COMMAND "${NEOMCP_INTEROP_PYTHON}" -I "${CMAKE_CURRENT_SOURCE_DIR}/interop/test_http_interop.py")
    set_tests_properties(interop_http_python_sdk PROPERTIES
        ENVIRONMENT "NEOMCP_HTTP_FIXTURE=$<TARGET_FILE:neomcp_conformance_fixture>"
        TIMEOUT 300)
endif()

# ---- The HTTP building blocks ----------------------------------------------------------------------------------------

add_executable(neomcp_http_model_test http_model_test.cpp)
target_link_libraries(neomcp_http_model_test PRIVATE neomcp::http GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_http_model_test)
gtest_discover_tests(neomcp_http_model_test DISCOVERY_TIMEOUT 120)

add_executable(neomcp_http_policy_test http_policy_test.cpp)
target_link_libraries(neomcp_http_policy_test PRIVATE neomcp::http GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_http_policy_test)
gtest_discover_tests(neomcp_http_policy_test DISCOVERY_TIMEOUT 120)

add_executable(neomcp_http_random_test http_random_test.cpp)
target_link_libraries(neomcp_http_random_test PRIVATE neomcp::http GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_http_random_test)
gtest_discover_tests(neomcp_http_random_test DISCOVERY_TIMEOUT 120)

# The listener tests use the HTTP library's own client as the peer; the library is header-only.
add_executable(neomcp_http_listener_test http_listener_test.cpp)
target_link_libraries(neomcp_http_listener_test PRIVATE neomcp::http neomcp_httplib GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_http_listener_test)
gtest_discover_tests(neomcp_http_listener_test DISCOVERY_TIMEOUT 120)

# ---- The Streamable HTTP endpoint ------------------------------------------------------------------------------------

# Real sockets on the loopback interface, with the HTTP library's client as the peer. The test reaches the endpoint's
# internal timer.
add_executable(neomcp_http_endpoint_test http_endpoint_test.cpp)
target_link_libraries(neomcp_http_endpoint_test PRIVATE neomcp::http neomcp_httplib GTest::gtest_main Threads::Threads)
target_include_directories(neomcp_http_endpoint_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
neomcp_configure_target(neomcp_http_endpoint_test)
gtest_discover_tests(neomcp_http_endpoint_test DISCOVERY_TIMEOUT 120)

# A tool call whose argument a schema `pattern` checks, next to another client's request, on a listener with one worker.
add_executable(neomcp_http_pattern_check_test http_pattern_check_test.cpp)
target_link_libraries(neomcp_http_pattern_check_test PRIVATE neomcp::http neomcp_httplib GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_http_pattern_check_test)
gtest_discover_tests(neomcp_http_pattern_check_test DISCOVERY_TIMEOUT 120)

# The server the official conformance runner tests (the CI job `conformance`). It is neither installed nor an example.
add_executable(neomcp_conformance_fixture conformance/everything_server.cpp)
target_link_libraries(neomcp_conformance_fixture PRIVATE neomcp::http Threads::Threads)
neomcp_configure_target(neomcp_conformance_fixture)

# The requests of the conformance runner's resource, prompt, completion and caching scenarios, sent over real sockets
# to the fixture's registrations in both eras: the stand-in for the runner where it cannot run.
add_executable(neomcp_conformance_fixture_test conformance/fixture_test.cpp)
target_link_libraries(neomcp_conformance_fixture_test PRIVATE neomcp::http neomcp_httplib GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_conformance_fixture_test)
gtest_discover_tests(neomcp_conformance_fixture_test DISCOVERY_TIMEOUT 120)
