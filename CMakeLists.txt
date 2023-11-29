cmake_minimum_required(VERSION 3.28)

project(libav-node)

add_executable(libav-node
    ${PROJECT_SOURCE_DIR}/src/main.cc
)

# Libraries to compile
target_link_libraries (libav-node PRIVATE
    avcodec
    avutil
)

# Include Paths
target_include_directories (libav-node PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)
