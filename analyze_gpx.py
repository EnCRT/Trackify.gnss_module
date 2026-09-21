#!/usr/bin/env python3
"""Audit and statistics for linear.gpx and circle.gpx вЂ” Trackify logger output."""
from __future__ import annotations

import xml.etree.ElementTree as ET
from collections import Counter
from dataclasses import dataclass, field
from datetime import datetime, timezone
from math import atan2, cos, degrees, radians, sin, sqrt
from pathlib import Path
from typing import List, Optional, Tuple

NS = {"gpx": "http://www.topografix.com/GPX/1/1"}


@dataclass
class Point:
    lat: float
    lon: float
    ele: float
    time: datetime
    speed: float  # m/s (from extensions)
    course: Optional[float] = None  # degrees


@dataclass
class FileStats:
    name: str
    points: List[Point] = field(default_factory=list)

    # --- computed ---
    duration_s: float = 0.0
    total_distance_m: float = 0.0
    avg_speed_ms: float = 0.0
    max_speed_ms: float = 0.0
    min_speed_ms: float = float("inf")
    avg_sampling_hz: float = 0.0

    # sampling analysis
    intervals: List[float] = field(default_factory=list)  # seconds between consecutive points
    zero_intervals_count: int = 0  # same-second points
    sub_40ms_intervals_count: int = 0  # intervals < 40ms (25 Hz)
    ms_40_100_intervals: int = 0  # 40-100ms (10-25 Hz target)
    ms_100_500_intervals: int = 0  # 100-500ms
    ms_500_plus_intervals: int = 0  # >500ms (gaps)

    # dedup / noise
    duplicate_points: int = 0  # consecutive identical lat+lon
    stationary_points: int = 0  # speed < 0.1 m/s
    moving_points: int = 0  # speed >= 0.1 m/s

    # course coverage
    course_available: int = 0  # points with course data
    course_missing: int = 0

    # elevation
    ele_min: float = float("inf")
    ele_max: float = -float("inf")
    ele_span: float = 0.0

    # jitter (position noise)
    position_jumps_m: List[float] = field(default_factory=list)  # consecutive distance deltas

    # speed acceleration
    accel_ms2: List[float] = field(default_factory=list)  # m/s per second

    def to_dict(self) -> dict:
        intervals = sorted(self.intervals)
        n = len(intervals)
        d = {
            "name": self.name,
            "points": len(self.points),
            "duration_s": self.duration_s,
            "duration_min": round(self.duration_s / 60, 2),
            "total_distance_km": round(self.total_distance_m / 1000, 3),
            # Speed
            "avg_speed_kmh": round(self.avg_speed_ms * 3.6, 2),
            "max_speed_kmh": round(self.max_speed_ms * 3.6, 2),
            "min_speed_kmh": round(self.min_speed_ms * 3.6, 2) if self.min_speed_ms != float("inf") else 0,
            # Sampling
            "avg_sampling_hz": round(self.avg_sampling_hz, 2),
            "median_interval_ms": round(intervals[n // 2] * 1000, 1) if n else 0,
            "p95_interval_ms": round(intervals[int(n * 0.95)] * 1000, 1) if n else 0,
            "p99_interval_ms": round(intervals[int(n * 0.99)] * 1000, 1) if n else 0,
            "min_interval_ms": round(intervals[0] * 1000, 1) if n else 0,
            "max_interval_ms": round(intervals[-1] * 1000, 1) if n else 0,
            "zero_interval_count": self.zero_intervals_count,
            "sub_40ms_count": self.sub_40ms_intervals_count,
            "40_100ms_count": self.ms_40_100_intervals,
            "100_500ms_count": self.ms_100_500_intervals,
            "500ms_plus_count": self.ms_500_plus_intervals,
            "gaps_gt_1s": sum(1 for i in self.intervals if i > 1.0),
            # Quality
            "duplicate_points": self.duplicate_points,
            "stationary_points": self.stationary_points,
            "moving_points": self.moving_points,
            "pct_moving": round(self.moving_points / max(len(self.points), 1) * 100, 1),
            "course_available": self.course_available,
            "course_missing": self.course_missing,
            "pct_with_course": round(self.course_available / max(len(self.points), 1) * 100, 1),
            # Elevation
            "ele_min": self.ele_min,
            "ele_max": self.ele_max,
            "ele_span": round(self.ele_span, 1),
            # Jitter / acceleration
            "max_position_jump_m": round(max(self.position_jumps_m) if self.position_jumps_m else 0, 2),
            "max_accel_ms2": round(max(self.accel_ms2) if self.accel_ms2 else 0, 2),
            "max_accel_g": round(max(self.accel_ms2) / 9.81 if self.accel_ms2 else 0, 2),
            "instances_high_accel_gt_2g": sum(1 for a in self.accel_ms2 if abs(a) > 2 * 9.81),
            "instances_high_accel_gt_5g": sum(1 for a in self.accel_ms2 if abs(a) > 5 * 9.81),
        }
        return d


def haversine(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Distance in meters between two lat/lon points."""
    R = 6371000
    dlat = radians(lat2 - lat1)
    dlon = radians(lon2 - lon1)
    a = sin(dlat / 2) ** 2 + cos(radians(lat1)) * cos(radians(lat2)) * sin(dlon / 2) ** 2
    return R * 2 * atan2(sqrt(a), sqrt(1 - a))


def parse_gpx(filepath: str) -> FileStats:
    tree = ET.parse(filepath)
    root = tree.getroot()
    name = Path(filepath).stem

    stats = FileStats(name=name)

    for seg in root.findall(".//gpx:trkseg", NS):
        trkpts = seg.findall("gpx:trkpt", NS)
        for pt in trkpts:
            lat = float(pt.attrib["lat"])
            lon = float(pt.attrib["lon"])
            ele = float(pt.find("gpx:ele", NS).text) if pt.find("gpx:ele", NS) is not None else 0.0
            time_str = pt.find("gpx:time", NS).text
            dt = datetime.fromisoformat(time_str.replace("Z", "+00:00"))

            speed = 0.0
            course = None
            ext = pt.find("gpx:extensions", NS)
            if ext is not None:
                s_el = ext.find("gpx:speed", NS)
                if s_el is not None and s_el.text:
                    speed = float(s_el.text)
                c_el = ext.find("gpx:course", NS)
                if c_el is not None and c_el.text:
                    course = float(c_el.text)

            p = Point(lat=lat, lon=lon, ele=ele, time=dt, speed=speed, course=course)
            stats.points.append(p)

    return stats


def compute_stats(stats: FileStats) -> FileStats:
    pts = stats.points
    n = len(pts)
    if n == 0:
        return stats

    # Duration
    stats.duration_s = (pts[-1].time - pts[0].time).total_seconds()

    # Intervals between consecutive points
    total_distance = 0.0
    total_speed = 0.0
    prev = pts[0]

    for i in range(1, n):
        cur = pts[i]
        dt = (cur.time - prev.time).total_seconds()
        stats.intervals.append(dt)

        if dt == 0:
            stats.zero_intervals_count += 1
        elif dt < 0.040:
            stats.sub_40ms_intervals_count += 1
        elif dt <= 0.100:
            stats.ms_40_100_intervals += 1
        elif dt <= 0.500:
            stats.ms_100_500_intervals += 1
        else:
            stats.ms_500_plus_intervals += 1

        # Distance
        dist = haversine(prev.lat, prev.lon, cur.lat, cur.lon)
        total_distance += dist
        if dt > 0:
            stats.position_jumps_m.append(dist)
            if dist > 0.01:  # reject near-zero distance for meaningful accel
                implied_speed = dist / dt
                accel = (implied_speed - prev.speed) / dt if prev.speed is not None else 0
                stats.accel_ms2.append(accel)

        # Duplicate check
        if prev.lat == cur.lat and prev.lon == cur.lon:
            stats.duplicate_points += 1

        # Speed stats (from extensions)
        if cur.speed > stats.max_speed_ms:
            stats.max_speed_ms = cur.speed
        if cur.speed < stats.min_speed_ms:
            stats.min_speed_ms = cur.speed

        # Stationary vs moving
        if cur.speed < 0.1:
            stats.stationary_points += 1
        else:
            stats.moving_points += 1

        # Course
        if cur.course is not None:
            stats.course_available += 1
        else:
            stats.course_missing += 1

        # Elevation
        if cur.ele < stats.ele_min:
            stats.ele_min = cur.ele
        if cur.ele > stats.ele_max:
            stats.ele_max = cur.ele

        total_speed += cur.speed
        prev = cur

    # Also count first point for stationary/moving/course
    if pts[0].speed < 0.1:
        stats.stationary_points += 1
    else:
        stats.moving_points += 1
    if pts[0].course is not None:
        stats.course_available += 1
    else:
        stats.course_missing += 1

    stats.total_distance_m = total_distance
    stats.avg_speed_ms = total_speed / n
    stats.avg_sampling_hz = n / stats.duration_s if stats.duration_s > 0 else 0
    stats.ele_span = stats.ele_max - stats.ele_min
    if stats.min_speed_ms == float("inf"):
        stats.min_speed_ms = 0.0

    return stats


def print_header(text: str):
    print(f"\n{'=' * 60}")
    print(f"  {text}")
    print(f"{'=' * 60}")

SEP = "-"


def print_stats(stats: FileStats):
    d = stats.to_dict()
    print_header(f"FILE: {d['name']}")

    print(f"\n  -- TRACK OVERVIEW --")
    print(f"    Points recorded:        {d['points']}")
    print(f"    Duration:               {d['duration_s']:.1f}s ({d['duration_min']} min)")
    print(f"    Total distance:         {d['total_distance_km']} km")
    print(f"    Avg sampling rate:      {d['avg_sampling_hz']} Hz")

    print(f"\n  в”Ђв”Ђ SPEED в”Ђв”Ђ")
    print(f"    Avg speed:              {d['avg_speed_kmh']} km/h")
    print(f"    Max speed:              {d['max_speed_kmh']} km/h")
    print(f"    Min speed:              {d['min_speed_kmh']} km/h")

    print(f"\n  в”Ђв”Ђ SAMPLING INTERVALS (real) в”Ђв”Ђ")
    print(f"    Median interval:        {d['median_interval_ms']} ms")
    print(f"    P95 interval:           {d['p95_interval_ms']} ms")
    print(f"    P99 interval:           {d['p99_interval_ms']} ms")
    print(f"    Min interval:           {d['min_interval_ms']} ms")
    print(f"    Max interval:           {d['max_interval_ms']} ms")
    print(f"    Zero-interval (same s): {d['zero_interval_count']}")
    print(f"    Sub-40ms (25Hz range):  {d['sub_40ms_count']}")
    print(f"    40-100ms (10-25Hz):     {d['40_100ms_count']}")
    print(f"    100-500ms:              {d['100_500ms_count']}")
    print(f"    500ms+:                 {d['500ms_plus_count']}")
    print(f"    Gaps >1s:               {d['gaps_gt_1s']}")

    print(f"\n  в”Ђв”Ђ DATA QUALITY в”Ђв”Ђ")
    print(f"    Duplicate points:       {d['duplicate_points']} ({100 * d['duplicate_points'] / max(d['points'], 1):.1f}%)")
    print(f"    Moving points:          {d['moving_points']} ({d['pct_moving']}%)")
    print(f"    Stationary points:      {d['stationary_points']}")
    print(f"    Course available:       {d['course_available']} ({d['pct_with_course']}%)")
    print(f"    Course missing:         {d['course_missing']}")

    print(f"\n  в”Ђв”Ђ ELEVATION в”Ђв”Ђ")
    print(f"    Min elevation:          {d['ele_min']} m")
    print(f"    Max elevation:          {d['ele_max']} m")
    print(f"    Elevation span:         {d['ele_span']} m")

    print(f"\n  в”Ђв”Ђ NOISE / JITTER в”Ђв”Ђ")
    print(f"    Max position jump:      {d['max_position_jump_m']:.2f} m (between cons. pts)")
    print(f"    Max accel (implied):    {d['max_accel_ms2']:.2f} m/sВІ ({d['max_accel_g']:.2f} g)")
    print(f"    Accel > 2g instances:   {d['instances_high_accel_gt_2g']}")
    print(f"    Accel > 5g instances:   {d['instances_high_accel_gt_5g']}")


def print_interval_distribution(stats: FileStats, bins_ms: List[float]):
    """Print histogram of interval distribution."""
    intervals = [i * 1000 for i in stats.intervals]
    counts = [0] * len(bins_ms)
    for iv in intervals:
        for j, thr in enumerate(bins_ms):
            if iv <= thr:
                counts[j] += 1
                break
    print(f"\n  в”Ђв”Ђ INTERVAL DISTRIBUTION (ms) в”Ђв”Ђ")
    for i, thr in enumerate(bins_ms):
        pct = counts[i] / max(len(intervals), 1) * 100
        bar = "#" * int(pct / 2)
        print(f"    <= {thr:>7.0f} ms: {counts[i]:>6} ({pct:5.1f}%) {bar}")


def print_comparison(s1: FileStats, s2: FileStats):
    print_header("COMPARISON SUMMARY")
    print(f"\n  {'Metric':<35} {'linear.gpx':>15} {'circle.gpx':>15}")
    print(f"  {'-'*35} {'-'*15} {'-'*15}")
    for label, key in [
        ("Points", "points"),
        ("Duration (s)", "duration_s"),
        ("Distance (km)", "total_distance_km"),
        ("Avg speed (km/h)", "avg_speed_kmh"),
        ("Max speed (km/h)", "max_speed_kmh"),
        ("Avg sampling (Hz)", "avg_sampling_hz"),
        ("Median interval (ms)", "median_interval_ms"),
        ("P95 interval (ms)", "p95_interval_ms"),
        ("Gaps >1s", "gaps_gt_1s"),
        ("Duplicate points", "duplicate_points"),
        ("Moving points %", "pct_moving"),
        ("Course available %", "pct_with_course"),
        ("Elev span (m)", "ele_span"),
        ("Max accel (g)", "max_accel_g"),
        ("Accel >5g instances", "instances_high_accel_gt_5g"),
    ]:
        v1 = s1.to_dict()[key]
        v2 = s2.to_dict()[key]
        if isinstance(v1, float):
            print(f"  {label:<35} {v1:>15.2f} {v2:>15.2f}")
        else:
            print(f"  {label:<35} {str(v1):>15} {str(v2):>15}")


def main():
    base = Path(__file__).parent / "include"
    files = ["linear.gpx", "circle.gpx"]

    all_stats: List[FileStats] = []
    for f in files:
        path = base / f
        if not path.exists():
            print(f"File not found: {path}")
            continue
        print(f"Parsing {f}... ({path.stat().st_size / 1024:.0f} KB)")
        stats = parse_gpx(str(path))
        compute_stats(stats)
        all_stats.append(stats)

    for s in all_stats:
        print_stats(s)
        print_interval_distribution(s, bins_ms=[20, 40, 60, 80, 100, 150, 200, 300, 500, 1000, 5000])

    if len(all_stats) == 2:
        print_comparison(all_stats[0], all_stats[1])

    # --- Specific analysis: Are points sampled at expected theoretical rate? ---
    print_header("SAMPLING RATE ANALYSIS")
    for s in all_stats:
        n = len(s.points)
        if n >= 2:
            intervals = sorted(s.intervals)
            # How many intervals cluster around 40ms (25Hz) vs 100ms (10Hz)?
            near_25 = sum(1 for i in intervals if 0.020 <= i <= 0.060)
            near_10 = sum(1 for i in intervals if 0.080 <= i <= 0.120)
            print(f"\n  {s.name}:")
            print(f"    Intervals near 40ms (25Hz): {near_25} ({near_25 / len(intervals) * 100:.1f}%)")
            print(f"    Intervals near 100ms (10Hz): {near_10} ({near_10 / len(intervals) * 100:.1f}%)")
            # Identify burst patterns
            burst_threshold = 0.200  # 200ms
            bursts = []
            current_burst = 0
            for i in intervals:
                if i <= burst_threshold:
                    current_burst += 1
                else:
                    if current_burst > 0:
                        bursts.append(current_burst)
                        current_burst = 0
            if current_burst > 0:
                bursts.append(current_burst)
            if bursts:
                avg_burst = sum(bursts) / len(bursts)
                print(f"    Burst patterns (<=200ms gaps): {len(bursts)} bursts, avg size: {avg_burst:.1f} pts/burst")

    print_header("RECOMMENDATIONS")
    print("""
  1. SAMPLING RATE: Actual rate much lower than configured 25Hz.
     - If GPS module is actually configured for 10Hz -> that would explain 8-9 Hz output
     - Check UBX CFG-RATE command being sent (lines 104-112 of main.cpp)
     - UART buffer at 25Hz with debug Serial.println may cause drops

  2. TIMESTAMP ISSUES: Many points share identical timestamps (same-second).
     - GPS NMEA outputs time with 1-second resolution
     - Consider using the module's precise UBX-NAV-PVT for sub-second timestamps
     - Or add a per-point millisecond counter for interpolation

  3. COURSE DATA: Significant portion of points (~45-55%) missing course.
     - Course is only in $GPRMC sentences, not all NMEA sentences
     - If the module sends RMC at lower rate than GGA, course will be sparse
     - Consider computing course from consecutive lat/lon deltas as fallback

  4. POSITION JITTER / ACCELERATION SPIKES: Implied accelerations exceed 2-5g.
     - At 25Hz with no filtering, GPS position noise creates apparent jumps
     - Add a simple moving-average or exponential filter on-device
     - Or apply median filter on 3-5 consecutive points

  5. DUPLICATE POINTS: Browser-side JS drops identical lat+lon points.
     - This loses temporal information about dwell time
     - Better approach: compute distance < threshold (e.g. 1m) instead of exact equality

  6. RAW NMEA STORAGE: Files store ALL NMEA sentences, wasting space.
     - Consider stripping unused sentences ($GPGSV, $GPGSA) before SD write
     - Or switching to binary UBX protocol with structured logging

  7. DEVICE-SIDE PROCESSING: No filtering, interpolation, or statistics computed on-device.
     - Moving all GPX conversion and filtering to ESP32 would:
       a) Reduce browser-side workload for large files
       b) Allow pre-computed statistics
       c) Enable real-time track smoothing

  8. CONFIG VERIFICATION: Verify the actual GPS module rate at runtime.
     - Read back UBX-CFG-RATE after setting to confirm module accepted the command
     - Log GPS update intervals to SD as diagnostic
""")


if __name__ == "__main__":
    main()

