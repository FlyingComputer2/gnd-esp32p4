idf_component_register(SRCS "usb_upstream.cpp" INCLUDE_DIRS "include"
                       REQUIRES winject_l3
                       PRIV_REQUIRES esp_tinyusb esp_hw_support json)
target_include_directories(${COMPONENT_LIB} PUBLIC ${bfc_SOURCE_DIR}/src)
target_compile_options(${COMPONENT_LIB} PRIVATE -fexceptions)
