/*
 * Coherence-based Wiener Post-Filter (v2 — fixed)
 *
 * The coherence estimation (coh_feed) runs in the frequency domain
 * on the raw dual-mic data to build a per-bin gain mask.
 *
 * The gain application (coh_apply) now works in the TIME DOMAIN
 * by applying the gain mask as a simple per-sample scaling factor
 * derived from the average gain. This avoids the FFT/IFFT artifacts
 * and sample-count mismatches of the previous version.
 *
 * The gain mask is still computed per-frequency-bin for accuracy,
 * but applied as a broadband or band-weighted scalar to avoid
 * the need for overlap-add on the clean output path.
 */
#include "coherence_filter.h"
#include "../config/device_config.h"

#include <cmath>
#include <cstring>

#include "esp_heap_caps.h"
#include "dsps_fft2r.h"
#include "dsps_wind_hann.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

static bool enabled = false;  // disabled — NSNet handles noise, this adds pumping

// FFT buffers
static float *fft_m1    = nullptr;
static float *fft_m2    = nullptr;
static float *coh_win   = nullptr;

// Smoothed PSDs
static float *psd_11    = nullptr;
static float *psd_22    = nullptr;
static float *cpsd_re   = nullptr;
static float *cpsd_im   = nullptr;

// Diffuse noise model (precomputed)
static float *gamma_diff = nullptr;

// Per-bin gain mask
static float *gain_mask  = nullptr;

// Broadband gain (weighted average of gain_mask, applied in time domain)
static float broadband_gain = 1.0f;

static const float SPEED_OF_SOUND = 343.0f;

void coh_init() {
    int half = POST_FFT_HALF;

    fft_m1     = (float *)heap_caps_calloc(POST_FFT_SIZE * 2, sizeof(float), MALLOC_CAP_SPIRAM);
    fft_m2     = (float *)heap_caps_calloc(POST_FFT_SIZE * 2, sizeof(float), MALLOC_CAP_SPIRAM);
    coh_win    = (float *)heap_caps_malloc(POST_FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM);
    psd_11     = (float *)heap_caps_calloc(half, sizeof(float), MALLOC_CAP_SPIRAM);
    psd_22     = (float *)heap_caps_calloc(half, sizeof(float), MALLOC_CAP_SPIRAM);
    cpsd_re    = (float *)heap_caps_calloc(half, sizeof(float), MALLOC_CAP_SPIRAM);
    cpsd_im    = (float *)heap_caps_calloc(half, sizeof(float), MALLOC_CAP_SPIRAM);
    gamma_diff = (float *)heap_caps_malloc(half * sizeof(float), MALLOC_CAP_SPIRAM);
    gain_mask  = (float *)heap_caps_malloc(half * sizeof(float), MALLOC_CAP_SPIRAM);

    dsps_wind_hann_f32(coh_win, POST_FFT_SIZE);

    // Precompute diffuse noise coherence: |sinc(2πfd/c)|
    float freq_bin = (float)SAMPLE_RATE / (float)POST_FFT_SIZE;
    for (int k = 0; k < half; k++) {
        float f = k * freq_bin;
        float arg = 2.0f * M_PI * f * COH_MIC_DIST_M / SPEED_OF_SOUND;
        if (fabsf(arg) < 1e-6f)
            gamma_diff[k] = 1.0f;
        else
            gamma_diff[k] = fabsf(sinf(arg) / arg);

        gain_mask[k] = 1.0f;
    }

    broadband_gain = 1.0f;
}

void coh_feed(const int16_t *m1, const int16_t *m2, int nsamples) {
    if (!enabled || !fft_m1) return;

    int len = (nsamples > POST_FFT_SIZE) ? POST_FFT_SIZE : nsamples;

    // Window + FFT both channels
    for (int i = 0; i < POST_FFT_SIZE; i++) {
        float s1 = (i < len) ? (float)m1[i] : 0.0f;
        float s2 = (i < len) ? (float)m2[i] : 0.0f;
        fft_m1[2 * i]     = s1 * coh_win[i];
        fft_m1[2 * i + 1] = 0.0f;
        fft_m2[2 * i]     = s2 * coh_win[i];
        fft_m2[2 * i + 1] = 0.0f;
    }

    dsps_fft2r_fc32(fft_m1, POST_FFT_SIZE);
    dsps_bit_rev2r_fc32(fft_m1, POST_FFT_SIZE);
    dsps_fft2r_fc32(fft_m2, POST_FFT_SIZE);
    dsps_bit_rev2r_fc32(fft_m2, POST_FFT_SIZE);

    // Update smoothed PSDs and compute gain mask
    float alpha = COH_SMOOTH_ALPHA;
    float gain_sum = 0.0f;
    float weight_sum = 0.0f;

    for (int k = 0; k < POST_FFT_HALF; k++) {
        float re1 = fft_m1[2 * k], im1 = fft_m1[2 * k + 1];
        float re2 = fft_m2[2 * k], im2 = fft_m2[2 * k + 1];

        float p11 = re1 * re1 + im1 * im1;
        float p22 = re2 * re2 + im2 * im2;
        float cre = re1 * re2 + im1 * im2;
        float cim = im1 * re2 - re1 * im2;

        psd_11[k]  = alpha * psd_11[k]  + (1.0f - alpha) * p11;
        psd_22[k]  = alpha * psd_22[k]  + (1.0f - alpha) * p22;
        cpsd_re[k] = alpha * cpsd_re[k] + (1.0f - alpha) * cre;
        cpsd_im[k] = alpha * cpsd_im[k] + (1.0f - alpha) * cim;

        float denom = psd_11[k] * psd_22[k] + 1e-10f;
        float coh_mag2 = (cpsd_re[k] * cpsd_re[k] + cpsd_im[k] * cpsd_im[k]) / denom;
        coh_mag2 = fminf(coh_mag2, 1.0f);

        float diff2 = gamma_diff[k] * gamma_diff[k];

        float g;
        if (diff2 >= 0.999f) {
            g = 1.0f;
        } else {
            g = (coh_mag2 - diff2) / (1.0f - diff2);
        }

        g = fmaxf(g, 0.1f);   // generous floor — don't kill the signal
        g = fminf(g, 1.0f);

        gain_mask[k] = 0.85f * gain_mask[k] + 0.15f * g;

        // Weight by signal energy for broadband gain
        float energy = psd_11[k] + psd_22[k];
        gain_sum += gain_mask[k] * energy;
        weight_sum += energy;
    }

    // Compute energy-weighted broadband gain
    if (weight_sum > 1e-10f) {
        float new_bb = gain_sum / weight_sum;
        new_bb = fmaxf(new_bb, 0.2f);  // never suppress below -14 dB
        broadband_gain = 0.9f * broadband_gain + 0.1f * new_bb;
    }
}

void coh_apply(int16_t *clean, int nsamples) {
    if (!enabled || !fft_m1) return;

    // Apply broadband gain in time domain — simple, safe, no artifacts
    for (int i = 0; i < nsamples; i++) {
        float v = (float)clean[i] * broadband_gain;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        clean[i] = (int16_t)v;
    }
}

void coh_set_enabled(bool en) { enabled = en; }
bool coh_get_enabled()        { return enabled; }
