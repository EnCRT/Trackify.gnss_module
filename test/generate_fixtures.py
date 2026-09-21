#!/usr/bin/env python3
"""
Generate synthetic Trackify .bin fixture files for parser testing.

Usage:
    python test/generate_fixtures.py          # regenerate all fixtures
    python test/generate_fixtures.py --v1     # only VER=1
    python test/generate_fixtures.py --v2     # only VER=2

Output: test/fixtures/v1_sample.bin, test/fixtures/v2_sample.bin
Each file contains a small circular track (~20 records at 25 Hz).
"""

import struct
import sys
import math
from datetime import datetime, timezone
from pathlib import Path

FIXTURES_DIR = Path(__file__).parent / "fixtures"
FIXTURES_DIR.mkdir(parents=True, exist_ok=True)

# --- Constants ---
RECORD_SIZE = 38   # bytes per TrackRecord
META_SIZE = 64     # bytes per LogMeta (VER >= 2)
LAT_SCALE = 1e7    # degrees → int32
LON_SCALE = 1e7
HEIGHT_SCALE = 1000.0  # meters → mm
SPEED_SCALE = 1000.0   # m/s → mm/s
HEADING_SCALE = 1e5    # degrees → 1e-5 deg

# --- Synthetic GPS track: small circle (diameter ~200m) ---
CENTER_LAT = 48.5359567
CENTER_LON = 9.0512345
CIRCLE_RADIUS_M = 100.0  # meters
SPEED_MS = 15.0           # m/s (~54 km/h)
NUM_RECORDS = 20
FREQ_HZ = 25
DT = 1.0 / FREQ_HZ  # time step between records


def generate_track(num_records: int, start_time: datetime) -> list[dict]:
    """Generate synthetic circular GPS track."""
    records = []
    # Circle around center point
    earth_radius = 6378137.0  # meters

    for i in range(num_records):
        t = i * DT  # seconds from start
        angle = (t * SPEED_MS / CIRCLE_RADIUS_M) % (2 * math.pi)  # rad

        # Position on circle
        lat_offset = (CIRCLE_RADIUS_M * math.cos(angle)) / earth_radius
        lon_offset = (CIRCLE_RADIUS_M * math.sin(angle)) / (
            earth_radius * math.cos(math.radians(CENTER_LAT))
        )
        lat = CENTER_LAT + math.degrees(lat_offset)
        lon = CENTER_LON + math.degrees(lon_offset)

        # Heading: tangent to circle
        heading = (math.degrees(angle) + 90) % 360

        # Accuracy: degrades slightly toward edges (simulated)
        h_acc = 1.5 + 0.3 * math.sin(angle * 3)  # meters
        s_acc = 0.05 + 0.01 * math.sin(angle * 5)  # m/s

        records.append({
            "iTOW": int(t * 1000),
            "nano": int((t * 1000 % 1) * 1e6),
            "lat": int(lat * LAT_SCALE),
            "lon": int(lon * LON_SCALE),
            "height": int(450.2 * HEIGHT_SCALE),  # ~450m above ellipsoid
            "gSpeed": int(SPEED_MS * SPEED_SCALE),
            "headMot": int(heading * HEADING_SCALE),
            "hAcc": int(h_acc * HEIGHT_SCALE),
            "sAcc": int(s_acc * SPEED_SCALE),
            "numSV": 16 + (i % 4),  # 16-19 satellites
            "fixType": 3,            # 3D fix
        })
    return records


def write_header_v1(buf: bytearray, freq_hz: int, utc: datetime) -> None:
    """Write VER=1 header line."""
    utc_str = utc.strftime("%Y-%m-%dT%H:%M:%S.000000000Z")
    header = f"#TRACKIFY:VER=1;FREQ={freq_hz};UTC={utc_str};PROTO=UBX-NAV-PVT\n"
    buf.extend(header.encode("utf-8"))


def write_header_v2(buf: bytearray, freq_hz: int, utc: datetime) -> None:
    """Write VER=2 header line."""
    utc_str = utc.strftime("%Y-%m-%dT%H:%M:%S.000000000Z")
    header = f"#TRACKIFY:VER=2;FREQ={freq_hz};UTC={utc_str};PROTO=UBX-NAV-PVT\n"
    buf.extend(header.encode("utf-8"))


def write_meta_v2(buf: bytearray) -> None:
    """Write 64-byte LogMeta block."""
    meta = bytearray(64)

    # name[16] = "Trackify MX"
    meta[0:11] = b"Trackify MX"

    # serial_number[16] = "A0B1C2" (last 3 bytes of MAC)
    meta[16:22] = b"A0B1C2"

    # model[16] = "Trackify GNSS"
    meta[32:45] = b"Trackify GNSS"

    # hardware_revision (offset 48): 0 = prototype
    meta[48] = 0x00

    # reserved1 (offset 49): 0
    meta[49] = 0x00

    # firmware_version (offset 50-51): 0x0200 = v2.0 (little-endian)
    struct.pack_into("<H", meta, 50, 0x0200)

    # mac_address[6] (offset 52-57)
    meta[52:58] = bytes([0x3C, 0x71, 0xBF, 0xA0, 0xB1, 0xC2])

    # sample_rate_hz (offset 58-59): 25 (little-endian)
    struct.pack_into("<H", meta, 58, 25)

    # record_count (offset 60-63): 0 (unknown or fresh file)
    struct.pack_into("<I", meta, 60, 0)

    assert len(meta) == 64, f"LogMeta must be 64 bytes, got {len(meta)}"
    buf.extend(meta)


def write_record(buf: bytearray, rec: dict) -> None:
    """Write a single 38-byte TrackRecord (little-endian)."""
    data = struct.pack(
        "<IiiiiiiIIBB",
        rec["iTOW"],
        rec["nano"],
        rec["lat"],
        rec["lon"],
        rec["height"],
        rec["gSpeed"],
        rec["headMot"],
        rec["hAcc"],
        rec["sAcc"],
        rec["numSV"],
        rec["fixType"],
    )
    assert len(data) == RECORD_SIZE, f"Record must be {RECORD_SIZE} bytes, got {len(data)}"
    buf.extend(data)


def generate_v1(path: Path) -> None:
    """Generate VER=1 fixture (no metadata block)."""
    utc = datetime(2026, 4, 18, 13, 47, 2, tzinfo=timezone.utc)
    records = generate_track(NUM_RECORDS, utc)

    buf = bytearray()
    write_header_v1(buf, FREQ_HZ, utc)
    header_len = len(buf)
    for r in records:
        write_record(buf, r)

    path.write_bytes(buf)
    record_count = (len(buf) - header_len) // RECORD_SIZE
    print(f"[V1] {path.name}: {len(buf)} bytes, {record_count} records "
          f"(header={header_len}B, records start at offset {header_len})")


def generate_v2(path: Path) -> None:
    """Generate VER=2 fixture (with 64-byte LogMeta block)."""
    utc = datetime(2026, 4, 18, 13, 47, 2, tzinfo=timezone.utc)
    records = generate_track(NUM_RECORDS, utc)

    buf = bytearray()
    write_header_v2(buf, FREQ_HZ, utc)
    header_len = len(buf)
    write_meta_v2(buf)
    meta_offset = header_len
    record_offset = header_len + META_SIZE
    for r in records:
        write_record(buf, r)

    path.write_bytes(buf)
    record_count = (len(buf) - record_offset) // RECORD_SIZE
    print(f"[V2] {path.name}: {len(buf)} bytes, {record_count} records "
          f"(header={header_len}B, meta={META_SIZE}B, "
          f"records start at offset {record_offset})")


def verify_roundtrip(path: Path, version: int) -> bool:
    """Parse back and verify structure."""
    data = path.read_bytes()

    # Find header end
    nl = data.find(b"\n")
    assert nl > 0, "No newline in header"
    header = data[:nl].decode("utf-8")
    assert f"VER={version}" in header, f"Expected VER={version} in header"
    print(f"  Header: {header}")

    # Determine record offset
    if version >= 2:
        record_start = nl + 1 + META_SIZE
        # Verify LogMeta
        meta = data[nl + 1 : nl + 1 + META_SIZE]
        assert len(meta) == META_SIZE
        name = meta[0:16].rstrip(b"\x00").decode()
        serial = meta[16:32].rstrip(b"\x00").decode()
        model = meta[32:48].rstrip(b"\x00").decode()
        fw = struct.unpack_from("<H", meta, 50)[0]
        mac = ":".join(f"{b:02X}" for b in meta[52:58])
        rate = struct.unpack_from("<H", meta, 58)[0]
        print(f"  Meta: name='{name}' serial='{serial}' model='{model}' "
              f"fw={fw:04X} mac={mac} rate={rate}Hz")
    else:
        record_start = nl + 1

    # Verify records
    remaining = data[record_start:]
    assert len(remaining) % RECORD_SIZE == 0, "Misaligned records"
    num_records = len(remaining) // RECORD_SIZE
    print(f"  Records: {num_records} x {RECORD_SIZE}B")

    # Read first and last record
    for idx in [0, num_records - 1]:
        off = idx * RECORD_SIZE
        fields = struct.unpack(
            "<IiiiiiiIIBB", remaining[off : off + RECORD_SIZE]
        )
        iTOW, nano, lat, lon, height, gSpeed, headMot, hAcc, sAcc, numSV, fixType = fields
        print(f"  Record[{idx}]: iTOW={iTOW} lat={lat/LAT_SCALE:.7f} "
              f"lon={lon/LAT_SCALE:.7f} speed={gSpeed/SPEED_SCALE:.1f}m/s "
              f"fix={fixType} sats={numSV}")

    return True


def main():
    args = set(sys.argv[1:])
    do_all = not args or "--all" in args

    if do_all or "--v1" in args:
        path = FIXTURES_DIR / "v1_sample.bin"
        generate_v1(path)
        verify_roundtrip(path, 1)

    if do_all or "--v2" in args:
        path = FIXTURES_DIR / "v2_sample.bin"
        generate_v2(path)
        verify_roundtrip(path, 2)

    print("\nDone. Fixtures in:", FIXTURES_DIR)


if __name__ == "__main__":
    main()
