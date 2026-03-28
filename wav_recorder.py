"""
WAV Recorder — Captures audio from ESP32 dual INMP441.

Usage:
  python wav_recorder.py                  # Processed mono, 10 sec
  python wav_recorder.py --ab             # A/B stereo (raw vs processed)
  python wav_recorder.py --mic 2 --dur 5  # Mic2 only, 5 sec
  python wav_recorder.py --port COM5      # Specify port

IMPORTANT: Close Serial Monitor before running!
Requirements: pip install pyserial
"""

import serial
import serial.tools.list_ports
import wave
import struct
import sys
import time
import os
import argparse

BAUD = 2000000

def find_port():
    ports = serial.tools.list_ports.comports()
    for p in ports:
        desc = (p.description or "").lower()
        hwid = (p.hwid or "").lower()
        if any(k in desc for k in ["cp210", "ch340", "usb", "serial", "esp"]):
            return p.device
        if any(k in hwid for k in ["1a86", "10c4", "303a"]):
            return p.device
    return ports[0].device if ports else None

def record(port, mic_mode='B', duration=10, ab_mode=False):
    mode_label = "A/B (raw vs processed)" if ab_mode else ("Both" if mic_mode=='B' else f"Mic{mic_mode}")
    cmd_char = 'A' if ab_mode else 'W'

    print()
    print("  ╔═════════════════════════════════════╗")
    print("  ║   MOSA Voice Recorder               ║")
    print("  ╚═════════════════════════════════════╝")
    print(f"  Port: {port}  Baud: {BAUD}")
    print(f"  Mode: {mode_label}")
    print(f"  Duration: {duration}s")
    print()

    try:
        ser = serial.Serial(port, BAUD, timeout=2)
    except serial.SerialException as e:
        print(f"  [ERROR] Cannot open {port}: {e}")
        print(f"  >> Close Serial Monitor first! <<")
        return

    # Wait for board to be ready — must see actual dashboard lines (M1/M2)
    print(f"  [0/5] Waiting for board to be ready (AFE init may take 15s)...")
    ready = False
    wait_start = time.time()
    dashboard_hits = 0
    while time.time() - wait_start < 45:
        avail = ser.in_waiting
        if avail > 0:
            data = ser.read(avail)
            if b' M1 ' in data or b' M2 ' in data:
                dashboard_hits += 1
                if dashboard_hits >= 3:
                    ready = True
                    elapsed = time.time() - wait_start
                    print(f"  Board ready ({elapsed:.1f}s)")
                    break
            try:
                txt = data.decode('utf-8', errors='replace').strip()
                for line in txt.split('\n'):
                    line = line.strip()
                    if line and not line.startswith((' M1', ' M2', '---', 'Mic')):
                        if 'MOSA' in line or 'AFE' in line or 'I (' in line or 'calibrat' in line.lower():
                            print(f"         .. {line[:70]}")
            except:
                pass
        time.sleep(0.05)

    if not ready:
        print("  [WARN] No dashboard output seen — trying anyway...")

    time.sleep(1.0)
    ser.reset_input_buffer()

    # Set mic mode
    print(f"  [1/5] Setting mic: {mic_mode}")
    ser.write(mic_mode.encode())
    time.sleep(0.5)
    ser.reset_input_buffer()

    # Send command
    print(f"  [2/5] Sending {'A/B' if ab_mode else 'WAV'} command...")
    ser.write(cmd_char.encode())
    time.sleep(0.1)

    # Wait for handshake
    print(f"  [3/5] Waiting for ESP32...")
    sample_rate = 16000
    channels = 1
    is_ab = False
    started = False
    go_sent = False
    timeout_t = time.time() + 30
    buf = b''

    while time.time() < timeout_t:
        avail = ser.in_waiting
        if avail > 0:
            buf += ser.read(avail)
        elif not started:
            time.sleep(0.01)
            continue

        while b'\n' in buf:
            idx = buf.index(b'\n')
            raw_line = buf[:idx]
            buf = buf[idx+1:]
            try:
                line = raw_line.decode('utf-8', errors='replace').strip()
            except:
                continue
            if not line:
                continue

            if not line.startswith((' M1', ' M2', '---', 'Mic', '===')):
                print(f"         << {line}")

            if not go_sent and ("send any key" in line.lower() or "start python" in line.lower()):
                time.sleep(0.1)
                ser.write(b'G')
                go_sent = True
                print(f"         >> Sent GO signal")
                continue

            if line.startswith("RATE:"):
                sample_rate = int(line.split(":")[1])
            elif line.startswith("CHANNELS:"):
                channels = int(line.split(":")[1])
            elif line == "MODE:AB":
                is_ab = True
            elif line == "DATA_BEGIN":
                started = True
                break

        if started:
            break

    if not started:
        print("  [ERROR] Timeout — no DATA_BEGIN received")
        print("  Make sure the firmware is running (check serial monitor first).")
        if not go_sent:
            print("  The WAV prompt was never seen — board may still be calibrating.")
        ser.close()
        return

    ch_label = "stereo_AB" if is_ab else ("stereo" if channels == 2 else "mono")
    expected_bytes = sample_rate * channels * 2 * duration

    print(f"  [4/5] RECORDING — SPEAK NOW!")
    print(f"         {duration}s, {sample_rate}Hz, {ch_label}")
    if is_ab:
        print(f"         Left=RAW (unprocessed)  Right=PROCESSED (AFE+post)")
    print(f"         Expected: {expected_bytes:,} bytes\n")

    # Collect PCM data
    audio_data = bytearray()
    start_time = time.time()
    last_pct = -1

    while True:
        elapsed = time.time() - start_time
        if elapsed > duration + 8:
            print(f"\n  Timeout after {elapsed:.0f}s — saving what we have")
            break

        avail = ser.in_waiting
        if avail > 0:
            chunk = ser.read(min(avail, 65536))
            for marker in [b"\nDATA_END", b"DATA_END"]:
                pos = chunk.find(marker)
                if pos >= 0:
                    audio_data.extend(chunk[:pos])
                    print(f"\n  Done! Got DATA_END")
                    started = False
                    break
            if not started:
                break
            audio_data.extend(chunk)

        pct = min(int(len(audio_data) / max(expected_bytes, 1) * 100), 100)
        if pct != last_pct and pct % 10 == 0:
            last_pct = pct
            bar = '#' * (pct // 5) + '-' * (20 - pct // 5)
            secs = len(audio_data) / max(sample_rate * channels * 2, 1)
            print(f"  [{bar}] {pct:3d}%  audio:{secs:.1f}s  elapsed:{elapsed:.1f}s")

        time.sleep(0.001)

    ser.close()

    if len(audio_data) < 100:
        print("\n  [ERROR] No audio data captured!")
        return

    # Save to Test_voice folder
    timestamp = time.strftime("%Y%m%d_%H%M%S")
    mic_label = "AB" if is_ab else ("BOTH" if mic_mode == 'B' else f"MIC{mic_mode}")
    save_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "Test_voice")
    os.makedirs(save_dir, exist_ok=True)

    # Save main file
    ch_suffix = "stereo" if channels == 2 else "mono"
    filename = f"rec_{timestamp}_{mic_label}_{ch_suffix}_{sample_rate}Hz.wav"
    filepath = os.path.join(save_dir, filename)

    with wave.open(filepath, 'w') as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(bytes(audio_data))

    actual_dur = len(audio_data) / (sample_rate * channels * 2)
    transfer_time = time.time() - start_time

    print()
    print("  ┌─────────────────────────────────────────────┐")
    print("  │           RECORDING SAVED!                   │")
    print("  ├─────────────────────────────────────────────┤")
    print(f"  │  File:     {filename}")
    print(f"  │  Size:     {os.path.getsize(filepath):,} bytes")
    print(f"  │  Duration: {actual_dur:.1f} seconds")
    print(f"  │  Transfer: {transfer_time:.1f}s for {duration}s audio")
    print(f"  │  Format:   {sample_rate}Hz 16bit {ch_suffix}")

    # If A/B mode, split into two mono files
    if is_ab and channels == 2:
        nframes = len(audio_data) // (channels * 2)
        samples = struct.unpack(f'<{nframes * 2}h', audio_data[:nframes * 4])

        raw_samples = samples[0::2]     # Left channel = raw
        proc_samples = samples[1::2]    # Right channel = processed

        raw_file = f"rec_{timestamp}_RAW_mono_{sample_rate}Hz.wav"
        proc_file = f"rec_{timestamp}_PROCESSED_mono_{sample_rate}Hz.wav"

        for fname, samps in [(raw_file, raw_samples), (proc_file, proc_samples)]:
            fpath = os.path.join(save_dir, fname)
            with wave.open(fpath, 'w') as wf:
                wf.setnchannels(1)
                wf.setsampwidth(2)
                wf.setframerate(sample_rate)
                wf.writeframes(struct.pack(f'<{len(samps)}h', *samps))

        print(f"  │")
        print(f"  │  Split into:")
        print(f"  │    RAW:       {raw_file}")
        print(f"  │    PROCESSED: {proc_file}")

    print("  └─────────────────────────────────────────────┘")
    print()

    if sys.platform == 'win32' and not is_ab:
        print(f"  Playing {filename}...")
        try:
            os.startfile(filepath)
        except:
            pass

    return filepath if is_ab else None

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="MOSA Voice Recorder")
    parser.add_argument("--port", default=None, help="Serial port (e.g. COM3)")
    parser.add_argument("--mic", default="B", choices=["1","2","B"], help="1, 2, or B(oth)")
    parser.add_argument("--dur", type=int, default=10, help="Seconds to record")
    parser.add_argument("--ab", action="store_true", help="A/B mode: raw vs processed stereo")
    args = parser.parse_args()

    port = args.port or find_port()
    if not port:
        print("  [ERROR] No serial port found. Use --port COM3")
        sys.exit(1)

    record(port, args.mic, args.dur, args.ab)
