# The validator of access tokens that asks the authorization server (neomcp::server_auth_introspection): the reading of
# an answer from tables, the requests and the bounds against a scripted introspection endpoint on a loopback port, and
# the validator behind the endpoint's gate. Nothing here is built without NEOMCP_HTTP_CLIENT.
if(NOT NEOMCP_HTTP_CLIENT)
    return()
endif()

add_executable(neomcp_server_auth_introspection_test
    answer_test.cpp
    exchange_test.cpp
    gate_test.cpp
    validator_test.cpp
    ../support/scripted_http_server.cpp)
target_link_libraries(neomcp_server_auth_introspection_test PRIVATE neomcp::server_auth_introspection neomcp::http
    neomcp::http_base neomcp_httplib asio::asio GTest::gtest_main Threads::Threads)
target_include_directories(neomcp_server_auth_introspection_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../support")
if(WIN32)
    target_link_libraries(neomcp_server_auth_introspection_test PRIVATE ws2_32)
endif()
neomcp_configure_target(neomcp_server_auth_introspection_test)
gtest_discover_tests(neomcp_server_auth_introspection_test DISCOVERY_TIMEOUT 120)
