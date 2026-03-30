# MOSA_MIC_PROJ

**Microphone Open Sound Architecture - Neural Voice Enhancement System**

A dual-microphone voice capture system built on the Seeed Studio XIAO ESP32-S3 using **ESP-IDF** with **ESP-SR AFE** (Audio Front End) for neural noise suppression. Uses Espressif's NSNet deep learning model for real-time voice enhancement, with VADNet-based voice activity detection and an A/B comparison framework for quality validation.

---

## Architecture Overview

```
                         Tier 2 Pipeline (current)
                         ========================

Mic1 (INMP441) --> I2S0 (32-bit) --> >>16 --+
                                             +--> Interleave "MM" --> ESP-SR AFE_VC
Mic2 (INMP441) --> I2S1 (32-bit) --> >>16 --+         |
                                                      |
                          +---------------------------+
                          |
                          v
              [NSNet1 Neural Noise Suppression]
                          |
                          v
              [VADNet Voice Activity Detection]
                          |
                          v
               [Spectral Tilt EQ (biquad)]
                          |
                          v
                  Clean Mono PCM 16kHz
                          |
              +-----------+-----------+
              |                       |
         Ring Buffer              Raw Ring Buffer
         (processed)              (unprocessed mic1)
              |                       |
              v                       v
      Dashboard / WAV           A/B Comparison
      Plotter / Serial         Stereo WAV output
```

---

## Framework

| | |
|---|---|
| **Audio Framework** | ESP-ADF v2.8 (Espressif Audio Development Framework) |
| **Base Framework** | ESP-IDF 5.5.2 |
| **AI Engine** | ESP-SR AFE (NSNet1 neural noise suppression) |
| **DSP Library** | esp-dsp (hardware-accelerated FFT on ESP32-S3) |
| **Build System** | ESP-IDF CMake (via VS Code ESP-IDF extension) |
| **Model Partition** | Custom binary format (pack_model.py), 1.4 MB |
| **Previous** | PlatformIO (migrated 2026-03-30) |

---

## Hardware

| Component | Details |
|-----------|---------|
| **MCU** | Seeed Studio XIAO ESP32-S3 (240 MHz dual-core, FPU, 8 MB PSRAM, 8 MB Flash) |
| **Mic 1** | INMP441 MEMS (I2S0) — 61 dBA SNR, 24-bit output |
| **Mic 2** | INMP441 MEMS (I2S1) — 61 dBA SNR, 24-bit output |
| **Mic Spacing** | 22-23 mm center-to-center (spatial aliasing free up to 7.6 kHz) |
| **Interface** | USB Serial/JTAG (native USB, 2 Mbaud) |

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
| Sample Rate | 16,000 Hz (speech-optimized) |
| Bit Depth | 16-bit signed PCM |
| I2S Resolution | 32-bit (shifted >>16 to 16-bit) |
| Output Channels | Mono (AFE-selected from dual input) |
| DMA Buffers | 8 x 256 samples |
| AFE Feed Chunk | 256 samples (16 ms) |
| AFE Fetch Chunk | 512 samples (32 ms) |
| NSNet Model | nsnet1 (819 KB, quantized neural network) |
| VAD Model | vadnet1_medium (287 KB, trained on 15k hrs) |
| Latency | ~30-50 ms end-to-end |

---

## ESP-SR AFE Configuration

### AFE Mode: Voice Communication (1MIC)

```
AFE_TYPE_VC + AFE_MODE_HIGH_PERF
Pipeline: [input] -> |NS(nsnet1)| -> |VAD(vadnet1_medium)| -> [output]
```

**Key finding:** ESP-SR's dual-mic (2MIC) mode supports BSS but NOT NSNet.
Single-mic (1MIC) mode with NSNet gives 15-20 dB noise reduction vs BSS's 5-6 dB.
The first mic channel from the "MM" input is automatically selected.

### Configuration Parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| `afe_type` | `AFE_TYPE_VC` | Voice Communication mode (enables NSNet) |
| `afe_mode` | `AFE_MODE_HIGH_PERF` | Maximum quality processing |
| `ns_init` | `true` | Neural noise suppression enabled |
| `afe_ns_mode` | `AFE_NS_MODE_NET` | NSNet (not WebRTC) |
| `ns_model_name` | `nsnet1` | Auto-selected from model partition |
| `vad_init` | `true` | DNN-based VAD enabled |
| `vad_mode` | `VAD_MODE_2` | Very aggressive noise rejection |
| `vad_min_speech_ms` | `200` | Minimum speech duration |
| `vad_min_noise_ms` | `800` | Minimum silence duration |
| `se_init` | `false` | BSS unavailable in VC mode |
| `agc_init` | `false` | Disabled for clean A/B testing |
| `aec_init` | `false` | No speaker playback |
| `wakenet_init` | `false` | Not needed for recording |
| `memory_alloc_mode` | `AFE_MEMORY_ALLOC_MORE_PSRAM` | Maximize available PSRAM |
| `afe_linear_gain` | `1.0` | Unity gain |

### ESP-SR Design Constraints Discovered

| Constraint | Impact |
|-----------|--------|
| `AFE_TYPE_VC` is 1MIC only | Cannot use BSS + NSNet together |
| `AFE_TYPE_SR` 2MIC has BSS but no NSNet | BSS gives only ~5-6 dB reduction |
| `afe_config_check()` silently overrides settings | Must verify POST-CHECK values |
| NSNet requires model in flash partition | Models not auto-flashed by PlatformIO |
| SR mode requires WakeNet model to enable NS | NS gets stripped if no WakeNet |
| Model partition uses custom binary format | NOT standard SPIFFS; use `pack_model.py` |

---

## Post-Processing Pipeline

After AFE output, additional processing stages are available (toggle via serial commands):

| Stage | Command | Default | CPU | Description |
|-------|---------|---------|-----|-------------|
| Spectral Tilt EQ | `E` | ON | <1% | High-shelf biquad to compensate INMP441 rising HF response |
| Spectral Noise Gate | `M` | OFF | ~4% | FFT-based per-bin gate (disabled; NSNet handles this) |
| Formant Enhancement | `F` | OFF | ~2% | LPC post-filter for speech intelligibility |
| Coherence Filter | `G` | OFF | ~3% | Dual-mic coherence Wiener (disabled; causes pumping) |

**Current recommendation:** Only EQ enabled. NSNet handles noise suppression. Post-filters cause artifacts (choppiness, pumping) when stacked with NSNet.

---

## A/B Quality Comparison System

### Recording Modes

| Command | Mode | Channels | Description |
|---------|------|----------|-------------|
| `W` | Processed | Mono | AFE + post-processing output only |
| `A` | A/B Stereo | Stereo | Left=raw mic, Right=processed (same moment) |

### Python Tools

```bash
# Record processed audio
python wav_recorder.py

# Record A/B comparison (raw vs processed, stereo)
python wav_recorder.py --ab

# Analyze latest A/B pair
python analyze_ab.py

# Analyze specific files
python analyze_ab.py --raw file_raw.wav --proc file_processed.wav
```

### Quality Metrics

| Metric | Target | Achieved (NSNet) |
|--------|--------|-----------------|
| SNR Improvement | >15 dB | ~15-20 dB (estimated with NSNet active) |
| Noise Reduction | >15 dB | ~15-25 dB |
| Speech Preserved | >-3 dB | ~-1 dB |
| Grade | EXCELLENT | Pending validation |

---

## Operating Modes

| Command | Mode | Description |
|:-------:|------|-------------|
| `1` | Mic 1 | Monitor single microphone |
| `2` | Mic 2 | Monitor second microphone |
| `B` | Both | Monitor both mics (default) |
| `D` | Dashboard | Live metrics with level bars |
| `P` | Plotter | CSV output for serial plotter |
| `W` | WAV Stream | Processed mono recording |
| `A` | A/B WAV | Stereo raw vs processed |
| `C` | Calibrate | Noise floor calibration (requires silence) |
| `V` | VAD Toggle | VAD auto-record on/off |
| `E` | EQ Toggle | Spectral tilt EQ on/off |
| `M` | MMSE Toggle | Spectral noise gate on/off |
| `F` | Formant Toggle | Formant enhancement on/off |
| `G` | Coherence Toggle | Coherence filter on/off |
| `H` | Help | Show command reference |

---

## Project Files

```
MOSA_MIC_PROJ/
  CMakeLists.txt              Root project (references $ADF_PATH/components)
  partitions.csv              Partition table (factory 2.5MB + model 5MB)
  sdkconfig.defaults          ESP-IDF / ESP-ADF settings
  main/                       *** ESP-ADF main component ***
    main.cpp                  Firmware: I2S, AFE, dashboard, WAV, A/B streaming
    CMakeLists.txt            Component registration (REQUIRES esp-sr, esp-dsp)
    idf_component.yml         esp-sr + esp-dsp dependency declaration
    audio/
      audio_buffer.h/cpp      Dual ring buffers (processed + raw)
      post_processor.h/cpp    EQ + spectral noise gate + formant enhancement
      coherence_filter.h/cpp  Dual-mic coherence Wiener filter
      vad.h/cpp               VAD state machine with pre-roll buffer
    config/
      device_config.h         Hardware constants (pins, mic spacing, FFT sizes)
      cal_store.h/cpp         NVS calibration persistence
    hal/
      serial_io.h/cpp         USB Serial/JTAG abstraction
  data/                       ESP-SR model files
    nsnet1/                   NSNet1 neural noise suppression (819 KB)
    wn9_hilexin/              WakeNet9 wake word model (290 KB)
    vadnet1_medium/           VADNet1 voice activity detection (287 KB)
  docs/
    PDS.md                    Product Design Specification
    WPS.md                    Work Plan Specification & Roadmap
  wav_recorder.py             Python WAV recorder (mono + A/B stereo)
  analyze_ab.py               Python A/B quality analyzer (SNR, spectra, plots)
  Test_voice/                 Recorded WAV files and analysis plots
  src/                        Legacy PlatformIO source (reference, to be removed)
  platformio.ini              Legacy PlatformIO config (reference, to be removed)
  PROJECT.md                  This file
```

---

## Model Partition

The ESP-SR models are stored in a custom binary format (NOT standard SPIFFS).

### Flashing Models

```bash
# 1. Pack models using ESP-SR tool
python managed_components/espressif__esp-sr/model/pack_model.py -m data -o srmodels.bin

# 2. Flash to model partition (offset 0x290000)
python ~/.platformio/packages/tool-esptoolpy/esptool.py \
  --chip esp32s3 --port COM3 --baud 921600 \
  write_flash 0x290000 data/srmodels.bin
```

### Models Included

| Model | Size | Purpose |
|-------|------|---------|
| `nsnet1` | 819 KB | Neural noise suppression (quantized DNN) |
| `vadnet1_medium` | 287 KB | Voice activity detection (trained on 15k hours) |
| `wn9_hilexin` | 290 KB | WakeNet (placeholder, disabled at runtime) |

---

## Dependencies

### Firmware
- **ESP-ADF** v2.8 (includes audio_pipeline, audio_stream, audio_recorder)
- **ESP-IDF** 5.5.2+
- **Components** (via `main/idf_component.yml`):
  - `espressif/esp-dsp ~1.4` (hardware-accelerated FFT)
  - `espressif/esp-sr ^2.0` (AFE + NSNet + VADNet)
- **VS Code** with ESP-IDF extension

### Python (PC)
- **pyserial**: `pip install pyserial`
- **numpy, matplotlib, scipy**: `pip install numpy matplotlib scipy` (for analyze_ab.py)

---

## Quick Start

1. **Wire** two INMP441 microphones per the pin mapping
2. **Install** ESP-IDF extension in VS Code, then install ESP-ADF via sidebar
3. **Open** project folder in VS Code
4. **Set target**: ESP-IDF sidebar -> Set Espressif Device Target -> ESP32-S3
5. **Build**: ESP-IDF sidebar -> Build Project
6. **Flash**: ESP-IDF sidebar -> Flash Device (firmware + models in one step)
7. **Monitor**: ESP-IDF sidebar -> Monitor Device (calibration runs on boot)
8. **Record**: Close monitor, then `python wav_recorder.py --ab`
9. **Analyze**: `python analyze_ab.py`

---

## Changelog

| Date | Change |
|------|--------|
| 2026-03-23 | Initial dual-mic system with dashboard, plotter, WAV at 16 kHz |
| 2026-03-26 | Upgraded to 44.1 kHz, APLL clock, HP filter, beamforming + spectral subtraction |
| 2026-03-26 | Migrated from Arduino IDE to ESP-IDF; esp-dsp HW-accelerated FFT |
| 2026-03-27 | Tier 2: ESP-SR AFE integration (BSS + NSNet + VADNet) |
| 2026-03-27 | Modular architecture: audio/, config/ modules, NVS calibration persistence |
| 2026-03-27 | Post-processing pipeline: EQ, spectral noise gate, formant, coherence filter |
| 2026-03-27 | A/B comparison system: stereo WAV (raw vs processed), Python analyzer |
| 2026-03-28 | Fixed NSNet: discovered AFE_TYPE_VC is 1MIC-only, model partition was empty |
| 2026-03-28 | Flashed NSNet1 + VADNet1 models to custom binary partition |
| 2026-03-28 | Pipeline confirmed: `[input] -> NS(nsnet1) -> VAD(vadnet1_medium) -> [output]` |
| 2026-03-30 | Migrated from PlatformIO to ESP-ADF v2.8 native build system |
| 2026-03-30 | Created main/ directory structure (ESP-IDF convention) |
| 2026-03-30 | Root CMakeLists.txt references $ADF_PATH/components |
| 2026-03-30 | Updated all documentation (README, PROJECT, WPS) |
