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

    // Register custom REST API endpoints
    RestApi::registerCustomRoute("/api/v1/power/profile", HTTP_POST, [](WebServer& server) {
        String body = server.arg("plain");
        String response;
        if (centralController.handleCommand("set_profile", body, response)) {
            server.send(200, "application/json", response);
        } else {
            server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Failed to set profile\"}");
        }
    });

    RestApi::registerCustomRoute("/api/v1/permit/request", HTTP_POST, [](WebServer& server) {
        String body = server.arg("plain");
        String response;
        if (centralController.handleCommand("request_permit", body, response)) {
            server.send(200, "application/json", response);
        } else {
            server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Failed to request permit\"}");
        }
    });

    RestApi::registerCustomRoute("/api/v1/permit/release", HTTP_POST, [](WebServer& server) {
        String body = server.arg("plain");
        String response;
        if (centralController.handleCommand("release_permit", body, response)) {
            server.send(200, "application/json", response);
        } else {
            server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Failed to release permit\"}");
        }
    });

    RestApi::registerCustomRoute("/api/v1/device/register", HTTP_POST, [](WebServer& server) {
        String body = server.arg("plain");
        String response;
        if (centralController.handleCommand("register_device", body, response)) {
            server.send(200, "application/json", response);
        } else {
            server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Failed to register device\"}");
        }
    });

    RestApi::registerCustomRoute("/api/v1/device/probe", HTTP_POST, [](WebServer& server) {
        String body = server.arg("plain");
        String response;
        if (centralController.handleCommand("probe_device", body, response)) {
            server.send(200, "application/json", response);
        } else {
            server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Failed to start probing\"}");
        }
    });

    RestApi::registerCustomRoute("/api/v1/pairing/open", HTTP_POST, [](WebServer& server) {
        centralController.openPairingWindow(60);
        server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Pairing window opened for 60s\"}");
    });

    RestApi::registerCustomRoute("/api/v1/power/live", HTTP_GET, [](WebServer& server) {
        server.send(200, "application/json", centralController.getStatusJson());
    });

    // Launch CraftThingyCore with CentralController attached
    CraftThingyCore.begin(&centralController);
}

void loop() {
    CraftThingyCore.loop();
}
