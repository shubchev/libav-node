cmake_minimum_required(VERSION 3.18)

project(libav-node)

set(CMAKE_CXX_STANDARD 20)



add_library(libav-node-lib
    ${PROJECT_SOURCE_DIR}/include/libav_service.h
    ${PROJECT_SOURCE_DIR}/src/ipc-pipe.h
    ${PROJECT_SOURCE_DIR}/src/ipc-pipe.cc
    ${PROJECT_SOURCE_DIR}/src/av-enc.cc
    ${PROJECT_SOURCE_DIR}/src/av-dec.cc
    ${PROJECT_SOURCE_DIR}/src/common.h
    ${PROJECT_SOURCE_DIR}/src/common.cc
    ${PROJECT_SOURCE_DIR}/src/svc.cc
)

target_compile_definitions(libav-node-lib PRIVATE
    __STDC_LIMIT_MACROS
)

# Include Paths
target_include_directories(libav-node-lib PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/external/CLI11/include
    ${PROJECT_SOURCE_DIR}/external/plog/include
    ${PROJECT_SOURCE_DIR}/prebuilt/ffmpeg/include
)

# Libraries to compile
set(ADDITIONAL_LIBS
    libavcodec
    libavdevice
    libavfilter
    libavformat
    libavutil
    libswresample
    libswscale
)

if (WIN32)
    set(ADDITIONAL_LIBS
        ${ADDITIONAL_LIBS}
        D3D12.lib
		Bcrypt.lib
		strmiids.lib
		DXGI.lib
		Secur32.lib
        Mfplat.lib
		Mfuuid.lib)
else()
    set(ADDITIONAL_LIBS
        ${ADDITIONAL_LIBS}
        pthread
        rt)
endif()










add_executable(libav-node WIN32
    ${PROJECT_SOURCE_DIR}/src/main.cc
)

add_dependencies(libav-node libav-node-lib)

# Include Paths
target_include_directories(libav-node PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/external/CLI11/include
    ${PROJECT_SOURCE_DIR}/external/plog/include
)

# Library Paths
target_link_directories(libav-node PRIVATE
    ${PROJECT_SOURCE_DIR}/prebuilt/ffmpeg/lib
)

# Libraries to compile
target_link_libraries(libav-node PRIVATE
    libav-node-lib
    ${ADDITIONAL_LIBS}
)

















add_executable(libav-node-test
    ${PROJECT_SOURCE_DIR}/src/test.cc
)

add_dependencies(libav-node-test libav-node-lib)

# Include Paths
target_include_directories(libav-node-test PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/external/CLI11/include
    ${PROJECT_SOURCE_DIR}/external/plog/include
)

# Library Paths
target_link_directories(libav-node-test PRIVATE
    ${PROJECT_SOURCE_DIR}/prebuilt/ffmpeg/lib
)

# Libraries to compile
target_link_libraries(libav-node-test PRIVATE
    libav-node-lib
    ${ADDITIONAL_LIBS}
)
