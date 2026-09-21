#include "ble_manager.h"
#include <NimBLEDevice.h>
#include "SdFat.h"

// Defined in main.cpp
extern SdFs sd;

#define SERVICE_UUID           "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define COMMAND_CHAR_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define DATA_CHAR_UUID         "2c27702b-a010-4ea5-a228-4efb7965aa1b"

// Chunk size: MTU (517) minus 3 bytes ATT header = 514, round down to 512 for alignment
#define BLE_CHUNK_SIZE 512

static NimBLEServer* pServer = nullptr;
static NimBLECharacteristic* pCommandCharacteristic = nullptr;
static NimBLECharacteristic* pDataCharacteristic = nullptr;

BLEManager bleManager;

class ServerCallbacks: public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override {
        Serial.println("[BLE] Client connected");
    }
    
    void onDisconnect(NimBLEServer* pServer) override {
        Serial.println("[BLE] Client disconnected");
        NimBLEDevice::startAdvertising();
    }
};

class CommandCallbacks: public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic) override {
        std::string value = pCharacteristic->getValue();
        if (value.length() > 0) {
            String cmd = String(value.c_str());
            cmd.trim();
            Serial.print("[BLE] Command received: ");
            Serial.println(cmd);
            
            if (cmd == "LIST") {
                bleManager.triggerListFiles();
            } else if (cmd == "WIFI") {
                bleManager.sendWifiInfo();
            } else if (cmd.startsWith("GET ")) {
                String filename = cmd.substring(4);
                filename.trim();
                bleManager.triggerFileTransfer(filename);
            }
        }
    }
};

void BLEManager::begin() {
    if (_isRunning) return;
    _isRunning = true;
    _transferState = IDLE;

    Serial.println("[BLE] Starting BLE Server...");
    
    NimBLEDevice::init("Trackify");
    NimBLEDevice::setMTU(517);

    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    NimBLEService* pService = pServer->createService(SERVICE_UUID);

    pCommandCharacteristic = pService->createCharacteristic(
                               COMMAND_CHAR_UUID,
                               NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
                             );
    pCommandCharacteristic->setCallbacks(new CommandCallbacks());

    pDataCharacteristic = pService->createCharacteristic(
                            DATA_CHAR_UUID,
                            NIMBLE_PROPERTY::NOTIFY
                          );

    pService->start();

    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->start();
    
    Serial.println("[BLE] Advertising started.");
}

void BLEManager::stop() {
    if (!_isRunning) return;
    _isRunning = false;
    _transferState = IDLE;
    
    if (_currentFile) {
        _currentFile.close();
    }
    
    Serial.println("[BLE] Stopping BLE Server...");
    NimBLEDevice::deinit(true);
    pServer = nullptr;
}

bool BLEManager::isConnected() {
    return (pServer != nullptr && pServer->getConnectedCount() > 0);
}

void BLEManager::triggerListFiles() {
    if (_transferState != IDLE) return;
    _transferState = LISTING;
}

void BLEManager::triggerFileTransfer(const String& filename) {
    if (_transferState != IDLE) return;
    
    _currentFileName = filename;
    _currentFileSize = 0;
    _bytesSent = 0;
    
    // Open file from SD root (handle filename with or without leading slash)
    String cleanName = filename.startsWith("/") ? filename.substring(1) : filename;
    String path = "/" + cleanName;
    if (!_currentFile.open(path.c_str(), O_RDONLY)) {
        Serial.printf("[BLE] File not found: %s\n", cleanName.c_str());
        String errMsg = "ERROR: File not found: " + cleanName + "\nEND_FILE\n";
        pDataCharacteristic->setValue((uint8_t*)errMsg.c_str(), errMsg.length());
        pDataCharacteristic->notify();
        return;
    }
    
    _currentFileSize = _currentFile.size();
    _transferState = SENDING_FILE;
    
    // Send header: FILE:name:size\n
    String header = "FILE:" + filename + ":" + String(_currentFileSize) + "\n";
    pDataCharacteristic->setValue((uint8_t*)header.c_str(), header.length());
    pDataCharacteristic->notify();
    
    Serial.printf("[BLE] Starting file transfer: %s (%u bytes)\n", filename.c_str(), _currentFileSize);
}

void BLEManager::sendWifiInfo() {
    String wifiData = "WIFI:Trackify:12345678\n";
    pDataCharacteristic->setValue((uint8_t*)wifiData.c_str(), wifiData.length());
    pDataCharacteristic->notify();
    Serial.println("[BLE] Sent WiFi credentials.");
}

void BLEManager::sendChunk(const uint8_t* data, size_t len) {
    pDataCharacteristic->setValue((uint8_t*)data, len);
    pDataCharacteristic->notify();
}

void BLEManager::handle() {
    if (!_isRunning) return;
    
    if (_transferState == LISTING) {
        Serial.println("[BLE] Sending file list...");
        FsFile dir;
        if (dir.open("/", O_RDONLY)) {
            FsFile file;
            String listData = "";
            while (file.openNext(&dir, O_RDONLY)) {
                if (!file.isDir()) {
                    char name[32];
                    file.getName(name, sizeof(name));
                    uint32_t size = file.size();
                    listData += String(name) + ";" + String(size) + "\n";
                    
                    if (listData.length() > 200) {
                        pDataCharacteristic->setValue((uint8_t*)listData.c_str(), listData.length());
                        pDataCharacteristic->notify();
                        delay(20);
                        listData = "";
                    }
                }
                file.close();
            }
            if (listData.length() > 0) {
                pDataCharacteristic->setValue((uint8_t*)listData.c_str(), listData.length());
                pDataCharacteristic->notify();
            }
            String endMarker = "END_LIST\n";
            pDataCharacteristic->setValue((uint8_t*)endMarker.c_str(), endMarker.length());
            pDataCharacteristic->notify();
            dir.close();
        } else {
            String errMsg = "ERROR: Failed to open root\nEND_LIST\n";
            pDataCharacteristic->setValue((uint8_t*)errMsg.c_str(), errMsg.length());
            pDataCharacteristic->notify();
        }
        _transferState = IDLE;
    }
    
    if (_transferState == SENDING_FILE) {
        // Client gone mid-transfer: stop reading — otherwise a
        // preallocated file would keep this state machine busy for a long
        // time calling notify() into the void.
        if (!isConnected()) {
            _currentFile.close();
            Serial.println("[BLE] Client disconnected — aborting file transfer.");
            _transferState = IDLE;
            return;
        }

        if (!_currentFile) {
            String errMsg = "ERROR: File closed unexpectedly\nEND_FILE\n";
            pDataCharacteristic->setValue((uint8_t*)errMsg.c_str(), errMsg.length());
            pDataCharacteristic->notify();
            _transferState = IDLE;
            return;
        }

        // A notification payload is capped at MTU-3 bytes. The fixed 512-byte
        // chunk works only when MTU >= 515 (Android at 517); iOS typically
        // negotiates ~185, where a 512-byte notify() would be truncated or
        // dropped. Slice the chunk to the negotiated MTU instead.
        uint16_t mtu = NimBLEDevice::getMTU();
        if (mtu < 23) mtu = 23;
        size_t chunkSize = (size_t)(mtu - 3) < BLE_CHUNK_SIZE ? (size_t)(mtu - 3) : BLE_CHUNK_SIZE;

        // Send a burst of up to 5 packets per handle() call to saturate BLE bandwidth
        // without starving the FreeRTOS scheduler or overflowing NimBLE buffers.
        for (int burst = 0; burst < 5; burst++) {
            uint8_t buffer[BLE_CHUNK_SIZE];
            size_t bytesRead = _currentFile.read(buffer, chunkSize);
            
            if (bytesRead > 0) {
                sendChunk(buffer, bytesRead);
                _bytesSent += bytesRead;
            }
            
            if (_bytesSent >= _currentFileSize || bytesRead == 0) {
                // All data sent — close file and send end marker
                _currentFile.close();
                String endMarker = "END_FILE\n";
                sendChunk((uint8_t*)endMarker.c_str(), endMarker.length());
                
                Serial.printf("[BLE] File transfer complete: %s (%u bytes sent)\n", 
                              _currentFileName.c_str(), _bytesSent);
                _transferState = IDLE;
                break;
            }
        }
    }
}
