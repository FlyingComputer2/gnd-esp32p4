# Ground P4: C6 SDIO radio proxy, local winject-l3, and USB application channels.
# Video recovery runs on P4; recovered RTP is shared by Ethernet and USB.
cmake_minimum_required(VERSION 3.22)
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "0.1.0")
endif()

include(FetchContent)
# One bfc for the whole firmware: the dependencies share its queue and reactor
# registries. BFC_PIN is checked against each dependency's own pin
# (BfcFetch.cmake), and BFC_CONFIG_DIR holds this firmware's bfc_config.hpp
# (function type, allocator, poll reactor), which every bfc user compiles with.
set(BFC_PIN 3d68f8e61ef933718a51f248c76be8894d0cb491)
set(BFC_CONFIG_DIR ${CMAKE_CURRENT_LIST_DIR}/bfc_config)
FetchContent_Declare(bfc
    GIT_REPOSITORY https://github.com/therooftopprinz/BFC.git
    GIT_TAG ${BFC_PIN}
)
FetchContent_Declare(winject_l3
    GIT_REPOSITORY https://github.com/winject-wireless/winject-l3.git
    GIT_TAG a047ed8059ca7354e8c89fea04ba875d801204f9
)
FetchContent_GetProperties(winject_l3)
if(NOT winject_l3_POPULATED)
    FetchContent_Populate(winject_l3)
endif()
include(${winject_l3_SOURCE_DIR}/src/BfcFetch.cmake)

FetchContent_Declare(winject_sdio_proxy
    GIT_REPOSITORY https://github.com/winject-wireless/winject-radio-esp32-sdio-proxy.git
    GIT_TAG 83ab05890a9564698ae136490f2abc2c6167234a
)
FetchContent_GetProperties(winject_sdio_proxy)
if(NOT winject_sdio_proxy_POPULATED)
    FetchContent_Populate(winject_sdio_proxy)
endif()
include(${winject_sdio_proxy_SOURCE_DIR}/cmake/winject_sdio_proxy.cmake)

FetchContent_Declare(vstreamer
    GIT_REPOSITORY https://github.com/FlyingComputer2/vstreamer.git
    GIT_TAG 0253187fdaee9e5e5ab7a47a40b0f13aedf0bcb2)
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

# Clock sync (ground = SynchServer, the reference).
FetchContent_Declare(krono
    GIT_REPOSITORY https://github.com/FlyingComputer2/krono.git
    GIT_TAG c7dc5caaca89d93467ddb2dfb349a8f7ef30f005)
FetchContent_GetProperties(krono)
if(NOT krono_POPULATED)
    FetchContent_Populate(krono)
endif()

# The system applet (get sys cpu / heap / stack / ...; console 5130). It uses
# this firmware's bfc and pins none.
FetchContent_Declare(sys_applet
    GIT_REPOSITORY https://github.com/FlyingComputer2/sys-applet-esp32p4.git
    GIT_TAG e5ec5087e856d69979ec42e52c65bd820bd670fe)
FetchContent_GetProperties(sys_applet)
if(NOT sys_applet_POPULATED)
    FetchContent_Populate(sys_applet)
endif()

set(EXTRA_COMPONENT_DIRS ${WINJECT_SDIO_PROXY_COMPONENT_DIRS}
                         ${winject_l3_SOURCE_DIR}/idf/winject_l3
                         ${winject_l3_SOURCE_DIR}/idf/app_bwtest
                         ${krono_SOURCE_DIR}/idf/krono
                         ${sys_applet_SOURCE_DIR}/idf/sys_applet)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gnd-esp32p4)
