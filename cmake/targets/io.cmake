# Byte streams for the transports that speak over a pair of streams: the line splitter that cuts input into messages,
# the reader and writer over file descriptors, and the guard that keeps SIGPIPE from ending the process on a write to a
# closed pipe. Internal: there is no public header, and a transport links it without exposing it.
#
# The abstract ByteReader and ByteWriter that these implement are declared in the public header neomcp/stdio.h, which
# includes neomcp/server.h. The sources therefore compile against the server's headers (nothing of the server is
# called), and the target links neomcp::server for them.
add_library(neomcp_io
    src/io/fd_streams.cpp)
add_library(neomcp::io ALIAS neomcp_io)
target_include_directories(neomcp_io
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(neomcp_io PRIVATE neomcp::server)
target_compile_features(neomcp_io PUBLIC cxx_std_20)
neomcp_configure_target(neomcp_io)
