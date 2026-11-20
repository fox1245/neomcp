# The child-process transport of the client. Where the target is not built (see cmake/targets/client_stdio.cmake),
# neither are its tests.
if(NOT TARGET neomcp::client_stdio)
    return()
endif()

# A server that does what a script says, so that a test can make it misbehave in each way it needs.
add_executable(neomcp_scripted_stdio_server ../fixtures/scripted_stdio_server.cpp)
target_link_libraries(neomcp_scripted_stdio_server PRIVATE neojson::neojson)
neomcp_configure_target(neomcp_scripted_stdio_server)

# The library's own stdio server, serving only the revisions named on its command line.
add_executable(neomcp_client_stdio_profile_server profile_server.cpp)
target_link_libraries(neomcp_client_stdio_profile_server PRIVATE neomcp::stdio Threads::Threads)
neomcp_configure_target(neomcp_client_stdio_profile_server)

# The tests reach the transport's internal pieces (the comparison of variable names, the Windows command line).
add_executable(neomcp_client_stdio_test
    client_test.cpp
    transport_test.cpp)
target_link_libraries(neomcp_client_stdio_test PRIVATE neomcp::client_stdio neomcp::client asio::asio
    GTest::gtest_main Threads::Threads)
target_include_directories(neomcp_client_stdio_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
target_compile_definitions(neomcp_client_stdio_test PRIVATE
    "NEOMCP_SCRIPTED_SERVER=\"$<TARGET_FILE:neomcp_scripted_stdio_server>\""
    "NEOMCP_STDIO_FIXTURE=\"$<TARGET_FILE:neomcp_stdio_fixture>\""
    "NEOMCP_PROFILE_SERVER=\"$<TARGET_FILE:neomcp_client_stdio_profile_server>\"")
add_dependencies(neomcp_client_stdio_test
    neomcp_scripted_stdio_server neomcp_stdio_fixture neomcp_client_stdio_profile_server)
neomcp_configure_target(neomcp_client_stdio_test)
gtest_discover_tests(neomcp_client_stdio_test DISCOVERY_TIMEOUT 120)
