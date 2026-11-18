if(NOT TARGET GTest::gtest_main)
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG b514bdc898e2951020cbdca1304b75f5950d1f59)  # v1.15.2
    set(INSTALL_GTEST OFF)
    # MSVC: build googletest with the same runtime library as the tests (the default /MD).
    set(gtest_force_shared_crt ON)
    FetchContent_MakeAvailable(googletest)
endif()

add_executable(neomcp_profile_test profile_test.cpp)
find_package(Threads REQUIRED)
target_link_libraries(neomcp_profile_test PRIVATE neomcp::profile neojson::neojson GTest::gtest_main Threads::Threads)
neomcp_configure_target(neomcp_profile_test)

include(GoogleTest)
gtest_discover_tests(neomcp_profile_test DISCOVERY_TIMEOUT 120)
