# CMakeList.txt : CMake project for oasis-utils
#
cmake_minimum_required (VERSION 3.8)

project ("oasis-utils")

execute_process(COMMAND git describe --dirty --always --tags
                OUTPUT_VARIABLE GIT_REV
                ERROR_QUIET)

if ("${GIT_REV}" STREQUAL "")
    set(GIT_REV "Unknown")
endif()

string(STRIP "${GIT_REV}" GIT_REV)
add_definitions(-DVERSION=\"${GIT_REV}\")

if(MSVC)
ADD_LIBRARY(mm_serial STATIC "mm_serial_win32.c" "mm_serial.h") # "third-party/getopt.c" "third-party/getopt.h")
else()
ADD_LIBRARY(mm_serial STATIC "mm_serial_posix.c" "mm_serial.h")
endif()

ADD_LIBRARY(oasis_sendrecv STATIC "oasis_sendrecv.c" "oasis_sendrecv.h")

# Add source to this project's executable.
add_executable (oasis "oasis.c"  "oasis.h" "oasis_utils.c" "oasis_utils.h")
add_executable (oasis_recv "oasis_recv.c"  "oasis.h" "oasis_sendrecv.c" "oasis_sendrecv.h" "oasis_utils.c" "oasis_utils.h")
target_link_libraries(oasis_recv oasis_sendrecv mm_serial)
add_executable (oasis_send "oasis_send.c"  "oasis.h" "oasis_sendrecv.c" "oasis_sendrecv.h" "oasis_utils.c" "oasis_utils.h")
if(MSVC)
SET_TARGET_PROPERTIES(oasis_send PROPERTIES LINK_FLAGS "setargv.obj")
endif()
target_link_libraries(oasis_send oasis_sendrecv mm_serial)
