# Trackify Binary Log Format (`.bin`) — Specification for Mobile App

## Overview

Trackify firmware v2.0+ writes GPS log data in a compact binary format (`.bin` files)
using UBX-NAV-PVT messages from the u-blox M10 GNSS module. This document describes
the file format for parsing in the Moto Lap Timer mobile application.

## File Structure

```
[Text Header Line] [Metadata Block (64 bytes)] [Record 0] [Record 1] ... [Record N-1]
```

> **Backward compatibility**: VER=1 files have NO metadata block — records start immediately after `\n`.
> VER≥2 files include a fixed 64-byte `LogMeta` block between the header and the first record.

### Header Line

The file starts with an ASCII text header terminated by `\n` (0x0A):

```
#TRACKIFY:VER=2;FREQ=25;UTC=2026-04-18T13:47:02.123456789Z;PROTO=UBX-NAV-PVT\n
```

| Field | Type | Description |
|-------|------|-------------|
| `VER` | int | Format version (`1` = no metadata, `2` = metadata block present) |
| `FREQ` | int | GPS measurement frequency in Hz (e.g., `10` or `25`) |
| `UTC` | ISO 8601 | UTC timestamp of the first record (nanosecond precision) |
| `PROTO` | string | Protocol identifier: `UBX-NAV-PVT` |

Parsing: read bytes until `\n` (0x0A), then decode as UTF-8. Split by `;`, then each key=value.

---

### Service Metadata Block (VER ≥ 2)

After the header newline, a fixed **64-byte** `LogMeta` structure follows. All multi-byte fields are little-endian.

#### LogMeta Layout (64 bytes)

```
Offset  Size  Type       Field               Description
------  ----  ----       -----               -----------
 0      16    char[16]   name                Device name, UTF-8 null-terminated (e.g., "Trackify MX")
16      16    char[16]   serial_number       Serial number string, null-terminated (e.g., "A0B1C2")
32      16    char[16]   model               Model string, null-terminated (e.g., "Trackify GNSS")
48       1    uint8      hardware_revision   Board revision (0=prototype, 1=v1.0, ...)
49       1    uint8      reserved1           (alignment padding)
50       2    uint16     firmware_version    Firmware version (0x0200 = v2.0)
52       6    uint8[6]   mac_address         Device MAC address (raw bytes)
58       2    uint16     sample_rate_hz      Actual recording frequency in Hz
60       4    uint32     record_count        Number of TrackRecords in the file; periodically updated by firmware v2.1+ every 5s and on clean stop; 0 = unknown (legacy firmware or crash before first meta flush — such files must not be auto-truncated)
```

**Total: 64 bytes**

#### Parsing Notes

- String fields (`name`, `serial_number`, `model`) are fixed 16-byte arrays. If the string is shorter, remaining bytes are `\0`. Parsers should read the full 16 bytes and trim trailing nulls.
- `firmware_version` is a packed BCD-like value: `0x0201` = v2.1, `0x0100` = v1.0.
- `mac_address` is 6 raw bytes (e.g., `{0x3C, 0x71, 0xBF, 0xA0, 0xB1, 0xC2}`).
- `sample_rate_hz` is the **actual measured** frequency from the GPS module (may differ from `FREQ` in header if module refused the requested rate).

#### Parsing Pseudocode (VER-aware)

```python
def parse_file(data: bytes):
    header_end = data.find(b'\n')
    header = data[:header_end].decode('utf-8')
    
    # Parse VER from header
    fields = dict(kv.split('=') for kv in header.lstrip('#TRACKIFY:').split(';'))
    version = int(fields['VER'])
    
    # Determine record start offset
    if version >= 2:
        meta_start = header_end + 1
        meta = data[meta_start:meta_start + 64]
        record_start = meta_start + 64
    else:
        meta = None
        record_start = header_end + 1
    
    record_size = 38
    num_records = (len(data) - record_start) // record_size
    records = [
        parse_record(data[record_start + i*record_size : record_start + (i+1)*record_size])
        for i in range(num_records)
    ]
    return header, meta, records
```

---

### Track Records

After the header newline, the file contains a sequence of fixed-size binary records.
Each record is **38 bytes**, little-endian byte order.

#### Record Layout (38 bytes)

```
Offset  Size  Type      Field       Description
------  ----  ----      -----       -----------
 0       4    uint32    iTOW        GPS time of week, milliseconds
 4       4    int32     nano        Nanosecond fraction of UTC second (-500M..+500M)
 8       4    int32     lat         Latitude, 1e-7 degrees (e.g., 485359567 = 48.5359567°)
12       4    int32     lon         Longitude, 1e-7 degrees
16       4    int32     height      Height above ellipsoid, millimeters
20       4    int32     gSpeed      Ground speed (2D), millimeters per second
24       4    int32     headMot     Heading of motion, 1e-5 degrees (e.g., 12345678 = 123.45678°)
28       4    uint32    hAcc        Horizontal accuracy estimate, millimeters
32       4    uint32    sAcc        Speed accuracy estimate, millimeters per second
36       1    uint8     numSV       Number of satellites used in navigation solution
37       1    uint8     fixType     GNSS fix type: 0=no fix, 2=2D, 3=3D, 4=GNSS+DR, 5=time only
```

#### Parsing Pseudocode

**Kotlin (Android):**
```kotlin
data class LogMeta(
    val name: String,           // 16 bytes, null-trimmed
    val serialNumber: String,   // 16 bytes, null-trimmed
    val model: String,          // 16 bytes, null-trimmed
    val hardwareRevision: Int,  // uint8
    val firmwareVersion: Int,   // uint16 little-endian (0x0200 = v2.0)
    val macAddress: ByteArray,  // 6 bytes raw
    val sampleRateHz: Int       // uint16 little-endian
) {
    val macAddressString: String get() = macAddress.joinToString(":") { "%02X".format(it) }
    val firmwareVersionString: String get() {
        val major = (firmwareVersion shr 8) and 0xFF
        val minor = firmwareVersion and 0xFF
        return "v$major.$minor"
    }
}

fun ByteBuffer.readLogMeta(): LogMeta {
    val name = readFixedString(16)
    val serialNumber = readFixedString(16)
    val model = readFixedString(16)
    val hwRev = (get().toInt() and 0xFF)
    get() // skip reserved1
    val fwVer = (short.get().toInt() and 0xFFFF)
    val mac = ByteArray(6) { get() }
    val sampleRate = (short.get().toInt() and 0xFFFF)
    getInt() // skip record_count (firmware crash-recovery helper, not needed for parsing)
    return LogMeta(name, serialNumber, model, hwRev, fwVer, mac, sampleRate)
}

fun ByteBuffer.readFixedString(length: Int): String {
    val bytes = ByteArray(length) { get() }
    val nullEnd = bytes.indexOf(0)
    return String(bytes, 0, if (nullEnd >= 0) nullEnd else length, Charsets.UTF_8)
}

data class TrackRecord(
    val iTOW: Int,       // uint32 little-endian
    val nano: Int,       // int32 little-endian
    val lat: Double,     // lat / 1e7
    val lon: Double,     // lon / 1e7
    val height: Double,  // height / 1000.0 (mm → m)
    val gSpeed: Double,  // gSpeed / 1000.0 (mm/s → m/s)
    val headMot: Double, // headMot / 1e5 (1e-5 deg → deg)
    val hAcc: Double,    // hAcc / 1000.0 (mm → m)
    val sAcc: Double,    // sAcc / 1000.0 (mm/s → m/s)
    val numSV: Int,      // uint8
    val fixType: Int     // uint8
)

fun ByteBuffer.readTrackRecord(): TrackRecord {
    val iTOW = int.get()
    val nano = int.get()
    val lat = int.get() / 1e7
    val lon = int.get() / 1e7
    val height = int.get() / 1000.0
    val gSpeed = int.get() / 1000.0
    val headMot = int.get() / 1e5
    val hAcc = (int.get().toLong() and 0xFFFFFFFFL) / 1000.0
    val sAcc = (int.get().toLong() and 0xFFFFFFFFL) / 1000.0
    val numSV = (get().toInt() and 0xFF)
    val fixType = (get().toInt() and 0xFF)
    return TrackRecord(iTOW, nano, lat, lon, height, gSpeed, headMot, hAcc, sAcc, numSV, fixType)
}
```

**Swift (iOS):**
```swift
struct TrackRecord {
    let iTOW: UInt32
    let nano: Int32
    let lat: Double      // degrees
    let lon: Double      // degrees
    let height: Double   // meters
    let gSpeed: Double   // m/s
    let headMot: Double  // degrees
    let hAcc: Double     // meters
    let sAcc: Double     // m/s
    let numSV: UInt8
    let fixType: UInt8

    init(data: Data, offset: Int) {
        iTOW = data.withUnsafeBytes { $0.load(fromByteOffset: offset, as: UInt32.self) }
        nano = data.withUnsafeBytes { $0.load(fromByteOffset: offset + 4, as: Int32.self) }
        lat = Double(Int32(littleEndian: data[offset+8..<offset+12].withUnsafeBytes { $0.load(as: Int32.self) })) / 1e7
        // ... etc for remaining fields
    }
}
```

**Dart (Flutter):**
```dart
class LogMeta {
  final String name;
  final String serialNumber;
  final String model;
  final int hardwareRevision;
  final int firmwareVersion;
  final List<int> macAddress;
  final int sampleRateHz;

  LogMeta(ByteData data, int offset) :
    name = _readFixedString(data, offset, 16),
    serialNumber = _readFixedString(data, offset + 16, 16),
    model = _readFixedString(data, offset + 32, 16),
    hardwareRevision = data.getUint8(offset + 48),
    firmwareVersion = data.getUint16(offset + 50, Endian.little),
    macAddress = [
      data.getUint8(offset + 52),
      data.getUint8(offset + 53),
      data.getUint8(offset + 54),
      data.getUint8(offset + 55),
      data.getUint8(offset + 56),
      data.getUint8(offset + 57),
    ],
    sampleRateHz = data.getUint16(offset + 58, Endian.little);

  static String _readFixedString(ByteData data, int offset, int length) {
    final bytes = <int>[];
    for (int i = 0; i < length; i++) {
      final b = data.getUint8(offset + i);
      if (b == 0) break;
      bytes.add(b);
    }
    return utf8.decode(bytes);
  }

  String get macAddressString => macAddress
      .map((b) => b.toRadixString(16).padLeft(2, '0').toUpperCase())
      .join(':');

  String get firmwareVersionString {
    final major = (firmwareVersion >> 8) & 0xFF;
    final minor = firmwareVersion & 0xFF;
    return 'v$major.$minor';
  }
}

class TrackRecord {
  final int iTOW;
  final int nano;
  final double lat;
  final double lon;
  final double height;  // meters
  final double gSpeed;  // m/s
  final double headMot; // degrees
  final double hAcc;    // meters
  final double sAcc;    // m/s
  final int numSV;
  final int fixType;

  TrackRecord(ByteData data, int offset) :
    iTOW = data.getUint32(offset, Endian.little),
    nano = data.getInt32(offset + 4, Endian.little),
    lat = data.getInt32(offset + 8, Endian.little) / 1e7,
    lon = data.getInt32(offset + 12, Endian.little) / 1e7,
    height = data.getInt32(offset + 16, Endian.little) / 1000.0,
    gSpeed = data.getInt32(offset + 20, Endian.little) / 1000.0,
    headMot = data.getInt32(offset + 24, Endian.little) / 1e5,
    hAcc = data.getUint32(offset + 28, Endian.little) / 1000.0,
    sAcc = data.getUint32(offset + 32, Endian.little) / 1000.0,
    numSV = data.getUint8(offset + 36),
    fixType = data.getUint8(offset + 37);
}
```

---

## Converting iTOW + nano to UTC Timestamp

GPS time of week (`iTOW`) is milliseconds since Sunday 00:00:00 GPS time.
u-blox modules output UTC time in the PVT message; we store the equivalent
GPS iTOW for compactness.

The NAV-PVT message also provides `year`, `month`, `day`, `hour`, `min`, `sec`, `nano`
in the header line. For each record, compute UTC as:

```python
GPS_EPOCH = datetime(1980, 1, 6, 0, 0, 0, tzinfo=timezone.utc)  # GPS epoch
leap_seconds = 18  # As of 2026; subtract for precise UTC

sec_of_week = iTOW / 1000.0
frac_seconds = (iTOW % 1000) / 1000.0 + nano / 1e9
utc = GPS_EPOCH + timedelta(seconds=sec_of_week - leap_seconds) + timedelta(seconds=frac_seconds)
```

**Simplified for track analysis (sub-millisecond not critical):**
- Use UTC timestamp from the header line as anchor
- Subsequent records: `time = header_utc + (record.iTOW - header_iTOW) / 1000.0`

---

## Filtering

Records with `fixType < 2` should be skipped for track analysis (no valid position).
Records with `hAcc > 10000` (>10m accuracy) may be excluded for high-quality tracks.

---

## Sample File (Hex Dump with VER=2 Metadata)

```
Offset  Hex                                              ASCII
------  ---                                              -----
000000  23 54 52 41 43 4b 49 46  59 3a 56 45 52 3d 32 3b  #TRACKIFY:VER=2;
000010  46 52 45 51 3d 32 35 3b  55 54 43 3d 32 30 32 36  FREQ=25;UTC=2026
000020  2d 30 34 2d 31 38 54 31  33 3a 34 37 3a 30 32 2e  -04-18T13:47:02.
000030  30 30 30 30 30 30 30 30  30 5a 3b 50 52 4f 54 4f  000000000Z;PROTO
000040  3d 55 42 58 2d 4e 41 56  2d 50 56 54 0a            =UBX-NAV-PVT.
                                                                         ← header ends at 0x4D
00004D  54 72 61 63 6b 69 66 79  20 4d 58 00 00 00 00 00  Trackify MX.....  ← name[16]
00005D  41 30 42 31 43 32 00 00  00 00 00 00 00 00 00 00  A0B1C2..........  ← serial[16]
00006D  54 72 61 63 6b 69 66 79  20 47 4e 53 53 00 00 00  Trackify GNSS...  ← model[16]
00007D  00 00 00 02 3c 71 bf a0  b1 c2 19 00 00 00 00 00  ......<q........  ← hw=0,res=0,fw=0x0200,MAC,sample=25,record_count=0
                                                                         ← metadata ends at 0x8D (64 bytes)
00008D  5c 0b d5 01 00 00 00 00  f8 83 03 03  c8 90 20 03  ← record 0 (38 bytes)
00009D  b0 ed 01 00 50 94 00 00  f4 92 00 00 60 09 00 00
0000AD  b8 0b 00 00 14 02
0000B3  60 0b d5 01 20 0e 00 00  f4 83 03 03  d0 90 20 03  ← record 1
...
```

Total records: `(file_size - header_length - metadata_length) / 38`

Where:
- `header_length` = offset of first `\n` + 1 (the newline byte)
- `metadata_length` = 64 if `VER ≥ 2`, else 0

---

## Crash Recovery

Firmware preallocates `log_*.bin` files to 200 MB so clusters are contiguous for
high-rate writes. On a power failure (or SD removal during logging) the file keeps
its preallocated size, and everything past the last written record is arbitrary
garbage — parsers must not rely on file size alone (they already compute the record
count from the file length and ignore a partial trailing record).

Firmware v2.1+ mitigates this in two ways:

1. **Periodic meta flush** — while logging, `record_count` in the LogMeta block is
   rewritten to the SD card every 5 seconds and once more on clean stop (before the
   final truncate).
2. **Boot/hot-plug recovery** — at boot (and when an SD card is re-inserted), the
   firmware scans the SD root and for every `log_*.bin` with a `#TRACKIFY:` header
   and `VER >= 2` reads `record_count` from offset `header_end + 1 + 60`. If
   `record_count > 0` and `header_end + 1 + 64 + record_count * 38 < file_size`,
   the file was interrupted and is truncated to
   `header_end + 1 + 64 + record_count * 38`. `VER=1` files have no LogMeta block
   and are always skipped — reading offset 60 in them would hit record bytes.
   Files with `record_count == 0`
   (legacy firmware or crash before the first meta flush), non-Trackify files, and
   files where the expected size is not smaller than the actual size are left
   untouched.

After recovery, an interrupted file loses at most 5 seconds of records (the meta
flush interval). The Flutter app needs no changes: it skips the 64-byte LogMeta and
parses `(file_size - record_start) / 38` complete records, tolerating a partial
tail.

---

## BLE Transfer Considerations

The `.bin` format is ~76% smaller than the old NMEA `.txt` format.
A 5-minute session at 25 Hz = 7500 records × 38 bytes = **285 KB** (vs 1.1 MB for NMEA).
This makes BLE file transfer significantly faster than before.
