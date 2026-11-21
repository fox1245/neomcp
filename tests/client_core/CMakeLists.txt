# The client half of the protocol core: the readers of the wire writers, the inverse input functions, the header value
# encoder, the client halves of the mechanisms with their registry, the client engine, alone and paired in process
# with the server's session, and the input handlers with their dispatcher. The tests reach internal headers of the core (the checker of argument headers, the wire
# names).
add_executable(neomcp_client_core_test
    client_input_test.cpp
    client_mechanism_test.cpp
    engine_authorization_test.cpp
    engine_http_probe_test.cpp
    engine_pair_test.cpp
    engine_pending_test.cpp
    engine_test.cpp
    header_value_test.cpp
    support.cpp
    input_inverse_test.cpp
    wire_read_test.cpp)
target_link_libraries(neomcp_client_core_test PRIVATE neomcp::client neomcp::core neomcp::server GTest::gtest_main
    Threads::Threads)
target_include_directories(neomcp_client_core_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
neomcp_configure_target(neomcp_client_core_test)
gtest_discover_tests(neomcp_client_core_test DISCOVERY_TIMEOUT 120)

# Input handlers whose form the library cannot read must not compile. Each case is an object library that is not part
# of the build; a test builds it. The control case must build, the others must not.
function(neomcp_input_handler_case case expect_failure)
    set(target neomcp_compile_check_input_handlers${case})
    add_library(${target} OBJECT EXCLUDE_FROM_ALL ../compile_check/input_handlers.cpp)
    target_compile_definitions(${target} PRIVATE NEOMCP_CASE=${case})
    target_link_libraries(${target} PRIVATE neomcp::client)
    neomcp_configure_target(${target})
    add_test(NAME CompileCheck.InputHandlers${case}
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG> --target ${target})
    if(expect_failure)
        set_tests_properties(CompileCheck.InputHandlers${case} PROPERTIES WILL_FAIL TRUE)
    endif()
endfunction()
neomcp_input_handler_case(0 FALSE)
neomcp_input_handler_case(1 TRUE)
neomcp_input_handler_case(2 TRUE)
neomcp_input_handler_case(3 TRUE)
