#ifndef DEVICE_INFO_H
#define DEVICE_INFO_H

#include <Arduino.h>

// Device capabilities based on platform
class DeviceInfo {
public:
    static void printCapabilities() {
        Serial.println("\n=== Device Capabilities ===");
        
        #ifdef HAS_WIFI
            Serial.println("✓ WiFi Support");
            #if defined(PLATFORM_ESP32_C6) || defined(PLATFORM_NRF54H20)
                Serial.println("  - WiFi 6 (802.11ax)");
            #else
                Serial.println("  - WiFi 4 (802.11n)");
            #endif
        #endif
        
        #ifdef HAS_BLUETOOTH
            Serial.println("✓ Bluetooth Support");
            #if defined(PLATFORM_NRF54H20)
                Serial.println("  - Bluetooth 5.4");
            #elif defined(PLATFORM_NRF52840)
                Serial.println("  - Bluetooth 5.0 Long Range");
            #else
                Serial.println("  - Bluetooth 5.0 LE");
            #endif
        #endif
        
        #ifdef HAS_USB_OTG
            Serial.println("✓ USB OTG");
        #endif
        
        #ifdef HAS_USB
            Serial.println("✓ USB 2.0");
        #endif
        
        #ifdef HAS_ZIGBEE
            Serial.println("✓ Zigbee Support");
        #endif
        
        #ifdef HAS_THREAD
            Serial.println("✓ Thread Support");
        #endif
        
        #ifdef HAS_MULTICORE
            Serial.println("✓ Multi-core Processor");
        #endif
        
        Serial.println("===========================\n");
    }
    
    static const char* getPlatformName() {
        #if defined(PLATFORM_ESP32_S3)
            return "ESP32-S3";
        #elif defined(PLATFORM_ESP32_C6)
            return "ESP32-C6";
        #elif defined(PLATFORM_NRF52840)
            return "nRF52840";
        #elif defined(PLATFORM_NRF54H20)
            return "nRF54H20";
        #else
            return "Unknown";
        #endif
    }
    
    static bool hasWiFi() {
        #ifdef HAS_WIFI
            return true;
        #else
            return false;
        #endif
    }
    
    static bool hasBluetooth() {
        #ifdef HAS_BLUETOOTH
            return true;
        #else
            return false;
        #endif
    }
};

#endif // DEVICE_INFO_H
