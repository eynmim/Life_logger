"""
WAV Recorder — Captures audio from ESP32 dual INMP441.

Usage:
  python wav_recorder.py                  # Both mics, 10 sec
  python wav_recorder.py --mic 2 --dur 5  # Mic2 only, 5 sec
  python wav_recorder.py --port COM5      # Specify port

IMPORTANT: Close Serial Monitor before running!
Requirements: pip install pyserial
"""

import serial
import serial.tools.list_ports
import wave
import sys
import time
import os
import argparse

BAUD = 2000000  # Must match Arduino SERIAL_BAUD (2 Mbaud for 44.1kHz)

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

def record(port, mic_mode='B', duration=10):
    print()
    print("  ╔═════════════════════════════════════╗")
    print("  ║   Dual INMP441 WAV Recorder         ║")
    print("  ╚═════════════════════════════════════╝")
    print(f"  Port: {port}  Baud: {BAUD}")
    print(f"  Mic:  {'Both' if mic_mode=='B' else 'Mic'+mic_mode}")
    print(f"  Duration: {duration}s")
    print()

    try:
        ser = serial.Serial(port, BAUD, timeout=2)
    except serial.SerialException as e:
        print(f"  [ERROR] Cannot open {port}: {e}")
        print(f"  >> Close Serial Monitor first! <<")
        return

    # Wait for board to finish calibration after boot (~6 seconds)
    print(f"  [0/4] Waiting for calibration to finish...")
    time.sleep(6)
    ser.reset_input_buffer()

    # Set mic mode
    print(f"  [1/4] Setting mic: {mic_mode}")
    ser.write(mic_mode.encode())
    time.sleep(0.3)
    ser.reset_input_buffer()

    # Send W and immediately start reading (don't sleep!)
    print(f"  [2/4] Sending WAV command...")
    ser.write(b'W')

    # Read response, send go signal when prompted
    print(f"  [3/4] Waiting for ESP32...")
    sample_rate = 44100
    channels = 1
    started = False
    go_sent = False
    timeout_t = time.time() + 20
    buf = b''

    while time.time() < timeout_t:
        avail = ser.in_waiting
        if avail > 0:
            buf += ser.read(avail)
        elif not started:
            time.sleep(0.01)
            continue

        # Process complete lines from buffer
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

            # Debug: show what we receive
            if not line.startswith((' M1', ' M2', '---', 'Mic', '===')):
                print(f"         << {line}")

            if not go_sent and ("send any key" in line.lower() or "start python" in line.lower()):
                time.sleep(0.1)
                ser.write(b'G')
                go_sent = True
                print(f"         >> Sent GO signal")
                continue

            if line == "WAV_START":
                pass
            elif line.startswith("RATE:"):
                sample_rate = int(line.split(":")[1])
            elif line.startswith("CHANNELS:"):
                channels = int(line.split(":")[1])
            elif line.startswith("DURATION:"):
                pass
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

    ch_label = "stereo" if channels == 2 else "mono"
    expected_bytes = sample_rate * channels * 2 * duration

    print(f"  [4/4] RECORDING — SPEAK NOW!")
    print(f"         {duration}s, {sample_rate}Hz, {ch_label}")
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

            # Check for end marker
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

        # Progress
        pct = min(int(len(audio_data) / max(expected_bytes, 1) * 100), 100)
        if pct != last_pct and pct % 10 == 0:
            last_pct = pct
            bar = '#' * (pct // 5) + '-' * (20 - pct // 5)
            secs = len(audio_data) / max(sample_rate * channels * 2, 1)
            elapsed_str = f"{elapsed:.1f}s"
            print(f"  [{bar}] {pct:3d}%  audio:{secs:.1f}s  elapsed:{elapsed_str}")

        time.sleep(0.001)

    ser.close()

    if len(audio_data) < 100:
        print("\n  [ERROR] No audio data captured!")
        return

    # Save to Test_voice folder
    timestamp = time.strftime("%Y%m%d_%H%M%S")
    mic_label = "BOTH" if mic_mode == 'B' else f"MIC{mic_mode}"
    filename = f"rec_{timestamp}_{mic_label}_{ch_label}_{sample_rate}Hz.wav"
    save_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "Test_voice")
    os.makedirs(save_dir, exist_ok=True)
    filepath = os.path.join(save_dir, filename)

    with wave.open(filepath, 'w') as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(bytes(audio_data))

    actual_dur = len(audio_data) / (sample_rate * channels * 2)
    transfer_time = time.time() - start_time

    print()
    print("  ┌─────────────────────────────────────┐")
    print("  │          RECORDING SAVED!            │")
    print("  ├─────────────────────────────────────┤")
    print(f"  │  File:     {filename}")
    print(f"  │  Size:     {os.path.getsize(filepath):,} bytes")
    print(f"  │  Duration: {actual_dur:.1f} seconds")
    print(f"  │  Transfer: {transfer_time:.1f}s for {duration}s audio")
    print(f"  │  Format:   {sample_rate}Hz 16bit {ch_label}")
    print("  └─────────────────────────────────────┘")
    print()

    if sys.platform == 'win32':
        print(f"  Playing {filename}...")
        try:
            os.startfile(filepath)
        except:
            print(f"  Open: {filepath}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Record WAV from ESP32 INMP441")
    parser.add_argument("--port", default=None, help="Serial port (e.g. COM3)")
    parser.add_argument("--mic", default="B", choices=["1","2","B"], help="1, 2, or B(oth)")
    parser.add_argument("--dur", type=int, default=10, help="Seconds to record")
    args = parser.parse_args()

    port = args.port or find_port()
    if not port:
        print("  [ERROR] No serial port found. Use --port COM3")
        sys.exit(1)

    record(port, args.mic, args.dur)
