# MOSA_MIC_PROJ

### Dual INMP441 Beamformed Noise Reduction System for ESP32-S3

A real-time voice extraction system that uses two side-by-side INMP441 MEMS microphones with beamforming and **hardware-accelerated FFT spectral subtraction** to isolate speech from environmental noise. Built on **ESP-IDF** with the **esp-dsp** library for maximum DSP performance on the ESP32-S3.

---

## Features

- **Hardware-Accelerated FFT** -- esp-dsp uses ESP32-S3 vector DSP extensions for fast FFT/IFFT
- **Dual-Mic Beamforming** -- Averages both mics for +3 dB SNR improvement
- **FFT Spectral Subtraction** -- Removes noise in the frequency domain using a calibrated noise profile
- **CD-Quality Audio** -- 44.1 kHz, 16-bit PCM with hardware APLL clock
- **ESP-IDF Native** -- Direct access to FreeRTOS, USB Serial/JTAG, and all ESP32-S3 hardware
- **Auto Calibration** -- Measures noise floor + full noise spectrum in silence
- **Clean Mono Output** -- Beamformed and noise-reduced WAV, ready to use
- **Live Dashboard** -- Real-time RMS, SNR (dB), peak levels, and noise gate status

---

## How It Works

```
Mic1 --> HP Filter --+
                     +--> Average --> FFT --> Spectral --> IFFT --> OLA --> Clean Mono WAV
Mic2 --> HP Filter --+  (beamform)  (HW)   Subtraction   (HW)
                                                ^
                                         Noise spectrum
                                       (from calibration)
```

1. Both mics capture audio via separate I2S buses
2. HP filter removes DC offset from each mic
3. Signals are averaged (beamformed) -- speech adds constructively
4. **esp-dsp hardware-accelerated** 512-point FFT transforms to frequency domain
5. Calibrated noise magnitude is subtracted per frequency bin
6. Hardware-accelerated IFFT + overlap-add reconstructs clean audio
7. Result streams as mono 16-bit PCM to the Python recorder

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
        +-------------+            +---------------+         +-------------+
        |  SCK --------+------------| GPIO5         |         |             |
        |  WS  --------+------------| GPIO43        |         |             |
        |  SD  --------+------------| GPIO6         |         |             |
        |  L/R --------+------------| GPIO44        |         |             |
        |  VDD --------+--- 3.3V --+|               |         |             |
        |  GND --------+--- GND ---+|               +- GPIO9 -+-- SCK       |
        +-------------+            |               +- GPIO7 -+-- WS        |
                                   |               +- GPIO8 -+-- SD        |
                                   |               +- GPIO4 -+-- L/R       |
                                   |               +- 3.3V --+-- VDD       |
                                   |               +- GND ---+-- GND       |
                                   +---------------+         +-------------+
```

> Place both mics **side-by-side**, close together, facing the speaker. Set both L/R pins LOW.

---

## Getting Started

### 1. Install PlatformIO

- Install [VS Code](https://code.visualstudio.com/)
- Install the [PlatformIO extension](https://platformio.org/install/ide?install=vscode)

### 2. Open and Build

- Open the `MOSA_MIC_PROJ/` folder in VS Code
- PlatformIO will auto-detect `platformio.ini` and download:
  - ESP-IDF framework
  - esp-dsp component (from `src/idf_component.yml`)
- Click **Build** (checkmark icon) or run `pio run`

### 3. Flash

- Connect the XIAO ESP32-S3 via USB-C
- Click **Upload** (arrow icon) or run `pio run -t upload`

### 4. Calibrate

- Open PlatformIO Serial Monitor (2,000,000 baud)
- Calibration runs automatically on boot -- **keep silent!**
- To recalibrate: send `C`

### 5. Record a Clean WAV

Close the serial monitor, then:

```bash
pip install pyserial
python wav_recorder.py
```

Output: `Test_voice/rec_20260326_143022_BOTH_mono_44100Hz.wav`

---

## Usage

### Serial Commands

| Key | Action |
|:---:|--------|
| `1` | Select Mic 1 (dashboard/plotter only) |
| `2` | Select Mic 2 (dashboard/plotter only) |
| `B` | Select Both (dashboard/plotter only) |
| `D` | Dashboard mode -- live metrics table |
| `P` | Plotter mode -- CSV output |
| `W` | WAV stream -- beamformed + noise reduced, always mono |
| `C` | Calibrate -- run in silence! |
| `H` | Help |

### Python Recorder

```bash
python wav_recorder.py                      # Record 10 sec
python wav_recorder.py --port COM5 --dur 20 # Specific port, 20 sec
```

| Option | Default | Description |
|--------|---------|-------------|
| `--port` | Auto | Serial port |
| `--mic` | `B` | `1`, `2`, or `B` -- used in filename label |
| `--dur` | `10` | Duration in seconds |

---

## Tuning the Noise Reduction

Edit these defines in `src/main.cpp`:

| Parameter | Default | Effect |
|-----------|:-------:|--------|
| `OVERSUB_FACTOR` | 2.0 | Higher = more noise removed, may distort speech |
| `SPECTRAL_FLOOR` | 0.02 | Higher = less musical noise artifacts |
| `FFT_SIZE` | 512 | 256 = less latency, 1024 = better frequency resolution |

---

## Project Structure

```
MOSA_MIC_PROJ/
+-- platformio.ini          # PlatformIO config (ESP-IDF framework)
+-- sdkconfig.defaults      # ESP-IDF settings
+-- src/
|   +-- main.cpp            # ESP-IDF firmware with esp-dsp
|   +-- CMakeLists.txt      # Build config
|   +-- idf_component.yml   # esp-dsp dependency
+-- include/                # Header files
+-- MOSA_MIC_PROJ.ino       # Legacy Arduino version (reference)
+-- wav_recorder.py         # PC recording script (Python 3)
+-- mic_test.py             # XIAO RP2040 test (MicroPython)
+-- PROJECT.md              # Detailed technical docs
+-- README.md               # This file
+-- Test_voice/             # Clean WAV recordings output
+-- mic test_wrongs/        # Early test recordings
```

---

## Dependencies

**Firmware:**
- PlatformIO with `espressif32` platform
- ESP-IDF framework (auto-downloaded by PlatformIO)
- `espressif/esp-dsp` component (auto-downloaded from ESP Component Registry)

**Python:**
- Python 3.7+
- `pyserial` (`pip install pyserial`)

---

## Arduino vs ESP-IDF

| Feature | Arduino | ESP-IDF (current) |
|---------|---------|-------------------|
| FFT | arduinoFFT (software) | esp-dsp (hardware accelerated) |
| Serial | Arduino Serial class | USB Serial/JTAG driver |
| Tasks | single loop() | FreeRTOS app_main |
| GPIO | pinMode/digitalWrite | gpio_set_direction/level |
| Timer | millis() | esp_timer_get_time() |
| Build | Arduino IDE | PlatformIO + CMake |

---

## Troubleshooting

| Problem | Solution |
|---------|----------|
| PlatformIO can't find board | Ensure `espressif32` platform is installed |
| `dsps_fft2r.h` not found | Check `src/idf_component.yml` exists with esp-dsp dependency |
| No serial output | Set monitor speed to 2000000 in PlatformIO |
| Speech sounds "underwater" | Lower `OVERSUB_FACTOR` to 1.5 |
| Musical noise / chirping | Increase `SPECTRAL_FLOOR` to 0.05 |
| High noise remains | Recalibrate (`C`) in silence |
| USB not detected | Try different USB-C cable; ensure it's data-capable |

---

## License

MIT

---

## Acknowledgments

Built with the Seeed Studio XIAO ESP32-S3, InvenSense INMP441 MEMS microphones, and Espressif's esp-dsp library.
