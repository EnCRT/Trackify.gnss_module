#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include <Arduino.h>

class BLEManager {
public:
    void begin();
    void stop();
    void handle();
    
    bool isConnected();
    
    // Internal state handling triggered by callbacks
    void triggerListFiles();
    void triggerFileDownload(String filename);

private:
    bool _isRunning = false;
    bool _deviceConnected = false;
    
    enum TransferState { IDLE, LISTING, SENDING_FILE };
    TransferState _transferState = IDLE;
    String _fileToDownload;
};

extern BLEManager bleManager;

#endif // BLE_MANAGER_H
