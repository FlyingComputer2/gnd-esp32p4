cmake_minimum_required(VERSION 3.22)
project(ground_video_test LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
include(FetchContent)
FetchContent_Declare(vstreamer GIT_REPOSITORY https://github.com/FlyingComputer2/vstreamer.git
    GIT_TAG 21c3c8dde50f6249f8c93f30e4120a141dbe09d7)
FetchContent_Declare(bfc GIT_REPOSITORY https://github.com/therooftopprinz/BFC.git
    GIT_TAG 8dc02add56771bf05f60519b3915c8fc9464d24b)
foreach(dep vstreamer bfc)
    FetchContent_GetProperties(${dep})
    if(NOT ${dep}_POPULATED)
        FetchContent_Populate(${dep})
    endif()
endforeach()
set(ENABLE_STREAM_RECEIVER ON)
include(${vstreamer_SOURCE_DIR}/cmake/vstreamer_sources.cmake)
list(TRANSFORM VSTREAMER_SOURCES PREPEND ${vstreamer_SOURCE_DIR}/)
configure_file(${vstreamer_SOURCE_DIR}/src/components/components_config.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/components/components_config.hpp)
file(READ ${vstreamer_SOURCE_DIR}/src/components/stream_receiver.hpp rx_header)
string(REPLACE "k_queue_depth = 4096;" "k_queue_depth = 64;" rx_header "${rx_header}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/components/stream_receiver.hpp "${rx_header}")
# The receiver has no FEC (winject-l3 delivers the stream in sequence).
add_library(video_core ${VSTREAMER_SOURCES})
target_include_directories(video_core PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${vstreamer_SOURCE_DIR}/src ${bfc_SOURCE_DIR}/src ${CMAKE_CURRENT_LIST_DIR}/../../bfc_config)
add_executable(video_test video_test.cpp)
target_include_directories(video_test PRIVATE ../../components/ground_video/include)
target_link_libraries(video_test PRIVATE video_core pthread)
enable_testing()
add_test(NAME video_test COMMAND video_test)
add_test(NAME video_queue_test COMMAND video_test queue)

# Exercise the real Config::validate path used by firmware startup, including
# the shared validate_upstreams pass, with the l3 the firmware pins (CI points
# FETCHCONTENT_SOURCE_DIR_WINJECT_L3 / _WINJECT_FEC at the firmware build's
# copies). l3's library brings winject-fec.
FetchContent_Declare(winject_l3 GIT_REPOSITORY https://github.com/winject-wireless/winject-l3.git
    GIT_TAG caa764b47effedaeb81008c31089fcfc47f4b5ee)
FetchContent_GetProperties(winject_l3)
if(NOT winject_l3_POPULATED)
    FetchContent_Populate(winject_l3)
endif()
set(BFC_PIN 8dc02add56771bf05f60519b3915c8fc9464d24b)
set(BFC_CONFIG_DIR ${CMAKE_CURRENT_LIST_DIR}/../../bfc_config)
add_subdirectory(${winject_l3_SOURCE_DIR}/src/manager winject_l3 EXCLUDE_FROM_ALL)
add_executable(config_test config_test.cpp)
target_link_libraries(config_test PRIVATE winject-l3)
add_test(NAME config_test COMMAND config_test)

add_executable(owned_ingress_test owned_ingress_test.cpp)
target_link_libraries(owned_ingress_test PRIVATE video_core)
add_test(NAME owned_ingress_test COMMAND owned_ingress_test)

