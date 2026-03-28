/*
 * Thread-safe ring buffers for audio samples
 * - Clean (processed) ring buffer: AFE + post-processing output
 * - Raw ring buffer: unprocessed mic1 samples for A/B comparison
 */
#pragma once

#include <cstdint>

// Processed audio ring buffer
void     rb_init(int capacity_samples);
void     rb_write(const int16_t *data, int nsamples);
int      rb_read(int16_t *out, int max_samples);
void     rb_reset();
int      rb_available();

// Raw audio ring buffer (for A/B comparison)
void     rb_raw_init(int capacity_samples);
void     rb_raw_write(const int16_t *data, int nsamples);
int      rb_raw_read(int16_t *out, int max_samples);
void     rb_raw_reset();
int      rb_raw_available();
