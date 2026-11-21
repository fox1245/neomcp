# The validator of JWT access tokens (RFC 9068) for the server's protected resource (include/neomcp/http/jwt.h): the
# reading of the token and of the authorization server's key set, the signatures on OpenSSL's libcrypto, and the fetches
# of the key set on libcurl, in a thread of its own. Built with NEOMCP_JWT, which needs libcrypto 3.0 or newer from the
# system and the HTTP client's libcurl (NEOMCP_HTTP_CLIENT, defined in client_http.cmake, which precedes this file in
# the order of names). The option is ON by default when both are there, and OFF otherwise, so that a configuration
# without OpenSSL (or without the HTTP client) builds everything else; set it ON to require them. Neither library
# appears in a public header: both are linked privately.
set(NEOMCP_OPENSSL_PACKAGE OpenSSL)
set(NEOMCP_OPENSSL_MINIMUM_VERSION 3.0)

find_package(${NEOMCP_OPENSSL_PACKAGE} ${NEOMCP_OPENSSL_MINIMUM_VERSION} COMPONENTS Crypto QUIET)
if(OPENSSL_FOUND AND TARGET OpenSSL::Crypto AND NEOMCP_HTTP_CLIENT)
    set(neomcp_jwt_default ON)
else()
    set(neomcp_jwt_default OFF)
endif()
option(NEOMCP_JWT
    "Build the validator of JWT access tokens (neomcp::server_auth_jwt), which needs OpenSSL's libcrypto 3.0 and libcurl"
    ${neomcp_jwt_default})
unset(neomcp_jwt_default)

if(NOT NEOMCP_JWT)
    message(STATUS "neomcp: the JWT access token validator (NEOMCP_JWT) is not built")
    return()
endif()

if(NOT OPENSSL_FOUND OR NOT TARGET OpenSSL::Crypto)
    if(DEFINED OPENSSL_VERSION AND NOT OPENSSL_VERSION STREQUAL "")
        set(neomcp_openssl_seen " (found ${OPENSSL_VERSION})")
    else()
        set(neomcp_openssl_seen "")
    endif()
    message(FATAL_ERROR
        "neomcp: NEOMCP_JWT is ON, which builds the JWT access token validator on OpenSSL's libcrypto, but libcrypto "
        "${NEOMCP_OPENSSL_MINIMUM_VERSION} or newer with its headers was not found${neomcp_openssl_seen}. Install it "
        "(apt: libssl-dev; Homebrew: openssl@3; vcpkg: openssl) and, where CMake does not look by itself, name its "
        "prefix in OPENSSL_ROOT_DIR; or configure with -DNEOMCP_JWT=OFF to build without the validator.")
endif()
if(NOT NEOMCP_HTTP_CLIENT)
    message(FATAL_ERROR
        "neomcp: NEOMCP_JWT is ON, but the JWT access token validator fetches the authorization server's keys with the "
        "HTTP client's libcurl, and NEOMCP_HTTP_CLIENT is OFF. Configure with -DNEOMCP_HTTP_CLIENT=ON, or with "
        "-DNEOMCP_JWT=OFF to build without the validator.")
endif()
if(NOT TARGET CURL::libcurl)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/NeomcpCurl.cmake")
endif()
message(STATUS "neomcp: the JWT access token validator (NEOMCP_JWT) uses OpenSSL ${OPENSSL_VERSION}")

add_library(neomcp_server_auth_jwt
    src/server_auth_jwt/fetch.cpp
    src/server_auth_jwt/keys.cpp
    src/server_auth_jwt/token.cpp
    src/server_auth_jwt/validator.cpp)
add_library(neomcp::server_auth_jwt ALIAS neomcp_server_auth_jwt)
target_include_directories(neomcp_server_auth_jwt
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(neomcp_server_auth_jwt PUBLIC neomcp::http
    PRIVATE neomcp::http_base neomcp::core neojson::neojson OpenSSL::Crypto CURL::libcurl)
# An installed copy is linked with libcrypto and libcurl, which a project that uses it finds as this build did
# (cmake/NeomcpInstall.cmake).
set_target_properties(neomcp_server_auth_jwt PROPERTIES
    NEOMCP_PACKAGE_DEPENDENCIES "${NEOMCP_OPENSSL_PACKAGE};${NEOMCP_CURL_PACKAGE}"
    NEOMCP_PACKAGE_DEPENDENCY_VERSIONS "${NEOMCP_OPENSSL_MINIMUM_VERSION};${NEOMCP_CURL_MINIMUM_VERSION}")
target_compile_features(neomcp_server_auth_jwt PUBLIC cxx_std_20)
neomcp_configure_target(neomcp_server_auth_jwt)
