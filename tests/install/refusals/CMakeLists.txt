# One question to the installed package, configured by a test of the parent project:
#
#   cmake -S refusals -B <dir> -Dneomcp_DIR=<the package's folder> -DREQUEST_VERSION=<version>
#         -DREQUEST_COMPONENT=<component> -DEXPECTED=found|refused
#
# The configuration fails unless find_package decides as EXPECTED says, and a refusal must come from the package that
# neomcp_DIR names: its version file (the version is outside the compatibility rule) or its configuration (the
# component is not installed).
cmake_minimum_required(VERSION 3.22)
project(neomcp_package_question LANGUAGES CXX)

foreach(name IN ITEMS neomcp_DIR REQUEST_VERSION REQUEST_COMPONENT EXPECTED)
    if(NOT DEFINED ${name})
        message(FATAL_ERROR "Pass -D${name}=...")
    endif()
endforeach()
set(package_dir "${neomcp_DIR}")

find_package(neomcp ${REQUEST_VERSION} CONFIG QUIET COMPONENTS ${REQUEST_COMPONENT} NO_DEFAULT_PATH)

if(EXPECTED STREQUAL "found")
    if(NOT neomcp_FOUND)
        message(FATAL_ERROR "neomcp ${REQUEST_VERSION} with ${REQUEST_COMPONENT} was not found in ${package_dir}")
    endif()
elseif(EXPECTED STREQUAL "refused")
    if(neomcp_FOUND)
        message(FATAL_ERROR "neomcp ${REQUEST_VERSION} with ${REQUEST_COMPONENT} was found, and should not be")
    endif()
    if(DEFINED neomcp_${REQUEST_COMPONENT}_FOUND)
        # The configuration was read: the version was accepted and the component must be the reason.
        if(neomcp_${REQUEST_COMPONENT}_FOUND)
            message(FATAL_ERROR "the configuration reports ${REQUEST_COMPONENT} as found")
        endif()
    elseif(NOT neomcp_CONSIDERED_VERSIONS)
        # The configuration was not read: its version file must have been seen and have refused the version.
        message(FATAL_ERROR "no version of neomcp was considered in ${package_dir}")
    else()
        message(STATUS "refused ${REQUEST_VERSION}; considered ${neomcp_CONSIDERED_VERSIONS}")
    endif()
else()
    message(FATAL_ERROR "EXPECTED must be found or refused, not ${EXPECTED}")
endif()
