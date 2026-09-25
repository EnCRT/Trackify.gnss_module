import math
import struct
import datetime
import os

def generate_trackify_bin(output_path):
    # Base location: Ternivka, Dnipropetrovsk Oblast, Ukraine
    LAT_BASE = 48.5230000
    LON_BASE = 36.0830000
    ALT_BASE = 118.0  # meters

    # Scale factors: meters to degrees
    METERS_PER_DEG_LAT = 111195.0
    METERS_PER_DEG_LON = 111195.0 * math.cos(math.radians(LAT_BASE))

    # Control waypoints defining a smooth closed race circuit (~3.9 km perimeter)
    # in local (x, y) coordinates (x = East, y = North in meters)
    control_points = [
        # Start/Finish straight (heading East)
        (-350.0, -400.0),
        (0.0, -400.0),
        (350.0, -400.0),
        (600.0, -400.0),
        # Turn 1 (fast right sweep)
        (820.0, -280.0),
        (900.0, -50.0),
        (800.0, 200.0),
        # Straight 2
        (680.0, 380.0),
        # Turn 2 - Hairpin (tight 180 left)
        (580.0, 560.0),
        (420.0, 600.0),
        (280.0, 480.0),
        # S-Chicane (Turns 3 & 4)
        (150.0, 280.0),
        (0.0, 360.0),
        (-150.0, 480.0),
        # Big carousel / fast long left curve
        (-420.0, 420.0),
        (-650.0, 220.0),
        (-750.0, -50.0),
        # Final corner (Turn 6) into main straight
        (-650.0, -280.0),
        (-500.0, -390.0),
    ]

    # Interpolate a dense smooth closed spline with high resolution
    def catmull_rom_closed(pts, num_samples_per_seg=100):
        n = len(pts)
        dense = []
        for i in range(n):
            p0 = pts[(i - 1) % n]
            p1 = pts[i]
            p2 = pts[(i + 1) % n]
            p3 = pts[(i + 2) % n]
            for step in range(num_samples_per_seg):
                t = step / float(num_samples_per_seg)
                t2 = t * t
                t3 = t2 * t
                # Catmull-Rom formulation
                x = 0.5 * ((2 * p1[0]) +
                           (-p0[0] + p2[0]) * t +
                           (2 * p0[0] - 5 * p1[0] + 4 * p2[0] - p3[0]) * t2 +
                           (-p0[0] + 3 * p1[0] - 3 * p2[0] + p3[0]) * t3)
                y = 0.5 * ((2 * p1[1]) +
                           (-p0[1] + p2[1]) * t +
                           (2 * p0[1] - 5 * p1[1] + 4 * p2[1] - p3[1]) * t2 +
                           (-p0[1] + 3 * p1[1] - 3 * p2[1] + p3[1]) * t3)
                dense.append((x, y))
        return dense

    dense_circuit = catmull_rom_closed(control_points, num_samples_per_seg=150)
    total_samples = len(dense_circuit)

    # Compute cumulative distance along circuit
    cum_dist = [0.0]
    for i in range(1, total_samples):
        dx = dense_circuit[i][0] - dense_circuit[i-1][0]
        dy = dense_circuit[i][1] - dense_circuit[i-1][1]
        cum_dist.append(cum_dist[-1] + math.hypot(dx, dy))
    
    # Loop back to close distance
    dx_close = dense_circuit[0][0] - dense_circuit[-1][0]
    dy_close = dense_circuit[0][1] - dense_circuit[-1][1]
    lap_length_m = cum_dist[-1] + math.hypot(dx_close, dy_close)

    # Calculate local curvature / curvature radius to derive natural cornering speed
    curvatures = []
    for i in range(total_samples):
        prev_pt = dense_circuit[(i - 5) % total_samples]
        curr_pt = dense_circuit[i]
        next_pt = dense_circuit[(i + 5) % total_samples]
        v1 = (curr_pt[0] - prev_pt[0], curr_pt[1] - prev_pt[1])
        v2 = (next_pt[0] - curr_pt[0], next_pt[1] - curr_pt[1])
        cross = v1[0] * v2[1] - v1[1] * v2[0]
        l1 = math.hypot(v1[0], v1[1])
        l2 = math.hypot(v2[0], v2[1])
        curv = abs(cross) / (l1 * l2 * (l1 + l2) * 0.5 + 1e-6)
        curvatures.append(curv)

    # Target speed profile around circuit (m/s) based on curvature and straights
    base_speeds = []
    for i in range(total_samples):
        c = curvatures[i]
        # In sharp turns (high curvature), speed ~ 10-12 m/s (~36-43 km/h)
        # On open straights (near 0 curvature), speed up to 36-38 m/s (~130-137 km/h)
        max_cornering_accel = 6.8  # m/s^2 lateral
        speed_lim = math.sqrt(max_cornering_accel / (c + 0.0032))
        speed_lim = min(36.5, max(10.2, speed_lim))
        base_speeds.append(speed_lim)

    # Forward-backward acceleration / braking smoothing
    # Max braking: ~5.5 m/s^2, Max accel: ~3.2 m/s^2
    smooth_speeds = list(base_speeds)
    # Forward pass (acceleration limit)
    for _ in range(2):
        for i in range(total_samples):
            idx_prev = (i - 1) % total_samples
            dist_step = cum_dist[i] - cum_dist[idx_prev] if i > 0 else (cum_dist[1] - cum_dist[0])
            if dist_step <= 0: dist_step = 0.5
            v_max = math.sqrt(smooth_speeds[idx_prev]**2 + 2 * 3.2 * dist_step)
            if smooth_speeds[i] > v_max:
                smooth_speeds[i] = v_max

        # Backward pass (braking limit)
        for i in range(total_samples - 1, -1, -1):
            idx_next = (i + 1) % total_samples
            dist_step = cum_dist[idx_next] - cum_dist[i] if i < total_samples - 1 else (cum_dist[1] - cum_dist[0])
            if dist_step <= 0: dist_step = 0.5
            v_max = math.sqrt(smooth_speeds[idx_next]**2 + 2 * 5.5 * dist_step)
            if smooth_speeds[i] > v_max:
                smooth_speeds[i] = v_max

    # Compute unscaled lap time
    raw_lap_time = 0.0
    for i in range(total_samples):
        idx_next = (i + 1) % total_samples
        ds = cum_dist[idx_next] - cum_dist[i] if i < total_samples - 1 else (lap_length_m - cum_dist[-1])
        if ds <= 0: ds = 0.5
        raw_lap_time += ds / smooth_speeds[i]

    # We want nominal lap time around 142.0 seconds (2.36 minutes)
    desired_nominal_time = 142.0 # seconds
    speed_scale = raw_lap_time / desired_nominal_time
    smooth_speeds = [v * speed_scale for v in smooth_speeds]

    # 4 distinct laps strictly in 2.3 - 2.5 min range (138 - 150 s):
    # Lap 1: ~147.5 s (2.46 min)
    # Lap 2: ~143.0 s (2.38 min)
    # Lap 3: ~139.5 s (2.33 min) - best lap
    # Lap 4: ~144.5 s (2.41 min)
    lap_multipliers = [
        desired_nominal_time / 144.5,
        desired_nominal_time / 140.0,
        desired_nominal_time / 136.5,
        desired_nominal_time / 141.5,
    ]

    FREQ_HZ = 25
    DT = 1.0 / FREQ_HZ  # 0.040s (40 ms)

    records = []
    
    # Starting GPS time & UTC
    # Friday 2026-09-25 15:00:00 UTC
    utc_start = datetime.datetime(2026, 9, 25, 15, 0, 0, 0, tzinfo=datetime.timezone.utc)
    iTOW_start = 5 * 24 * 3600 * 1000 + 15 * 3600 * 1000  # Friday 15:00 in ms of week
    
    current_time_s = 0.0
    current_dist_m = 0.0
    
    # We will simulate 4 laps
    total_laps = 4
    lap_timings = []
    
    # Interpolator for (x, y, speed, heading) given s (distance along circuit)
    def get_circuit_state(s, speed_mult):
        s_mod = s % lap_length_m
        # binary search or index search
        idx = int((s_mod / lap_length_m) * total_samples)
        idx = min(idx, total_samples - 1)
        
        pt = dense_circuit[idx]
        pt_next = dense_circuit[(idx + 1) % total_samples]
        dx = pt_next[0] - pt[0]
        dy = pt_next[1] - pt[1]
        heading_rad = math.atan2(dx, dy) # 0 = North, pi/2 = East
        heading_deg = (math.degrees(heading_rad) + 360.0) % 360.0
        
        speed = smooth_speeds[idx] * speed_mult
        return pt[0], pt[1], speed, heading_deg

    print(f"Generating circuit: length = {lap_length_m:.1f} m")

    for lap_idx, mult in enumerate(lap_multipliers, start=1):
        lap_start_time = current_time_s
        lap_dist_start = current_dist_m
        
        while (current_dist_m - lap_dist_start) < lap_length_m:
            x, y, speed, heading = get_circuit_state(current_dist_m, mult)
            
            # Subtle elevation change (+- 3.5m over the lap)
            elevation = ALT_BASE + 3.2 * math.sin(2.0 * math.pi * ((current_dist_m - lap_dist_start) / lap_length_m))
            
            # Convert local (x, y) to WGS84 lat/lon
            lat = LAT_BASE + (y / METERS_PER_DEG_LAT)
            lon = LON_BASE + (x / METERS_PER_DEG_LON)
            
            # Simulated GNSS receiver metrics (high precision u-blox M10)
            hAcc_m = 0.70 + 0.15 * math.sin(current_time_s * 0.1) # 0.55m - 0.85m
            sAcc_ms = 0.08 + 0.04 * math.sin(current_time_s * 0.15)
            num_sv = 18 + int(3 * math.sin(current_time_s * 0.05)) # 16 to 21 satellites
            fix_type = 3 # 3D Fix
            
            iTOW_curr = int(iTOW_start + current_time_s * 1000.0)
            nano_curr = int((current_time_s * 1e9) % 1e9)
            if nano_curr > 500000000:
                nano_curr -= 1000000000
                
            records.append({
                'iTOW': iTOW_curr,
                'nano': nano_curr,
                'lat': int(round(lat * 1e7)),
                'lon': int(round(lon * 1e7)),
                'height': int(round(elevation * 1000.0)),
                'gSpeed': int(round(speed * 1000.0)),
                'headMot': int(round(heading * 1e5)),
                'hAcc': int(round(hAcc_m * 1000.0)),
                'sAcc': int(round(sAcc_ms * 1000.0)),
                'numSV': num_sv,
                'fixType': fix_type
            })
            
            # Advance simulation by 1 tick (40 ms)
            step_m = speed * DT
            current_dist_m += step_m
            current_time_s += DT
            
        lap_duration = current_time_s - lap_start_time
        lap_timings.append(lap_duration)
        print(f"  Lap {lap_idx}: {lap_duration:.2f} s ({lap_duration/60.0:.2f} min), avg speed: {(lap_length_m / lap_duration)*3.6:.1f} km/h")

    total_records = len(records)
    print(f"Total records: {total_records}, total time: {current_time_s:.2f} s ({current_time_s/60.0:.2f} min)")

    # Prepare binary output
    with open(output_path, 'wb') as f:
        # 1. Header (UTF-8)
        utc_str = utc_start.strftime("%Y-%m-%dT%H:%M:%S.000000000Z")
        header = f"#TRACKIFY:VER=2;FREQ={FREQ_HZ};UTC={utc_str};PROTO=UBX-NAV-PVT\n"
        f.write(header.encode('utf-8'))
        
        # 2. LogMeta (64 bytes)
        meta = bytearray(64)
        def write_str(offset, s, max_len=16):
            b = s.encode('utf-8')[:max_len]
            meta[offset:offset+len(b)] = b

        write_str(0, "Trackify MX")
        write_str(16, "TRK-TERNOVKA")
        write_str(32, "Trackify GNSS")
        meta[48] = 1 # hardware_revision
        meta[49] = 0 # reserved
        struct.pack_into('<H', meta, 50, 0x0200) # firmware v2.0
        meta[52:58] = bytes([0x24, 0x6F, 0x28, 0x88, 0x44, 0x12]) # MAC
        struct.pack_into('<H', meta, 58, FREQ_HZ) # sample_rate_hz
        struct.pack_into('<I', meta, 60, total_records) # record_count
        f.write(meta)
        
        # 3. TrackRecords (38 bytes each)
        # struct format: '<I i i i i i i I I B B'
        rec_struct = struct.Struct('<I i i i i i i I I B B')
        for r in records:
            packed = rec_struct.pack(
                r['iTOW'],
                r['nano'],
                r['lat'],
                r['lon'],
                r['height'],
                r['gSpeed'],
                r['headMot'],
                r['hAcc'],
                r['sAcc'],
                r['numSV'],
                r['fixType']
            )
            f.write(packed)

    file_size = os.path.getsize(output_path)
    print(f"File successfully created: {output_path} ({file_size} bytes, {file_size / (1024*1024):.2f} MB)")
    return lap_timings

if __name__ == '__main__':
    out_file = r'd:\ESP32\Trackify.gnss_module\log_ternovka_4laps.bin'
    generate_trackify_bin(out_file)
