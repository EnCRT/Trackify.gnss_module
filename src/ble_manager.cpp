#include "ble_manager.h"
#include <NimBLEDevice.h>
#include "SdFat.h"

// Defined in main.cpp
extern SdFs sd;

#define SERVICE_UUID           "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define COMMAND_CHAR_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define DATA_CHAR_UUID         "2c27702b-a010-4ea5-a228-4efb7965aa1b"

static NimBLEServer* pServer = nullptr;
static NimBLECharacteristic* pCommandCharacteristic = nullptr;
static NimBLECharacteristic* pDataCharacteristic = nullptr;

BLEManager bleManager;

class ServerCallbacks: public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override {
        Serial.println("[BLE] Client connected");
        // NimBLEDevice::setMTU(517); is called by client usually, but we can accept it
    }
    
    void onDisconnect(NimBLEServer* pServer) override {
        Serial.println("[BLE] Client disconnected");
        // Restart advertising
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
            } else if (cmd.startsWith("GET ")) {
                String filename = cmd.substring(4);
                filename.trim();
                bleManager.triggerFileDownload(filename);
            }
        }
    }
};

void BLEManager::begin() {
    if (_isRunning) return;
    _isRunning = true;
    _transferState = IDLE;

    Serial.println("[BLE] Starting BLE Server...");
    
    // Initialize NimBLE, hidden mode is tricky but we can just set a name
    // and start advertising. The user said "hidden" but the app needs to find it. 
    // We'll just advertise when this mode is active.
    NimBLEDevice::init("Trackify");
    
    // Increase MTU
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
    // Start advertising
    pAdvertising->start();
    
    Serial.println("[BLE] Advertising started.");
}

void BLEManager::stop() {
    if (!_isRunning) return;
    _isRunning = false;
    _transferState = IDLE;
    
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

void BLEManager::triggerFileDownload(String filename) {
    if (_transferState != IDLE) return;
    _fileToDownload = filename;
    if (!_fileToDownload.startsWith("/")) {
        _fileToDownload = "/" + _fileToDownload;
    }
    _transferState = SENDING_FILE;
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
                    
                    // Send in chunks if list is too big
                    if (listData.length() > 200) {
                        pDataCharacteristic->setValue((uint8_t*)listData.c_str(), listData.length());
                        pDataCharacteristic->notify();
                        delay(20); // small delay to let BLE stack process
                        listData = "";
                    }
                }
                file.close();
            }
            if (listData.length() > 0) {
                pDataCharacteristic->setValue((uint8_t*)listData.c_str(), listData.length());
                pDataCharacteristic->notify();
            }
            // End of list marker
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
    else if (_transferState == SENDING_FILE) {
        Serial.print("[BLE] Sending file: ");
        Serial.println(_fileToDownload);
        
        FsFile file;
        if (file.open(_fileToDownload.c_str(), O_RDONLY)) {
            // Get negotiated MTU size
            uint16_t mtu = NimBLEDevice::getMTU();
            size_t chunkSize = mtu > 3 ? mtu - 3 : 20; // MTU - 3 is max payload for notification
            if (chunkSize > 500) chunkSize = 500; // Cap it
            
            uint8_t* buffer = new uint8_t[chunkSize];
            int bytesRead;
            
            while ((bytesRead = file.read(buffer, chunkSize)) > 0) {
                pDataCharacteristic->setValue(buffer, bytesRead);
                pDataCharacteristic->notify();
                
                // Allow background tasks to run and avoid watchdog
                vTaskDelay(1); 
                
                // If client disconnects, abort
                if (!isConnected()) {
                    Serial.println("[BLE] Client disconnected during transfer. Aborting.");
                    break;
                }
            }
            
            delete[] buffer;
            file.close();
            
            // Send EOF marker (0 byte payload)
            pDataCharacteristic->setValue((uint8_t*)nullptr, 0);
            pDataCharacteristic->notify();
            Serial.println("[BLE] File transfer complete.");
        } else {
            Serial.println("[BLE] Failed to open file for reading.");
            String errMsg = "ERROR: File not found";
            pDataCharacteristic->setValue((uint8_t*)errMsg.c_str(), errMsg.length());
            pDataCharacteristic->notify();
        }
        
        _transferState = IDLE;
    }
}
