# The client half of the protocol core: the readers of the wire writers, the inverse input functions, the header value
# encoder, the client halves of the mechanisms with their registry, and the client engine, alone and paired in process
# with the server's session. The tests reach internal headers of the core (the checker of argument headers, the wire
# names).
add_executable(neomcp_client_core_test
    client_mechanism_test.cpp
    engine_pair_test.cpp
    engine_pending_test.cpp
    engine_test.cpp
    header_value_test.cpp
    support.cpp
    input_inverse_test.cpp
    wire_read_test.cpp)
target_link_libraries(neomcp_client_core_test PRIVATE neomcp::client neomcp::core neomcp::server GTest::gtest_main
    Threads::Threads)
target_include_directories(neomcp_client_core_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
neomcp_configure_target(neomcp_client_core_test)
gtest_discover_tests(neomcp_client_core_test DISCOVERY_TIMEOUT 120)
