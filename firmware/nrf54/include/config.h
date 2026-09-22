#ifndef CONFIG_H
#define CONFIG_H

// Device configuration
#define DEVICE_VERSION "1.0.0"

// Platform detection
#if defined(ESP32_S3)
    #define PLATFORM_ESP32_S3
    #define HAS_WIFI
    #define HAS_BLUETOOTH
    #define HAS_USB_OTG
#elif defined(ESP32_C6)
    #define PLATFORM_ESP32_C6
    #define HAS_WIFI
    #define HAS_BLUETOOTH
    #define HAS_ZIGBEE
    #define HAS_THREAD
#elif defined(NRF52840)
    #define PLATFORM_NRF52840
    #define HAS_BLUETOOTH
    #define HAS_USB
#elif defined(NRF54H20)
    #define PLATFORM_NRF54H20
    #define HAS_BLUETOOTH
    #define HAS_WIFI
    #define HAS_MULTICORE
#endif

// Communication settings
#define SERIAL_BAUD_RATE 115200

// WiFi settings (ESP32-S3, ESP32-C6, nRF54H20)
#if defined(PLATFORM_ESP32_S3) || defined(PLATFORM_ESP32_C6) || defined(PLATFORM_NRF54H20)
    // Define these in secrets.h or as build flags
    #ifndef WIFI_SSID
        #define WIFI_SSID "your-ssid"
    #endif
    #ifndef WIFI_PASSWORD
        #define WIFI_PASSWORD "your-password"
    #endif
#endif

// BLE settings (ESP32 and nRF)
#if defined(HAS_BLUETOOTH)
    #define BLE_DEVICE_NAME "EdgeDevice"
#endif

// Sensor update intervals
#define SENSOR_UPDATE_INTERVAL 5000  // ms

#endif // CONFIG_H
