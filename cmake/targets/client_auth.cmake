# The client's OAuth authorization flow without I/O: discovery of the protected resource metadata and of the
# authorization server, the identity checks of the resource and of the issuer, the choice of client registration, the
# scopes, PKCE and state, the authorization URL and the check of the callback, the token and refresh requests and
# their answers, and the bounds on repeated authorization. The flow describes the requests it needs and reads their
# answers; it opens no socket and needs no HTTP library, so it builds with or without NEOMCP_HTTP_CLIENT. Its headers
# are internal (src/client_auth/); the code that drives it sends the requests through a transport.
add_library(neomcp_client_auth STATIC
    src/client_auth/callback.cpp
    src/client_auth/discovery.cpp
    src/client_auth/document.cpp
    src/client_auth/flow.cpp
    src/client_auth/metadata.cpp
    src/client_auth/pkce.cpp
    src/client_auth/registration.cpp
    src/client_auth/scopes.cpp
    src/client_auth/tokens.cpp
    src/client_auth/types.cpp
    src/client_auth/url.cpp)
add_library(neomcp::client_auth ALIAS neomcp_client_auth)
target_include_directories(neomcp_client_auth
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(neomcp_client_auth PUBLIC neomcp::profile neomcp::http_base neojson::neojson
    PRIVATE neomcp::core neomcp::crypto)
target_compile_features(neomcp_client_auth PUBLIC cxx_std_20)
neomcp_configure_target(neomcp_client_auth)
