# Install rules and the CMake package configuration, so that a project can use an installed neomcp:
#
#   find_package(neomcp CONFIG REQUIRED COMPONENTS server stdio)
#   target_link_libraries(app PRIVATE neomcp::server neomcp::stdio)
#
# Included from CMakeLists.txt when NEOMCP_INSTALL is ON, after every target has been defined.
#
# What is installed:
# - neomcp's libraries and its public headers, as the components below, in one export set (the libraries link each
#   other, so they are exported together and the components are checked in the package configuration);
# - the dependencies that appear in the public headers: standalone Asio (headers only) and neojson (its headers and its
#   library, which holds the JSON parser it vendors). Their headers go to a folder of neomcp's own and their libraries
#   beside the others in a folder of neomcp's own, so that installing neomcp into a shared prefix overwrites no other
#   installation of them. They are exported as neomcp::asio and neomcp::neojson;
# - in a static build, the libraries that neomcp's own libraries link privately and that a program must link with
#   them (RE2 and the parts of Abseil it reaches), found by following the targets' links. They are libraries only: no
#   public header of neomcp includes theirs. In a shared build they are inside neomcp's shared libraries;
# - the package configuration, its version file (see NEOMCP_PACKAGE_COMPATIBILITY) and the exported targets.
# cpp-httplib is linked for the build only (header-only and private), and nothing of it is installed. libcurl and the
# thread library come from the system: the package configuration finds them as the build did.

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# The components, one per library target ${PROJECT_NAME}_<component>, exported as ${PROJECT_NAME}::<component> (the
# name the build gives the same target as an alias). A target added later is one more name here. A name whose target
# this configuration does not build (the HTTP client without NEOMCP_HTTP_CLIENT, the child-process transport on a C
# library without the posix_spawn file actions) is left out, and find_package reports it as not found when it is asked
# for. `io` is internal (it has no public header) and is installed because the stdio transports link it.
set(NEOMCP_PACKAGE_COMPONENTS
    profile
    describe
    schema
    crypto
    core
    io
    server
    stdio
    http_base
    http
    client
    client_in_process
    client_stdio
    client_http
    client_auth
    server_auth_introspection
    server_auth_jwt)

# A 0.x library breaks its interface in a minor release and keeps it in a patch release: a project that asks for
# 0.y.z gets 0.y.w with w >= z, and no other 0.y. From 1.0 on the rule is SameMajorVersion.
set(NEOMCP_PACKAGE_COMPATIBILITY SameMinorVersion)

set(NEOMCP_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}")
# The folders of neomcp's own for the dependencies it installs.
set(NEOMCP_INSTALL_DEPS_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}/${PROJECT_NAME}/deps")
set(NEOMCP_INSTALL_DEPS_LIBDIR "${CMAKE_INSTALL_LIBDIR}/${PROJECT_NAME}")
set(NEOMCP_EXPORT_SET "${PROJECT_NAME}-targets")

# A dependency that the including project provides (the target asio::asio, neojson::neojson, re2::re2 or absl::base
# existed before neomcp looked for it) is that project's to install, and neomcp cannot export a target it did not
# create.
foreach(neomcp_fetched IN ITEMS NEOMCP_ASIO_FETCHED NEOMCP_NEOJSON_FETCHED NEOMCP_RE2_FETCHED NEOMCP_ABSEIL_FETCHED)
    if(NOT ${neomcp_fetched})
        message(FATAL_ERROR
            "neomcp: NEOMCP_INSTALL is ON, but the including project provides one of the dependencies neomcp would "
            "fetch (${neomcp_fetched} is not set); neomcp installs only the dependencies it fetches itself. Configure "
            "with -DNEOMCP_INSTALL=OFF, or install neomcp as a project of its own.")
    endif()
endforeach()
unset(neomcp_fetched)

# Shared libraries built here find the ones they load beside them and in the dependencies' folder.
if(APPLE)
    set(neomcp_origin "@loader_path")
else()
    set(neomcp_origin "$ORIGIN")
endif()
file(RELATIVE_PATH neomcp_deps_from_libdir
    "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}" "${CMAKE_INSTALL_PREFIX}/${NEOMCP_INSTALL_DEPS_LIBDIR}")
set(NEOMCP_INSTALL_RPATH_ENTRIES "${neomcp_origin}" "${neomcp_origin}/${neomcp_deps_from_libdir}")
unset(neomcp_origin)
unset(neomcp_deps_from_libdir)

function(neomcp_install_rpath target)
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "SHARED_LIBRARY")
        set_property(TARGET ${target} APPEND PROPERTY INSTALL_RPATH ${NEOMCP_INSTALL_RPATH_ENTRIES})
    endif()
endfunction()

# The real name of a target (an alias resolved), in `out`.
function(neomcp_real_target out name)
    get_target_property(aliased ${name} ALIASED_TARGET)
    if(aliased)
        set(${out} "${aliased}" PARENT_SCOPE)
    else()
        set(${out} "${name}" PARENT_SCOPE)
    endif()
endfunction()

# Every target of this build that the targets given link and that an installed copy of them needs, those given excluded,
# in `out`. A static library needs everything it links (its private links are linked by the program); a shared or
# interface library needs only what it links publicly. Imported targets come from packages the configuration finds,
# and an item that is not a target (a library name, a flag, an expression that is not evaluated here) is left to CMake,
# which stops the generation when an exported target needs a target outside every export set.
function(neomcp_link_closure out)
    set(seen ${ARGN})
    set(pending ${ARGN})
    set(found "")
    while(pending)
        list(POP_FRONT pending current)
        get_target_property(type ${current} TYPE)
        set(items "")
        if(type STREQUAL "STATIC_LIBRARY")
            get_target_property(links ${current} LINK_LIBRARIES)
            if(links)
                list(APPEND items ${links})
            endif()
        endif()
        get_target_property(links ${current} INTERFACE_LINK_LIBRARIES)
        if(links)
            list(APPEND items ${links})
        endif()
        foreach(item IN LISTS items)
            if(NOT TARGET "${item}")
                continue()
            endif()
            neomcp_real_target(real "${item}")
            get_target_property(imported ${real} IMPORTED)
            if(imported OR real IN_LIST seen)
                continue()
            endif()
            list(APPEND seen ${real})
            list(APPEND found ${real})
            list(APPEND pending ${real})
        endforeach()
    endwhile()
    set(${out} "${found}" PARENT_SCOPE)
endfunction()

# ---- neomcp's own libraries

set(NEOMCP_INSTALLED_COMPONENTS "")
set(neomcp_own_targets "")
foreach(component IN LISTS NEOMCP_PACKAGE_COMPONENTS)
    set(target "${PROJECT_NAME}_${component}")
    if(TARGET ${target})
        set_target_properties(${target} PROPERTIES EXPORT_NAME ${component})
        neomcp_install_rpath(${target})
        list(APPEND NEOMCP_INSTALLED_COMPONENTS ${component})
        list(APPEND neomcp_own_targets ${target})
    endif()
endforeach()

install(TARGETS ${neomcp_own_targets}
    EXPORT ${NEOMCP_EXPORT_SET}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/${PROJECT_NAME}" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# ---- The dependencies in the public headers

# Asio: the folder that holds asio.hpp and asio/, and nothing else of the checkout.
target_include_directories(neomcp_asio SYSTEM INTERFACE "$<INSTALL_INTERFACE:${NEOMCP_INSTALL_DEPS_INCLUDEDIR}>")
set_target_properties(neomcp_asio PROPERTIES EXPORT_NAME asio)
install(FILES "${NEOMCP_ASIO_HEADER_DIR}/asio.hpp" DESTINATION ${NEOMCP_INSTALL_DEPS_INCLUDEDIR})
install(DIRECTORY "${NEOMCP_ASIO_HEADER_DIR}/asio" DESTINATION ${NEOMCP_INSTALL_DEPS_INCLUDEDIR})

# neojson: its include folder.
target_include_directories(neojson SYSTEM INTERFACE "$<INSTALL_INTERFACE:${NEOMCP_INSTALL_DEPS_INCLUDEDIR}>")
install(DIRECTORY "${NEOMCP_NEOJSON_HEADER_DIR}/" DESTINATION ${NEOMCP_INSTALL_DEPS_INCLUDEDIR})

# ---- Every other target the installed libraries need (the public dependencies above among them)

neomcp_link_closure(neomcp_dependency_targets ${neomcp_own_targets})
foreach(target IN LISTS neomcp_dependency_targets)
    # A target of neomcp's own (defined in this folder, as Asio's target is; the fetched dependencies are defined in
    # their own folders) that an installed library links must be a component, or it would be installed as a
    # dependency under its target name.
    get_target_property(folder ${target} SOURCE_DIR)
    if(folder STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT target STREQUAL "neomcp_asio")
        message(FATAL_ERROR
            "neomcp: the installed libraries link ${target}, which is not in NEOMCP_PACKAGE_COMPONENTS "
            "(cmake/NeomcpInstall.cmake); add its component name there.")
    endif()
    neomcp_install_rpath(${target})
    # Headers a dependency lists as its own (RE2 does) are not needed by a program that uses neomcp; those that are
    # needed are installed above.
    set_property(TARGET ${target} PROPERTY PUBLIC_HEADER "")
endforeach()
install(TARGETS ${neomcp_dependency_targets}
    EXPORT ${NEOMCP_EXPORT_SET}
    ARCHIVE DESTINATION ${NEOMCP_INSTALL_DEPS_LIBDIR}
    LIBRARY DESTINATION ${NEOMCP_INSTALL_DEPS_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(EXPORT ${NEOMCP_EXPORT_SET}
    NAMESPACE ${PROJECT_NAME}::
    FILE ${PROJECT_NAME}-targets.cmake
    DESTINATION ${NEOMCP_INSTALL_CMAKEDIR})

# ---- The package configuration

# Packages the configuration finds for each component: what a target declares in NEOMCP_PACKAGE_DEPENDENCIES (with
# the versions in NEOMCP_PACKAGE_DEPENDENCY_VERSIONS, in the same order), for the component's target and every target
# of neomcp it links.
set(NEOMCP_PACKAGE_COMPONENT_DEPENDENCIES "")
set(neomcp_all_packages "")
foreach(component IN LISTS NEOMCP_INSTALLED_COMPONENTS)
    set(target "${PROJECT_NAME}_${component}")
    neomcp_link_closure(reached ${target})
    set(packages "")
    foreach(source IN LISTS target reached)
        get_target_property(names ${source} NEOMCP_PACKAGE_DEPENDENCIES)
        get_target_property(versions ${source} NEOMCP_PACKAGE_DEPENDENCY_VERSIONS)
        if(NOT names)
            continue()
        endif()
        foreach(name version IN ZIP_LISTS names versions)
            if(NOT name IN_LIST packages)
                list(APPEND packages ${name})
            endif()
            if(NOT name IN_LIST neomcp_all_packages)
                list(APPEND neomcp_all_packages ${name})
                string(APPEND NEOMCP_PACKAGE_COMPONENT_DEPENDENCIES
                    "set(_neomcp_version_${name} \"${version}\")\n")
            endif()
        endforeach()
    endforeach()
    string(APPEND NEOMCP_PACKAGE_COMPONENT_DEPENDENCIES "set(_neomcp_packages_${component} \"${packages}\")\n")
endforeach()
string(APPEND NEOMCP_PACKAGE_COMPONENT_DEPENDENCIES "set(_neomcp_all_packages \"${neomcp_all_packages}\")")

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/neomcp-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config.cmake"
    INSTALL_DESTINATION ${NEOMCP_INSTALL_CMAKEDIR}
    NO_SET_AND_CHECK_MACRO)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY ${NEOMCP_PACKAGE_COMPATIBILITY})
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config-version.cmake"
    DESTINATION ${NEOMCP_INSTALL_CMAKEDIR})
