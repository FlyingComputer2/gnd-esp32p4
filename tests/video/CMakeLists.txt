cmake_minimum_required(VERSION 3.22)
project(ground_video_test LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
include(FetchContent)
FetchContent_Declare(vstreamer GIT_REPOSITORY https://github.com/FlyingComputer2/vstreamer.git
    GIT_TAG e4fd44853249b74113b76f4ff668d87c5b3b8deb)
FetchContent_Declare(bfc GIT_REPOSITORY https://github.com/therooftopprinz/BFC.git
    GIT_TAG 6c02a495d1a2cf4a8a13dfa6b25b342780527c89)
FetchContent_Declare(isa-l GIT_REPOSITORY https://github.com/intel/isa-l.git GIT_TAG v2.31.1 GIT_SHALLOW TRUE)
foreach(dep vstreamer bfc isa-l)
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
add_library(video_core ${VSTREAMER_SOURCES} ${isa-l_SOURCE_DIR}/erasure_code/ec_base.c)
target_include_directories(video_core PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${vstreamer_SOURCE_DIR}/src ${bfc_SOURCE_DIR}/src
    PRIVATE ${isa-l_SOURCE_DIR}/include ${isa-l_SOURCE_DIR}/erasure_code)
target_compile_definitions(video_core PRIVATE NDEBUG)
add_executable(video_test video_test.cpp)
target_include_directories(video_test PRIVATE ../../components/ground_video/include)
target_link_libraries(video_test PRIVATE video_core pthread)
enable_testing()
add_test(NAME video_test COMMAND video_test)
add_test(NAME video_queue_test COMMAND video_test queue)

# Exercise the real Config::validate path used by firmware startup, including
# the shared validate_upstreams pass (USB/FEC tests do not construct Config).
FetchContent_Declare(winject_l3 GIT_REPOSITORY https://github.com/winject-wireless/winject-l3.git
    GIT_TAG 8ff2517a0818da2d5e7eac11cd0d386b82042581)
FetchContent_GetProperties(winject_l3)
if(NOT winject_l3_POPULATED)
    FetchContent_Populate(winject_l3)
endif()
set(manager ${winject_l3_SOURCE_DIR}/src/manager)
add_executable(config_test config_test.cpp
    ${manager}/Config.cpp ${manager}/utils/NetUtil.cpp ${manager}/radio/PhyAirtime.cpp
    ${manager}/fec/vendor/rs_block_erasure.cpp
    ${manager}/fec/vendor/rs_block_erasure_rx.cpp
    ${manager}/fec/vendor/rs_encode_scalar.cpp)
target_include_directories(config_test PRIVATE ${manager} ${isa-l_SOURCE_DIR}/include)
target_link_libraries(config_test PRIVATE video_core)
add_test(NAME config_test COMMAND config_test)

target_compile_definitions(video_core PUBLIC VSTREAMER_RX_DEADLINE_MARGIN_MIN_MS=40)
add_executable(fec_schedule_test fec_schedule_test.cpp)
target_link_libraries(fec_schedule_test PRIVATE video_core)
add_test(NAME fec_schedule_test COMMAND fec_schedule_test)
add_executable(fec_decode_test fec_decode_test.cpp)
target_link_libraries(fec_decode_test PRIVATE video_core)
add_test(NAME fec_decode_test COMMAND fec_decode_test)

add_executable(owned_ingress_test owned_ingress_test.cpp)
target_link_libraries(owned_ingress_test PRIVATE video_core)
add_test(NAME owned_ingress_test COMMAND owned_ingress_test)

