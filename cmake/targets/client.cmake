# The client: the client halves of the mechanisms and their registry, and the client engine. Like the core it does no
# I/O of its own. The halves read and write what a server also reads and writes through the internal
# input functions of the core (src/core/input.h), so the target sees the sources' folder privately; no public header of
# it includes an internal one.
add_library(neomcp_client
    src/client/engine.cpp
    src/client/mechanisms.cpp)
add_library(neomcp::client ALIAS neomcp_client)
target_include_directories(neomcp_client
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(neomcp_client PUBLIC neomcp::core)
target_compile_features(neomcp_client PUBLIC cxx_std_20)
neomcp_configure_target(neomcp_client)
