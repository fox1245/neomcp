# libcurl, which the client's HTTP transport (neomcp::client_http) runs on. It comes from the system: the platform's
# package manager installs it (apt: libcurl4-openssl-dev; Homebrew: curl; vcpkg: curl), and CMake finds it as a package.
# Nothing is fetched or built here. libcurl appears in no public header of neomcp.
#
# The minimum is the oldest version the CI builds and tests against (Ubuntu 24.04's package). The code itself needs
# 7.87.0: curl_multi_poll (7.66.0), curl_multi_wakeup (7.68.0), CURLOPT_PROTOCOLS_STR (7.85.0) and
# CURL_WRITEFUNC_ERROR (7.87.0); a version between the two may work, but no build checks it.
#
# Included only when NEOMCP_HTTP_CLIENT is ON (cmake/targets/client_http.cmake). Without libcurl the configuration
# stops here and says how to go on: install it, point CMake at it, or build without the HTTP client.
set(NEOMCP_CURL_MINIMUM_VERSION 8.5.0)

find_package(CURL ${NEOMCP_CURL_MINIMUM_VERSION} QUIET)
if(NOT CURL_FOUND OR NOT TARGET CURL::libcurl)
    if(DEFINED CURL_VERSION_STRING AND NOT CURL_VERSION_STRING STREQUAL "")
        set(neomcp_curl_seen " (found ${CURL_VERSION_STRING})")
    else()
        set(neomcp_curl_seen "")
    endif()
    message(FATAL_ERROR
        "neomcp: NEOMCP_HTTP_CLIENT is ON, which builds the client's HTTP transport on libcurl, but libcurl "
        "${NEOMCP_CURL_MINIMUM_VERSION} or newer with its headers was not found${neomcp_curl_seen}. Install it "
        "(apt: libcurl4-openssl-dev; Homebrew: curl; vcpkg: curl) and, where CMake does not look by itself, name its "
        "prefix in CMAKE_PREFIX_PATH or CURL_ROOT; or configure with -DNEOMCP_HTTP_CLIENT=OFF to build without the "
        "HTTP client (a server needs no libcurl).")
endif()
# FindCURL names the version CURL_VERSION_STRING; libcurl's own package configuration names it CURL_VERSION.
if(DEFINED CURL_VERSION_STRING AND NOT CURL_VERSION_STRING STREQUAL "")
    message(STATUS "neomcp: the HTTP client uses libcurl ${CURL_VERSION_STRING}")
else()
    message(STATUS "neomcp: the HTTP client uses libcurl ${CURL_VERSION}")
endif()
