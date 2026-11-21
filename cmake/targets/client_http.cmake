# The client's HTTP transports, on libcurl: the transfer thread, Streamable HTTP and the older transport of events over
# HTTP, the guard of URLs a server supplies, the forms of Client::open and make_transport that take an HttpEndpoint, and
# the OAuth authorizer (neomcp/client/oauth.h), which drives the flow of neomcp::client_auth through a transport and
# uses the transport's URL guard as the flow's rule for URLs, so it is built here.
# Built only with NEOMCP_HTTP_CLIENT, which needs libcurl from the system (cmake/NeomcpCurl.cmake); a program that only
# serves builds without it. libcurl is linked privately and appears in no public header, so a program that links this target does not
# see it. The event stream reader it uses is in neomcp::http_base.
option(NEOMCP_HTTP_CLIENT "Build the client's HTTP transport (neomcp::client_http), which needs libcurl from the system" ON)

if(NEOMCP_HTTP_CLIENT)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/NeomcpCurl.cmake")

    add_library(neomcp_client_http
        src/client_auth/authorizer.cpp
        src/client_http/authorization_gate.cpp
        src/client_http/header_fields.cpp
        src/client_http/http_sse.cpp
        src/client_http/streamable.cpp
        src/client_http/transfers.cpp
        src/client_http/url_guard.cpp)
    add_library(neomcp::client_http ALIAS neomcp_client_http)
    target_include_directories(neomcp_client_http
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(neomcp_client_http PUBLIC neomcp::client neomcp::http_base
        PRIVATE neomcp::client_auth neomcp::crypto CURL::libcurl)
    # An installed copy is linked with libcurl, which a project that uses it finds as this build did
    # (cmake/NeomcpInstall.cmake).
    set_target_properties(neomcp_client_http PROPERTIES
        NEOMCP_PACKAGE_DEPENDENCIES ${NEOMCP_CURL_PACKAGE}
        NEOMCP_PACKAGE_DEPENDENCY_VERSIONS ${NEOMCP_CURL_MINIMUM_VERSION})
    target_compile_features(neomcp_client_http PUBLIC cxx_std_20)
    neomcp_configure_target(neomcp_client_http)
endif()
