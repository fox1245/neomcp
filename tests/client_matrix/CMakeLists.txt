# The matrix of the Client against servers over real pipes and sockets: the library's own servers (a program of the
# suite over stdio, and in process over Streamable HTTP and the transport of events over HTTP) on every revision each
# transport defines, and servers that serve one era only; and a peer program given on the command line over every
# transport, run against the suite's own program and, with NEOMCP_INTEROP_TESTS, against the official Python SDK's
# servers (tests/interop/python_server.py).

# The matrix server as a program (stdio, or HTTP on a port the system chooses).
add_executable(neomcp_client_matrix_server server.cpp ../support/http_sse_server.cpp)
target_link_libraries(neomcp_client_matrix_server PRIVATE neomcp::stdio neomcp::http asio::asio Threads::Threads)
target_include_directories(neomcp_client_matrix_server PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../support")
neomcp_configure_target(neomcp_client_matrix_server)

if(NOT TARGET neomcp::client_stdio AND NOT NEOMCP_HTTP_CLIENT)
    return()
endif()

# The flow every cell runs.
add_library(neomcp_client_matrix_flow STATIC flow.cpp)
target_link_libraries(neomcp_client_matrix_flow PUBLIC neomcp::client neomcp::server GTest::gtest asio::asio
    Threads::Threads)
neomcp_configure_target(neomcp_client_matrix_flow)

if(TARGET neomcp::client_stdio)
    add_executable(neomcp_client_matrix_stdio_test stdio_test.cpp)
    target_link_libraries(neomcp_client_matrix_stdio_test PRIVATE neomcp_client_matrix_flow neomcp::client_stdio
        GTest::gtest_main Threads::Threads)
    target_compile_definitions(neomcp_client_matrix_stdio_test PRIVATE
        "NEOMCP_MATRIX_SERVER=\"$<TARGET_FILE:neomcp_client_matrix_server>\"")
    add_dependencies(neomcp_client_matrix_stdio_test neomcp_client_matrix_server)
    neomcp_configure_target(neomcp_client_matrix_stdio_test)
    gtest_discover_tests(neomcp_client_matrix_stdio_test DISCOVERY_TIMEOUT 120)
endif()

if(NOT NEOMCP_HTTP_CLIENT)
    return()
endif()

add_executable(neomcp_client_matrix_http_test http_test.cpp ../support/http_sse_server.cpp)
target_link_libraries(neomcp_client_matrix_http_test PRIVATE neomcp_client_matrix_flow neomcp::client_http
    neomcp::http GTest::gtest_main Threads::Threads)
target_include_directories(neomcp_client_matrix_http_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../support")
if(WIN32)
    target_link_libraries(neomcp_client_matrix_http_test PRIVATE ws2_32)
endif()
neomcp_configure_target(neomcp_client_matrix_http_test)
gtest_discover_tests(neomcp_client_matrix_http_test DISCOVERY_TIMEOUT 120)

# The peer test starts its HTTP servers with a POSIX helper (process.cpp); it needs both transports of the client.
if(NOT UNIX OR NOT TARGET neomcp::client_stdio)
    return()
endif()

add_executable(neomcp_client_matrix_peer_test peer_test.cpp process.cpp)
target_link_libraries(neomcp_client_matrix_peer_test PRIVATE neomcp_client_matrix_flow neomcp::client_stdio
    neomcp::client_http GTest::gtest Threads::Threads)
add_dependencies(neomcp_client_matrix_peer_test neomcp_client_matrix_server)
neomcp_configure_target(neomcp_client_matrix_peer_test)
# One test per peer; the cells are the program's own tests, named in its output.
add_test(NAME ClientMatrixPeer.LibraryServer
    COMMAND neomcp_client_matrix_peer_test --paged -- $<TARGET_FILE:neomcp_client_matrix_server>)
set_tests_properties(ClientMatrixPeer.LibraryServer PROPERTIES TIMEOUT 600)

if(NEOMCP_INTEROP_TESTS)
    # The official Python SDK's servers as peers. Isolated mode (-I): the interpreter reads neither the environment nor
    # the script's folder; the packages come from its own site-packages.
    add_test(NAME interop_python_sdk_servers
        COMMAND neomcp_client_matrix_peer_test -- "${NEOMCP_INTEROP_PYTHON}" -I
            "${CMAKE_CURRENT_SOURCE_DIR}/../interop/python_server.py")
    set_tests_properties(interop_python_sdk_servers PROPERTIES TIMEOUT 900)
endif()
