idf_component_register(SRCS "main.cpp"
                       PRIV_REQUIRES winject_proxy winject_l3 usb_upstream ground_video esp_eth esp_netif
                                     esp_event nvs_flash esp_hw_support pthread)
target_compile_options(${COMPONENT_LIB} PRIVATE -fexceptions)
