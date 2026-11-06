# mplane_relay: portable core plus a named-reactor ESP-IDF applet.
idf_component_register(
    SRCS mplane_relay.cpp
         src/applet.cpp
    INCLUDE_DIRS include
    REQUIRES app_kit
    PRIV_REQUIRES lwip log esp_timer
)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)
