# MOSA_MIC_PROJ

**Microphone Open Sound Architecture - Dual INMP441 Recording System**

A dual-microphone audio capture and analysis system built on the Seeed Studio XIAO ESP32-S3. It records CD-quality WAV files from two INMP441 MEMS microphones, streams real-time audio metrics, and provides signal processing tools for noise-robust measurement.

---

## Hardware

| Component | Details |
|-----------|---------|
| **MCU** | Seeed Studio XIAO ESP32-S3 |
| **Mic 1** | INMP441 MEMS (I2S0) |
| **Mic 2** | INMP441 MEMS (I2S1) |
| **Interface** | USB Serial at 2 Mbaud |

### Pin Mapping

| Signal | Mic 1 (I2S0) | Mic 2 (I2S1) |
|--------|:------------:|:------------:|
| SCK | GPIO5 | GPIO9 |
| WS | GPIO43 | GPIO7 |
| SD | GPIO6 | GPIO8 |
| L/R | GPIO44 | GPIO4 |

---

## Audio Specifications

| Parameter | Value |
|-----------|-------|
| Sample Rate | 44,100 Hz (CD quality) |
| Bit Depth | 16-bit signed PCM |
| I2S Resolution | 32-bit (downsampled to 16-bit) |
| Channels | Mono (Mic1 or Mic2) or Stereo (both) |
| Clock Source | APLL (hardware PLL for precise timing) |
| DMA Buffers | 8 x 512 samples |
| Read Buffer | 1024 samples per cycle |
| Output Format | Standard WAV (RIFF) |

---

## Signal Processing Pipeline

```
INMP441 Mic ──► I2S (32-bit) ──► Downsample (>>16) ──► HP Filter ──► Output
                                                           │
                                                    DC offset removal
                                                    (alpha = 0.995)
```

### Processing Features

- **High-Pass Filter** -- First-order IIR (alpha=0.995) removes DC offset for clean audio
- **RMS Calculation** -- AC component power measurement
- **Peak Detection** -- Absolute peak tracking per buffer
- **Noise Gate** -- Threshold-based voice activity detection (calibrated noise floor x 1.5)
- **SNR Metering** -- Real-time signal-to-noise ratio in dB
- **Exponential Smoothing** -- Smooth AC level display (alpha=0.85)
- **Calibration** -- 80-round silence measurement to establish noise floor and DC offset

---

## Operating Modes

Interactive serial commands switch between modes:

| Command | Mode | Description |
|:-------:|------|-------------|
| `1` | Mic 1 | Monitor/record single microphone |
| `2` | Mic 2 | Monitor/record second microphone |
| `B` | Both | Stereo monitoring/recording |
| `D` | Dashboard | Live metrics with level bars (default) |
| `P` | Plotter | CSV output for Arduino Serial Plotter |
| `W` | WAV Stream | Binary PCM recording to PC |
| `C` | Calibrate | Noise floor calibration (requires silence) |
| `H` | Help | Show command reference |

### Dashboard Mode (D)
Displays a live table with gate status, DC offset, HP-filtered RMS, smoothed level, SNR (dB), peak range, and a visual level bar for each active mic.

### Plotter Mode (P)
Outputs HP-filtered samples as CSV for the Arduino IDE Serial Plotter. Decimated (every 8th sample) to avoid flooding the serial link.

### WAV Stream Mode (W)
Streams HP-filtered 16-bit PCM over serial using a binary protocol. A companion Python script captures and saves as `.wav`.

---

## WAV Recording Protocol

```
ESP32 sends:
  "WAV_START\n"
  "RATE:44100\n"
  "BITS:16\n"
  "CHANNELS:2\n"        (or 1 for mono)
  "DURATION:10\n"
  "DATA_BEGIN\n"
  <binary 16-bit PCM samples, little-endian>
  "\nDATA_END\n"
```

The HP filter is applied during WAV streaming to remove DC offset, producing clean recordings without post-processing.

---

## Project Files

```
MOSA_MIC_PROJ/
  ├── MOSA_MIC_PROJ.ino      ESP32-S3 firmware (Arduino C++)
  ├── wav_recorder.py         PC-side WAV capture script (Python)
  ├── mic_test.py             XIAO RP2040 intensity test (MicroPython)
  ├── PROJECT.md              This file
  └── mic test_wrongs/        Early test recordings (16 kHz, pre-HP filter)
```

### MOSA_MIC_PROJ.ino
Main firmware. Configures dual I2S interfaces, runs calibration on boot, handles serial commands, and implements all operating modes including HP-filtered WAV streaming.

### wav_recorder.py
Python companion script that connects to the ESP32 via serial, triggers WAV mode, captures the binary PCM stream, and writes a standard WAV file with progress display.

```bash
# Record both mics, 10 seconds (default)
python wav_recorder.py

# Mic 2 only, 5 seconds, specific port
python wav_recorder.py --mic 2 --dur 5 --port COM5
```

**CLI Options:**

| Flag | Default | Description |
|------|---------|-------------|
| `--port` | Auto-detect | Serial port (e.g. COM3) |
| `--mic` | `B` | `1`, `2`, or `B` (both/stereo) |
| `--dur` | `10` | Recording duration in seconds |

**Output:** `rec_YYYYMMDD_HHMMSS_[mono|stereo]_44100Hz.wav`

### mic_test.py
MicroPython script for XIAO RP2040. Reads a single INMP441 mic via I2S, computes mean absolute intensity, and visualizes it on the onboard NeoPixel LED with color-mapped brightness.

---

## Dependencies

### Firmware (Arduino IDE)
- **Board**: `esp32` by Espressif (Board Manager)
- **Board Selection**: Seeed Studio XIAO ESP32-S3
- **Libraries**: `driver/i2s.h` (included with ESP32 core), `math.h`
- **Serial Monitor Baud**: 2,000,000

### Python (PC)
- **Python** 3.7+
- **pyserial**: `pip install pyserial`
- **wave**: Standard library (no install needed)

---

## Quick Start

1. **Wire** two INMP441 microphones to the ESP32-S3 per the pin mapping above
2. **Flash** `MOSA_MIC_PROJ.ino` via Arduino IDE (select XIAO ESP32-S3, baud 2000000)
3. **Open Serial Monitor** at 2,000,000 baud -- calibration runs automatically
4. **Calibrate** in silence by sending `C`
5. **Record**: Close Serial Monitor, then run:
   ```bash
   python wav_recorder.py
   ```
6. **Play** the output WAV file -- it opens automatically on Windows

---

## Recording Tips

- Always **calibrate** (`C`) before recording -- it measures your noise floor
- Keep mics **away from the ESP32 board** to avoid electrical noise pickup
- Use a **shielded USB cable** -- cheap cables introduce noise at 2 Mbaud
- If 2 Mbaud causes serial errors, lower `SERIAL_BAUD` to `1500000` in both `.ino` and `.py`
- For the cleanest recordings, ensure a **quiet environment** during calibration

---

## Changelog

| Date | Change |
|------|--------|
| 2026-03-23 | Initial dual-mic system with dashboard, plotter, WAV streaming at 16 kHz |
| 2026-03-26 | Upgraded to 44.1 kHz, APLL clock, HP filter on WAV output, 2 Mbaud serial |
