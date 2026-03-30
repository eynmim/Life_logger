# MOSA

### Neural Voice Enhancement System for ESP32-S3

A dual-microphone voice capture system built on the Seeed Studio XIAO ESP32-S3 using **ESP-ADF** (Audio Development Framework) with **ESP-SR NSNet** neural noise suppression. Extracts clean speech from noisy environments using deep learning, with a built-in A/B quality comparison framework.

---

## Features

- **NSNet Neural Noise Suppression** -- Espressif's deep learning model for 15-20 dB noise reduction
- **VADNet Voice Activity Detection** -- DNN-based, trained on 15,000 hours of multilingual audio
- **Dual INMP441 Microphones** -- 22-23 mm spacing, optimized for speech band
- **ESP-ADF Audio Framework** -- Production-ready pipeline architecture for BLE, SD card, Wi-Fi
- **A/B Quality Comparison** -- Record raw vs processed simultaneously, analyze with Python
- **NVS Calibration Persistence** -- Calibration survives reboots
- **Real-Time Dashboard** -- Live RMS, SNR, VAD state, level meters

---

## Architecture

```
Mic1 (INMP441) --+
                  +--> ESP-SR AFE_VC --> NSNet1 --> VADNet --> EQ --> Clean PCM
Mic2 (INMP441) --+

Pipeline: [input] -> |NS(nsnet1)| -> |VAD(vadnet1_medium)| -> [output]
```

---

## Hardware

| Component | Details |
|-----------|---------|
| **MCU** | Seeed Studio XIAO ESP32-S3 (240 MHz, 8 MB PSRAM, 8 MB Flash) |
| **Mic 1** | INMP441 MEMS (I2S0): SCK=5, WS=43, SD=6, L/R=44 |
| **Mic 2** | INMP441 MEMS (I2S1): SCK=9, WS=7, SD=8, L/R=4 |
| **Mic Spacing** | 22-23 mm center-to-center |

---

## Getting Started

### Prerequisites

- [VS Code](https://code.visualstudio.com/) with [ESP-IDF extension](https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension)
- ESP-ADF v2.8 (install via ESP-IDF extension sidebar: "Install ESP-ADF")
- Python 3.7+ with `pyserial`

### Build and Flash

1. Open `MOSA_MIC_PROJ/` in VS Code
2. ESP-IDF sidebar: **Set Espressif Device Target** -> ESP32-S3
3. ESP-IDF sidebar: **Build Project**
4. ESP-IDF sidebar: **Flash Device**
5. ESP-IDF sidebar: **Monitor Device**

### Record Audio

Close the serial monitor, then:

```bash
pip install pyserial numpy matplotlib scipy

# Record processed audio (mono)
python wav_recorder.py

# Record A/B comparison (raw vs processed, stereo)
python wav_recorder.py --ab

# Analyze noise suppression quality
python analyze_ab.py
```

---

## Serial Commands

| Key | Action |
|:---:|--------|
| `D` | Dashboard -- live metrics |
| `P` | Plotter -- CSV for serial plotter |
| `W` | WAV -- processed mono recording |
| `A` | A/B -- stereo raw vs processed |
| `1/2/B` | Select Mic1 / Mic2 / Both |
| `C` | Calibrate (silence required) |
| `V` | Toggle VAD auto-record |
| `E` | Toggle spectral tilt EQ |
| `H` | Help |

---

## Project Structure

```
MOSA_MIC_PROJ/
  CMakeLists.txt              Root project (references $ADF_PATH)
  partitions.csv              Partition table (factory + model)
  sdkconfig.defaults          ESP-IDF / ESP-ADF settings
  main/
    main.cpp                  Firmware entry point
    CMakeLists.txt            Component registration
    idf_component.yml         esp-sr + esp-dsp dependencies
    audio/                    Ring buffers, post-processing, coherence, VAD
    config/                   Device config, NVS calibration
    hal/                      Serial I/O abstraction
  data/                       ESP-SR model files (nsnet1, vadnet1, wn9)
  docs/
    PDS.md                    Product Design Specification
    WPS.md                    Work Plan Specification & Roadmap
  wav_recorder.py             Python WAV recorder (mono + A/B stereo)
  analyze_ab.py               Python A/B quality analyzer
  Test_voice/                 Recorded WAV files and analysis plots
```

---

## Roadmap

| Tier | Name | Status |
|:----:|------|--------|
| 1 | POC -- dual mic + basic NR | DONE |
| 2 | AI Voice Engine -- NSNet + VADNet + A/B | IN PROGRESS |
| 3 | Wireless & Power -- BLE audio, battery, SD card | Planned |
| 4 | Cloud & Platform -- Wi-Fi, mobile app, OTA | Planned |
| 5 | Industrial Hardening -- custom PCB, certification | Planned |
| 6 | Production & Scale -- manufacturing, QC | Planned |

See [docs/PDS.md](docs/PDS.md) and [docs/WPS.md](docs/WPS.md) for full specifications.

---

## License

MIT
