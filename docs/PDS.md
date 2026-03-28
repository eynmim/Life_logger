# MOSA — Product Design Specification (PDS)

**Document ID:** MOSA-PDS-001
**Revision:** 1.0
**Date:** 2026-03-27
**Author:** MOSA Engineering Team
**Status:** Draft

---

## 1. Product Overview

### 1.1 Product Name
**MOSA** — Microphone Open Sound Architecture

### 1.2 Product Vision
A compact, low-power, high-fidelity voice capture device that delivers studio-grade noise-suppressed audio in any environment. MOSA combines dual MEMS microphone arrays with neural noise suppression (NSNet) to extract clean speech from noisy surroundings, suitable for life logging, voice journaling, field recording, and industrial voice command interfaces.

### 1.3 Target Markets
| Market Segment | Use Case |
|---|---|
| **Consumer** | Life logging, voice journaling, personal audio diary |
| **Professional** | Field interviews, meeting capture, lecture recording |
| **Industrial** | Voice command in noisy factories, hands-free inspection logging |
| **Accessibility** | Hearing aid front-end, assistive listening devices |
| **IoT / Smart Home** | Far-field voice capture, always-on voice gateway |

### 1.4 Product Variants

| Variant | Target | Form Factor | Communication |
|---|---|---|---|
| **MOSA-Dev** | Engineers / POC | Dev board + breadboard | USB Serial |
| **MOSA-Lite** | Consumer / Maker | Compact PCB, clip-on | BLE 5.0 |
| **MOSA-Pro** | Professional | Ruggedized enclosure | Wi-Fi + BLE |
| **MOSA-Ind** | Industrial | IP67 enclosure, DIN mount | Wi-Fi + MQTT / BLE Mesh |

---

## 2. Functional Requirements

### 2.1 Audio Capture

| ID | Requirement | Target | Priority |
|---|---|---|---|
| FR-A01 | Sample rate | 16 kHz (voice) / 48 kHz (hi-fi mode) | Must |
| FR-A02 | Bit depth | 16-bit PCM (capture), 32-bit internal processing | Must |
| FR-A03 | Microphone count | 2 (minimum), expandable to 4 | Must |
| FR-A04 | Microphone type | INMP441 MEMS (POC), ICS-43434 / SPH0645 (production) | Must |
| FR-A05 | Signal-to-Noise Ratio (raw mic) | > 65 dB | Must |
| FR-A06 | Total Harmonic Distortion | < 1% at 94 dB SPL | Should |
| FR-A07 | Acoustic overload point | > 120 dB SPL | Should |
| FR-A08 | Frequency response | 50 Hz — 8 kHz (voice), 20 Hz — 20 kHz (hi-fi) | Must |

### 2.2 Audio Processing Pipeline

| ID | Requirement | Description | Priority |
|---|---|---|---|
| FR-P01 | DC removal | High-pass filter (IIR, fc < 20 Hz) | Must |
| FR-P02 | Neural noise suppression | ESP-SR AFE NSNet DNN on-device | Must |
| FR-P03 | Acoustic Echo Cancellation (AEC) | For speaker + mic applications | Should |
| FR-P04 | Voice Activity Detection (VAD) | Auto-start/stop recording | Must |
| FR-P05 | Beamforming | Delay-and-sum or MVDR from dual mics | Should |
| FR-P06 | Automatic Gain Control (AGC) | Normalize output level | Must |
| FR-P07 | Wind noise rejection | Low-frequency attenuation + cross-mic coherence | Should |
| FR-P08 | Real-time processing latency | < 30 ms end-to-end | Must |

### 2.3 Communication

| ID | Requirement | Description | Priority |
|---|---|---|---|
| FR-C01 | USB Serial | WAV streaming over USB-C (development) | Must |
| FR-C02 | BLE 5.0 Audio | LC3/opus compressed audio streaming | Must |
| FR-C03 | Wi-Fi (STA mode) | HTTP/WebSocket upload to cloud/local server | Should |
| FR-C04 | MQTT | Lightweight telemetry + audio chunk publish | Should |
| FR-C05 | ESP-NOW | Ultra-low-latency device-to-device (< 1 ms) | Could |
| FR-C06 | OTA firmware update | Secure over-the-air updates via Wi-Fi | Must |

### 2.4 Storage

| ID | Requirement | Description | Priority |
|---|---|---|---|
| FR-S01 | Local buffer | PSRAM ring buffer (12+ sec) | Must |
| FR-S02 | SD card (optional) | FAT32/exFAT, auto-segment by duration/silence | Could |
| FR-S03 | SPIFFS/LittleFS | Configuration, calibration profiles, models | Must |

### 2.5 Power Management

| ID | Requirement | Description | Priority |
|---|---|---|---|
| FR-B01 | Supply voltage | 3.3V regulated (USB 5V or LiPo 3.7V input) | Must |
| FR-B02 | Active current (recording) | < 80 mA average (Wi-Fi off) | Must |
| FR-B03 | Active current (streaming) | < 150 mA average (Wi-Fi on) | Must |
| FR-B04 | Deep sleep current | < 10 uA | Must |
| FR-B05 | Battery capacity | 500 mAh — 2000 mAh (LiPo) | Must |
| FR-B06 | Battery life (recording) | > 6 hours continuous at 500 mAh | Must |
| FR-B07 | Battery life (standby + VAD) | > 72 hours at 500 mAh | Should |
| FR-B08 | Charging | USB-C PD or 5V, 500 mA charge rate | Must |
| FR-B09 | Battery fuel gauge | MAX17048 or equivalent, SOC% over BLE | Should |
| FR-B10 | Low power modes | Light sleep (VAD listening), deep sleep (scheduled wake) | Must |

### 2.6 User Interface

| ID | Requirement | Description | Priority |
|---|---|---|---|
| FR-U01 | LED indicator | RGB: recording, BLE connected, battery status | Must |
| FR-U02 | Button | Single multi-function (press/long-press/double-tap) | Must |
| FR-U03 | Mobile app (BLE) | iOS/Android: live audio, recordings, settings | Should |
| FR-U04 | Web dashboard (Wi-Fi) | Browser-based config, live waveform, download | Should |
| FR-U05 | Serial CLI | Debug console for development | Must |
| FR-U06 | Haptic feedback | Vibration motor for silent operation confirmation | Could |

---

## 3. Non-Functional Requirements

### 3.1 Performance

| ID | Requirement | Target |
|---|---|---|
| NFR-01 | Boot to ready | < 3 seconds |
| NFR-02 | AFE processing load | < 60% CPU on Core 1 |
| NFR-03 | Memory footprint | < 4 MB PSRAM for AFE + buffers |
| NFR-04 | Flash usage | < 6 MB (firmware + models) |

### 3.2 Reliability

| ID | Requirement | Target |
|---|---|---|
| NFR-05 | Continuous operation | > 24 hours without watchdog reset |
| NFR-06 | Data integrity | Zero dropped samples during recording |
| NFR-07 | Recovery | Auto-restart on fatal error, preserve last recording |

### 3.3 Environmental (Industrial Variant)

| ID | Requirement | Target |
|---|---|---|
| NFR-08 | Operating temperature | -20C to +60C |
| NFR-09 | Ingress protection | IP67 (MOSA-Ind) |
| NFR-10 | Vibration tolerance | IEC 60068-2-6 |
| NFR-11 | EMC compliance | FCC Part 15, CE |

---

## 4. Hardware Architecture

### 4.1 Block Diagram

```
                    +---------------------------+
                    |    MOSA Hardware Block     |
                    +---------------------------+
                    |                           |
  +----------+     |   +-----------------+      |     +-----------+
  | INMP441  |--I2S0-->|                 |      |     | USB-C     |
  | Mic 1    |     |   |   ESP32-S3      |      |---->| (Power +  |
  +----------+     |   |                 |      |     |  Data)    |
                   |   |  - Dual Core    |      |     +-----------+
  +----------+     |   |  - 8MB Flash    |      |
  | INMP441  |--I2S1-->|  - 8MB PSRAM    |      |     +-----------+
  | Mic 2    |     |   |  - Wi-Fi/BLE    |------+---->| BLE 5.0   |
  +----------+     |   |  - ESP-SR AFE   |      |     +-----------+
                   |   |                 |      |
  +----------+     |   |                 |      |     +-----------+
  | LiPo     |---->|   +-----------------+      |---->| Wi-Fi     |
  | Battery  |     |          |                 |     +-----------+
  +----------+     |   +------+------+          |
                   |   | PMU / Fuel  |          |     +-----------+
  +----------+     |   | Gauge       |          |---->| RGB LED   |
  | USB-C    |---->|   +-------------+          |     +-----------+
  | Charger  |     |                            |
  +----------+     +----------------------------+     +-----------+
                                                |---->| Button    |
                                                      +-----------+
```

### 4.2 MCU Selection

| Phase | MCU | Justification |
|---|---|---|
| POC / Dev | XIAO ESP32-S3 | Available, USB-C, 8MB PSRAM, low cost |
| Production (Lite/Pro) | ESP32-S3-WROOM-1 N16R8 | 16MB flash, 8MB PSRAM, antenna options |
| Production (Industrial) | ESP32-S3-MINI-1 | Compact, certified module, -40C to +85C |

### 4.3 Microphone Selection

| Phase | Mic | SNR | AOP | Justification |
|---|---|---|---|---|
| POC | INMP441 | 61 dB | 120 dB | Low cost, widely available |
| Production | ICS-43434 | 65 dB | 120 dB | Better SNR, TDM multi-mic on single I2S |
| Hi-Fi variant | SPH0645LM4H | 65 dB | 120 dB | Wide frequency response, PDM option |

### 4.4 Power Management IC

| Component | Part | Function |
|---|---|---|
| LiPo charger | TP4056 / BQ24075 | USB 5V to LiPo charge management |
| LDO regulator | AP2112K-3.3 | 3.3V output, low quiescent (55 uA) |
| Fuel gauge | MAX17048 | I2C battery SOC%, voltage, rate |
| Power switch | TPS22918 | Load switch for mic power gating |
| ESD protection | USBLC6-2SC6 | USB-C ESD protection |

---

## 5. Software Architecture

### 5.1 Layer Diagram

```
+================================================================+
|                      USER APPLICATIONS                          |
|  Mobile App (Flutter/RN)  |  Web Dashboard  |  Serial CLI      |
+================================================================+
|                    COMMUNICATION LAYER                           |
|  BLE Audio (LC3)  |  Wi-Fi (HTTP/WS/MQTT)  |  USB Serial      |
+================================================================+
|                    APPLICATION LAYER                             |
|  Recording Manager  |  VAD Controller  |  Power Manager        |
|  OTA Manager        |  Config Store    |  Event System         |
+================================================================+
|                    AUDIO PROCESSING LAYER                       |
|  ESP-SR AFE  |  NSNet DNN  |  Beamforming  |  AGC  |  Codec   |
+================================================================+
|                    HARDWARE ABSTRACTION LAYER                   |
|  I2S Driver  |  GPIO  |  SPI/SD  |  I2C (Fuel Gauge)  |  ADC  |
+================================================================+
|                    RTOS + PLATFORM                              |
|  FreeRTOS  |  ESP-IDF  |  esp-dsp  |  esp-sr  |  NVS          |
+================================================================+
```

### 5.2 Task Architecture

| Task | Core | Priority | Stack | Function |
|---|---|---|---|---|
| `afe_feed` | 1 | 5 (high) | 8 KB | I2S read, interleave, feed AFE |
| `afe_fetch` | 1 | 5 (high) | 4 KB | Fetch clean audio, ring buffer write |
| `audio_manager` | 0 | 4 | 8 KB | Recording FSM, VAD logic, codec |
| `comm_manager` | 0 | 3 | 8 KB | BLE/Wi-Fi/Serial dispatch |
| `power_manager` | 0 | 2 | 4 KB | Battery monitoring, sleep control |
| `ui_manager` | 0 | 1 (low) | 4 KB | LED, button, haptic |
| `app_main` | 0 | 1 | 4 KB | Init, event loop, watchdog |

### 5.3 Communication Protocol Selection

| Protocol | Use Case | Bandwidth | Range | Power | Selection Criteria |
|---|---|---|---|---|---|
| **USB Serial** | Dev/debug, wired recording | 2 Mbps | Cable | N/A | Tier 1 — immediate, zero config |
| **BLE 5.0 (LE Audio)** | Mobile app, short range | 2 Mbps | 30 m | Low | Tier 3 — best for wearable/mobile |
| **Wi-Fi (TCP/WS)** | Cloud upload, dashboard | 20+ Mbps | 50 m | High | Tier 4 — best for stationary/server |
| **MQTT** | IoT telemetry, event-driven | 256 Kbps | WAN | Medium | Tier 4 — lightweight pub/sub |
| **ESP-NOW** | Multi-device mesh | 1 Mbps | 200 m | Very Low | Tier 5 — device-to-device |

### 5.4 Audio Codec Selection

| Codec | Bitrate | Latency | Quality | Use Case |
|---|---|---|---|---|
| **PCM (raw)** | 256 kbps | 0 ms | Lossless | USB wired recording |
| **Opus** | 16-64 kbps | 20 ms | Excellent | BLE/Wi-Fi streaming |
| **LC3** | 16-32 kbps | 7.5 ms | Good | BLE LE Audio |
| **FLAC** | 128-192 kbps | 50 ms | Lossless | Wi-Fi upload, archival |
| **ADPCM** | 64 kbps | 1 ms | Acceptable | Low-resource fallback |

---

## 6. Battery Management Strategy

### 6.1 Power Budget

| Mode | Components Active | Current | Duration at 500 mAh |
|---|---|---|---|
| **Deep Sleep** | RTC only | 10 uA | > 5 years |
| **VAD Listening** | 1 mic + ULP + BLE adv | 5 mA | 100 hours |
| **Recording (local)** | 2 mics + AFE + PSRAM | 80 mA | 6.25 hours |
| **Recording + BLE** | 2 mics + AFE + BLE | 120 mA | 4.2 hours |
| **Recording + Wi-Fi** | 2 mics + AFE + Wi-Fi TX | 180 mA | 2.8 hours |
| **OTA Update** | Wi-Fi RX/TX + Flash | 200 mA | N/A (one-time) |

### 6.2 Power Optimization Techniques

| Technique | Savings | Implementation Tier |
|---|---|---|
| Dynamic frequency scaling (80/160/240 MHz) | 30-50% idle | Tier 3 |
| Mic power gating (TPS22918 load switch) | 0.8 mA per mic | Tier 3 |
| Wi-Fi modem sleep (DTIM listen interval) | 60% Wi-Fi power | Tier 4 |
| BLE connection interval optimization | 40% BLE power | Tier 3 |
| Light sleep between AFE chunks | 15-20% in recording | Tier 4 |
| ULP co-processor for VAD wake | 95% main CPU | Tier 5 |

---

## 7. Quality Targets

### 7.1 Audio Quality Metrics

| Metric | POC (Current) | Production Target | Industrial Target |
|---|---|---|---|
| SNR (post-processing) | 20-30 dB | > 40 dB | > 45 dB |
| THD+N | < 3% | < 1% | < 0.5% |
| Noise floor | -60 dBFS | -75 dBFS | -80 dBFS |
| Speech intelligibility (PESQ) | 2.5-3.0 | > 3.5 | > 4.0 |
| Latency (mic to output) | ~50 ms | < 30 ms | < 20 ms |

### 7.2 Compliance

| Standard | Variant | Requirement |
|---|---|---|
| FCC Part 15 | All (with radio) | EMC, intentional radiator |
| CE (RED) | All (EU market) | Radio equipment directive |
| RoHS | All | Hazardous substances |
| UL 62368 | Pro / Industrial | Safety |
| IP67 | Industrial | Ingress protection |
| IEC 61672 | Industrial (optional) | Sound level meter class 2 |

---

## 8. Risk Register

| ID | Risk | Impact | Likelihood | Mitigation |
|---|---|---|---|---|
| R01 | PSRAM bandwidth bottleneck with Wi-Fi + AFE | High | Medium | Profile early, use DMA, partition bus access |
| R02 | BLE audio quality degradation | Medium | Medium | Evaluate LC3 vs Opus, fallback to ADPCM |
| R03 | Battery life below target | High | Medium | Power profiling at each tier, dynamic scaling |
| R04 | NSNet model too large for OTA | Medium | Low | Delta updates, model quantization |
| R05 | I2S clock jitter at 48 kHz | Medium | Low | Use APLL, verify with oscilloscope |
| R06 | Thermal throttling in enclosed case | Medium | Medium | Thermal simulation, duty cycling |
| R07 | Regulatory certification delays | High | Medium | Use pre-certified modules, engage test lab early |
| R08 | Microphone aging / drift | Low | Low | Periodic auto-calibration, redundancy |

---

*End of PDS*
