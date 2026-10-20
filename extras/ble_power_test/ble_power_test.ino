#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>

// Set to 1 ONLY to test whether BLE works while you fix the power problem.
// Not a real fix: it hides the brownout protection.
#define DISABLE_BROWNOUT 0

#if DISABLE_BROWNOUT
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#endif

void setup() {
#if DISABLE_BROWNOUT
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
#endif

    Serial.begin(115200);
    delay(1500);
    Serial.println("1) boot OK");

    setCpuFrequencyMhz(80); // lower current draw
    Serial.println("2) starting BLE...");
    delay(300);

    BLEDevice::init("Quran Test");
    BLEDevice::setPower(ESP_PWR_LVL_N3);
    BLEServer *server = BLEDevice::createServer();
    BLEService *service = server->createService("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
    service->start();
    BLEDevice::getAdvertising()->addServiceUUID("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
    BLEDevice::startAdvertising();

    Serial.println("3) BLE ready: Quran Test");
}

void loop() {
    delay(2000);
    Serial.println("alive");
}
