cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# mmipal registers both lwIP callbacks on the HaLow network interface. The
# prebuilt archive and ESP-IDF's lwIP component must agree on these options.
idf_build_set_property(COMPILE_DEFINITIONS "LWIP_NETIF_LINK_CALLBACK=1" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "LWIP_NETIF_STATUS_CALLBACK=1" APPEND)

project(iot_provisioning)
