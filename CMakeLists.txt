# CMakeLists.txt for oasis-utils
cmake_minimum_required(VERSION 3.24)

# GoogleTest requires at least C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  GTest
  URL https://github.com/google/googletest/archive/52eb8108c5bdec04579160ae17225d66034bd723.zip
  DOWNLOAD_EXTRACT_TIMESTAMP true
)

# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "Disable GTest installation by parent project" FORCE)

FetchContent_MakeAvailable(GTest)


if(DEFINED ENV{GITHUB_ACTIONS})
  message(STATUS "Running on GitHub Actions")
FetchContent_Declare(
  libimd
  URL https://github.com/hharte/libimd/archive/refs/heads/main.zip
  DOWNLOAD_EXTRACT_TIMESTAMP true
)
else()
  message(STATUS "Not running on GitHub Actions")
FetchContent_Declare(
  libimd
  SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../libimd
)
endif()

FetchContent_MakeAvailable(libimd)


# --- Versioning ---
find_package(Git QUIET)
set(FULL_GIT_VERSION "0.0.0-nogit")     # Default full descriptive version
set(CMAKE_PROJECT_VERSION "0.0.0")      # Default version for project() command

if(Git_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    # 1. Get the full descriptive version (tags, hash, dirty status)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --dirty --always
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE _full_git_version_temp
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_DESCRIBE_RESULT
    )
    if(GIT_DESCRIBE_RESULT EQUAL 0)
        set(FULL_GIT_VERSION ${_full_git_version_temp})
        message(STATUS "Raw Git describe version: ${FULL_GIT_VERSION}")
    else()
        message(WARNING "git describe --tags --dirty --always failed. Using fallback version ${FULL_GIT_VERSION}.")
    endif()

    # 2. Try to get the latest tag and extract Major.Minor.Patch for project()
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE LATEST_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_TAG_RESULT
    )
    if(GIT_TAG_RESULT EQUAL 0)
        # Try to extract X.Y.Z or X.Y from tag (allowing optional 'v' prefix)
        string(REGEX MATCH "^v?([0-9]+\\.[0-9]+(\\.[0-9]+)?)" _extracted_version ${LATEST_TAG})
        if(CMAKE_MATCH_1)
            set(CMAKE_PROJECT_VERSION ${CMAKE_MATCH_1})
            message(STATUS "Using semantic version for CMake project: ${CMAKE_PROJECT_VERSION}")
        else()
            message(WARNING "Latest Git tag (${LATEST_TAG}) doesn't match expected format (X.Y or X.Y.Z). Using default ${CMAKE_PROJECT_VERSION} for project().")
        endif()
    else()
        message(WARNING "Could not get latest Git tag. Using default version ${CMAKE_PROJECT_VERSION} for project().")
    endif()

    if("${CMAKE_PROJECT_VERSION}" STREQUAL "${FULL_GIT_VERSION}")
        set(FULL_GIT_VERSION "OFFICIAL RELEASE")
        message(STATUS "Current commit matches latest tag. Setting version to OFFICIAL RELEASE.")
    endif()

else()
    message(WARNING "Git not found or not a Git repository. Using fallback versions.")
    # Defaults are already set above
endif()

# Project name and version (using sanitized, CMake-compatible version)
project(oasis-utils VERSION ${CMAKE_PROJECT_VERSION})

# Find Python3 for the integration script
find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_FOUND)
    message(WARNING "Python3 interpreter not found. The send/recv integration test might be skipped or fail.")
    set(PYTHON_EXECUTABLE "python3") # Fallback
else()
    set(PYTHON_EXECUTABLE ${Python3_EXECUTABLE})
    message(STATUS "Found Python3 interpreter: ${PYTHON_EXECUTABLE}")
endif()

# --- Testing Framework ---
enable_testing()

# Add version definitions globally for all subsequent targets
add_compile_definitions(
    CMAKE_VERSION_STR="${CMAKE_PROJECT_VERSION}"
    GIT_VERSION_STR="${FULL_GIT_VERSION}"
)

# Standard C11
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF) # Use pure C11, disable GNU extensions if not needed

# --- Platform specific checks ---
include(CheckIncludeFiles)
check_include_files("unistd.h" HAVE_UNISTD_H)
check_include_files("fcntl.h" HAVE_FCNTL_H)
check_include_files("sys/types.h" HAVE_SYS_TYPES_H)
check_include_files("sys/stat.h" HAVE_SYS_STAT_H)

# Check for timestamp setting headers
check_include_files("utime.h" HAVE_UTIME_H)
check_include_files("sys/utime.h" HAVE_SYS_UTIME_H)

if(HAVE_UNISTD_H)
    add_definitions(-DHAVE_UNISTD_H)
endif()
if(HAVE_FCNTL_H)
    add_definitions(-DHAVE_FCNTL_H)
endif()
if(HAVE_SYS_TYPES_H)
    add_definitions(-DHAVE_SYS_TYPES_H)
endif()
if(HAVE_SYS_STAT_H)
    add_definitions(-DHAVE_SYS_STAT_H)
endif()

# Prefer sys/utime.h if available, otherwise use utime.h
if(HAVE_SYS_UTIME_H)
    add_definitions(-DHAVE_SYS_UTIME_H)
elseif(HAVE_UTIME_H)
    add_definitions(-DHAVE_UTIME_H)
endif()

if(MSVC)
add_definitions(-D_CRT_SECURE_NO_DEPRECATE)
add_definitions(-D_CRT_NONSTDC_NO_DEPRECATE)
else()
add_definitions(-D_DEFAULT_SOURCE)
endif()

# --- Project Files ---

# Define source directory (relative to CMakeLists.txt)
set(SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set(TEST_DATA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/disk-images)

# --- Library: liboasis ---
add_library(liboasis STATIC
    ${SOURCE_DIR}/oasis_alloc.c
    ${SOURCE_DIR}/oasis_ascii.c
    ${SOURCE_DIR}/oasis_deb.c
    ${SOURCE_DIR}/oasis_file_copy.c
    ${SOURCE_DIR}/oasis_file_erase.c
    ${SOURCE_DIR}/oasis_file_read.c
    ${SOURCE_DIR}/oasis_file_rename.c
    ${SOURCE_DIR}/oasis_file_write.c
    ${SOURCE_DIR}/oasis_sector_io.c
    ${SOURCE_DIR}/oasis_time.c
    ${SOURCE_DIR}/oasis_utils.c
    ${SOURCE_DIR}/oasis_wildcard.c
    ${SOURCE_DIR}/oasis_extract.c
    ${SOURCE_DIR}/oasis_initdisk.c
)

target_include_directories(liboasis
    PUBLIC
        ${SOURCE_DIR}
        ${libimd_SOURCE_DIR}/src
    PRIVATE
        ${SOURCE_DIR}
)

target_link_libraries(liboasis PRIVATE libimdf)

# Add executable target
add_executable(oasis_disk_util ${SOURCE_DIR}/oasis_disk_util.c)
target_link_libraries(oasis_disk_util PRIVATE liboasis)

# Add executable target
add_executable(oasis_chkdsk ${SOURCE_DIR}/oasis_chkdsk.c)
target_link_libraries(oasis_chkdsk PRIVATE liboasis)

add_executable(oasis_initdisk src/oasis_initdisk_main.c)
target_link_libraries(oasis_initdisk PRIVATE liboasis libimdf)

# Include directory for project headers
target_include_directories(oasis_disk_util PRIVATE ${SOURCE_DIR})
target_include_directories(oasis_chkdsk PRIVATE ${SOURCE_DIR})

# Common source files
set(OASIS_COMMON_SOURCES
    ${SOURCE_DIR}/oasis_pcap.c
    ${SOURCE_DIR}/oasis_sendrecv.c
    ${SOURCE_DIR}/oasis_transfer_utils.c
)

# Platform-specific serial implementation (add to common sources)
if(WIN32)
    list(APPEND OASIS_COMMON_SOURCES ${SOURCE_DIR}/mm_serial_win32.c)
    list(APPEND OASIS_COMMON_SOURCES ${SOURCE_DIR}/oasis_glob_win32.c)
    set(WS2_32_LIBRARY ws2_32)
    set(SETUPAPI_LIBRARY setupapi)
    set(PLATFORM_LIBS ${WS2_32_LIBRARY} ${SETUPAPI_LIBRARY})
else()
    list(APPEND OASIS_COMMON_SOURCES ${SOURCE_DIR}/mm_serial_posix.c)
    set(PLATFORM_LIBS "")
    list(APPEND PLATFORM_LIBS m)
endif()

# Add oasis_recv target
add_executable(oasis_recv ${SOURCE_DIR}/oasis_recv.c ${OASIS_COMMON_SOURCES})
target_link_libraries(oasis_recv PRIVATE liboasis ${PLATFORM_LIBS})
target_include_directories(oasis_recv PRIVATE ${SOURCE_DIR})


# Add oasis_send target
add_executable(oasis_send ${SOURCE_DIR}/oasis_send.c ${OASIS_COMMON_SOURCES})
target_link_libraries(oasis_send PRIVATE liboasis ${PLATFORM_LIBS})
target_include_directories(oasis_send PRIVATE ${SOURCE_DIR})


# --- Unit Tests ---
    message(STATUS "GoogleTest found, configuring liboasis_tests.")

    # Define the test executable for liboasis_tests
    add_executable(liboasis_tests
        ${TEST_DIR}/test_oasis_ascii.cpp
        ${TEST_DIR}/test_oasis_time.cpp
        ${TEST_DIR}/test_oasis_alloc.cpp
        ${TEST_DIR}/test_oasis_sector_io.cpp
        ${TEST_DIR}/test_oasis_deb.cpp
        ${TEST_DIR}/test_oasis_wildcard.cpp
        ${TEST_DIR}/test_oasis_utils.cpp
        ${TEST_DIR}/test_oasis_file_read.cpp
        ${TEST_DIR}/test_oasis_file_write.cpp
        ${TEST_DIR}/test_oasis_extract.cpp
        ${TEST_DIR}/test_oasis_pcap.cpp
        ${TEST_DIR}/test_oasis_sendrecv.cpp
        ${TEST_DIR}/test_oasis_file_erase.cpp
        ${TEST_DIR}/test_oasis_file_rename.cpp
        ${TEST_DIR}/test_oasis_file_copy.cpp
        ${TEST_DIR}/test_oasis_initdisk.cpp
        ${TEST_DIR}/test_oasis_common.cpp
        ${OASIS_COMMON_SOURCES}
    )

    # Define a CMake variable to specify the disk image path (can be set with -D)
    # This is used by test_oasis_file_read.cpp and test_oasis_disk_util_integration.cpp
    set(OASIS_DISK_IMAGE_FOR_TESTS "${TEST_DATA_DIR}/OASIS_Users_Group_Vol01.img" CACHE FILEPATH "Path to OASIS disk image for tests")
    target_compile_definitions(liboasis_tests PRIVATE "OASIS_DISK_IMAGE_PATH_FROM_CMAKE=\"${OASIS_DISK_IMAGE_FOR_TESTS}\"")

    target_link_libraries(liboasis_tests PRIVATE liboasis GTest::gtest GTest::gtest_main ${PLATFORM_LIBS})

    add_executable(test_oasis_transfer_utils
        ${TEST_DIR}/test_oasis_transfer_utils.cpp
        ${TEST_DIR}/test_oasis_common.cpp
        ${SOURCE_DIR}/oasis_transfer_utils.c
    )
    target_link_libraries(test_oasis_transfer_utils PRIVATE liboasis GTest::gtest GTest::gtest_main)

    set(OASIS_DISK_UTIL_EXEC_PATH "$<TARGET_FILE:oasis_disk_util>")
    add_executable(test_oasis_disk_util_integration # Renamed from test_oasis_disk_util to be more specific
        ${TEST_DIR}/test_oasis_disk_util_integration.cpp
        ${TEST_DIR}/test_oasis_common.cpp
        ${OASIS_COMMON_SOURCES} # For any common utilities it might use indirectly
    )

    target_compile_definitions(test_oasis_disk_util_integration PRIVATE
        "OASIS_DISK_IMAGE_PATH_FROM_CMAKE=\"${OASIS_DISK_IMAGE_FOR_TESTS}\""
        "OASIS_DISK_UTIL_EXECUTABLE_PATH_FROM_CMAKE=\"${OASIS_DISK_UTIL_EXEC_PATH}\""
    )
    target_link_libraries(test_oasis_disk_util_integration PRIVATE liboasis GTest::gtest GTest::gtest_main ${PLATFORM_LIBS})


    target_include_directories(liboasis_tests PRIVATE
        ${SOURCE_DIR}
        ${TEST_DIR}
    )
    target_include_directories(test_oasis_transfer_utils PRIVATE ${SOURCE_DIR} ${TEST_DIR})
    target_include_directories(test_oasis_disk_util_integration PRIVATE ${SOURCE_DIR} ${TEST_DIR})


    include(GoogleTest)
    gtest_discover_tests(liboasis_tests)
    gtest_discover_tests(test_oasis_transfer_utils)
    gtest_discover_tests(test_oasis_disk_util_integration)

    # Path to the Python integration test script
    set(PYTHON_SEND_RECV_TEST_SCRIPT ${TEST_DIR}/test_oasis_send_recv_integration.py) # Assume this is the name of your python script

    # Common arguments for the Python script
    set(PYTHON_SCRIPT_COMMON_ARGS
        $<TARGET_FILE:oasis_disk_util>
        $<TARGET_FILE:oasis_send>
        $<TARGET_FILE:oasis_recv>
        ${OASIS_DISK_IMAGE_FOR_TESTS}
        ${TEST_DIR}/compare_md5.py
    )

    # Add the send/recv integration test for POSIX systems (Linux, macOS)
    if(UNIX)
        find_program(SOCAT_EXECUTABLE socat) # socat is only strictly needed if COM ports are NOT specified
        if(Python3_FOUND AND SOCAT_EXECUTABLE) # Check for socat as it's the fallback for POSIX
            if(EXISTS ${PYTHON_SEND_RECV_TEST_SCRIPT})
                add_test(
                    NAME OasisSendRecvIntegrationTest_Posix
                    COMMAND ${PYTHON_EXECUTABLE} ${PYTHON_SEND_RECV_TEST_SCRIPT}
                            ${PYTHON_SCRIPT_COMMON_ARGS}
                    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                )
                set_tests_properties(OasisSendRecvIntegrationTest_Posix PROPERTIES TIMEOUT 300)
                message(STATUS "Configured OasisSendRecvIntegrationTest_Posix (uses socat if no COM ports).")
            else()
                message(WARNING "Python Send/Recv integration test script not found at ${PYTHON_SEND_RECV_TEST_SCRIPT}. Skipping Posix test.")
            endif()
        else()
            message(WARNING "Python3 or socat not found. Skipping OasisSendRecvIntegrationTest_Posix.")
            if(NOT Python3_FOUND)
                message(WARNING "  Reason: Python3 interpreter not found.")
            endif()
            if(NOT SOCAT_EXECUTABLE)
                message(WARNING "  Reason: socat executable not found (needed for default POSIX test mode).")
            endif()
        endif()
    endif()

    if(DEFINED ENV{GITHUB_ACTIONS})
        message(STATUS "Skipping send/recv integration test for GitHub Actions")
    else()
        # Add the send/recv integration test for Windows
        if(WIN32)
            if(Python3_FOUND)
                if(EXISTS ${PYTHON_SEND_RECV_TEST_SCRIPT})
                    add_test(
                        NAME OasisSendRecvIntegrationTest_Windows
                        COMMAND ${PYTHON_EXECUTABLE} ${PYTHON_SEND_RECV_TEST_SCRIPT}
                                ${PYTHON_SCRIPT_COMMON_ARGS}
                                --com-port-recv "\\\\.\\COM92"
                                --com-port-send "\\\\.\\COM93"
                        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                    )
                    set_tests_properties(OasisSendRecvIntegrationTest_Windows PROPERTIES TIMEOUT 300)
                    message(STATUS "Configured OasisSendRecvIntegrationTest_Windows (uses COM92/COM93).")
                else()
                    message(WARNING "Python Send/Recv integration test script not found at ${PYTHON_SEND_RECV_TEST_SCRIPT}. Skipping Windows test.")
                endif()
            else()
                message(WARNING "Python3 not found. Skipping OasisSendRecvIntegrationTest_Windows.")
            endif()
        endif()
    endif()

# --- Compiler Flags ---
if(MSVC)
    # Set runtime libraries and warning levels for C++ targets
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MDd /W4 /EHsc")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MD /W4 /EHsc /O2 /GL")
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} /MD /W4 /EHsc /O2 /Zi")
    set(CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL} /MD /W4 /EHsc /Os")

    # Set runtime libraries and warning levels for C targets (e.g., liboasis)
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /MDd /W4")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /MD /W4 /O2 /GL")
    set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} /MD /W4 /O2 /Zi")
    set(CMAKE_C_FLAGS_MINSIZEREL "${CMAKE_C_FLAGS_MINSIZEREL} /MD /W4 /Os")

    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    # Flags for GCC/Clang (apply to both C and CXX)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
    # For C++17 with GCC/Clang, you might need -pthread for GTest
    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
    endif()
endif()

# --- Installation ---
install(FILES README.md LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/wireshark/README_wireshark.pdf DESTINATION .)
install(TARGETS oasis_disk_util oasis_send oasis_recv oasis_chkdsk RUNTIME DESTINATION bin)
install(TARGETS oasis_initdisk RUNTIME DESTINATION bin)
install(TARGETS liboasis ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)

install(DIRECTORY ${SOURCE_DIR}/ DESTINATION include/oasis_utils FILES_MATCHING PATTERN "*.h")
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/wireshark/oasis_wireshark_dissector.lua DESTINATION share/oasis_utils/wireshark)

# Install man pages (for Linux/macOS)
if(UNIX)
    # Ensure CMAKE_INSTALL_MANDIR is set, or default it if GNUInstallDirs is not used.
    # Often defaults to "share/man" or "man" under CMAKE_INSTALL_PREFIX.
    # The common location for man1 pages is ${CMAKE_INSTALL_MANDIR}/man1.
    if(NOT CMAKE_INSTALL_MANDIR)
        set(CMAKE_INSTALL_MANDIR "share/man" CACHE PATH "Default directory for man pages")
    endif()

    install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/man/man1/oasis_disk_util.1
        ${CMAKE_CURRENT_SOURCE_DIR}/man/man1/oasis_chkdsk.1
        ${CMAKE_CURRENT_SOURCE_DIR}/man/man1/oasis_initdisk.1
        ${CMAKE_CURRENT_SOURCE_DIR}/man/man1/oasis_send.1
        ${CMAKE_CURRENT_SOURCE_DIR}/man/man1/oasis_recv.1
        ${CMAKE_CURRENT_SOURCE_DIR}/man/man1/oasis_check_imd.py.1
        DESTINATION man/man1
    )
endif()

# --- CPack configuration ---

# Set basic package information
set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
set(CPACK_PACKAGE_VENDOR "Howard M. Harte")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "OASIS Disk Utilities")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

if(WIN32)
  set(CPACK_GENERATOR "ZIP")
else()
  set(CPACK_GENERATOR "TGZ")
endif()

include(CPack)

# --- Output ---
message(STATUS "Configuring OASIS Utilities project...")
message(STATUS "  Source directory: ${SOURCE_DIR}")
message(STATUS "  Test directory: ${TEST_DIR}")
message(STATUS "  CMAKE_INSTALL_MANDIR: ${CMAKE_INSTALL_MANDIR}")
message(STATUS "  Building library: liboasis")
message(STATUS "  Building executables: oasis_disk_util, oasis_chkdsk, oasis_send, oasis_recv")
message(STATUS "  Building tests: liboasis_tests, test_oasis_transfer_utils, test_oasis_disk_util_integration")
message(STATUS "    Test Data Image: ${OASIS_DISK_IMAGE_FOR_TESTS}")
