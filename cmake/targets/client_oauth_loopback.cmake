# The optional receiver of the browser's return for the client's OAuth sign-in (neomcp/client/oauth_loopback.h): a
# prompt handler for neomcp::OAuthAuthorizer that listens once on 127.0.0.1, with the library's own HTTP listener, for
# the request the browser makes after the person signed in. An application that has a listener of its own, or hands the
# callback over some other way, leaves it out.
# Built only with NEOMCP_HTTP_CLIENT (a sign-in needs the HTTP client) and NEOMCP_CLIENT_OAUTH_LOOPBACK.
option(NEOMCP_CLIENT_OAUTH_LOOPBACK "Build the loopback receiver of the client's OAuth sign-in (neomcp::client_oauth_loopback)" ON)

if(NEOMCP_HTTP_CLIENT AND NEOMCP_CLIENT_OAUTH_LOOPBACK)
    add_library(neomcp_client_oauth_loopback
        src/client_oauth_loopback/loopback.cpp)
    add_library(neomcp::client_oauth_loopback ALIAS neomcp_client_oauth_loopback)
    target_include_directories(neomcp_client_oauth_loopback
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(neomcp_client_oauth_loopback PUBLIC neomcp::client neomcp::http_base
        PRIVATE neomcp::http asio::asio)
    target_compile_features(neomcp_client_oauth_loopback PUBLIC cxx_std_20)
    neomcp_configure_target(neomcp_client_oauth_loopback)
endif()
