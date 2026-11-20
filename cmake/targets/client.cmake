# The client: the client halves of the mechanisms and their registry, the client engine, the input handlers with their
# dispatcher, and the Client with its driver (one thread that runs an asio::io_context, the timers, the worker pool of
# plain input handlers) and the in-memory transport. The halves read and write what a server also reads and writes
# through the internal input functions of the core (src/core/input.h), so the target sees the sources' folder
# privately; no public header of it includes an internal one. Input handlers may be coroutines (asio), the engine
# compiles the schemas a server sends (the validator), and typed calls convert values through their descriptions. The
# waits before a lost subscription is reopened are drawn from the operating system's random source (crypto).
add_library(neomcp_client
    src/client/client.cpp
    src/client/engine.cpp
    src/client/feed.cpp
    src/client/input.cpp
    src/client/mechanisms.cpp
    src/client/memory_transport.cpp
    src/client/result.cpp)
add_library(neomcp::client ALIAS neomcp_client)
target_include_directories(neomcp_client
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(neomcp_client PUBLIC neomcp::core neomcp::describe neomcp::schema asio::asio Threads::Threads
    PRIVATE neomcp::crypto)
target_compile_features(neomcp_client PUBLIC cxx_std_20)
neomcp_configure_target(neomcp_client)
