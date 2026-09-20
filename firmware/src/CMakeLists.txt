idf_component_register(
  SRCS "main.cpp" "display.cpp"
  INCLUDE_DIRS "."
  REQUIRES cjson driver mqtt esp_wifi network_provisioning nvs_flash esp_event esp_netif esp_system mbedtls esp_driver_tsens u8g2
)
