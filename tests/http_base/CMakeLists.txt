# The HTTP pieces in neomcp::http_base that a client of HTTP uses beside a server: the reader of WWW-Authenticate
# challenges with the Bearer challenge of OAuth. No socket is opened.
add_executable(neomcp_http_base_test
    challenge_test.cpp)
target_link_libraries(neomcp_http_base_test PRIVATE neomcp::http_base GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_http_base_test)
gtest_discover_tests(neomcp_http_base_test DISCOVERY_TIMEOUT 120)
