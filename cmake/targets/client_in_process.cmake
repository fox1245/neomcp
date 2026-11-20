# A client connected to a Server in the same process, through the in-memory transport. A target of its own so that
# the client's library does not depend on the server's.
add_library(neomcp_client_in_process
    src/client_in_process/in_process.cpp)
add_library(neomcp::client_in_process ALIAS neomcp_client_in_process)
target_include_directories(neomcp_client_in_process
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(neomcp_client_in_process PUBLIC neomcp::client neomcp::server)
target_compile_features(neomcp_client_in_process PUBLIC cxx_std_20)
neomcp_configure_target(neomcp_client_in_process)
