# The client's OAuth flow without I/O (neomcp::client_auth), driven from tables against scripted peers that answer
# each request it describes: URLs and their identity, protected resource metadata, authorization server metadata,
# registration, PKCE and scopes, the callback, the token requests, discovery on the server's origin, and the flow as a
# whole. No socket is opened. The tests reach the flow's internal headers.
add_executable(neomcp_client_auth_test
    bounds_test.cpp
    callback_test.cpp
    flow_fallback_test.cpp
    flow_test.cpp
    pkce_scopes_test.cpp
    registration_test.cpp
    resource_metadata_test.cpp
    server_metadata_test.cpp
    random_input_test.cpp
    tokens_test.cpp
    url_test.cpp)
target_link_libraries(neomcp_client_auth_test PRIVATE neomcp::client_auth neomcp::client neomcp::http_base
    neomcp::profile neojson::neojson GTest::gtest_main Threads::Threads)
target_include_directories(neomcp_client_auth_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
neomcp_configure_target(neomcp_client_auth_test)
gtest_discover_tests(neomcp_client_auth_test DISCOVERY_TIMEOUT 120)
