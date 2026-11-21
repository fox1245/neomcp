# The validator of JWT access tokens (neomcp::server_auth_jwt): the reading of a token, of the metadata and of a key set
# from tables, the validator against a scripted key set endpoint on a loopback port, and the validator behind the
# endpoint's gate. The test keys are made by OpenSSL when the tests run. Nothing here is built without NEOMCP_JWT.
if(NOT TARGET neomcp::server_auth_jwt)
    return()
endif()

add_executable(neomcp_server_auth_jwt_test
    gate_test.cpp
    support.cpp
    token_test.cpp
    validator_test.cpp
    ../support/scripted_http_server.cpp)
target_link_libraries(neomcp_server_auth_jwt_test PRIVATE neomcp::server_auth_jwt neomcp::http neomcp::http_base
    neomcp::core neomcp_httplib asio::asio OpenSSL::Crypto GTest::gtest_main Threads::Threads)
target_include_directories(neomcp_server_auth_jwt_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../support")
if(WIN32)
    target_link_libraries(neomcp_server_auth_jwt_test PRIVATE ws2_32)
endif()
neomcp_configure_target(neomcp_server_auth_jwt_test)
gtest_discover_tests(neomcp_server_auth_jwt_test DISCOVERY_TIMEOUT 120)
