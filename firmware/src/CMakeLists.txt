idf_component_register(
  SRCS "main.cpp" "display.cpp" "halow_network.cpp"
  INCLUDE_DIRS "."
  REQUIRES cjson driver mqtt morse_halow network_provisioning nvs_flash esp_event esp_netif esp_system mbedtls esp_driver_tsens u8g2
)

# The packaged Morse headers contain C initializers and enum conversions that
# the matching ESP-IDF 5.5 C++ compiler otherwise promotes to build errors.
target_compile_options(${COMPONENT_LIB} PRIVATE
  -Wno-error=missing-field-initializers
  -Wno-error=deprecated-enum-enum-conversion
)
