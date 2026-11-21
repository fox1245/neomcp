# Errors of the application's own (neomcp::ApplicationError): the answer at the host interface, and the Client reading
# them back from handlers of every kind over the in-memory transport, a child process and Streamable HTTP.

add_executable(neomcp_application_errors_host_test host_test.cpp)
target_link_libraries(neomcp_application_errors_host_test PRIVATE neomcp::core GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_application_errors_host_test)
gtest_discover_tests(neomcp_application_errors_host_test DISCOVERY_TIMEOUT 120)

# The server of the client tests as a program over stdio (the Client starts it as a child process).
add_executable(neomcp_application_errors_server fixture.cpp)
target_link_libraries(neomcp_application_errors_server PRIVATE neomcp::stdio Threads::Threads)
neomcp_configure_target(neomcp_application_errors_server)

# What the Client reads back, from handlers of every kind.
add_library(neomcp_application_errors_checks STATIC checks.cpp)
target_link_libraries(neomcp_application_errors_checks PUBLIC neomcp::client neomcp::server GTest::gtest
    Threads::Threads)
neomcp_configure_target(neomcp_application_errors_checks)

add_executable(neomcp_application_errors_client_test client_test.cpp)
target_link_libraries(neomcp_application_errors_client_test PRIVATE neomcp_application_errors_checks
    neomcp::client_in_process GTest::gtest_main Threads::Threads)
if(TARGET neomcp::client_stdio)
    target_link_libraries(neomcp_application_errors_client_test PRIVATE neomcp::client_stdio)
    target_compile_definitions(neomcp_application_errors_client_test PRIVATE
        "NEOMCP_APPLICATION_ERRORS_SERVER=\"$<TARGET_FILE:neomcp_application_errors_server>\"")
    add_dependencies(neomcp_application_errors_client_test neomcp_application_errors_server)
endif()
if(NEOMCP_HTTP_CLIENT)
    target_link_libraries(neomcp_application_errors_client_test PRIVATE neomcp::client_http neomcp::http)
    # The server of the older transport of events over HTTP is a test-only one.
    target_sources(neomcp_application_errors_client_test PRIVATE ../support/http_sse_server.cpp)
    target_include_directories(neomcp_application_errors_client_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../support")
    target_compile_definitions(neomcp_application_errors_client_test PRIVATE NEOMCP_APPLICATION_ERRORS_HTTP)
    if(WIN32)
        target_link_libraries(neomcp_application_errors_client_test PRIVATE ws2_32)
    endif()
endif()
neomcp_configure_target(neomcp_application_errors_client_test)
gtest_discover_tests(neomcp_application_errors_client_test DISCOVERY_TIMEOUT 120)

# A call that runs as a task, under both revisions that have tasks.
add_executable(neomcp_application_errors_tasks_test tasks_test.cpp)
target_link_libraries(neomcp_application_errors_tasks_test PRIVATE neomcp::server GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_application_errors_tasks_test)
gtest_discover_tests(neomcp_application_errors_tasks_test DISCOVERY_TIMEOUT 120)
