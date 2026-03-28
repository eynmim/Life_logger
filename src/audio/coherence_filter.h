/*
 * Coherence-based Wiener Post-Filter (Dual-Microphone)
 * =====================================================
 * Exploits the known coherence function of diffuse noise between
 * two close-spaced microphones to derive a frequency-dependent
 * Wiener gain. Near-field speech has coherence ≈ 1 while diffuse
 * noise follows sinc(2πfd/c).
 *
 * This filter is applied to the RAW dual-channel data from the
 * AFE feed path, producing a per-bin gain mask that is then
 * applied to the AFE's clean output.
 *
 * Cost: ~3% CPU
 */
#pragma once

#include <cstdint>

// Initialize coherence filter (call once)
void coh_init();

// Feed raw dual-mic frame for coherence estimation.
// m1, m2: raw int16 samples from mic 1 and mic 2
// nsamples: number of samples per channel
void coh_feed(const int16_t *m1, const int16_t *m2, int nsamples);

// Apply coherence-based gain to clean audio (in-place).
// This uses the gain mask computed from the most recent coh_feed.
void coh_apply(int16_t *clean, int nsamples);

// Enable/disable
void coh_set_enabled(bool en);
bool coh_get_enabled();
