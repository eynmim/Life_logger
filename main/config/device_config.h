/*
 * MOSA Device Configuration
 * All hardware constants, pin maps, and audio parameters in one place.
 */
#pragma once

#include "driver/gpio.h"
#include "driver/i2s.h"

// ══════════════════════════════════════
// PIN DEFINITIONS
// ══════════════════════════════════════

#define M1_SCK  GPIO_NUM_5
#define M1_WS   GPIO_NUM_43
#define M1_SD   GPIO_NUM_6
#define M1_LR   GPIO_NUM_44

#define M2_SCK  GPIO_NUM_9
#define M2_WS   GPIO_NUM_7
#define M2_SD   GPIO_NUM_8
#define M2_LR   GPIO_NUM_4

// ══════════════════════════════════════
// AUDIO CONFIGURATION
// ══════════════════════════════════════

#define SAMPLE_RATE         16000       // ESP-SR AFE requires 16 kHz
#define DMA_BUF_COUNT       8
#define DMA_BUF_LEN         256
#define BUF_SAMPLES         512

// ══════════════════════════════════════
// CALIBRATION
// ══════════════════════════════════════

#define CAL_ROUNDS          80
#define CAL_SKIP            20
#define NOISE_MARGIN        1.5f
#define DC_HP_ALPHA         0.995f
#define SMOOTH_ALPHA        0.85f

// ══════════════════════════════════════
// WAV / TIMING
// ══════════════════════════════════════

#define WAV_DURATION_S      10

// ══════════════════════════════════════
// RING BUFFER
// ══════════════════════════════════════

#define CLEAN_RB_SAMPLES    (16000 * 12)    // 12 seconds at 16 kHz

// ══════════════════════════════════════
// POST-PROCESSING
// ══════════════════════════════════════

// FFT for post-processing filters (coherence, MMSE-LSA)
#define POST_FFT_SIZE       256
#define POST_FFT_HALF       (POST_FFT_SIZE / 2 + 1)    // 129 bins
#define POST_FFT_HOP        (POST_FFT_SIZE / 2)         // 50% overlap

// Coherence-based Wiener filter
#define COH_SMOOTH_ALPHA    0.92f       // Smoothing for PSD estimates
#define COH_MIC_DIST_M      0.0225f     // 22.5 mm mic spacing (center-to-center)

// MMSE-LSA
#define MMSE_ALPHA_S        0.98f       // A priori SNR smoothing
#define MMSE_ALPHA_N        0.95f       // Noise PSD update rate
#define MMSE_MIN_GAIN       0.05f       // Spectral floor (-26 dB)
#define MMSE_XI_MIN         0.001f      // Minimum a priori SNR

// Spectral tilt EQ — compensate INMP441 rising response above 1 kHz
#define EQ_HIGH_SHELF_FREQ  0.0625f     // 1 kHz / 16 kHz = normalized
#define EQ_HIGH_SHELF_GAIN  -2.5f       // -2.5 dB shelf (tames HF rise)
#define EQ_HIGH_SHELF_Q     0.707f

// Formant enhancement
#define LPC_ORDER           12
#define FORMANT_BOOST       0.4f        // Post-filter strength (0=off, 1=max)

// VAD state machine
#define VAD_PRE_ROLL_MS     500         // Capture before speech onset
#define VAD_HOLD_ON_MS      300         // Min speech duration
#define VAD_HOLD_OFF_MS     1500        // Silence before stop

// NVS calibration
#define NVS_NAMESPACE       "mosa_cal"
#define NVS_KEY_VALID       "cal_valid"
#define NVS_KEY_M1_NRMS     "m1_nrms"
#define NVS_KEY_M1_NPEAK    "m1_npk"
#define NVS_KEY_M1_DC       "m1_dc"
#define NVS_KEY_M1_GATE     "m1_gate"
#define NVS_KEY_M2_NRMS     "m2_nrms"
#define NVS_KEY_M2_NPEAK    "m2_npk"
#define NVS_KEY_M2_DC       "m2_dc"
#define NVS_KEY_M2_GATE     "m2_gate"
