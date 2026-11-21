# The regular expression engine behind the JSON Schema keywords `pattern` and `patternProperties`: RE2, which needs
# Abseil. A project that already provides the target re2::re2 keeps its own (it then also provides what RE2 needs); one
# that provides the Abseil targets (absl::base) but not RE2 gets RE2 built against its Abseil.
#
# Pinned by commit of a release tag:
#   RE2     2025-11-05   (commit 927f5d53caf8111721e734cf24724686bb745f55)
#   Abseil  20260817.0   (commit 2065f4ded0558c6f89fee67c8e5228feb4eb960e)
# RE2's own module file asks for Abseil 20250512.1 or newer; the newest release is taken, where corrections land.
# RE2 still uses two interfaces that this Abseil marks as deprecated, which shows as warnings in RE2's own sources.
if(NOT TARGET re2::re2)
    include(FetchContent)

    # What the fetched projects are built with: static libraries that can be linked into a shared one, one language
    # level for both, and none of their tests, benchmarks or install rules. Each variable is set for the fetch only and
    # put back afterwards, so that nothing is left behind in a project that uses neomcp as a subdirectory.
    set(NEOMCP_RE2_NAMES
        BUILD_TESTING BUILD_SHARED_LIBS CMAKE_POSITION_INDEPENDENT_CODE
        CMAKE_CXX_STANDARD CMAKE_CXX_STANDARD_REQUIRED CMAKE_CXX_EXTENSIONS
        ABSL_BUILD_TESTING ABSL_BUILD_TEST_HELPERS ABSL_ENABLE_INSTALL ABSL_PROPAGATE_CXX_STD ABSL_USE_SYSTEM_INCLUDES
        RE2_BUILD_TESTING RE2_TEST RE2_BENCHMARK RE2_INSTALL RE2_USE_ICU RE2_BUILD_FRAMEWORK USEPCRE)
    set(NEOMCP_RE2_VALUES
        OFF OFF ON
        20 ON OFF
        OFF OFF OFF ON ON
        OFF OFF OFF OFF OFF OFF OFF)
    foreach(NEOMCP_RE2_NAME NEOMCP_RE2_VALUE IN ZIP_LISTS NEOMCP_RE2_NAMES NEOMCP_RE2_VALUES)
        if(DEFINED ${NEOMCP_RE2_NAME})
            set(NEOMCP_RE2_SAVED_${NEOMCP_RE2_NAME} "${${NEOMCP_RE2_NAME}}")
            set(NEOMCP_RE2_HAD_${NEOMCP_RE2_NAME} ON)
        else()
            set(NEOMCP_RE2_HAD_${NEOMCP_RE2_NAME} OFF)
        endif()
        set(${NEOMCP_RE2_NAME} "${NEOMCP_RE2_VALUE}")
    endforeach()

    # GCC 13 with the null checks of UndefinedBehaviorSanitizer (-fsanitize=undefined, also -fsanitize=null,
    # -fsanitize=nonnull-attribute and -fsanitize=returns-nonnull-attribute) cannot compile a constant expression in
    # Abseil's hash tables, which RE2 reaches: the comparison of a function address with nullptr in
    # hash_policy_traits.h is no longer constant. The seven releases from 20250127.2 to 20260817.0 that were tried do
    # this. The code of the two fetched projects is therefore not instrumented for those checks. The option is added
    # for the fetch and taken away afterwards, so that it does not reach this project's own targets.
    get_directory_property(NEOMCP_RE2_SAVED_COMPILE_OPTIONS COMPILE_OPTIONS)
    add_compile_options(
        "$<$<CXX_COMPILER_ID:GNU>:-fno-sanitize=null,nonnull-attribute,returns-nonnull-attribute>")

    if(NOT TARGET absl::base)
        FetchContent_Declare(neomcp_abseil
            GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
            GIT_TAG 2065f4ded0558c6f89fee67c8e5228feb4eb960e)
        FetchContent_MakeAvailable(neomcp_abseil)
        # Only the parts of Abseil that RE2 reaches are built.
        set_property(DIRECTORY "${neomcp_abseil_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
        # Read by cmake/NeomcpInstall.cmake, which installs the libraries of a static build.
        set(NEOMCP_ABSEIL_FETCHED ON)
    endif()

    FetchContent_Declare(neomcp_re2
        GIT_REPOSITORY https://github.com/google/re2.git
        GIT_TAG 927f5d53caf8111721e734cf24724686bb745f55)
    FetchContent_MakeAvailable(neomcp_re2)
    set_property(DIRECTORY "${neomcp_re2_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)

    set_directory_properties(PROPERTIES COMPILE_OPTIONS "${NEOMCP_RE2_SAVED_COMPILE_OPTIONS}")
    unset(NEOMCP_RE2_SAVED_COMPILE_OPTIONS)
    unset(NEOMCP_RE2_VALUE)
    foreach(NEOMCP_RE2_NAME IN LISTS NEOMCP_RE2_NAMES)
        if(NEOMCP_RE2_HAD_${NEOMCP_RE2_NAME})
            set(${NEOMCP_RE2_NAME} "${NEOMCP_RE2_SAVED_${NEOMCP_RE2_NAME}}")
        else()
            unset(${NEOMCP_RE2_NAME})
        endif()
    endforeach()

    # Warnings in the headers of a fetched dependency are not this project's to report. (Abseil's own directories are
    # marked by its ABSL_USE_SYSTEM_INCLUDES; this is RE2's.)
    get_target_property(NEOMCP_RE2_INCLUDES re2 INTERFACE_INCLUDE_DIRECTORIES)
    set_target_properties(re2 PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${NEOMCP_RE2_INCLUDES}")

    # Read by cmake/NeomcpInstall.cmake, which installs the libraries of a static build.
    set(NEOMCP_RE2_FETCHED ON)
endif()
