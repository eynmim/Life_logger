# MOSA — Work Plan Specification (WPS) & Product Roadmap

**Document ID:** MOSA-WPS-001
**Revision:** 1.0
**Date:** 2026-03-27
**Author:** MOSA Engineering Team
**Status:** Draft
**Reference:** MOSA-PDS-001

---

## 1. Development Philosophy

Each tier builds on the previous one. No tier is skipped. Every tier ends with a
**deliverable milestone** — a working system that can be demonstrated and validated
before moving forward. Hardware and software evolve together.

```
 TIER 1        TIER 2        TIER 3        TIER 4        TIER 5        TIER 6
  POC       Core Audio    Wireless &     Cloud &       Industrial    Production
            Engine        Power          Platform      Hardening     & Scale
  [DONE]     [NEXT]        [  ]           [  ]          [  ]          [  ]
    |           |             |              |             |             |
    v           v             v              v             v             v
  Basic      Production    BLE/Wi-Fi      Web/App       Certify       Manufacture
  I2S+AFE    quality DSP   + Battery      + OTA         + Ruggedize   + Ship
```

---

## 2. Tier 1 — Proof of Concept (COMPLETE)

**Status:** DONE
**Branch:** `feature/new-architecture`
**Duration:** 2026-03-23 to 2026-03-27

### 2.1 Objectives
Validate that dual INMP441 microphones on ESP32-S3 can capture, process, and stream
clean audio using ESP-SR AFE with neural noise suppression.

### 2.2 Deliverables

| # | Deliverable | Status |
|---|---|---|
| 1 | Dual I2S mic capture at 16 kHz | DONE |
| 2 | ESP-SR AFE_VC + NSNet integration | DONE |
| 3 | Feed/Fetch task pipeline on Core 1 | DONE |
| 4 | Ring buffer (clean audio) | DONE |
| 5 | Serial dashboard (RMS, SNR, gate, level) | DONE |
| 6 | Serial plotter mode (clean AFE output) | DONE |
| 7 | WAV streaming over USB Serial | DONE |
| 8 | Python WAV recorder (`wav_recorder.py`) | DONE |
| 9 | Auto-calibration (noise floor, DC, gate threshold) | DONE |
| 10 | Basic serial command interface | DONE |

### 2.3 Hardware
- Seeed Studio XIAO ESP32-S3
- 2x INMP441 MEMS microphones (breadboard)
- USB-C cable

### 2.4 Validated Performance
- NSNet noise suppression operational
- Dual-mic interleave to AFE working
- Clean audio retrievable via ring buffer
- WAV recording functional (10 sec, 16 kHz, mono)

---

## 3. Tier 2 — Core Audio Engine

**Status:** IN PROGRESS (80% complete)
**Started:** 2026-03-27
**Estimated Completion:** 2026-04-05

### 3.1 Objectives
Build a production-quality audio processing engine with modular architecture,
Voice Activity Detection, Automatic Gain Control, and multi-rate support.

### 3.2 Work Packages

#### WP-2.1: Modular Code Architecture
**Goal:** Refactor monolithic `main.cpp` into a clean, maintainable module structure.

```
src/
+-- main.cpp                 Application entry point + event loop
+-- audio/
|   +-- audio_pipeline.h/c   Pipeline orchestration (init, start, stop)
|   +-- i2s_capture.h/c      I2S driver setup, mic read
|   +-- afe_engine.h/c       ESP-SR AFE init, feed/fetch tasks
|   +-- audio_buffer.h/c     Ring buffer with thread-safe API
|   +-- vad.h/c              Voice Activity Detection logic
|   +-- agc.h/c              Automatic Gain Control
|   +-- codec.h/c            Encoding (PCM, Opus, ADPCM)
+-- hal/
|   +-- serial_io.h/c        USB Serial/JTAG abstraction
|   +-- gpio_ctrl.h/c        Pin definitions, L/R control
|   +-- led.h/c              RGB LED driver
+-- ui/
|   +-- cli.h/c              Serial command parser
|   +-- dashboard.h/c        Dashboard display logic
+-- config/
|   +-- device_config.h      Pin maps, audio params, build variants
|   +-- cal_store.h/c        Save/load calibration to NVS
+-- utils/
    +-- ring_buffer.h/c      Generic ring buffer (templated)
    +-- event_bus.h/c        Lightweight event publish/subscribe
```

**Tasks:**
| # | Task | Priority | Estimate |
|---|---|---|---|
| 2.1.1 | Define module interfaces (headers) | Must | 2 days | DONE |
| 2.1.2 | Extract I2S capture module | Must | 1 day | DONE |
| 2.1.3 | Extract AFE engine module | Must | 1 day | DONE |
| 2.1.4 | Extract ring buffer (processed + raw) | Must | 0.5 day | DONE |
| 2.1.5 | Extract serial I/O + CLI module | Must | 1 day | DONE |
| 2.1.6 | Create device_config.h with all constants | Must | 0.5 day | DONE |
| 2.1.7 | Migrate to ESP-ADF build system | Must | 1 day | DONE |
| 2.1.8 | Verify all modes work after refactor | Must | 1 day | PENDING (need device) |

#### WP-2.2: Voice Activity Detection (VAD)
**Goal:** Automatically detect speech segments and trigger recording.

**Approach:**
- Primary: ESP-SR AFE built-in VAD (from `afe_fetch_result_t`)
- Secondary: Energy-based VAD from calibrated noise gate (already partially implemented)
- Hysteresis: 300 ms hold-on, 1.5 sec hold-off to prevent choppy cuts

**Tasks:**
| # | Task | Priority | Estimate |
|---|---|---|---|
| 2.2.1 | Expose AFE VAD state from fetch results | Must | 0.5 day | DONE |
| 2.2.2 | Implement VAD state machine (idle/pre-speech/speech/post-speech) | Must | 1 day | DONE |
| 2.2.3 | Add pre-roll buffer (capture 500 ms before VAD trigger) | Must | 1 day | DONE |
| 2.2.4 | Serial command to enable/disable VAD auto-record | Must | 0.5 day | DONE |
| 2.2.5 | LED indicator for VAD state | Should | 0.5 day | Tier 3 |

#### WP-2.3: Automatic Gain Control (AGC)
**Goal:** Normalize output audio level regardless of speaker distance.

**Approach:**
- Slow-attack, fast-release AGC on clean audio output
- Target output level: -12 dBFS (headroom for peaks)
- Limiter at -1 dBFS to prevent clipping

**Tasks:**
| # | Task | Priority | Estimate |
|---|---|---|---|
| 2.3.1 | Implement AGC algorithm (attack/release/target) | Must | 1 day | DONE (ESP-SR WebRTC AGC) |
| 2.3.2 | Integrate AGC after AFE fetch, before ring buffer | Must | 0.5 day | DONE (disabled — boosts residual noise) |
| 2.3.3 | Add AGC enable/disable + parameter tuning via CLI | Should | 0.5 day | DONE |

#### WP-2.4: Multi-Rate Audio Support
**Goal:** Support both 16 kHz (voice) and 48 kHz (hi-fi) modes.

**Tasks:**
| # | Task | Priority | Estimate |
|---|---|---|---|
| 2.4.1 | Abstract sample rate in device_config.h | Must | 0.5 day |
| 2.4.2 | Test ESP-SR AFE at 16 kHz (locked requirement) | Must | 0.5 day |
| 2.4.3 | Add bypass mode (48 kHz raw capture, no AFE) | Should | 1 day |
| 2.4.4 | Update WAV streaming protocol for multi-rate | Must | 0.5 day |

#### WP-2.5: Calibration Persistence
**Goal:** Save calibration data to NVS so device doesn't need recalibration on every boot.

**Tasks:**
| # | Task | Priority | Estimate |
|---|---|---|---|
| 2.5.1 | Define calibration data struct for NVS | Must | 0.5 day | DONE |
| 2.5.2 | Save calibration to NVS after successful cal | Must | 0.5 day | DONE |
| 2.5.3 | Load calibration from NVS on boot | Must | 0.5 day | DONE |
| 2.5.4 | CLI command to clear saved calibration | Must | 0.5 day | DONE |

#### WP-2.6: Post-Processing Pipeline (Added)
**Goal:** Classical DSP stages after AFE for additional voice clarity.

**Status:** Implemented but disabled (NSNet handles noise; stacking causes artifacts).

| # | Task | Priority | Status |
|---|---|---|---|
| 2.6.1 | Spectral tilt EQ (biquad high-shelf for INMP441) | Must | DONE (active) |
| 2.6.2 | Spectral noise gate (FFT per-bin, OLA) | Should | DONE (disabled — fights NSNet) |
| 2.6.3 | Formant enhancement (LPC post-filter) | Could | DONE (disabled — causes artifacts) |
| 2.6.4 | Coherence filter (dual-mic Wiener) | Could | DONE (disabled — causes pumping) |

**Key finding:** Post-processing after NSNet causes choppiness and gain pumping. Only EQ is recommended. NSNet handles noise suppression; classical DSP should not be stacked.

#### WP-2.7: A/B Quality Comparison System (Added)
**Goal:** Measure noise suppression effectiveness objectively.

| # | Task | Priority | Status |
|---|---|---|---|
| 2.7.1 | Raw ring buffer (unprocessed mic1 parallel capture) | Must | DONE |
| 2.7.2 | A/B stereo WAV mode (L=raw, R=processed, same moment) | Must | DONE |
| 2.7.3 | Python analyzer (SNR, spectral comparison, plots) | Must | DONE |
| 2.7.4 | Validate NSNet gives >15 dB SNR improvement | Must | PENDING (need device) |

#### WP-2.8: ESP-ADF Migration (Added)
**Goal:** Migrate from PlatformIO to native ESP-ADF for Tier 3 readiness.

| # | Task | Priority | Status |
|---|---|---|---|
| 2.8.1 | Install ESP-ADF v2.8 with ESP-IDF 5.5 | Must | DONE |
| 2.8.2 | Create ESP-ADF project structure (main/, CMakeLists) | Must | DONE |
| 2.8.3 | Migrate firmware to main/ with idf_component.yml | Must | DONE |
| 2.8.4 | Verify build with ESP-IDF VS Code extension | Must | DONE |
| 2.8.5 | Flash and verify on hardware | Must | PENDING (need device) |
| 2.8.6 | Remove legacy PlatformIO src/ directory | Should | After validation |

#### WP-2.9: ESP-SR Constraints Documented (Added)
**Goal:** Document critical ESP-SR limitations discovered during integration.

Key findings:
- `AFE_TYPE_VC` = 1MIC only (uses NSNet, ignores second mic)
- `AFE_TYPE_SR` = 2MIC + BSS but NO NSNet (only ~5-6 dB reduction)
- NSNet + BSS are mutually exclusive in ESP-SR
- `afe_config_check()` silently overrides settings
- Model partition uses custom binary format (`pack_model.py`), not SPIFFS

### 3.3 Tier 2 Exit Criteria

| # | Criterion | Validation Method |
|---|---|---|
| 1 | Modular codebase compiles and runs identically to Tier 1 | Side-by-side WAV comparison |
| 2 | VAD correctly detects speech onset within 300 ms | Serial log + manual test |
| 3 | AGC normalizes whisper and shout to within 6 dB of target | WAV amplitude analysis |
| 4 | Calibration persists across reboots | Power cycle test |
| 5 | No audio glitches in 30-minute continuous recording | Waveform inspection |

---

## 4. Tier 3 — Wireless Communication & Power Management

**Status:** Planned
**Estimated Duration:** 3-4 weeks
**Prerequisites:** Tier 2 complete

### 4.1 Objectives
Add BLE 5.0 audio streaming to a mobile device, implement battery management,
and introduce low-power operating modes.

### 4.2 Work Packages

#### WP-3.1: BLE 5.0 Integration
| # | Task | Priority | Estimate |
|---|---|---|---|
| 3.1.1 | BLE GATT server with audio service (custom UUID) | Must | 2 days |
| 3.1.2 | Audio codec: Opus or ADPCM compression (16 kHz → ~32 kbps) | Must | 2 days |
| 3.1.3 | BLE notification-based audio streaming | Must | 2 days |
| 3.1.4 | BLE control characteristics (start/stop, VAD mode, gain) | Must | 1 day |
| 3.1.5 | Connection parameter optimization (CI=7.5ms, MTU=247) | Should | 1 day |
| 3.1.6 | BLE bonding and security (LE Secure Connections) | Should | 1 day |

#### WP-3.2: Battery Management
| # | Task | Priority | Estimate |
|---|---|---|---|
| 3.2.1 | LiPo charger circuit (TP4056 or BQ24075) | Must | 1 day |
| 3.2.2 | ADC battery voltage reading (voltage divider) | Must | 0.5 day |
| 3.2.3 | MAX17048 fuel gauge I2C driver | Should | 1 day |
| 3.2.4 | Battery SOC% reporting over BLE | Should | 0.5 day |
| 3.2.5 | Low battery warning + graceful shutdown | Must | 0.5 day |
| 3.2.6 | Charge detection and LED indication | Must | 0.5 day |

#### WP-3.3: Power Optimization
| # | Task | Priority | Estimate |
|---|---|---|---|
| 3.3.1 | Dynamic CPU frequency scaling (80/160/240 MHz) | Must | 1 day |
| 3.3.2 | Mic power gating via load switch GPIO | Should | 0.5 day |
| 3.3.3 | Light sleep between AFE processing chunks | Should | 1.5 days |
| 3.3.4 | BLE advertising interval optimization | Must | 0.5 day |
| 3.3.5 | Deep sleep with RTC wake (button or timer) | Must | 1 day |
| 3.3.6 | Power profiling and documentation | Must | 1 day |

#### WP-3.4: Physical Button & LED
| # | Task | Priority | Estimate |
|---|---|---|---|
| 3.4.1 | Button driver: debounce, press/long-press/double-tap | Must | 1 day |
| 3.4.2 | Button actions: record toggle, BLE pair, power off | Must | 0.5 day |
| 3.4.3 | RGB LED driver (WS2812 or discrete) | Must | 0.5 day |
| 3.4.4 | LED patterns: recording (red), BLE (blue), charging (green) | Must | 0.5 day |

### 4.3 Tier 3 Hardware Additions
| Component | Part | Purpose |
|---|---|---|
| LiPo battery | 503035 (500 mAh) | Main power |
| Charger IC | TP4056 module | USB charging |
| Fuel gauge | MAX17048 breakout | SOC% monitoring |
| Load switch | TPS22918 | Mic power gating |
| Button | Tactile 6x6mm | User input |
| LED | WS2812B (Neopixel) | Status indication |

### 4.4 Tier 3 Exit Criteria
| # | Criterion | Validation |
|---|---|---|
| 1 | BLE audio stream received on phone (nRF Connect or custom app) | Live playback test |
| 2 | Battery lasts > 4 hours continuous BLE streaming | Discharge test |
| 3 | Deep sleep current < 15 uA | Multimeter measurement |
| 4 | Button controls work reliably | 100 press test |
| 5 | Clean power transitions (sleep → active → sleep) | Serial log |

---

## 5. Tier 4 — Cloud Platform & User Applications

**Status:** Planned
**Estimated Duration:** 4-5 weeks
**Prerequisites:** Tier 3 complete

### 5.1 Objectives
Build the user-facing platform: mobile app, web dashboard, cloud storage,
and OTA firmware updates.

### 5.2 Work Packages

#### WP-4.1: Wi-Fi Connectivity
| # | Task | Priority | Estimate |
|---|---|---|---|
| 4.1.1 | Wi-Fi STA mode with credential provisioning (BLE) | Must | 2 days |
| 4.1.2 | mDNS service discovery (`mosa.local`) | Should | 0.5 day |
| 4.1.3 | HTTPS client for cloud API upload | Must | 1 day |
| 4.1.4 | WebSocket server for local real-time dashboard | Should | 2 days |
| 4.1.5 | MQTT client for IoT telemetry (AWS IoT / EMQX) | Should | 1 day |
| 4.1.6 | Wi-Fi modem sleep configuration | Must | 0.5 day |

#### WP-4.2: Mobile Application
| # | Task | Priority | Estimate |
|---|---|---|---|
| 4.2.1 | Cross-platform framework selection (Flutter / React Native) | Must | 1 day |
| 4.2.2 | BLE device scanning and pairing flow | Must | 2 days |
| 4.2.3 | Live audio waveform display | Must | 2 days |
| 4.2.4 | Recording list with playback | Must | 2 days |
| 4.2.5 | Device settings (gain, VAD sensitivity, sample rate) | Should | 1 day |
| 4.2.6 | Battery level and connection status display | Must | 0.5 day |
| 4.2.7 | Cloud sync (upload recordings to server) | Should | 2 days |

#### WP-4.3: Web Dashboard
| # | Task | Priority | Estimate |
|---|---|---|---|
| 4.3.1 | Embedded HTTP server on ESP32-S3 (for local network) | Must | 1 day |
| 4.3.2 | Real-time waveform via WebSocket | Should | 1 day |
| 4.3.3 | Device configuration page | Must | 1 day |
| 4.3.4 | Recording download interface | Must | 1 day |
| 4.3.5 | Cloud dashboard (user accounts, device management) | Could | 3 days |

#### WP-4.4: OTA Firmware Updates
| # | Task | Priority | Estimate |
|---|---|---|---|
| 4.4.1 | ESP-IDF OTA partition scheme (dual app partitions) | Must | 1 day |
| 4.4.2 | HTTPS OTA update from cloud server | Must | 1 day |
| 4.4.3 | Rollback on failed update | Must | 0.5 day |
| 4.4.4 | Version reporting over BLE/Wi-Fi | Must | 0.5 day |
| 4.4.5 | Mobile app triggered OTA | Should | 1 day |

### 5.3 Platform Architecture

```
+------------------+     +------------------+     +------------------+
|   MOSA Device    |     |   Mobile App     |     |   Cloud Server   |
|                  |     |   (Flutter)      |     |   (AWS/GCP)      |
| +--------------+ | BLE | +--------------+ | API | +--------------+ |
| | Audio Engine |-+---->| | BLE Service  |-+---->| | Audio Store  | |
| +--------------+ |     | +--------------+ |     | +--------------+ |
| | BLE Server   | |     | | Waveform UI  | |     | | User Auth    | |
| +--------------+ |     | +--------------+ |     | +--------------+ |
| | Wi-Fi Client |-+---->| | Settings     | |     | | OTA Server   | |
| +--------------+ | WS  | +--------------+ |     | +--------------+ |
| | HTTP/WS Srv  |-+--+  | | Cloud Sync   | |     | | Dashboard    | |
| +--------------+ |  |  | +--------------+ |     | +--------------+ |
|                  |  |  |                  |     |                  |
+------------------+  |  +------------------+     +------------------+
                      |        ^                        ^
                      |  Wi-Fi |     +---------+        |
                      +------->+---->| Browser |        |
                                     | (Local) |--------+
                                     +---------+
```

### 5.4 Communication Protocol Summary

| Scenario | Protocol | Direction | Data |
|---|---|---|---|
| Live audio → phone | BLE GATT Notify | Device → App | Opus encoded chunks |
| Settings control | BLE GATT Write | App → Device | Config parameters |
| Live waveform → browser | WebSocket | Device → Browser | PCM samples (JSON) |
| Recording upload | HTTPS POST | Device → Cloud | WAV or Opus file |
| OTA update | HTTPS GET | Cloud → Device | Firmware binary |
| Telemetry | MQTT pub | Device → Cloud | JSON (battery, status) |
| Wi-Fi provisioning | BLE | App → Device | SSID + password |

### 5.5 Tier 4 Exit Criteria
| # | Criterion | Validation |
|---|---|---|
| 1 | Mobile app connects and streams audio from MOSA | Live demo |
| 2 | Web dashboard shows real-time waveform on local network | Browser test |
| 3 | Recordings upload to cloud and can be played back | End-to-end test |
| 4 | OTA update succeeds without bricking device | 10 update cycles |
| 5 | Wi-Fi provisioning via BLE works first time | Fresh device test |

---

## 6. Tier 5 — Industrial Hardening & Certification

**Status:** Planned
**Estimated Duration:** 6-8 weeks
**Prerequisites:** Tier 4 complete

### 6.1 Objectives
Harden the product for industrial environments, achieve regulatory certification,
and design the production-ready PCB.

### 6.2 Work Packages

#### WP-5.1: Custom PCB Design
| # | Task | Priority | Estimate |
|---|---|---|---|
| 5.1.1 | Schematic design (KiCad / Altium) | Must | 2 weeks |
| 5.1.2 | PCB layout (4-layer, RF considerations) | Must | 2 weeks |
| 5.1.3 | Component BOM optimization (cost, availability) | Must | 1 week |
| 5.1.4 | DFM review with PCB manufacturer | Must | 1 week |
| 5.1.5 | Prototype board fabrication + assembly (PCBA) | Must | 2 weeks |
| 5.1.6 | Bring-up and validation | Must | 1 week |

#### WP-5.2: Enclosure Design
| # | Task | Priority | Estimate |
|---|---|---|---|
| 5.2.1 | Industrial design (form factor, aesthetics) | Must | 1 week |
| 5.2.2 | Acoustic port design (mic placement, wind screen) | Must | 1 week |
| 5.2.3 | IP67 sealing (gaskets, membrane mic covers) | Must (Ind) | 1 week |
| 5.2.4 | 3D printing prototypes | Must | 1 week |
| 5.2.5 | Injection mold tooling (production) | Must | 4 weeks |

#### WP-5.3: EMC & Regulatory Certification
| # | Task | Priority | Estimate |
|---|---|---|---|
| 5.3.1 | Pre-compliance EMC testing (in-house) | Must | 1 week |
| 5.3.2 | FCC Part 15 certification | Must | 4-6 weeks |
| 5.3.3 | CE (RED) certification | Must | 4-6 weeks |
| 5.3.4 | RoHS documentation | Must | 1 week |
| 5.3.5 | UL safety testing (if required) | Could | 6-8 weeks |

#### WP-5.4: Reliability Engineering
| # | Task | Priority | Estimate |
|---|---|---|---|
| 5.4.1 | Temperature cycling test (-20C to +60C) | Must | 1 week |
| 5.4.2 | Vibration testing (IEC 60068) | Should (Ind) | 1 week |
| 5.4.3 | Drop test (1.5 m onto concrete) | Should | 1 day |
| 5.4.4 | Accelerated life testing (HALT) | Should | 2 weeks |
| 5.4.5 | Watchdog and recovery validation | Must | 1 week |
| 5.4.6 | 1000-hour burn-in test | Must | 6 weeks |

#### WP-5.5: Security Hardening
| # | Task | Priority | Estimate |
|---|---|---|---|
| 5.5.1 | Secure boot (ESP32-S3 eFuse) | Must | 1 day |
| 5.5.2 | Flash encryption | Must | 1 day |
| 5.5.3 | BLE bonding with MITM protection | Must | 1 day |
| 5.5.4 | TLS 1.3 for all cloud communication | Must | 1 day |
| 5.5.5 | Firmware signing for OTA | Must | 1 day |

### 6.3 Tier 5 Exit Criteria
| # | Criterion | Validation |
|---|---|---|
| 1 | Custom PCB passes all functional tests | Test report |
| 2 | FCC/CE certification obtained | Certificate |
| 3 | Device operates 1000 hours without failure | Burn-in log |
| 4 | Secure boot + flash encryption enabled | Security audit |
| 5 | Enclosure meets IP67 (Industrial variant) | Water ingress test |

---

## 7. Tier 6 — Production & Scale

**Status:** Future
**Estimated Duration:** 8-12 weeks
**Prerequisites:** Tier 5 complete

### 7.1 Objectives
Scale from prototype to volume manufacturing with quality control,
supply chain management, and go-to-market readiness.

### 7.2 Work Packages

#### WP-6.1: Manufacturing
| # | Task | Priority |
|---|---|---|
| 6.1.1 | Select contract manufacturer (CM) for PCBA | Must |
| 6.1.2 | Create manufacturing test jig (ICT / FCT) | Must |
| 6.1.3 | Production firmware with factory test mode | Must |
| 6.1.4 | Audio quality end-of-line test (automated) | Must |
| 6.1.5 | Packaging design | Must |
| 6.1.6 | Pilot run (50 units) | Must |
| 6.1.7 | First production run (500 units) | Must |

#### WP-6.2: Quality Control
| # | Task | Priority |
|---|---|---|
| 6.2.1 | Incoming quality inspection (IQI) for components | Must |
| 6.2.2 | Statistical process control (SPC) for PCBA | Must |
| 6.2.3 | Audio golden sample + automated comparison | Must |
| 6.2.4 | Final quality audit (FQA) before shipment | Must |

#### WP-6.3: Software Infrastructure
| # | Task | Priority |
|---|---|---|
| 6.3.1 | CI/CD pipeline for firmware builds | Must |
| 6.3.2 | Device provisioning system (unique IDs, certificates) | Must |
| 6.3.3 | Cloud infrastructure scaling (AWS/GCP) | Must |
| 6.3.4 | Customer support portal | Should |
| 6.3.5 | Analytics dashboard (fleet health, usage metrics) | Should |

### 7.3 Unit Economics Target

| Component | POC Cost | Target Production Cost (1000 units) |
|---|---|---|
| ESP32-S3-WROOM-1 N16R8 | $3.50 | $2.80 |
| 2x MEMS Microphones | $2.00 | $1.20 |
| PMU + Fuel Gauge | $1.50 | $0.90 |
| LiPo Battery (500 mAh) | $2.00 | $1.50 |
| PCB (4-layer) | $3.00 | $0.80 |
| Enclosure | $5.00 | $1.50 |
| Passive components | $1.00 | $0.50 |
| Assembly (PCBA) | $5.00 | $2.00 |
| **Total BOM** | **$23.00** | **$11.20** |
| Test + QC | $2.00 | $1.00 |
| Packaging | $1.00 | $0.50 |
| **Total landed cost** | **$26.00** | **$12.70** |

---

## 8. Timeline Overview

```
2026
Mar         Apr              May              Jun              Jul         Aug
 |           |                |                |                |           |
 +--TIER 1---+                |                |                |           |
 |  [DONE]   |                |                |                |           |
 |           +---TIER 2-------+                |                |           |
 |           | Core Audio     |                |                |           |
 |           | Engine         |                |                |           |
 |           |                +----TIER 3------+                |           |
 |           |                | Wireless &     |                |           |
 |           |                | Battery        |                |           |
 |           |                |                +----TIER 4------+           |
 |           |                |                | Cloud &        |           |
 |           |                |                | Platform       |           |
 |           |                |                |                +--TIER 5---+-->
 |           |                |                |                | Industrial|
 |           |                |                |                | Hardening |
 |           |                |                |                |           |
 v           v                v                v                v           v
POC         VAD+AGC          BLE+Battery      App+OTA          PCB+Cert   MFG
DONE        Modular code     Power mgmt       Mobile app       Custom PCB  |
            Multi-rate       LED+Button        Web dash         EMC/FCC    |
                                              MQTT              Security   |
                                                                          |
                                                                 +--TIER 6-+-->
                                                                 | Production
                                                                 | & Scale
```

---

## 9. Key Decision Points

| Gate | Decision | Criteria | Timing |
|---|---|---|---|
| G1 | Proceed to Tier 2 | POC validates audio quality | PASSED |
| G2 | BLE vs Wi-Fi priority | Battery life vs bandwidth requirements | End of Tier 2 |
| G3 | Mobile framework | Flutter vs React Native (team skills, BLE support) | Start of Tier 4 |
| G4 | Cloud provider | AWS IoT vs GCP vs self-hosted | Start of Tier 4 |
| G5 | Go / No-Go for custom PCB | Audio quality meets PESQ > 3.5, battery > 4 hrs | End of Tier 4 |
| G6 | Production variant selection | Lite / Pro / Industrial based on market feedback | End of Tier 5 |
| G7 | Manufacturing partner | CM selection based on pilot run quality | Start of Tier 6 |

---

## 10. Resource Requirements

### 10.1 Team (recommended for Tier 3+)

| Role | Tiers | FTE |
|---|---|---|
| Embedded firmware engineer | 1-6 | 1.0 |
| Hardware / PCB engineer | 3-6 | 0.5 |
| Mobile app developer | 4-5 | 0.5 |
| Cloud / backend engineer | 4-6 | 0.5 |
| Industrial designer | 5-6 | 0.3 |
| QA / test engineer | 5-6 | 0.5 |

### 10.2 Tools & Licenses

| Tool | Purpose | Cost |
|---|---|---|
| PlatformIO | Build system | Free |
| KiCad | PCB design | Free |
| Flutter | Mobile app | Free |
| AWS IoT Core | Cloud platform | Pay-per-use |
| nRF Connect SDK | BLE testing | Free |
| JLCPCB / PCBWay | PCB fabrication | ~$50/prototype |
| EMC test lab | Pre-compliance | ~$2000-5000 |
| FCC/CE lab | Certification | ~$5000-15000 |

---

## 11. Document References

| Document | ID | Description |
|---|---|---|
| Product Design Specification | MOSA-PDS-001 | Full product requirements |
| Project Technical Documentation | PROJECT.md | Current implementation details |
| Hardware Pin Mapping | README.md | Wiring and setup guide |
| ESP-SR AFE Documentation | Espressif docs | AFE_VC configuration reference |

---

*End of WPS*
