# CMakeList.txt : CMake project for nsdos, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.8)

project ("oasis")

# Add source to this project's executable.
add_executable (oasis "oasis.c"  "oasis.h" "oasis_sendrecv.h" "oasis_utils.c" "oasis_utils.h")
if (NOT APPLE)
    target_link_libraries(oasis -static)
else ()
    target_link_libraries(oasis)
endif (NOT APPLE)

set(CMAKE_FIND_LIBRARY_SUFFIXES ".lib")
if (MSVC)
    target_compile_options(oasis PRIVATE /MT)
    target_link_options(oasis PRIVATE /INCREMENTAL:NO /NODEFAULTLIB:MSVCRT)
endif (MSVC)

if (MSVC)
add_executable (oasis_recv "oasis_recv.c"  "oasis.h" "oasis_sendrecv.h" "oasis_utils.c" "oasis_utils.h")
target_link_libraries(oasis_recv -static)

set(CMAKE_FIND_LIBRARY_SUFFIXES ".lib")
if (MSVC)
    target_compile_options(oasis_recv PRIVATE /MT)
    target_link_options(oasis_recv PRIVATE /INCREMENTAL:NO /NODEFAULTLIB:MSVCRT)
endif (MSVC)

add_executable (oasis_send "oasis_send.c"  "oasis.h" "oasis_sendrecv.h" "oasis_utils.c" "oasis_utils.h")
target_link_libraries(oasis_send -static)

set(CMAKE_FIND_LIBRARY_SUFFIXES ".lib")
if (MSVC)
    target_compile_options(oasis_send PRIVATE /MT)
    target_link_options(oasis_send PRIVATE /INCREMENTAL:NO /NODEFAULTLIB:MSVCRT)
endif (MSVC)
endif (MSVC)
