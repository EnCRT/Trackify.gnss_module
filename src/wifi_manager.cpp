#include "wifi_manager.h"
#include "logs_ui.h"

static const byte DNS_PORT = 53;
static const IPAddress local_IP(192, 168, 0, 11);
static const IPAddress gateway(192, 168, 0, 1);
static const IPAddress subnet(255, 255, 255, 0);

WiFiManager::WiFiManager() : _server(80), _running(false) {}

void WiFiManager::begin() {
    if (_running) return;

    Serial.println("Starting WiFi AP: Trackify...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Trackify", "12345678", 1, 0, 4);
    WiFi.setSleep(false);
    WiFi.softAPConfig(local_IP, gateway, subnet);

    _dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    _dnsServer.start(DNS_PORT, "*", local_IP);

    _setupRoutes();
    _server.begin();
    _running = true;
    Serial.println("Async Web Server Started.");
}

void WiFiManager::stop() {
    if (!_running) return;

    _dnsServer.stop();
    _server.end();
    WiFi.softAPdisconnect(true);
    _running = false;
    Serial.println("WiFi AP and Web Server Stopped.");
}

void WiFiManager::handle() {
    if (_running) {
        _dnsServer.processNextRequest();
    }
}

void WiFiManager::_setupRoutes() {
    _server.on("/", HTTP_GET, [this](AsyncWebServerRequest *request) {
        Serial.println("Web request: index");
        String html = FPSTR(LOGS_HTML);
        html.replace("%FILE_LIST%", _generateFileList());
        request->send(200, "text/html", html);
    });

    _server.on("/download", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (request->hasParam("file")) {
            String fileName = request->getParam("file")->value();
            if (!fileName.startsWith("/")) fileName = "/" + fileName;

            Serial.print("Download request for: "); Serial.println(fileName);

            if (SD.exists(fileName)) {
                request->send(SD, fileName, "application/octet-stream");
            } else {
                request->send(404, "text/plain", "File not found");
            }
        } else {
            request->send(400, "text/plain", "File param missing");
        }
    });

    _server.on("/delete", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (request->hasParam("file")) {
            String fileName = request->getParam("file")->value();
            if (!fileName.startsWith("/")) fileName = "/" + fileName;

            Serial.print("Delete request for: "); Serial.println(fileName);

            if (SD.exists(fileName)) {
                if (SD.remove(fileName)) {
                    request->send(200, "text/plain", "OK");
                } else {
                    request->send(500, "text/plain", "Delete Failed");
                }
            } else {
                request->send(404, "text/plain", "File not found");
            }
        } else {
            request->send(400, "text/plain", "File param missing");
        }
    });

    _server.onNotFound([this](AsyncWebServerRequest *request) {
        String url = request->url();
        if (url.endsWith(".ico") || url.endsWith(".map")) {
            request->send(404, "text/plain", "Not Found");
            return;
        }
        
        Serial.print("Not Found, redirecting: "); Serial.println(url);
        request->redirect("http://192.168.0.11/");
    });
}

String WiFiManager::_generateFileList() {
    String fileList = "";
    fileList.reserve(2048);

    File root = SD.open("/");
    if (!root) return "SD Error";

    File file = root.openNextFile();
    int count = 1;
    while (file) {
        String name = String(file.name());
        if (!file.isDirectory() && name.endsWith(".txt")) {
            String displayName = name;
            if (displayName.startsWith("/")) displayName.remove(0, 1);

            fileList += "<tr><td>" + String(count++) + "</td>";
            fileList += "<td><span class='file-name'>" + displayName + "</span></td>";
            fileList += "<td><span class='file-size'>" + _getHumanSize(file.size()) + "</span></td>";
            fileList += "<td><div class='action-buttons'><button onclick='downloadTxt(this, \"" + displayName + "\")' class='btn'>TXT</button>";
            fileList += "<button onclick='downloadGPX(this, \"" + displayName + "\")' class='btn'>GPX</button>";
            fileList += "<button onclick='deleteFile(\"" + displayName + "\")' class='btn btn-danger'>Delete</button></div></td></tr>";
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
    return fileList;
}

String WiFiManager::_getHumanSize(uint32_t bytes) {
    if (bytes < 1024) return String(bytes) + " B";
    if (bytes < 1024 * 1024) return String(bytes / 1024.0, 1) + " KB";
    return String(bytes / 1024.0 / 1024.0, 1) + " MB";
}
