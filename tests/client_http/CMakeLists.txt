# The client's Streamable HTTP transport (neomcp::client_http): the reader of event streams, the transfer thread, the
# transport against a scripted peer that records every request's bytes, its event streams (sessions, the stream of the
# connection, resumption), the probe of which kind of server answers, the guard of URLs a server supplies, and round
# trips with the library's own HTTP server on every revision that defines the transport, and the OAuth authorizer
# against a scripted authorization server and resource. The tests reach the transport's internal headers. Nothing here
# is built without NEOMCP_HTTP_CLIENT.
if(NOT NEOMCP_HTTP_CLIENT)
    return()
endif()

add_executable(neomcp_client_http_test
    auth_client_test.cpp
    auth_fetch_test.cpp
    auth_seam_test.cpp
    bounds_test.cpp
    client_http_test.cpp
    header_fields_requests_test.cpp
    header_fields_test.cpp
    http_sse_test.cpp
    oauth_authorizer_host_test.cpp
    oauth_authorizer_prompt_test.cpp
    oauth_authorizer_test.cpp
    oauth_bounds_test.cpp
    oauth_end_to_end_test.cpp
    oauth_security_callback_test.cpp
    oauth_security_documents_test.cpp
    oauth_security_host_test.cpp
    oauth_security_origins_test.cpp
    sse_parser_test.cpp
    streams_test.cpp
    transfer_loop_test.cpp
    transport_test.cpp
    url_guard_auth_test.cpp
    url_guard_edges_test.cpp
    url_guard_test.cpp
    ../support/http_sse_server.cpp
    ../support/scripted_http_server.cpp)
target_link_libraries(neomcp_client_http_test PRIVATE neomcp::client_http neomcp::client neomcp::client_auth
    neomcp::http_base neomcp::http neomcp::server asio::asio GTest::gtest_main Threads::Threads)
target_include_directories(neomcp_client_http_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../support"
    "${CMAKE_CURRENT_SOURCE_DIR}/../client")
if(WIN32)
    target_link_libraries(neomcp_client_http_test PRIVATE ws2_32)
endif()
neomcp_configure_target(neomcp_client_http_test)
gtest_discover_tests(neomcp_client_http_test DISCOVERY_TIMEOUT 120)

# The OAuth client against the library's own protected server (the ServerEndToEnd tests), over loopback sockets in one
# process: the first contact, scopes, step-up, a changed authorization server, a token for another resource, a stream
# that ends with its token, and the introspection and JWT validators against a scripted authorization server. They live
# here, with the rig of the authorizer's tests, because they need the client's transport, the server and the
# validators, which the tests of the flow without I/O (tests/client_auth) do not link. They are programs of their own so
# that the object files of the other tests are not made larger. The two validators are in two programs: both define
# neomcp::http::detail::resolver_problem(bool), so a program cannot link both of them.
add_executable(neomcp_server_end_to_end_test
    end_to_end_server_flow_test.cpp
    end_to_end_server_introspection_test.cpp
    end_to_end_server_stream_test.cpp
    ../support/scripted_http_server.cpp)
target_link_libraries(neomcp_server_end_to_end_test PRIVATE neomcp::client_http neomcp::client neomcp::client_auth
    neomcp::http_base neomcp::http neomcp::server neomcp::server_auth_introspection asio::asio GTest::gtest_main
    Threads::Threads)
target_include_directories(neomcp_server_end_to_end_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../support"
    "${CMAKE_CURRENT_SOURCE_DIR}/../client")
if(WIN32)
    target_link_libraries(neomcp_server_end_to_end_test PRIVATE ws2_32)
endif()
neomcp_configure_target(neomcp_server_end_to_end_test)
gtest_discover_tests(neomcp_server_end_to_end_test DISCOVERY_TIMEOUT 120)

# The JWT part needs the keys the validator's tests make with OpenSSL, and is built where the JWT validator is.
if(TARGET neomcp::server_auth_jwt)
    add_executable(neomcp_server_end_to_end_jwt_test
        end_to_end_server_jwt_test.cpp
        ../server_auth_jwt/support.cpp
        ../support/scripted_http_server.cpp)
    target_link_libraries(neomcp_server_end_to_end_jwt_test PRIVATE neomcp::client_http neomcp::client
        neomcp::client_auth neomcp::http_base neomcp::http neomcp::server neomcp::server_auth_jwt neomcp::core
        OpenSSL::Crypto asio::asio GTest::gtest_main Threads::Threads)
    target_include_directories(neomcp_server_end_to_end_jwt_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
        "${CMAKE_CURRENT_SOURCE_DIR}/../support"
        "${CMAKE_CURRENT_SOURCE_DIR}/../client")
    if(WIN32)
        target_link_libraries(neomcp_server_end_to_end_jwt_test PRIVATE ws2_32)
    endif()
    neomcp_configure_target(neomcp_server_end_to_end_jwt_test)
    gtest_discover_tests(neomcp_server_end_to_end_jwt_test DISCOVERY_TIMEOUT 120)

    # A program that links both validators, built with the tests: two targets defining one name with external
    # linkage would fail the build here.
    add_executable(neomcp_server_end_to_end_both_validators end_to_end_server_both_validators.cpp)
    target_link_libraries(neomcp_server_end_to_end_both_validators PRIVATE neomcp::server_auth_introspection
        neomcp::server_auth_jwt Threads::Threads)
    neomcp_configure_target(neomcp_server_end_to_end_both_validators)
    add_test(NAME ServerEndToEndBothValidatorsLink COMMAND neomcp_server_end_to_end_both_validators)
endif()

# The client the official conformance runner tests (the CI job `conformance-client`). It is neither installed nor an
# example; the runner starts it with the URL of each scenario's server as its last argument. It requests the
# authorization URL of a sign-in with libcurl itself, standing in for the person's browser.
add_executable(neomcp_conformance_client ../conformance/client_fixture.cpp)
target_link_libraries(neomcp_conformance_client PRIVATE neomcp::client_http neomcp::client CURL::libcurl
    Threads::Threads)
neomcp_configure_target(neomcp_conformance_client)
