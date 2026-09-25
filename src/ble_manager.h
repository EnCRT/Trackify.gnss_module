#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include <Arduino.h>
#include "SdFat.h"

class BLEManager {
public:
    void begin();
    void stop();
    void handle();
    
    bool isConnected();
    
    // Internal state handling triggered by callbacks
    void triggerListFiles();
    void triggerFileTransfer(const String& filename);
    void sendWifiInfo();
    void sendDeviceInfo();

private:
    bool _isRunning = false;
    bool _deviceConnected = false;
    
    enum TransferState { IDLE, LISTING, SENDING_FILE };
    TransferState _transferState = IDLE;

    // File transfer state
    String _currentFileName;
    FsFile _currentFile;
    uint32_t _currentFileSize = 0;
    uint32_t _bytesSent = 0;

    void sendChunk(const uint8_t* data, size_t len);
};

extern BLEManager bleManager;

#endif // BLE_MANAGER_H
