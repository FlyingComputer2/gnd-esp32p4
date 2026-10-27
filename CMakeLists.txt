# Ground P4: C6 SDIO radio proxy, local winject-l3, and USB application channels.
# Video recovery runs on P4; recovered RTP is shared by Ethernet and USB.
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
# A cached dependency may contain an older version of this compatibility patch.
execute_process(COMMAND git apply --reverse --check ${CMAKE_CURRENT_LIST_DIR}/patches/winject-l3-upstream-queue.patch
    WORKING_DIRECTORY ${winject_l3_SOURCE_DIR} RESULT_VARIABLE queue_patch_check
    OUTPUT_QUIET ERROR_QUIET)
if(NOT queue_patch_check EQUAL 0)
    message(FATAL_ERROR "L3 compatibility patch is stale; remove ${winject_l3_SOURCE_DIR} and reconfigure")
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
# Keep the original radio ownership copy in PSRAM when ground video retains it.
execute_process(COMMAND git apply --reverse --check ${CMAKE_CURRENT_LIST_DIR}/patches/proxy-psram-packets.patch
    WORKING_DIRECTORY ${winject_sdio_proxy_SOURCE_DIR} RESULT_VARIABLE psram_patch_check
    OUTPUT_QUIET ERROR_QUIET)
if(NOT psram_patch_check EQUAL 0)
    execute_process(COMMAND git apply ${CMAKE_CURRENT_LIST_DIR}/patches/proxy-psram-packets.patch
        WORKING_DIRECTORY ${winject_sdio_proxy_SOURCE_DIR} COMMAND_ERROR_IS_FATAL ANY)
endif()
include(${winject_sdio_proxy_SOURCE_DIR}/cmake/winject_sdio_proxy.cmake)

FetchContent_Declare(vstreamer
    GIT_REPOSITORY https://github.com/FlyingComputer2/vstreamer.git
    GIT_TAG 74db519187f716fb443b5fe546c2e947099e41c1)
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
                         ${winject_l3_SOURCE_DIR}/idf/winject_l3)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rover-ground-esp32p4)
