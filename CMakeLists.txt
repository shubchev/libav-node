cmake_minimum_required(VERSION 3.18)

project(libav-node)

set(CMAKE_CXX_STANDARD 20)

set(boost_use_static_libs on)
find_package(Boost REQUIRED date_time)

if (WIN32)
    set(LIBAV_ROOT $ENV{LIBAV_ROOT})
    set(LIBAV_INCLUDE_DIRS ${LIBAV_ROOT}/include)
    set(LIBAV_LIBRARY_DIRS ${LIBAV_ROOT}/lib)
endif()

add_executable(libav-node
    ${PROJECT_SOURCE_DIR}/include/libav_service.h
    ${PROJECT_SOURCE_DIR}/src/main.cc
    ${PROJECT_SOURCE_DIR}/src/av-enc.cc
    ${PROJECT_SOURCE_DIR}/src/av-dec.cc
)

# Library Paths
target_link_directories (libav-node PRIVATE
    ${LIBAV_LIBRARY_DIRS}
    ${Boost_LIBRARY_DIRS}
)

# Libraries to compile
target_link_libraries (libav-node PRIVATE
    avcodec
    avutil
)

# Include Paths
target_include_directories (libav-node PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${LIBAV_INCLUDE_DIRS}
    ${Boost_INCLUDE_DIRS}
)
