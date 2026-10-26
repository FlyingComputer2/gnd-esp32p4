if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register(REQUIRES usb_upstream lwip pthread PRIV_REQUIRES esp_timer)
    return()
endif()
# Compile the pinned stock receiver with a small P4 pool/egress queue.
set(rx_root ${vstreamer_SOURCE_DIR})
set(ENABLE_STREAM_RECEIVER ON)
set(VSTREAMER_SOURCES)
include(${rx_root}/cmake/vstreamer_sources.cmake)
list(TRANSFORM VSTREAMER_SOURCES PREPEND ${rx_root}/)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    configure_file(${rx_root}/src/components/components_config.hpp.in
                   ${CMAKE_CURRENT_BINARY_DIR}/generated/components/components_config.hpp)
    file(READ ${rx_root}/src/components/stream_receiver.hpp rx_header)
    if(NOT rx_header MATCHES "k_queue_depth = 4096;")
        message(FATAL_ERROR "Review ground receiver queue bound for new vstreamer pin")
    endif()
    string(REPLACE "k_queue_depth = 4096;" "k_queue_depth = 64;" rx_header "${rx_header}")
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/components/stream_receiver.hpp "${rx_header}")
endif()
idf_component_register(SRCS "ground_video.cpp" ${VSTREAMER_SOURCES}
    ${VSTREAMER_ISAL_ROOT}/erasure_code/ec_base.c
    INCLUDE_DIRS "include" ${CMAKE_CURRENT_BINARY_DIR}/generated ${rx_root}/src
                 ${bfc_SOURCE_DIR}/src
    PRIV_INCLUDE_DIRS ${rx_root}/idf/vstreamer/compat ${VSTREAMER_ISAL_ROOT}/include ${VSTREAMER_ISAL_ROOT}/erasure_code
    REQUIRES usb_upstream lwip pthread
    PRIV_REQUIRES esp_timer)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fexceptions;-std=gnu++17>)
set_source_files_properties(${VSTREAMER_ISAL_ROOT}/erasure_code/ec_base.c
    PROPERTIES COMPILE_DEFINITIONS NDEBUG)
