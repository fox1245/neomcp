# The Client and its driver: against the library's own Server in the same process (the in-memory transport and the
# in-process server), and against scripted peers where a test needs a server that loses answers or asks in rounds.
add_executable(neomcp_client_test
    bounds_test.cpp
    client_test.cpp
    driver_test.cpp
    feed_test.cpp
    schema_bounds_test.cpp
    tasks_server_test.cpp
    tasks_test.cpp
    typed_test.cpp)
target_link_libraries(neomcp_client_test PRIVATE neomcp::client neomcp::client_in_process neomcp::server
    GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_client_test)
gtest_discover_tests(neomcp_client_test DISCOVERY_TIMEOUT 120)

# A program that opens a client only over the in-memory transport links with the client's target alone: the
# child-process and HTTP forms of opening are defined by their own targets and are never named by it.
add_executable(neomcp_client_link_check link_check.cpp)
target_link_libraries(neomcp_client_link_check PRIVATE neomcp::client)
neomcp_configure_target(neomcp_client_link_check)
add_test(NAME Client.LinksWithTheClientTargetAlone COMMAND neomcp_client_link_check)
