#include <Arduino.h>
#include <CraftThingyCore.h>
#include "CentralController.h"

static CentralController centralController;

void setup() {
    Serial.begin(115200);
    delay(300);

    Serial.println();
    Serial.println(F("=========================================================="));
    Serial.println(F(" CraftThingy - Central Control & 1300W Energy Arbiter    "));
    Serial.println(F(" Subsystems: PZEM-004T (3.3V) | INA226 | 8GB SD | NVS   "));
    Serial.println(F("=========================================================="));

    // Initialize core persistent configuration
    ConfigStore::init();
    ConfigStore::setActiveRole("central_control");
    ConfigStore::setGitHubConfig(
        "CraftThingy-Digital-Innovation",
        "craftthingy-esp-core-ota",
        "craftthingy-esp-central-control",
        ""
    );

    // Launch CraftThingyCore with CentralController attached
    // All commands are dispatched via /api/v1/device/command and status via /api/v1/device/status
    CraftThingyCore.begin(&centralController);
}

void loop() {
    CraftThingyCore.loop();
}
