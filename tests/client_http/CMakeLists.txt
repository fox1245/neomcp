# The client's Streamable HTTP transport (neomcp::client_http): the reader of event streams, the transfer thread, the
# transport against a scripted peer that records every request's bytes, its event streams (sessions, the stream of the
# connection, resumption), the probe of which kind of server answers, the guard of URLs a server supplies, and round
# trips with the library's own HTTP server on every revision that defines the transport. The tests reach the
# transport's internal headers. Nothing here is built without NEOMCP_HTTP_CLIENT.
if(NOT NEOMCP_HTTP_CLIENT)
    return()
endif()

add_executable(neomcp_client_http_test
    client_http_test.cpp
    http_sse_test.cpp
    sse_parser_test.cpp
    streams_test.cpp
    transfer_loop_test.cpp
    transport_test.cpp
    url_guard_test.cpp
    ../support/http_sse_server.cpp
    ../support/scripted_http_server.cpp)
target_link_libraries(neomcp_client_http_test PRIVATE neomcp::client_http neomcp::client neomcp::http_base neomcp::http
    neomcp::server asio::asio GTest::gtest_main Threads::Threads)
target_include_directories(neomcp_client_http_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../support"
    "${CMAKE_CURRENT_SOURCE_DIR}/../client")
if(WIN32)
    target_link_libraries(neomcp_client_http_test PRIVATE ws2_32)
endif()
neomcp_configure_target(neomcp_client_http_test)
gtest_discover_tests(neomcp_client_http_test DISCOVERY_TIMEOUT 120)

# The client the official conformance runner tests (the CI job `conformance-client`). It is neither installed nor an
# example; the runner starts it with the URL of each scenario's server as its last argument.
add_executable(neomcp_conformance_client ../conformance/client_fixture.cpp)
target_link_libraries(neomcp_conformance_client PRIVATE neomcp::client_http neomcp::client Threads::Threads)
neomcp_configure_target(neomcp_conformance_client)
