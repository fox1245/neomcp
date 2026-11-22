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

add_executable(neomcp_profile_test profile_test.cpp profile_authorization_test.cpp)
find_package(Threads REQUIRED)
target_link_libraries(neomcp_profile_test PRIVATE neomcp::profile neojson::neojson GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_profile_test)

include(GoogleTest)
gtest_discover_tests(neomcp_profile_test DISCOVERY_TIMEOUT 120)

add_executable(neomcp_describe_test describe_test.cpp)
target_link_libraries(neomcp_describe_test PRIVATE neomcp::describe neomcp::schema neojson::neojson GTest::gtest_main Threads::Threads)
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

# A number that is not finite in the option of a member must not compile; the control case (0) must.
function(neomcp_bounds_case case expect_failure)
    set(target neomcp_compile_check_bounds${case})
    add_library(${target} OBJECT EXCLUDE_FROM_ALL compile_check/describe_bounds.cpp)
    target_compile_definitions(${target} PRIVATE NEOMCP_CASE=${case})
    target_link_libraries(${target} PRIVATE neomcp::describe neojson::neojson)
    neomcp_configure_target(${target})
    add_test(NAME CompileCheck.DescribeBounds${case}
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG> --target ${target})
    if(expect_failure)
        set_tests_properties(CompileCheck.DescribeBounds${case} PROPERTIES WILL_FAIL TRUE)
    endif()
endfunction()
neomcp_bounds_case(0 FALSE)
neomcp_bounds_case(1 TRUE)
neomcp_bounds_case(2 TRUE)
neomcp_bounds_case(3 TRUE)
neomcp_bounds_case(4 TRUE)

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

# The differential test of the pattern translator against a JavaScript engine: the checker reads a file of cases that
# tests/pattern_oracle/generate.js wrote (the CI job `pattern-oracle` generates and checks them). The hand-written
# cases of tests/data/pattern_cases.json are checked on every test run; a file of generated cases is checked when
# NEOMCP_PATTERN_ORACLE_CASES names it, so that the ordinary test run does not need Node.
set(NEOMCP_PATTERN_ORACLE_CASES "" CACHE FILEPATH "A file of cases written by tests/pattern_oracle/generate.js; when set, ctest checks it")
add_executable(neomcp_pattern_oracle pattern_oracle/check.cpp)
target_link_libraries(neomcp_pattern_oracle PRIVATE neomcp::schema neojson::neojson)
neomcp_configure_target(neomcp_pattern_oracle)
add_test(NAME PatternOracle.HandWrittenCases COMMAND neomcp_pattern_oracle "${CMAKE_CURRENT_SOURCE_DIR}/data/pattern_cases.json")
if(NEOMCP_PATTERN_ORACLE_CASES)
    add_test(NAME PatternOracle.GeneratedCases COMMAND neomcp_pattern_oracle "${NEOMCP_PATTERN_ORACLE_CASES}")
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

# Authorization in the core: scope-tokens, declared implications, the principal and its digest in request state, and a
# session that checks the scopes a host declares; the test reaches the sealing of request state.
add_executable(neomcp_authorization_test authorization_test.cpp)
target_link_libraries(neomcp_authorization_test PRIVATE neomcp::core GTest::gtest_main Threads::Threads)
target_include_directories(neomcp_authorization_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
neomcp_configure_target(neomcp_authorization_test)
gtest_discover_tests(neomcp_authorization_test DISCOVERY_TIMEOUT 120)

add_executable(neomcp_wire_annotations_test wire_annotations_test.cpp)
target_link_libraries(neomcp_wire_annotations_test PRIVATE neomcp::core GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_wire_annotations_test)
gtest_discover_tests(neomcp_wire_annotations_test DISCOVERY_TIMEOUT 120)

# Input requests in the session engine, under both mechanisms; the test reaches the sealing of request state.
add_executable(neomcp_input_test input_test.cpp)
target_link_libraries(neomcp_input_test PRIVATE neomcp::core GTest::gtest_main Threads::Threads)
target_include_directories(neomcp_input_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
neomcp_configure_target(neomcp_input_test)
gtest_discover_tests(neomcp_input_test DISCOVERY_TIMEOUT 120)

# The change feed in the session engine, under both mechanisms, and a burst of changes behind a held writer on every
# built-in revision.
add_executable(neomcp_feed_test feed_test.cpp feed_burst_test.cpp)
target_link_libraries(neomcp_feed_test PRIVATE neomcp::core GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_feed_test)
gtest_discover_tests(neomcp_feed_test DISCOVERY_TIMEOUT 120)

# The resources a connection watches, ended by a later unit whose authorization cannot read them.
add_executable(neomcp_session_subscription_test session_subscription_test.cpp)
target_link_libraries(neomcp_session_subscription_test PRIVATE neomcp::core GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_session_subscription_test)
gtest_discover_tests(neomcp_session_subscription_test DISCOVERY_TIMEOUT 120)

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
target_include_directories(neomcp_server_feed_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
neomcp_configure_target(neomcp_server_feed_test)
gtest_discover_tests(neomcp_server_feed_test DISCOVERY_TIMEOUT 120)

add_executable(neomcp_server_input_test server_input_test.cpp)
target_link_libraries(neomcp_server_input_test PRIVATE neomcp::server GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_server_input_test)
gtest_discover_tests(neomcp_server_input_test DISCOVERY_TIMEOUT 120)

# Declared inputs: the form built from a described struct, the rounds under both mechanisms, the answers refused.
add_executable(neomcp_server_declared_test server_declared_test.cpp)
target_link_libraries(neomcp_server_declared_test PRIVATE neomcp::server GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_server_declared_test)
gtest_discover_tests(neomcp_server_declared_test DISCOVERY_TIMEOUT 120)

# Sampling with tools on the wire: the two rounds of a tool under both ways a server asks, and the revisions and
# clients that are not asked.
add_executable(neomcp_server_sampling_tools_test server_sampling_tools_test.cpp)
target_link_libraries(neomcp_server_sampling_tools_test PRIVATE neomcp::server GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_server_sampling_tools_test)
gtest_discover_tests(neomcp_server_sampling_tools_test DISCOVERY_TIMEOUT 120)

# The task store: records of long-running requests, their bounds, and the races a store settles.
add_executable(neomcp_task_store_test task_store_test.cpp)
target_link_libraries(neomcp_task_store_test PRIVATE neomcp::server GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_task_store_test)
gtest_discover_tests(neomcp_task_store_test DISCOVERY_TIMEOUT 120)

# Tasks run by the server: their own stop token, their place in the bounds, cancellation, failures, recovery.
add_executable(neomcp_server_tasks_test server_tasks_test.cpp)
target_link_libraries(neomcp_server_tasks_test PRIVATE neomcp::server GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_server_tasks_test)
gtest_discover_tests(neomcp_server_tasks_test DISCOVERY_TIMEOUT 120)

# Tasks on the wire of 2026-07-28 (the extension io.modelcontextprotocol/tasks), through a session.
add_executable(neomcp_server_tasks_wire_test server_tasks_wire_test.cpp)
target_link_libraries(neomcp_server_tasks_wire_test PRIVATE neomcp::server GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_server_tasks_wire_test)
gtest_discover_tests(neomcp_server_tasks_wire_test DISCOVERY_TIMEOUT 120)

# Tasks on the wire of 2025-11-25 (tasks of the core protocol), through a session.
add_executable(neomcp_server_tasks_2025_test server_tasks_2025_test.cpp)
target_link_libraries(neomcp_server_tasks_2025_test PRIVATE neomcp::server GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_server_tasks_2025_test)
gtest_discover_tests(neomcp_server_tasks_2025_test DISCOVERY_TIMEOUT 120)

add_executable(neomcp_server_tasks_2025_flow_test server_tasks_2025_flow_test.cpp)
target_link_libraries(neomcp_server_tasks_2025_flow_test PRIVATE neomcp::server GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_server_tasks_2025_flow_test)
gtest_discover_tests(neomcp_server_tasks_2025_flow_test DISCOVERY_TIMEOUT 120)

# The bounds of ServerOptions that mirror those of SessionOptions, each at its limit and one past it.
add_executable(neomcp_server_bounds_test server_bounds_test.cpp)
target_link_libraries(neomcp_server_bounds_test PRIVATE neomcp::server GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_server_bounds_test)
gtest_discover_tests(neomcp_server_bounds_test DISCOVERY_TIMEOUT 120)

# The form of an elicitation as the revision's profile defines it: at its building, its registration and its asking.
add_executable(neomcp_server_form_test server_form_test.cpp)
target_link_libraries(neomcp_server_form_test PRIVATE neomcp::server GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_server_form_test)
gtest_discover_tests(neomcp_server_form_test DISCOVERY_TIMEOUT 120)

# The scopes a server's items declare, checked against the authorization of each input unit.
add_executable(neomcp_server_authorization_test server_authorization_test.cpp)
target_link_libraries(neomcp_server_authorization_test PRIVATE neomcp::server GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_server_authorization_test)
gtest_discover_tests(neomcp_server_authorization_test DISCOVERY_TIMEOUT 120)

# ---- The stdio transport ---------------------------------------------------------------------------------------------

add_executable(neomcp_stdio_test stdio_test.cpp)
target_link_libraries(neomcp_stdio_test PRIVATE neomcp::stdio neomcp::io GTest::gtest_main Threads::Threads)
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

# A request body sent slowly, inside the read timeout, next to another client's request, on a listener with one worker.
add_executable(neomcp_http_slow_body_test http_slow_body_test.cpp)
target_link_libraries(neomcp_http_slow_body_test PRIVATE neomcp::http neomcp_httplib GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_http_slow_body_test)
gtest_discover_tests(neomcp_http_slow_body_test DISCOVERY_TIMEOUT 120)

# The body deadline at its edges, with peers that write raw bytes on a plain socket.
add_executable(neomcp_http_body_deadline_test http_body_deadline_test.cpp)
target_link_libraries(neomcp_http_body_deadline_test PRIVATE neomcp::http neomcp_httplib GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_http_body_deadline_test)
gtest_discover_tests(neomcp_http_body_deadline_test DISCOVERY_TIMEOUT 120)

# ---- The Streamable HTTP endpoint ------------------------------------------------------------------------------------

# Real sockets on the loopback interface, with the HTTP library's client as the peer. The test reaches the endpoint's
# internal timer.
add_executable(neomcp_http_endpoint_test http_endpoint_test.cpp)
target_link_libraries(neomcp_http_endpoint_test PRIVATE neomcp::http neomcp_httplib GTest::gtest_main Threads::Threads)
target_include_directories(neomcp_http_endpoint_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
neomcp_configure_target(neomcp_http_endpoint_test)
gtest_discover_tests(neomcp_http_endpoint_test DISCOVERY_TIMEOUT 120)

# Tasks of 2026-07-28 over the endpoint, on real sockets: a task outlives the request (and the session) that created it.
add_executable(neomcp_http_tasks_test http_tasks_test.cpp)
target_link_libraries(neomcp_http_tasks_test PRIVATE neomcp::http neomcp_httplib GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_http_tasks_test)
gtest_discover_tests(neomcp_http_tasks_test DISCOVERY_TIMEOUT 120)

add_executable(neomcp_http_tasks_2025_test http_tasks_2025_test.cpp)
target_link_libraries(neomcp_http_tasks_2025_test PRIVATE neomcp::http neomcp_httplib GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_http_tasks_2025_test)
gtest_discover_tests(neomcp_http_tasks_2025_test DISCOVERY_TIMEOUT 120)

# The endpoint as an OAuth protected resource: over sockets, and through its handler for the order of the requests that
# wait for one validation. Both reach the endpoint's internal view of what it feeds its sessions.
add_executable(neomcp_http_resource_server_test http_resource_server_test.cpp)
target_link_libraries(neomcp_http_resource_server_test PRIVATE neomcp::http neomcp_httplib GTest::gtest_main Threads::Threads)
target_include_directories(neomcp_http_resource_server_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
neomcp_configure_target(neomcp_http_resource_server_test)
gtest_discover_tests(neomcp_http_resource_server_test DISCOVERY_TIMEOUT 120)

add_executable(neomcp_http_resource_server_flight_test http_resource_server_flight_test.cpp)
target_link_libraries(neomcp_http_resource_server_flight_test PRIVATE neomcp::http GTest::gtest_main Threads::Threads)
target_include_directories(neomcp_http_resource_server_flight_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
neomcp_configure_target(neomcp_http_resource_server_flight_test)
gtest_discover_tests(neomcp_http_resource_server_flight_test DISCOVERY_TIMEOUT 120)

# The same feature, one scenario per test over loopback sockets (tests/support/resource_scenario.h holds what they
# share): the checks on every token, the scopes of each kind of item, and the metadata, the owner of a session and the
# end of a stream, the bounds of the remembered verdicts, the authorization servers and the metadata document, and the
# subscriptions of a session whose later requests bring a narrower token. Kept in files apart so that no compile takes
# long.
foreach(scenario_name IN ITEMS token scope session bounds subscription)
    add_executable(neomcp_http_resource_server_scenario_${scenario_name}_test
        http_resource_server_scenario_${scenario_name}_test.cpp)
    target_link_libraries(neomcp_http_resource_server_scenario_${scenario_name}_test
        PRIVATE neomcp::http neomcp_httplib GTest::gtest_main Threads::Threads)
    target_include_directories(neomcp_http_resource_server_scenario_${scenario_name}_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    neomcp_configure_target(neomcp_http_resource_server_scenario_${scenario_name}_test)
    gtest_discover_tests(neomcp_http_resource_server_scenario_${scenario_name}_test DISCOVERY_TIMEOUT 120)
endforeach()

# The endpoint protected by a fixed table of tokens: the table on its own (which reaches the internal scan of its
# digests), the scenarios over loopback sockets, the table replaced while requests run, the open streams of tokens
# when the table is replaced, the subscriptions of a session under narrower tokens and replacements, and the library's
# own client with a static token (with the HTTP client only). The scenarios share tests/support/fixed_tokens_scenario.h.
add_executable(neomcp_http_fixed_tokens_table_test http_fixed_tokens_table_test.cpp)
target_link_libraries(neomcp_http_fixed_tokens_table_test PRIVATE neomcp::http GTest::gtest_main)
target_include_directories(neomcp_http_fixed_tokens_table_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
neomcp_configure_target(neomcp_http_fixed_tokens_table_test)
gtest_discover_tests(neomcp_http_fixed_tokens_table_test DISCOVERY_TIMEOUT 120)

# The stream of a session that opens after a replacement's sweep, driven through the endpoint's handler and its
# internal probe.
add_executable(neomcp_http_fixed_tokens_probe_test http_fixed_tokens_probe_test.cpp)
target_link_libraries(neomcp_http_fixed_tokens_probe_test PRIVATE neomcp::http GTest::gtest_main Threads::Threads)
target_include_directories(neomcp_http_fixed_tokens_probe_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
neomcp_configure_target(neomcp_http_fixed_tokens_probe_test)
gtest_discover_tests(neomcp_http_fixed_tokens_probe_test DISCOVERY_TIMEOUT 120)

foreach(fixed_tokens_name IN ITEMS scenario replace stream subscription)
    add_executable(neomcp_http_fixed_tokens_${fixed_tokens_name}_test http_fixed_tokens_${fixed_tokens_name}_test.cpp)
    target_link_libraries(neomcp_http_fixed_tokens_${fixed_tokens_name}_test
        PRIVATE neomcp::http neomcp_httplib GTest::gtest_main Threads::Threads)
    target_include_directories(neomcp_http_fixed_tokens_${fixed_tokens_name}_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    neomcp_configure_target(neomcp_http_fixed_tokens_${fixed_tokens_name}_test)
    gtest_discover_tests(neomcp_http_fixed_tokens_${fixed_tokens_name}_test DISCOVERY_TIMEOUT 120)
endforeach()

if(NEOMCP_HTTP_CLIENT)
    add_executable(neomcp_http_fixed_tokens_client_test http_fixed_tokens_client_test.cpp)
    target_link_libraries(neomcp_http_fixed_tokens_client_test
        PRIVATE neomcp::client_http neomcp::client neomcp::http neomcp_httplib GTest::gtest_main Threads::Threads)
    target_include_directories(neomcp_http_fixed_tokens_client_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    if(WIN32)
        target_link_libraries(neomcp_http_fixed_tokens_client_test PRIVATE ws2_32)
    endif()
    neomcp_configure_target(neomcp_http_fixed_tokens_client_test)
    gtest_discover_tests(neomcp_http_fixed_tokens_client_test DISCOVERY_TIMEOUT 120)
endif()

# A tool call whose argument a schema `pattern` checks, next to another client's request, on a listener with one worker.
add_executable(neomcp_http_pattern_check_test http_pattern_check_test.cpp)
target_link_libraries(neomcp_http_pattern_check_test PRIVATE neomcp::http neomcp_httplib GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_http_pattern_check_test)
gtest_discover_tests(neomcp_http_pattern_check_test DISCOVERY_TIMEOUT 120)

# A table of legacy sessions filled by handshakes alone, and a new client.
add_executable(neomcp_http_session_table_test http_session_table_test.cpp)
target_link_libraries(neomcp_http_session_table_test PRIVATE neomcp::http neomcp_httplib GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_http_session_table_test)
gtest_discover_tests(neomcp_http_session_table_test DISCOVERY_TIMEOUT 120)

# Sessions nobody used at the edges of their bound and timeout, and handshakes racing.
add_executable(neomcp_http_unused_session_test http_unused_session_test.cpp)
target_link_libraries(neomcp_http_unused_session_test PRIVATE neomcp::http neomcp_httplib GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_http_unused_session_test)
gtest_discover_tests(neomcp_http_unused_session_test DISCOVERY_TIMEOUT 120)

# The session header on a request of the revision without sessions, and on the revisions that keep them.
add_executable(neomcp_http_session_header_era_test http_session_header_era_test.cpp)
target_link_libraries(neomcp_http_session_header_era_test
    PRIVATE neomcp::http neomcp_httplib GTest::gtest_main Threads::Threads)
target_include_directories(neomcp_http_session_header_era_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
neomcp_configure_target(neomcp_http_session_header_era_test)
gtest_discover_tests(neomcp_http_session_header_era_test DISCOVERY_TIMEOUT 120)

# A burst of change notifications held in the server's feed, then carried by the stream of a session and of a
# subscription, over loopback sockets.
add_executable(neomcp_http_change_burst_test http_change_burst_test.cpp)
target_link_libraries(neomcp_http_change_burst_test PRIVATE neomcp::http neomcp_httplib GTest::gtest_main Threads::Threads)
target_include_directories(neomcp_http_change_burst_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
neomcp_configure_target(neomcp_http_change_burst_test)
gtest_discover_tests(neomcp_http_change_burst_test DISCOVERY_TIMEOUT 120)

# The bytes a legacy session keeps for the resources its client watches.
add_executable(neomcp_http_watch_bytes_test http_watch_bytes_test.cpp)
target_link_libraries(neomcp_http_watch_bytes_test PRIVATE neomcp::http neomcp_httplib GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_http_watch_bytes_test)
gtest_discover_tests(neomcp_http_watch_bytes_test DISCOVERY_TIMEOUT 120)

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

# The flows of the runner's scenarios of the tasks extension, sent to the fixture's registrations with tasks on.
add_executable(neomcp_conformance_fixture_tasks_test conformance/fixture_tasks_test.cpp)
target_link_libraries(neomcp_conformance_fixture_tasks_test PRIVATE neomcp::server GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_conformance_fixture_tasks_test)
gtest_discover_tests(neomcp_conformance_fixture_tasks_test DISCOVERY_TIMEOUT 120)

# ---- Test suites that live in a folder of their own ------------------------------------------------------------------

# A folder tests/<name>/ with its own CMakeLists.txt is a suite. It is added when a file tests/suites/<name>.suite
# exists (its content is free: a line saying what the suite checks), so that a suite is one folder and one file, and
# tests/consumer, which is a project of its own, is never added by accident. The suites are added in the order of their
# names, after everything above, and may use the targets and the function neomcp_configure_target. A listed suite
# without the folder is an error.
file(GLOB NEOMCP_TEST_SUITE_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/suites/*.suite")
list(SORT NEOMCP_TEST_SUITE_FILES)
foreach(neomcp_suite_file IN LISTS NEOMCP_TEST_SUITE_FILES)
    get_filename_component(neomcp_suite "${neomcp_suite_file}" NAME_WE)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${neomcp_suite}/CMakeLists.txt")
        message(FATAL_ERROR "neomcp: tests/suites/${neomcp_suite}.suite lists a suite, but tests/${neomcp_suite}/CMakeLists.txt does not exist")
    endif()
    add_subdirectory("${neomcp_suite}")
endforeach()
unset(neomcp_suite_file)
unset(neomcp_suite)
