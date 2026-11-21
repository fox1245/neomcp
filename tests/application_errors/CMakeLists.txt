# Errors of the application's own (neomcp::ApplicationError): the answer at the host interface, and the Client reading
# them back from handlers of every kind over the in-memory transport, a child process and Streamable HTTP.

add_executable(neomcp_application_errors_host_test host_test.cpp)
target_link_libraries(neomcp_application_errors_host_test PRIVATE neomcp::core GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_application_errors_host_test)
gtest_discover_tests(neomcp_application_errors_host_test DISCOVERY_TIMEOUT 120)

