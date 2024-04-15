cmake_minimum_required(VERSION 3.18)

project(libav-node)

set(CMAKE_CXX_STANDARD 20)

if (WIN32)
    set(LIBAV_ROOT $ENV{LIBAV_ROOT})
    set(LIBAV_INCLUDE_DIRS ${LIBAV_ROOT}/include)
    set(LIBAV_LIBRARY_DIRS ${LIBAV_ROOT}/lib)
endif()

add_executable(libav-node WIN32
    ${PROJECT_SOURCE_DIR}/include/libav_service.h
    ${PROJECT_SOURCE_DIR}/src/ipc-pipe.cc
    ${PROJECT_SOURCE_DIR}/src/av-enc.cc
    ${PROJECT_SOURCE_DIR}/src/av-dec.cc
    ${PROJECT_SOURCE_DIR}/src/main.cc
)

# Library Paths
target_link_directories (libav-node PRIVATE
    ${LIBAV_LIBRARY_DIRS}
)

# Libraries to compile
if (WIN32)

else()
set(ADDITIONAL_LIBS pthread rt)
endif()

target_link_libraries (libav-node PRIVATE
    avcodec
    avutil
    avformat
    ${ADDITIONAL_LIBS}
)

# Include Paths
target_include_directories (libav-node PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/external/CLI11/include
    ${PROJECT_SOURCE_DIR}/external/plog/include
    ${LIBAV_INCLUDE_DIRS}
)
