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
    void sendWifiInfo();

private:
    bool _isRunning = false;
    bool _deviceConnected = false;
    
    enum TransferState { IDLE, LISTING };
    TransferState _transferState = IDLE;
};

extern BLEManager bleManager;

#endif // BLE_MANAGER_H
