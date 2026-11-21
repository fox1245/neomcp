# The validator of access tokens that asks the authorization server (token introspection, RFC 7662) for the server's
# protected resource (include/neomcp/http/introspection.h): the request on libcurl's multi interface, in a thread of its
# own, and the reading of the answer. Built only with NEOMCP_HTTP_CLIENT, which needs libcurl from the system
# (cmake/NeomcpCurl.cmake); a program that serves without it builds without this target. libcurl is linked privately
# and appears in no public header. The option is the client's, defined in client_http.cmake, which precedes this file in
# the order of names; the package is found there too, and here when that file did not.
if(NEOMCP_HTTP_CLIENT)
    if(NOT TARGET CURL::libcurl)
        include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/NeomcpCurl.cmake")
    endif()

    add_library(neomcp_server_auth_introspection
        src/server_auth_introspection/answer.cpp
        src/server_auth_introspection/exchange.cpp
        src/server_auth_introspection/validator.cpp)
    add_library(neomcp::server_auth_introspection ALIAS neomcp_server_auth_introspection)
    target_include_directories(neomcp_server_auth_introspection
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(neomcp_server_auth_introspection PUBLIC neomcp::http
        PRIVATE neomcp::http_base neomcp::core neojson::neojson CURL::libcurl)
    # An installed copy is linked with libcurl, which a project that uses it finds as this build did
    # (cmake/NeomcpInstall.cmake).
    set_target_properties(neomcp_server_auth_introspection PROPERTIES
        NEOMCP_PACKAGE_DEPENDENCIES ${NEOMCP_CURL_PACKAGE}
        NEOMCP_PACKAGE_DEPENDENCY_VERSIONS ${NEOMCP_CURL_MINIMUM_VERSION})
    target_compile_features(neomcp_server_auth_introspection PUBLIC cxx_std_20)
    neomcp_configure_target(neomcp_server_auth_introspection)
endif()
