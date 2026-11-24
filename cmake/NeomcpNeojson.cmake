# The JSON dependency. A project that already provides the target neojson::neojson keeps its own.
if(NOT TARGET neojson::neojson)
    include(FetchContent)
    FetchContent_Declare(neojson
        GIT_REPOSITORY https://github.com/fox1245/neojson.git
        GIT_TAG 177289eccfcd80ec52c6da8872f524da3fa95110)  # v0.1.0

    # Only the library is needed. The option is set for this call and put back afterwards.
    if(DEFINED NEOJSON_BUILD_TESTS)
        set(NEOMCP_SAVED_NEOJSON_BUILD_TESTS "${NEOJSON_BUILD_TESTS}")
        set(NEOMCP_HAD_NEOJSON_BUILD_TESTS ON)
    else()
        set(NEOMCP_HAD_NEOJSON_BUILD_TESTS OFF)
    endif()
    set(NEOJSON_BUILD_TESTS OFF)
    FetchContent_MakeAvailable(neojson)
    if(NEOMCP_HAD_NEOJSON_BUILD_TESTS)
        set(NEOJSON_BUILD_TESTS "${NEOMCP_SAVED_NEOJSON_BUILD_TESTS}")
    else()
        unset(NEOJSON_BUILD_TESTS)
    endif()

    # Read by cmake/NeomcpInstall.cmake, which installs the library and its headers.
    set(NEOMCP_NEOJSON_FETCHED ON)
    set(NEOMCP_NEOJSON_HEADER_DIR "${neojson_SOURCE_DIR}/include")

    # Warnings in the headers of a fetched dependency are not this project's to report.
    get_target_property(NEOMCP_NEOJSON_INCLUDES neojson INTERFACE_INCLUDE_DIRECTORIES)
    set_target_properties(neojson PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${NEOMCP_NEOJSON_INCLUDES}")
endif()
