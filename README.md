# MOSA_MIC_PROJ

### Dual INMP441 Microphone Recording System for ESP32-S3

A real-time audio capture and analysis platform that records CD-quality stereo WAV files from two INMP441 MEMS microphones using the Seeed Studio XIAO ESP32-S3.

Built for audio research, noise measurement, and embedded sound projects.

---

## Features

- **Dual Microphone Capture** -- Two independent I2S buses, mono or stereo recording
- **CD-Quality Audio** -- 44.1 kHz, 16-bit PCM with hardware APLL clock
- **HP-Filtered WAV Output** -- DC offset removal applied during recording for clean files
- **Auto Calibration** -- Measures noise floor in silence, sets gate thresholds automatically
- **Live Dashboard** -- Real-time RMS, SNR (dB), peak levels, and noise gate status
- **Serial Plotter** -- CSV waveform output compatible with Arduino IDE Serial Plotter
- **SNR Metering** -- Signal-to-noise ratio computed against calibrated noise floor
- **One-Command Recording** -- Python script handles serial handshake, capture, and WAV save

---

## Hardware Requirements

| Component | Quantity |
|-----------|:--------:|
| Seeed Studio XIAO ESP32-S3 | 1 |
| INMP441 I2S MEMS Microphone | 2 |
| USB-C Cable (shielded recommended) | 1 |
| Breadboard + Jumper Wires | -- |

### Wiring

```
         INMP441 Mic 1              XIAO ESP32-S3            INMP441 Mic 2
        ┌───────────┐              ┌─────────────┐           ┌───────────┐
        │  SCK ─────┼──────────────┤ GPIO5       │           │           │
        │  WS  ─────┼──────────────┤ GPIO43      │           │           │
        │  SD  ─────┼──────────────┤ GPIO6       │           │           │
        │  L/R ─────┼──────────────┤ GPIO44      │           │           │
        │  VDD ─────┼──── 3.3V ───┤             │           │           │
        │  GND ─────┼──── GND ────┤             ├── GPIO9 ──┼── SCK     │
        └───────────┘              │             ├── GPIO7 ──┼── WS      │
                                   │             ├── GPIO8 ──┼── SD      │
                                   │             ├── GPIO4 ──┼── L/R     │
                                   │             ├── 3.3V ───┼── VDD     │
                                   │             ├── GND ────┼── GND     │
                                   └─────────────┘           └───────────┘
```

> Set both L/R pins LOW for left-channel data on each I2S bus.

---

## Getting Started

### 1. Flash the Firmware

- Open `MOSA_MIC_PROJ.ino` in Arduino IDE
- Install the **esp32** board package (by Espressif) via Board Manager
- Select board: **XIAO ESP32-S3**
- Set Serial Monitor baud to **2,000,000**
- Upload

### 2. Calibrate

Open Serial Monitor. Calibration runs automatically on boot. For best results, recalibrate in silence:

```
Send: C
```

### 3. Record a WAV

Close Serial Monitor first, then:

```bash
pip install pyserial
python wav_recorder.py
```

Output: `Test_voice/rec_20260326_143022_BOTH_stereo_44100Hz.wav`

---

## Usage

### Serial Commands

| Key | Action |
|:---:|--------|
| `1` | Select Mic 1 only |
| `2` | Select Mic 2 only |
| `B` | Select Both (stereo) |
| `D` | Dashboard mode -- live metrics table |
| `P` | Plotter mode -- CSV for Serial Plotter |
| `W` | WAV stream mode -- binary PCM to PC |
| `C` | Calibrate -- run in silence |
| `H` | Help |

### Python Recorder

```bash
# Both mics, 10 seconds, auto-detect port
python wav_recorder.py

# Mic 1 only, 20 seconds
python wav_recorder.py --mic 1 --dur 20

# Specific port
python wav_recorder.py --port COM5 --mic B --dur 15
```

| Option | Default | Description |
|--------|---------|-------------|
| `--port` | Auto | Serial port (COM3, /dev/ttyUSB0, etc.) |
| `--mic` | `B` | `1`, `2`, or `B` (both) |
| `--dur` | `10` | Duration in seconds |

### Dashboard Output Example

```
═══ BOTH ═══  [1][2][B] [C]al [P]lot [W]av [H]elp
Mic │ Gate │ DC(raw)│ HP RMS│Smooth│  SNR  │ Min~Max │ Level
────┼──────┼────────┼───────┼──────┼───────┼─────────┼─────────
 M1 │ OPEN │  -12   │  847  │  790 │ 18.3dB│ -2041~ 1876│ ######---
 M2 │ shut │   -8   │   45  │   42 │  0.2dB│   -98~   87│ ---------
```

---

## Audio Specs

| Parameter | Value |
|-----------|-------|
| Sample Rate | 44,100 Hz |
| Bit Depth | 16-bit signed PCM |
| Clock | APLL (hardware PLL) |
| HP Filter | First-order IIR, alpha=0.995 |
| Noise Gate | Calibrated RMS x 1.5 margin |
| Serial Baud | 2,000,000 |
| WAV Format | Standard RIFF/WAV |

---

## Signal Processing

```
Mic ──► I2S (32-bit) ──► >>16 (to 16-bit) ──► HP Filter ──► WAV / Dashboard
                                                   │
                                            Removes DC offset
                                            Prevents "muffled" audio
```

- **HP Filter** removes DC bias from the MEMS microphone output
- **Calibration** measures noise floor RMS, peak, and DC offset over 80 rounds
- **Noise Gate** suppresses output when signal is below threshold
- **SNR** is computed as `20 * log10(signal_RMS / noise_RMS)` in dB

---

## Project Structure

```
MOSA_MIC_PROJ/
├── MOSA_MIC_PROJ.ino      # ESP32-S3 firmware (Arduino C++)
├── wav_recorder.py         # PC recording script (Python 3)
├── mic_test.py             # XIAO RP2040 single-mic test (MicroPython)
├── PROJECT.md              # Detailed technical documentation
├── README.md               # This file
├── Test_voice/             # WAV recordings output folder
└── mic test_wrongs/        # Early test recordings (16 kHz)
```

---

## Dependencies

**Firmware:**
- Arduino IDE 2.x
- ESP32 board package by Espressif
- No external libraries required

**Python:**
- Python 3.7+
- `pyserial` (`pip install pyserial`)

---

## Troubleshooting

| Problem | Solution |
|---------|----------|
| No serial port found | Close Arduino Serial Monitor before running `wav_recorder.py` |
| Garbled audio / clicks | Use a shielded USB cable; try lowering baud to `1500000` in both files |
| High noise on one mic | Check wiring; run calibration (`C`); check the WARN message |
| Short/empty WAV file | Increase `--dur`; ensure baud rates match between firmware and Python |
| `DATA_BEGIN` timeout | Re-flash firmware; verify board is XIAO ESP32-S3 |

---

## License

MIT

---

## Acknowledgments

Built with the Seeed Studio XIAO ESP32-S3 and InvenSense INMP441 MEMS microphones.
