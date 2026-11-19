# What an HTTP client needs as well as an HTTP server, and nothing that serves: the header collection and the request
# and response model, media types, the server-sent events writer, the parsers for an authority and an origin with the
# strict IP text grammar they use, and (as a header only) the random source under the names of the HTTP blocks. It
# knows nothing of the protocol and does not use the HTTP library; neomcp::http, which serves, links it.
#
# The headers of this target are in include/neomcp/http/ beside those of neomcp::http. message.h includes body_stream.h
# (the stream a server fills while it answers) for the type of Response::stream, and policy.h declares the host policy
# next to the authority parser; both are read here as headers, and the code behind body_stream.h and the host policy
# is in neomcp::http.
add_library(neomcp_http_base
    src/http_base/message.cpp
    src/http_base/sse.cpp
    src/http_base/media_type.cpp
    src/http_base/authority.cpp)
add_library(neomcp::http_base ALIAS neomcp_http_base)
target_include_directories(neomcp_http_base
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# Asio parses the IP addresses; it is in no public header. The random source is in the header of this target.
target_link_libraries(neomcp_http_base PUBLIC neomcp::crypto Threads::Threads PRIVATE asio::asio)
if(WIN32)
    target_link_libraries(neomcp_http_base PRIVATE ws2_32)
endif()
target_compile_features(neomcp_http_base PUBLIC cxx_std_20)
neomcp_configure_target(neomcp_http_base)
