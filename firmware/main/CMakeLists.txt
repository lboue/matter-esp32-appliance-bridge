idf_component_register(SRC_DIRS "." "homeconnect" "smartthings" "bridge" "devices"
                       INCLUDE_DIRS "."
                       REQUIRES esp_eth esp_netif esp_event esp_http_server esp_http_client esp-tls mbedtls
                                nvs_flash mdns driver json esp_timer espressif__esp_matter
                                espressif__esp_websocket_client)

# Build the React app as part of `idf.py build` and embed the result in the
# firmware binary, so that an OTA update carries the UI with it.
#
# This is a custom *command* rather than a custom target so that its outputs have
# a producing rule: target_add_binary_data() lists each file as MAIN_DEPENDENCY,
# and ninja needs to know how to make them on a clean tree. It also keeps the
# build incremental -- npm only runs when something under html_app/ has changed.
set(WEB_APP_DIR "${CMAKE_SOURCE_DIR}/html_compiled_app")
set(WEB_APP_FILES "${WEB_APP_DIR}/index.html" "${WEB_APP_DIR}/app.css" "${WEB_APP_DIR}/app.js")

file(GLOB_RECURSE WEB_APP_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/html_app/src/*"
     "${CMAKE_SOURCE_DIR}/html_app/public/*")

add_custom_command(
    OUTPUT ${WEB_APP_FILES}
    COMMAND npm run build
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/html_app"
    DEPENDS ${WEB_APP_SOURCES}
            "${CMAKE_SOURCE_DIR}/html_app/index.html"
            "${CMAKE_SOURCE_DIR}/html_app/package.json"
            "${CMAKE_SOURCE_DIR}/html_app/vite.config.ts"
    COMMENT "Building React app..."
    VERBATIM)

add_custom_target(react_build DEPENDS ${WEB_APP_FILES})

foreach(web_app_file ${WEB_APP_FILES})
    target_add_binary_data(${COMPONENT_LIB} "${web_app_file}" BINARY)
endforeach()

idf_component_get_property(matter_lib espressif__esp_matter COMPONENT_LIB)

# This project provides its own ESPEthernetDriver::Init in matter_ethernet_driver.cpp,
# so exclude esp-matter's stock implementation to avoid a duplicate symbol.
set_source_files_properties(
    "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_matter/connectedhomeip/connectedhomeip/src/platform/ESP32/NetworkCommissioningDriver_Ethernet.cpp"
    TARGET_DIRECTORY ${matter_lib}
    PROPERTIES HEADER_FILE_ONLY TRUE
)

# esp-matter's CMakeLists excludes ESP32DnssdImpl.cpp when Wi-Fi is disabled, but
# DnssdImpl.cpp (included when CONFIG_USE_MINIMAL_MDNS=n) depends on its symbols.
# Add it back so platform mDNS links on Ethernet-only builds.
target_sources(${matter_lib} PRIVATE
    "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_matter/connectedhomeip/connectedhomeip/src/platform/ESP32/ESP32DnssdImpl.cpp"
)

# Force the W5500 SPI-Ethernet driver members and their esp_netif defaults into
# the link. matter_ethernet_driver.cpp is the only referrer of these symbols, and
# the archive members that define them are otherwise not pulled from
# libesp_eth.a / libesp_netif.a, giving "undefined reference" link errors even
# though the symbols are present.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,-u,esp_eth_mac_new_w5500"
    "-Wl,-u,esp_eth_phy_new_w5500"
    "-Wl,-u,esp_eth_new_netif_glue"
    "-Wl,-u,_g_esp_netif_netstack_default_eth"
    "-Wl,-u,_g_esp_netif_inherent_eth_config"
)
