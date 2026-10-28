# Ground P4: C6 SDIO radio proxy, local winject-l3, and USB application channels.
# Video recovery runs on P4; recovered RTP is shared by Ethernet and USB.
cmake_minimum_required(VERSION 3.22)
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "0.1.0")
endif()

include(FetchContent)
FetchContent_Declare(winject_l3
    GIT_REPOSITORY https://github.com/winject-wireless/winject-l3.git
    GIT_TAG ffc3f403f00d2fb82969ec91b4faf9d6ba283faa
)
FetchContent_GetProperties(winject_l3)
if(NOT winject_l3_POPULATED)
    FetchContent_Populate(winject_l3)
endif()
# Declare the shared BFC pin before the proxy declares it.
include(${winject_l3_SOURCE_DIR}/src/BfcFetch.cmake)

FetchContent_Declare(winject_sdio_proxy
    GIT_REPOSITORY https://github.com/winject-wireless/winject-radio-esp32-sdio-proxy.git
    GIT_TAG 0dc783a1b0fd1aadd9c88620af47dde5e8150630
)
FetchContent_GetProperties(winject_sdio_proxy)
if(NOT winject_sdio_proxy_POPULATED)
    FetchContent_Populate(winject_sdio_proxy)
endif()
include(${winject_sdio_proxy_SOURCE_DIR}/cmake/winject_sdio_proxy.cmake)

FetchContent_Declare(vstreamer
    GIT_REPOSITORY https://github.com/FlyingComputer2/vstreamer.git
    GIT_TAG 187c140d41e0f4ef7e8d59e1f585d82bf9d072fa)
FetchContent_GetProperties(vstreamer)
if(NOT vstreamer_POPULATED)
    FetchContent_Populate(vstreamer)
endif()
# Receive-only integration: no camera, hardware encoder, SDL or TX pipeline.
FetchContent_Declare(isa-l GIT_REPOSITORY https://github.com/intel/isa-l.git
    GIT_TAG v2.31.1 GIT_SHALLOW TRUE)
FetchContent_GetProperties(isa-l)
if(NOT isa-l_POPULATED)
    FetchContent_Populate(isa-l)
endif()
set(VSTREAMER_ISAL_ROOT ${isa-l_SOURCE_DIR})

set(EXTRA_COMPONENT_DIRS ${WINJECT_SDIO_PROXY_COMPONENT_DIRS}
                         ${winject_l3_SOURCE_DIR}/idf/winject_l3
                         ${winject_l3_SOURCE_DIR}/idf/app_bwtest)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gnd-esp32p4)
