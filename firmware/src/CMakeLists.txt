idf_component_register(
  SRCS "main.cpp" "halow_network.cpp"
       "../nrf54/src/usb_control.pb.c"
       "../nrf54/src/nanopb/pb_common.c"
       "../nrf54/src/nanopb/pb_decode.c"
  INCLUDE_DIRS "." "../nrf54/src" "../nrf54/src/nanopb"
  REQUIRES cjson driver mqtt morse_halow network_provisioning nvs_flash esp_event esp_netif esp_system mbedtls esp_adc esp_https_ota
)

# The packaged Morse headers contain C initializers and enum conversions that
# the matching ESP-IDF 5.5 C++ compiler otherwise promotes to build errors.
target_compile_options(${COMPONENT_LIB} PRIVATE
  -Wno-error=missing-field-initializers
  -Wno-error=deprecated-enum-enum-conversion
)
