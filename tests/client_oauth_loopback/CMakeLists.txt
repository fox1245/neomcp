# The loopback receiver of the client's OAuth sign-in (neomcp::client_oauth_loopback) against browsers that are plain
# HTTP requests made with the HTTP library's client, and with the OAuthAuthorizer against a scripted authorization
# server and resource. Every port is one the system chooses on the loopback interface. Nothing here is built without the
# target.
if(NOT TARGET neomcp::client_oauth_loopback)
    return()
endif()

add_executable(neomcp_client_oauth_loopback_test
    loopback_test.cpp
    sign_in_test.cpp
    ../support/scripted_http_server.cpp)
target_link_libraries(neomcp_client_oauth_loopback_test PRIVATE neomcp::client_oauth_loopback neomcp::client_http
    neomcp::client neomcp::client_auth neomcp::http_base neomcp::http neomcp::server neomcp_httplib asio::asio
    GTest::gtest_main Threads::Threads)
target_include_directories(neomcp_client_oauth_loopback_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../support")
if(WIN32)
    target_link_libraries(neomcp_client_oauth_loopback_test PRIVATE ws2_32)
endif()
neomcp_configure_target(neomcp_client_oauth_loopback_test)
gtest_discover_tests(neomcp_client_oauth_loopback_test DISCOVERY_TIMEOUT 120)
