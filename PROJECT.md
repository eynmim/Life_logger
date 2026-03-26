# MOSA_MIC_PROJ

**Microphone Open Sound Architecture - Dual INMP441 Beamformed Noise Reduction System**

A dual-microphone audio capture and noise reduction system built on the Seeed Studio XIAO ESP32-S3 using **ESP-IDF** with **hardware-accelerated DSP**. It uses two side-by-side INMP441 MEMS microphones with beamforming and FFT-based spectral subtraction to extract clean speech from noisy environments, recording the result as a CD-quality mono WAV file.

---

## Framework

| | |
|---|---|
| **Framework** | ESP-IDF (via PlatformIO) |
| **DSP Library** | esp-dsp (hardware-accelerated FFT on ESP32-S3) |
| **Build System** | PlatformIO + CMake |
| **Previous** | Arduino IDE (migrated 2026-03-26) |

### Why ESP-IDF over Arduino?

- **Hardware-accelerated FFT** via esp-dsp — uses ESP32-S3 vector DSP extensions
- **Direct FreeRTOS** control — task priorities, dual-core utilization
- **Full ESP-IDF API** — USB Serial/JTAG driver, precise timers, GPIO config
- **No abstraction overhead** — direct register access when needed
- **Better memory alignment** — `__attribute__((aligned(16)))` for DSP buffers

---

## Hardware

| Component | Details |
|-----------|---------|
| **MCU** | Seeed Studio XIAO ESP32-S3 (240 MHz dual-core, FPU, 512 KB SRAM) |
| **Mic 1** | INMP441 MEMS (I2S0) |
| **Mic 2** | INMP441 MEMS (I2S1) |
| **Mic Position** | Side-by-side in one package |
| **Interface** | USB Serial/JTAG (native USB, no external UART chip) |

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
| Output Channels | Mono (beamformed from both mics) |
| Clock Source | APLL (hardware PLL for precise timing) |
| DMA Buffers | 8 x 512 samples |
| Read Buffer | 1024 samples per cycle |
| FFT Size | 512 samples, 50% overlap (Hann window) |
| FFT Engine | esp-dsp `dsps_fft2r_fc32` (hardware accelerated) |
| Output Format | Standard WAV (RIFF) |

---

## Noise Reduction Pipeline (WAV Mode)

```
Mic1 --> I2S (32-bit) --> >>16 --> HP Filter --+
                                               +--> Average --> FFT --> Spectral --> IFFT --> OLA --> Clean
Mic2 --> I2S (32-bit) --> >>16 --> HP Filter --+  (beamform)  (HW)   Subtraction   (HW)           Mono WAV
                                                                          ^
                                                                   Noise spectrum
                                                                 (from calibration)
```

### Processing Stages

1. **I2S Capture** -- 32-bit samples from each INMP441, downsampled to 16-bit
2. **HP Filter** -- First-order IIR (alpha=0.995) removes DC offset per mic
3. **Beamforming** -- Average both mics: coherent speech adds +6 dB, incoherent noise adds +3 dB = **+3 dB SNR gain**
4. **FFT** -- 512-point Hann-windowed FFT via `dsps_fft2r_fc32` (hardware accelerated)
   - Data format: interleaved complex `[re0, im0, re1, im1, ...]`
   - 16-byte aligned buffers for SIMD performance
5. **Spectral Subtraction** -- Subtract calibrated noise magnitude spectrum per bin
   - Oversubtraction factor: 2.0x (configurable, 1.0=gentle, 4.0=aggressive)
   - Spectral floor: 2% (prevents musical noise artifacts)
   - Phase preserved from original signal
   - Conjugate mirror symmetry maintained
6. **IFFT** -- Inverse via conjugate + forward FFT + normalize (hardware accelerated)
7. **Overlap-Add** -- 50% overlap with Hann window for seamless reconstruction
8. **Output** -- Clean mono 16-bit PCM streamed via USB Serial/JTAG

### Dashboard Mode Processing

- **RMS Calculation** -- AC component power measurement
- **Peak Detection** -- Absolute peak tracking per buffer
- **Noise Gate** -- Threshold-based voice activity detection (noise floor x 1.5)
- **SNR Metering** -- Real-time signal-to-noise ratio in dB
- **Exponential Smoothing** -- Smooth AC level display (alpha=0.85)

### Calibration

Runs automatically on boot and on `C` command. Requires silence.

1. **Time-domain calibration** (80 rounds) -- measures noise RMS, peak, DC offset per mic
2. **Spectral calibration** (25 reads + 5 settling) -- computes average noise magnitude spectrum across ~40 FFT frames using hardware-accelerated FFT

---

## Operating Modes

| Command | Mode | Description |
|:-------:|------|-------------|
| `1` | Mic 1 | Monitor single microphone (dashboard/plotter) |
| `2` | Mic 2 | Monitor second microphone (dashboard/plotter) |
| `B` | Both | Monitor both mics (dashboard/plotter) |
| `D` | Dashboard | Live metrics with level bars (default) |
| `P` | Plotter | CSV output for serial plotter tools |
| `W` | WAV Stream | Beamformed + noise-reduced mono recording |
| `C` | Calibrate | Noise floor + spectrum calibration (requires silence) |
| `H` | Help | Show command reference |

---

## WAV Recording Protocol

```
ESP32 sends:
  "WAV_START\n"
  "RATE:44100\n"
  "BITS:16\n"
  "CHANNELS:1\n"          (always mono -- beamformed + noise reduced)
  "DURATION:10\n"
  "DATA_BEGIN\n"
  <binary 16-bit PCM, little-endian, noise-reduced>
  "\nDATA_END\n"
```

---

## Project Files

```
MOSA_MIC_PROJ/
  ├── platformio.ini          PlatformIO config (ESP-IDF framework)
  ├── sdkconfig.defaults      ESP-IDF settings (USB console, CPU freq, DSP)
  ├── src/
  │   ├── main.cpp            ESP-IDF firmware (C++)
  │   ├── CMakeLists.txt      Build config for main component
  │   └── idf_component.yml   esp-dsp dependency declaration
  ├── include/                 Header files (empty for now)
  ├── MOSA_MIC_PROJ.ino        Legacy Arduino firmware (reference)
  ├── wav_recorder.py          PC-side WAV capture script (Python)
  ├── mic_test.py              XIAO RP2040 intensity test (MicroPython)
  ├── PROJECT.md               This file
  ├── README.md                GitHub readme
  ├── Test_voice/              Clean WAV recordings output folder
  └── mic test_wrongs/         Early test recordings (16 kHz, pre-NR)
```

### src/main.cpp
Main ESP-IDF firmware. Uses `driver/i2s.h` for dual I2S, `driver/usb_serial_jtag.h` for serial I/O, `dsps_fft2r.h` / `dsps_wind_hann.h` for hardware-accelerated FFT and Hann window generation. FreeRTOS-based with `app_main()` entry point.

### wav_recorder.py
Python companion script. Unchanged from Arduino version -- the serial protocol is identical.

```bash
python wav_recorder.py                          # Both mics, 10 sec
python wav_recorder.py --mic B --dur 20         # 20 seconds
python wav_recorder.py --port COM5 --dur 15     # Specific port
```

**Output:** `Test_voice/rec_YYYYMMDD_HHMMSS_[MIC1|MIC2|BOTH]_mono_44100Hz.wav`

---

## Dependencies

### Firmware (PlatformIO + ESP-IDF)
- **PlatformIO** with `espressif32` platform
- **Framework**: ESP-IDF (configured in `platformio.ini`)
- **Components**:
  - `espressif/esp-dsp ~1.4` (declared in `src/idf_component.yml`)
  - `driver/i2s.h` (ESP-IDF built-in)
  - `driver/usb_serial_jtag.h` (ESP-IDF built-in)

### Python (PC)
- **Python** 3.7+
- **pyserial**: `pip install pyserial`
- **wave**: Standard library

---

## Quick Start

1. **Wire** two INMP441 microphones to the ESP32-S3 per the pin mapping
2. **Open** the project folder in VS Code with PlatformIO extension
3. **Build**: PlatformIO will auto-download ESP-IDF and esp-dsp
4. **Upload**: Flash to XIAO ESP32-S3
5. **Monitor**: Open serial monitor at 2,000,000 baud -- calibration runs on boot (keep silent!)
6. **Record**: Close monitor, then:
   ```bash
   python wav_recorder.py
   ```

---

## Tuning the Noise Reduction

| Parameter | Default | Range | Effect |
|-----------|---------|-------|--------|
| `OVERSUB_FACTOR` | 2.0 | 1.0 - 4.0 | Higher = more noise removed but more speech distortion |
| `SPECTRAL_FLOOR` | 0.02 | 0.001 - 0.1 | Higher = less musical noise but more residual noise |
| `FFT_SIZE` | 512 | 256 / 512 / 1024 | Larger = better frequency resolution but more latency |

---

## ESP-IDF API Mapping (from Arduino)

| Arduino | ESP-IDF | Notes |
|---------|---------|-------|
| `Serial.begin()` | `usb_serial_jtag_driver_install()` | Native USB, no baud rate needed |
| `Serial.printf()` | `serial_printf()` (custom wrapper) | Uses `usb_serial_jtag_write_bytes` |
| `Serial.write()` | `serial_write_bytes()` | Direct binary write |
| `Serial.available()` | `serial_available()` | Peek-based implementation |
| `Serial.read()` | `serial_read()` | Non-blocking with peek |
| `delay(ms)` | `vTaskDelay(pdMS_TO_TICKS(ms))` | FreeRTOS tick-based |
| `millis()` | `esp_timer_get_time() / 1000` | Microsecond timer |
| `pinMode()` | `gpio_set_direction()` | ESP-IDF GPIO driver |
| `digitalWrite()` | `gpio_set_level()` | Direct register write |
| `arduinoFFT` | `dsps_fft2r_fc32()` | Hardware accelerated on ESP32-S3 |

---

## Changelog

| Date | Change |
|------|--------|
| 2026-03-23 | Initial dual-mic system with dashboard, plotter, WAV streaming at 16 kHz |
| 2026-03-26 | Upgraded to 44.1 kHz, APLL clock, HP filter on WAV output, 2 Mbaud serial |
| 2026-03-26 | WAV output to `Test_voice/` folder; filename includes mic source |
| 2026-03-26 | Added FFT-based noise reduction: beamforming + spectral subtraction + OLA |
| 2026-03-26 | Migrated from Arduino IDE to ESP-IDF (PlatformIO); esp-dsp HW-accelerated FFT |
