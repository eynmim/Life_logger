/*
 * Audio Post-Processing Pipeline (v3)
 * ====================================
 *
 * Stage 1: Spectral Tilt EQ (biquad high shelf)
 *   Compensates INMP441 rising HF response. Time-domain, <1% CPU.
 *
 * Stage 2: Spectral Noise Gate
 *   Simple but effective: track noise floor per FFT bin using
 *   minimum statistics, then suppress bins below threshold.
 *   Uses conservative gain floor to avoid musical noise.
 *   Proper overlap-add with sqrt(Hann) window for COLA.
 *   ~3% CPU.
 *
 * Stage 3: Formant Enhancement (disabled by default)
 *   LPC post-filter. Only enable after stages 1-2 are validated.
 */
#include "post_processor.h"
#include "../config/device_config.h"

#include <cmath>
#include <cstring>

#include "esp_heap_caps.h"
#include "dsps_fft2r.h"
#include "dsps_biquad.h"
#include "dsps_biquad_gen.h"

// ══════════════════════════════════════
// TOGGLES
// ══════════════════════════════════════

static bool eq_enabled      = true;
static bool mmse_enabled    = false;   // disabled — NSNet already handles noise
static bool formant_enabled = false;

// ══════════════════════════════════════
// STAGE 1: EQ
// ══════════════════════════════════════

static float eq_coeff[5];
static float eq_state[2];

// ══════════════════════════════════════
// STAGE 2: SPECTRAL NOISE GATE
// ══════════════════════════════════════

static float *fft_buf       = nullptr;  // POST_FFT_SIZE * 2
static float *ana_window    = nullptr;  // POST_FFT_SIZE — sqrt(Hann) for analysis
static float *syn_window    = nullptr;  // POST_FFT_SIZE — sqrt(Hann) for synthesis
static float *noise_floor   = nullptr;  // POST_FFT_HALF — tracked noise floor per bin
static float *prev_mag      = nullptr;  // POST_FFT_HALF — previous frame magnitude
static int    gate_frame_count = 0;

// Noise gate parameters
#define GATE_NOISE_UP        0.001f    // Noise floor rise rate (very slow)
#define GATE_NOISE_DOWN      0.1f      // Noise floor fall rate (fast — track drops)
#define GATE_OVER_FACTOR     3.5f      // Only suppress bins well below noise (was 2.5)
#define GATE_MIN_GAIN        0.15f     // Floor gain (-16 dB) — generous, avoid killing speech
#define GATE_GAIN_SMOOTH     0.6f      // Temporal smoothing
#define GATE_WARMUP_FRAMES   30        // Longer warmup for better noise estimate

// ══════════════════════════════════════
// STAGE 3: FORMANT (same as before)
// ══════════════════════════════════════

static float lpc_a[LPC_ORDER + 1];

static void levinson_durbin(const float *r, int order, float *a) {
    float e = r[0];
    a[0] = 1.0f;
    for (int i = 1; i <= order; i++) a[i] = 0.0f;
    if (e <= 0.0f) return;
    for (int i = 1; i <= order; i++) {
        float lambda = 0.0f;
        for (int j = 1; j < i; j++)
            lambda += a[j] * r[i - j];
        lambda = (r[i] - lambda) / e;
        float tmp[LPC_ORDER + 1];
        memcpy(tmp, a, (i + 1) * sizeof(float));
        for (int j = 1; j < i; j++)
            a[j] = tmp[j] - lambda * tmp[i - j];
        a[i] = lambda;
        e *= (1.0f - lambda * lambda);
        if (e <= 0.0f) break;
    }
}

static void autocorrelation(const float *x, int n, float *r, int order) {
    for (int k = 0; k <= order; k++) {
        float sum = 0.0f;
        for (int i = 0; i < n - k; i++)
            sum += x[i] * x[i + k];
        r[k] = sum;
    }
}

// ══════════════════════════════════════
// INIT
// ══════════════════════════════════════

void post_init() {
    // EQ
    dsps_biquad_gen_highShelf_f32(eq_coeff, EQ_HIGH_SHELF_FREQ,
                                   EQ_HIGH_SHELF_GAIN, EQ_HIGH_SHELF_Q);
    memset(eq_state, 0, sizeof(eq_state));

    // Spectral noise gate
    dsps_fft2r_init_fc32(NULL, POST_FFT_SIZE);

    fft_buf     = (float *)heap_caps_calloc(POST_FFT_SIZE * 2, sizeof(float), MALLOC_CAP_SPIRAM);
    ana_window  = (float *)heap_caps_malloc(POST_FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM);
    syn_window  = (float *)heap_caps_malloc(POST_FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM);
    noise_floor = (float *)heap_caps_calloc(POST_FFT_HALF, sizeof(float), MALLOC_CAP_SPIRAM);
    prev_mag    = (float *)heap_caps_calloc(POST_FFT_HALF, sizeof(float), MALLOC_CAP_SPIRAM);

    // Generate sqrt(Hann) window for perfect reconstruction with 50% overlap
    // sqrt(Hann) * sqrt(Hann) with 50% OLA sums to exactly 1.0 (COLA compliant)
    for (int i = 0; i < POST_FFT_SIZE; i++) {
        float hann = 0.5f * (1.0f - cosf(2.0f * 3.14159265f * i / POST_FFT_SIZE));
        float sq = sqrtf(hann);
        ana_window[i] = sq;
        syn_window[i] = sq;
    }

    gate_frame_count = 0;
}

// ══════════════════════════════════════
// STAGE 2: SPECTRAL NOISE GATE (one frame)
// ══════════════════════════════════════

static void noise_gate_frame(const float *frame_in, float *frame_out) {
    // Analysis: apply sqrt(Hann) window
    for (int i = 0; i < POST_FFT_SIZE; i++) {
        fft_buf[2 * i]     = frame_in[i] * ana_window[i];
        fft_buf[2 * i + 1] = 0.0f;
    }

    dsps_fft2r_fc32(fft_buf, POST_FFT_SIZE);
    dsps_bit_rev2r_fc32(fft_buf, POST_FFT_SIZE);

    // Process each frequency bin
    for (int k = 0; k < POST_FFT_HALF; k++) {
        float re = fft_buf[2 * k];
        float im = fft_buf[2 * k + 1];
        float mag = sqrtf(re * re + im * im);

        // Track noise floor: asymmetric — falls fast, rises very slowly
        if (gate_frame_count < GATE_WARMUP_FRAMES) {
            // During warmup: fast tracking to learn initial noise floor
            if (noise_floor[k] == 0.0f)
                noise_floor[k] = mag;
            else
                noise_floor[k] = 0.8f * noise_floor[k] + 0.2f * mag;
        } else {
            // Normal operation: noise floor tracks slowly
            if (mag < noise_floor[k]) {
                noise_floor[k] = (1.0f - GATE_NOISE_DOWN) * noise_floor[k] + GATE_NOISE_DOWN * mag;
            } else {
                noise_floor[k] = (1.0f - GATE_NOISE_UP) * noise_floor[k] + GATE_NOISE_UP * mag;
            }
        }

        // Compute gain: only suppress bins clearly below noise floor
        float threshold = noise_floor[k] * GATE_OVER_FACTOR;
        float gain;
        if (mag > threshold) {
            // Above threshold: full pass-through
            gain = 1.0f;
        } else if (mag > noise_floor[k]) {
            // Between noise floor and threshold: gentle soft knee
            float ratio = (mag - noise_floor[k]) / (threshold - noise_floor[k] + 1e-10f);
            gain = GATE_MIN_GAIN + ratio * (1.0f - GATE_MIN_GAIN);
        } else {
            // Below noise floor: suppress but not kill
            gain = GATE_MIN_GAIN;
        }

        // Temporal smoothing to prevent musical noise
        gain = GATE_GAIN_SMOOTH * prev_mag[k] + (1.0f - GATE_GAIN_SMOOTH) * gain;
        prev_mag[k] = gain;

        // Apply gain
        fft_buf[2 * k]     = re * gain;
        fft_buf[2 * k + 1] = im * gain;
    }

    // Mirror conjugate
    for (int k = POST_FFT_HALF; k < POST_FFT_SIZE; k++) {
        int mirror = POST_FFT_SIZE - k;
        fft_buf[2 * k]     =  fft_buf[2 * mirror];
        fft_buf[2 * k + 1] = -fft_buf[2 * mirror + 1];
    }

    // IFFT
    for (int i = 0; i < POST_FFT_SIZE; i++)
        fft_buf[2 * i + 1] = -fft_buf[2 * i + 1];
    dsps_fft2r_fc32(fft_buf, POST_FFT_SIZE);
    dsps_bit_rev2r_fc32(fft_buf, POST_FFT_SIZE);

    float norm = 1.0f / (float)POST_FFT_SIZE;
    // Synthesis: apply sqrt(Hann) window (analysis × synthesis = Hann, COLA = 1.0)
    for (int i = 0; i < POST_FFT_SIZE; i++)
        frame_out[i] = -fft_buf[2 * i] * norm * syn_window[i];

    gate_frame_count++;
}

// ══════════════════════════════════════
// STAGE 3: FORMANT
// ══════════════════════════════════════

static void formant_enhance(float *samples, int n) {
    if (n < LPC_ORDER * 4) return;
    float r[LPC_ORDER + 1];
    autocorrelation(samples, n, r, LPC_ORDER);
    if (r[0] < 1e-6f) return;
    levinson_durbin(r, LPC_ORDER, lpc_a);

    float gamma1 = 1.0f - FORMANT_BOOST;
    float gamma2 = 1.0f - FORMANT_BOOST * 0.5f;
    float num_a[LPC_ORDER + 1], den_a[LPC_ORDER + 1];
    float g1p = 1.0f, g2p = 1.0f;
    num_a[0] = den_a[0] = 1.0f;
    for (int i = 1; i <= LPC_ORDER; i++) {
        g1p *= gamma1; g2p *= gamma2;
        num_a[i] = lpc_a[i] * g1p;
        den_a[i] = lpc_a[i] * g2p;
    }

    float hist_x[LPC_ORDER] = {}, hist_y[LPC_ORDER] = {};
    for (int i = 0; i < n; i++) {
        float x = samples[i];
        float y_fir = x;
        for (int j = 0; j < LPC_ORDER; j++)
            y_fir += num_a[j + 1] * hist_x[j];
        for (int j = LPC_ORDER - 1; j > 0; j--) hist_x[j] = hist_x[j - 1];
        hist_x[0] = x;
        float y = y_fir;
        for (int j = 0; j < LPC_ORDER; j++)
            y -= den_a[j + 1] * hist_y[j];
        for (int j = LPC_ORDER - 1; j > 0; j--) hist_y[j] = hist_y[j - 1];
        hist_y[0] = y;
        samples[i] = 0.6f * y + 0.4f * x;
    }
}

// ══════════════════════════════════════
// MAIN ENTRY POINT
// ══════════════════════════════════════

int post_process(int16_t *samples, int nsamples) {
    if (nsamples <= 0) return 0;

    float *fbuf = (float *)heap_caps_malloc(nsamples * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!fbuf) return nsamples;

    for (int i = 0; i < nsamples; i++)
        fbuf[i] = (float)samples[i];

    // ─── Stage 1: EQ ───
    if (eq_enabled) {
        dsps_biquad_f32(fbuf, fbuf, nsamples, eq_coeff, eq_state);
    }

    // ─── Stage 2: Spectral Noise Gate (overlap-add) ───
    if (mmse_enabled && nsamples >= POST_FFT_SIZE) {
        float *out_buf = (float *)heap_caps_calloc(nsamples + POST_FFT_SIZE,
                                                    sizeof(float), MALLOC_CAP_SPIRAM);
        if (out_buf) {
            float frame_out[POST_FFT_SIZE];
            for (int pos = 0; pos + POST_FFT_SIZE <= nsamples; pos += POST_FFT_HOP) {
                noise_gate_frame(fbuf + pos, frame_out);
                for (int i = 0; i < POST_FFT_SIZE; i++)
                    out_buf[pos + i] += frame_out[i];
            }
            memcpy(fbuf, out_buf, nsamples * sizeof(float));
            heap_caps_free(out_buf);
        }
    }

    // ─── Stage 3: Formant ───
    if (formant_enabled && nsamples >= LPC_ORDER * 4) {
        formant_enhance(fbuf, nsamples);
    }

    // Float → int16 with clipping
    for (int i = 0; i < nsamples; i++) {
        float v = fbuf[i];
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        samples[i] = (int16_t)v;
    }

    heap_caps_free(fbuf);
    return nsamples;
}

// ══════════════════════════════════════
// TOGGLES
// ══════════════════════════════════════

void post_set_eq_enabled(bool en)      { eq_enabled = en; }
void post_set_mmse_enabled(bool en)    { mmse_enabled = en; }
void post_set_formant_enabled(bool en) { formant_enabled = en; }
bool post_get_eq_enabled()      { return eq_enabled; }
bool post_get_mmse_enabled()    { return mmse_enabled; }
bool post_get_formant_enabled() { return formant_enabled; }
