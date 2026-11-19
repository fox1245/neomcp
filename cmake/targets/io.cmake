# Byte streams for the transports that speak over a pair of streams: the line splitter that cuts input into messages,
# the reader and writer over file descriptors, and the guard that keeps SIGPIPE from ending the process on a write to a
# closed pipe. Internal: the implementations have no public header, and a transport links the target without exposing it.
#
# The abstract ByteReader and ByteWriter that these implement are declared in the public header neomcp/byte_stream.h,
# which this target owns and which includes nothing of the project. The target therefore depends on that header and on
# the standard library, and links nothing of the project; neomcp/stdio.h includes the header for the server's transport.
add_library(neomcp_io
    src/io/fd_streams.cpp)
add_library(neomcp::io ALIAS neomcp_io)
target_include_directories(neomcp_io
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(neomcp_io PUBLIC cxx_std_20)
neomcp_configure_target(neomcp_io)
