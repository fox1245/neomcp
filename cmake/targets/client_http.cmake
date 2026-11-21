# The client's HTTP transports, on libcurl: the transfer thread, Streamable HTTP and the older transport of events over
# HTTP, the guard of URLs a server supplies, and the forms of Client::open and make_transport that take an HttpEndpoint.
# Built only with NEOMCP_HTTP_CLIENT, which needs libcurl from the system (cmake/NeomcpCurl.cmake); a program that only
# serves builds without it. libcurl is linked privately and appears in no public header, so a program that links this target does not
# see it. The event stream reader it uses is in neomcp::http_base.
option(NEOMCP_HTTP_CLIENT "Build the client's HTTP transport (neomcp::client_http), which needs libcurl from the system" ON)

if(NEOMCP_HTTP_CLIENT)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/NeomcpCurl.cmake")

    add_library(neomcp_client_http
        src/client_http/authorization_gate.cpp
        src/client_http/http_sse.cpp
        src/client_http/streamable.cpp
        src/client_http/transfers.cpp
        src/client_http/url_guard.cpp)
    add_library(neomcp::client_http ALIAS neomcp_client_http)
    target_include_directories(neomcp_client_http
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(neomcp_client_http PUBLIC neomcp::client PRIVATE neomcp::http_base CURL::libcurl)
    target_compile_features(neomcp_client_http PUBLIC cxx_std_20)
    neomcp_configure_target(neomcp_client_http)
endif()
