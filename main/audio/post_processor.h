/*
 * Audio Post-Processing Pipeline
 * ==============================
 * Applied AFTER ESP-SR AFE fetch output (clean mono PCM).
 *
 * Pipeline order:
 *   1. Spectral tilt EQ (biquad high shelf — compensate INMP441 HF rise)
 *   2. MMSE-LSA residual noise suppression
 *   3. Formant enhancement (LPC post-filter)
 *
 * All stages are independently toggleable at runtime.
 */
#pragma once

#include <cstdint>

// Initialize all post-processing stages (call once after AFE init)
void post_init();

// Process a block of clean PCM from AFE fetch.
// Operates in-place on the buffer.
// Returns number of output samples (may differ due to overlap-add framing).
int post_process(int16_t *samples, int nsamples);

// Runtime toggles
void post_set_eq_enabled(bool en);
void post_set_mmse_enabled(bool en);
void post_set_formant_enabled(bool en);

bool post_get_eq_enabled();
bool post_get_mmse_enabled();
bool post_get_formant_enabled();
