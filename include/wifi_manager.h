#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <memory>

// Undefine conflicting macros from FS.h
#ifdef FILE_READ
#undef FILE_READ
#endif
#ifdef FILE_WRITE
#undef FILE_WRITE
#endif

#include "SdFat.h"

extern SdFs sd;

class WiFiManager {
public:
    WiFiManager();
    void begin(uint8_t gpsFreqHz);
    void stop();
    void handle();
    bool isRunning() const { return _running; }

private:
    AsyncWebServer _server;
    DNSServer _dnsServer;
    bool _running;
    uint8_t _gpsFreq;

    void _setupRoutes();
    String _getHumanSize(uint32_t bytes);
    String _generateFileList();
};

#endif // WIFI_MANAGER_H
