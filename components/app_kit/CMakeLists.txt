if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    include(FetchContent)
    FetchContent_GetProperties(bfc)
    if(NOT bfc_POPULATED)
        FetchContent_Populate(bfc)
    endif()
endif()

idf_component_register(
    SRCS app_kit.cpp
    INCLUDE_DIRS include compat
    REQUIRES pthread vfs
    PRIV_REQUIRES lwip log heap freertos
)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_include_directories(${COMPONENT_LIB} PUBLIC ${bfc_SOURCE_DIR}/src)
endif()
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17 -fexceptions>)
