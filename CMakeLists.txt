# Ground P4: C6 SDIO radio proxy, local winject-l3, and USB application channels.
# Video wire decoding/FEC remains in rover-ground-android.
cmake_minimum_required(VERSION 3.22)
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "0.1.0")
endif()

include(FetchContent)
FetchContent_Declare(winject_l3
    GIT_REPOSITORY https://github.com/winject-wireless/winject-l3.git
    GIT_TAG fb3eea11d3edc81ce39287a8fd5c77e3d43d41f1
)
FetchContent_GetProperties(winject_l3)
if(NOT winject_l3_POPULATED)
    FetchContent_Populate(winject_l3)
endif()
# The pinned release has named app queues; expose the L3-owned UpstreamQueue.
if(NOT EXISTS ${winject_l3_SOURCE_DIR}/src/manager/endpoint/UpstreamQueue.h)
    execute_process(COMMAND git apply ${CMAKE_CURRENT_LIST_DIR}/patches/winject-l3-upstream-queue.patch
                    WORKING_DIRECTORY ${winject_l3_SOURCE_DIR} COMMAND_ERROR_IS_FATAL ANY)
endif()
# Declare the shared BFC pin before the older proxy declares it.
include(${winject_l3_SOURCE_DIR}/src/BfcFetch.cmake)

FetchContent_Declare(winject_sdio_proxy
    GIT_REPOSITORY https://github.com/winject-wireless/winject-radio-esp32-sdio-proxy.git
    GIT_TAG ad4a1672424b726b4d9b8596180a3ed02e04b330
)
FetchContent_GetProperties(winject_sdio_proxy)
if(NOT winject_sdio_proxy_POPULATED)
    FetchContent_Populate(winject_sdio_proxy)
endif()
include(${winject_sdio_proxy_SOURCE_DIR}/cmake/winject_sdio_proxy.cmake)

set(EXTRA_COMPONENT_DIRS ${WINJECT_SDIO_PROXY_COMPONENT_DIRS}
                         ${winject_l3_SOURCE_DIR}/idf/winject_l3)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rover-ground-esp32p4)
