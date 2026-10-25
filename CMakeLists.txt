# Ground-station radio on the Guition JC-ESP32P4-M3-DEV: the winject SDIO
# proxy only, exposing the module's ESP32-C6 as a winject radio on UDP 22 /
# 9000 over Ethernet. winject-l3 stays on the GS host (opi5).
# Local checkout: -DFETCHCONTENT_SOURCE_DIR_WINJECT_SDIO_PROXY=<dir>.
cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "0.1.0")
endif()

include(FetchContent)
FetchContent_Declare(winject_sdio_proxy
    GIT_REPOSITORY https://github.com/winject-wireless/winject-radio-esp32-sdio-proxy.git
    GIT_TAG ad4a1672424b726b4d9b8596180a3ed02e04b330
)
FetchContent_GetProperties(winject_sdio_proxy)
if(NOT winject_sdio_proxy_POPULATED)
    FetchContent_Populate(winject_sdio_proxy)
endif()
include(${winject_sdio_proxy_SOURCE_DIR}/cmake/winject_sdio_proxy.cmake)

set(EXTRA_COMPONENT_DIRS ${WINJECT_SDIO_PROXY_COMPONENT_DIRS})
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rover-ground-esp32p4)
