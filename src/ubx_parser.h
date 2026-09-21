#pragma once
#include <cstdint>
#include <cstring>

// NAV-PVT payload (92 bytes) — packed as per u-blox M10 ICD
struct __attribute__((packed)) NavPvtPayload {
    uint32_t iTOW;
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  min;
    uint8_t  sec;
    uint8_t  valid;
    uint32_t tAcc;
    int32_t  nano;
    uint8_t  fixType;
    uint8_t  flags;
    uint8_t  flags2;
    uint8_t  numSV;
    int32_t  lon;
    int32_t  lat;
    int32_t  height;
    int32_t  hMSL;
    uint32_t hAcc;
    uint32_t vAcc;
    int32_t  velN;
    int32_t  velE;
    int32_t  velD;
    int32_t  gSpeed;
    int32_t  headMot;
    uint32_t sAcc;
    uint32_t headAcc;
    uint16_t pDOP;
    uint8_t  reserved1[6];      // flags3 (1) + reserved1[5] per u-blox M10 ICD
    int32_t  headVeh;
    int16_t  magDec;
    uint16_t magAcc;
};
static_assert(sizeof(NavPvtPayload) == 92, "NAV-PVT payload must be 92 bytes");

// Compact track record (38 bytes) written to SD
struct __attribute__((packed)) TrackRecord {
    uint32_t iTOW;
    int32_t  nano;
    int32_t  lat;
    int32_t  lon;
    int32_t  height;
    int32_t  gSpeed;
    int32_t  headMot;
    uint32_t hAcc;
    uint32_t sAcc;
    uint8_t  numSV;
    uint8_t  fixType;
};
static_assert(sizeof(TrackRecord) == 38, "TrackRecord must be 38 bytes");

// Service metadata block (64 bytes) — written after header \n, before records
// Format version VER>=2
struct __attribute__((packed)) LogMeta {
    char     name[16];            //  0   Device name, null-terminated UTF-8
    char     serial_number[16];   // 16   Serial number string (e.g., "A0B1C2" from MAC)
    char     model[16];           // 32   Model string ("Trackify GNSS")
    uint8_t  hardware_revision;   // 48   Board revision (0=proto, 1=v1.0, ...)
    uint8_t  reserved1;           // 49   (alignment)
    uint16_t firmware_version;    // 50   Firmware version (0x0200 = v2.0)
    uint8_t  mac_address[6];      // 52   Device MAC address (6 bytes raw)
    uint16_t sample_rate_hz;      // 58   Actual recording frequency in Hz
    uint32_t record_count;        // 60   Number of TrackRecords written; maintained periodically by firmware v2.1+; 0 = unknown (legacy files or crash before first meta flush)
};
static_assert(sizeof(LogMeta) == 64, "LogMeta must be 64 bytes");

// UBX frame parser state machine
enum UbxParseState {
    UBX_SYNC1,
    UBX_SYNC2,
    UBX_CLASS,
    UBX_ID,
    UBX_LEN1,
    UBX_LEN2,
    UBX_PAYLOAD,
    UBX_CKA,
    UBX_CKB
};

// UBX sync bytes
#define UBX_SYNC1_BYTE 0xB5
#define UBX_SYNC2_BYTE 0x62

// NAV-PVT class/id (macro renamed from UBX_CLASS_NAV to UBX_NAV_CLASS to avoid
// collision with SparkFun_u-blox_GNSS_v3's global `const uint8_t UBX_CLASS_NAV`)
#define UBX_NAV_CLASS 0x01
#define UBX_ID_PVT    0x07

// Maximum payload we expect (NAV-PVT = 92 bytes)
#define UBX_MAX_PAYLOAD 128

class UbxParser {
public:
    UbxParser() : _state(UBX_SYNC1), _payloadLen(0), _payloadIdx(0) {}

    bool feed(uint8_t byte) {
        switch (_state) {
            case UBX_SYNC1:
                if (byte == UBX_SYNC1_BYTE) {
                    _state = UBX_SYNC2;
                }
                break;

            case UBX_SYNC2:
                if (byte == UBX_SYNC2_BYTE) {
                    _state = UBX_CLASS;
                    _ckA = 0;
                    _ckB = 0;
                } else {
                    _state = UBX_SYNC1;
                }
                break;

            case UBX_CLASS:
                if (byte == UBX_NAV_CLASS) {
                    _msgClass = byte;
                    _ckA += byte;
                    _ckB += _ckA;
                    _state = UBX_ID;
                } else {
                    _state = UBX_SYNC1;
                }
                break;

            case UBX_ID:
                if (byte == UBX_ID_PVT) {
                    _msgId = byte;
                    _ckA += byte;
                    _ckB += _ckA;
                    _state = UBX_LEN1;
                } else {
                    _state = UBX_SYNC1;
                }
                break;

            case UBX_LEN1:
                _payloadLen = byte;
                _ckA += byte;
                _ckB += _ckA;
                _state = UBX_LEN2;
                break;

            case UBX_LEN2:
                _payloadLen |= ((uint16_t)byte << 8);
                _ckA += byte;
                _ckB += _ckA;
                _payloadIdx = 0;
                if (_payloadLen == 0) {
                    _state = UBX_CKA;
                } else if (_payloadLen > UBX_MAX_PAYLOAD) {
                    _state = UBX_SYNC1;
                } else {
                    _state = UBX_PAYLOAD;
                }
                break;

            case UBX_PAYLOAD:
                _ckA += byte;
                _ckB += _ckA;
                if (_payloadIdx < UBX_MAX_PAYLOAD) {
                    _payloadBuf[_payloadIdx] = byte;
                }
                _payloadIdx++;
                if (_payloadIdx >= _payloadLen) {
                    _state = UBX_CKA;
                }
                break;

            case UBX_CKA:
                if (byte == _ckA) {
                    _state = UBX_CKB;
                } else {
                    _state = UBX_SYNC1;
                }
                break;

            case UBX_CKB:
                _state = UBX_SYNC1;
                if (byte == _ckB) {
                    if (_msgClass == UBX_NAV_CLASS && _msgId == UBX_ID_PVT && _payloadLen == sizeof(NavPvtPayload)) {
                        memcpy(&_pvt, _payloadBuf, sizeof(NavPvtPayload));
                        return true;
                    }
                }
                break;
        }
        return false;
    }

    const NavPvtPayload& pvt() const { return _pvt; }

    void reset() { _state = UBX_SYNC1; }

private:
    UbxParseState _state;
    uint16_t _payloadLen;
    uint16_t _payloadIdx;
    uint8_t _payloadBuf[UBX_MAX_PAYLOAD];
    uint8_t _msgClass;
    uint8_t _msgId;
    uint8_t _ckA;
    uint8_t _ckB;
    NavPvtPayload _pvt;
};
