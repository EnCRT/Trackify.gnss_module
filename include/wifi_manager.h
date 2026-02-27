#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <SD.h>

class WiFiManager {
public:
    WiFiManager();
    void begin();
    void stop();
    void handle();
    bool isRunning() const { return _running; }

private:
    AsyncWebServer _server;
    DNSServer _dnsServer;
    bool _running;

    void _setupRoutes();
    String _getHumanSize(uint32_t bytes);
    String _generateFileList();
};

#endif // WIFI_MANAGER_H
