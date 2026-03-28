/*
 * MOSA_MIC_PROJ — Tier 2: High-Quality Voice Engine
 * ================================================================
 * Dual INMP441 microphones on XIAO ESP32-S3
 * ESP-SR AFE_VC HIGH_PERF + NSNet + VADNet + AGC + BSS
 *
 * Audio Enhancement Pipeline:
 *   I2S (16kHz) → ESP-SR AFE_VC HIGH_PERF [BSS + NSNet + VADNet + AGC]
 *     → Coherence-based Wiener filter (dual-mic diffuse noise rejection)
 *     → MMSE-LSA residual noise suppression
 *     → Spectral tilt EQ (INMP441 compensation)
 *     → Formant enhancement (LPC post-filter)
 *     → Clean mono PCM → Ring buffer → Output
 *
 * Architecture:
 *   Core 1: AFE feed (I2S → interleave → AFE) + AFE fetch (clean → post-proc → RB)
 *   Core 0: Serial CLI, dashboard, plotter, WAV streaming
 *
 * Mic1 (I2S0): SCK=GPIO5  WS=GPIO43 SD=GPIO6  L/R=GPIO44
 * Mic2 (I2S1): SCK=GPIO9  WS=GPIO7  SD=GPIO8  L/R=GPIO4
 */

#include <cstdio>
#include <cstring>
#include <cmath>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

// ESP-SR AFE
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"

// Modules
#include "config/device_config.h"
#include "config/cal_store.h"
#include "hal/serial_io.h"
#include "audio/audio_buffer.h"
#include "audio/post_processor.h"
#include "audio/coherence_filter.h"
#include "audio/vad.h"

static const char *TAG = "MOSA";

// ══════════════════════════════════════
// I2S RAW BUFFERS (for calibration only)
// ══════════════════════════════════════

static int32_t raw1[BUF_SAMPLES];
static int32_t raw2[BUF_SAMPLES];

// ══════════════════════════════════════
// PER-MIC STATE (dashboard display)
// ══════════════════════════════════════

struct Mic {
    float hpPrev, hpOut;
    bool  hpReady;
    float calNoiseRMS, calNoisePeak, calDC, gateThreshold, gain;
    bool  calibrated;
    float rawDC, acRMS, acPeak, smoothAC;
    int16_t rawMin, rawMax;
    bool  active;
    float snr;
};

static Mic mic1 = {}, mic2 = {};
static char mode = 'B';
static char viewMode = 'D';
static int printCount = 0;
static bool wavActive = false;
static bool wavABMode = false;   // true = stereo A/B (ch1=raw, ch2=processed)
static int64_t wavStartUs = 0;

// ══════════════════════════════════════
// ESP-SR AFE STATE
// ══════════════════════════════════════

static const esp_afe_sr_iface_t *afe_handle = NULL;
static esp_afe_sr_data_t *afe_data = NULL;
static int afe_feed_chunksize = 0;
static int afe_feed_nch = 0;
static int afe_fetch_chunksize = 0;
static volatile bool afe_running = false;

// Dashboard snapshots from feed task
static volatile float snap_m1_rms = 0, snap_m2_rms = 0;
static volatile float snap_m1_peak = 0, snap_m2_peak = 0;
static volatile float snap_m1_dc = 0, snap_m2_dc = 0;
static volatile int16_t snap_m1_min = 0, snap_m1_max = 0;
static volatile int16_t snap_m2_min = 0, snap_m2_max = 0;

// Raw mic snapshots for coherence filter
static int16_t *snap_raw_m1 = NULL;
static int16_t *snap_raw_m2 = NULL;
static volatile int snap_raw_len = 0;

// ══════════════════════════════════════
// HELPERS
// ══════════════════════════════════════

static inline uint32_t millis() {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

// ══════════════════════════════════════
// I2S SETUP
// ══════════════════════════════════════

static void setup_i2s(i2s_port_t port, int sck, int ws, int sd) {
    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate          = SAMPLE_RATE;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = DMA_BUF_COUNT;
    cfg.dma_buf_len          = DMA_BUF_LEN;
    cfg.use_apll             = false;
    cfg.tx_desc_auto_clear   = false;
    cfg.fixed_mclk           = 0;

    ESP_ERROR_CHECK(i2s_driver_install(port, &cfg, 0, NULL));

    i2s_pin_config_t pins = {};
    pins.mck_io_num   = I2S_PIN_NO_CHANGE;
    pins.bck_io_num   = sck;
    pins.ws_io_num    = ws;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num  = sd;

    ESP_ERROR_CHECK(i2s_set_pin(port, &pins));
    i2s_zero_dma_buffer(port);
}

static size_t read_mic(i2s_port_t port, int32_t *buf) {
    size_t bytesRead = 0;
    esp_err_t ret = i2s_read(port, buf, BUF_SAMPLES * sizeof(int32_t), &bytesRead, pdMS_TO_TICKS(500));
    if (ret != ESP_OK || bytesRead == 0) {
        memset(buf, 0, BUF_SAMPLES * sizeof(int32_t));
        return 0;
    }
    return bytesRead / sizeof(int32_t);
}

// ══════════════════════════════════════
// CALIBRATION (with NVS persistence)
// ══════════════════════════════════════

static void process_mic(int32_t *raw, size_t totalSamples, Mic &m) {
    if (totalSamples < 2) return;
    m.rawMin = 32767; m.rawMax = -32768;
    float dcSum = 0, sqSum = 0, peakAbs = 0;
    size_t count = 0;
    for (size_t i = 0; i < totalSamples; i += 2) {
        int16_t s = (int16_t)(raw[i] >> 16);
        float x = (float)s;
        dcSum += x;
        if (s < m.rawMin) m.rawMin = s;
        if (s > m.rawMax) m.rawMax = s;
        float hp;
        if (!m.hpReady) {
            m.hpPrev = x; m.hpOut = 0; m.hpReady = true; hp = 0;
        } else {
            hp = DC_HP_ALPHA * (m.hpOut + x - m.hpPrev);
            m.hpPrev = x; m.hpOut = hp;
        }
        float absHP = fabsf(hp);
        sqSum += hp * hp;
        if (absHP > peakAbs) peakAbs = absHP;
        count++;
    }
    if (count == 0) return;
    m.rawDC  = dcSum / count;
    m.acRMS  = sqrtf(sqSum / count);
    m.acPeak = peakAbs;
    if (m.smoothAC == 0) m.smoothAC = m.acRMS;
    else m.smoothAC = SMOOTH_ALPHA * m.smoothAC + (1.0f - SMOOTH_ALPHA) * m.acRMS;
    if (m.calibrated) {
        m.active = (m.smoothAC > m.gateThreshold);
        m.snr = (m.calNoiseRMS > 0) ? 20.0f * log10f(m.acRMS / m.calNoiseRMS) : 0;
    } else {
        m.active = (m.acRMS > 100);
        m.snr = 0;
    }
}

static void calibrate(bool save_to_nvs) {
    serial_print("\n  Calibrating — SILENCE PLEASE!\n");

    bool was_running = afe_running;
    if (was_running) {
        afe_running = false;
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    mic1 = {}; mic2 = {};

    for (int i = 0; i < CAL_SKIP; i++) {
        read_mic(I2S_NUM_0, raw1); read_mic(I2S_NUM_1, raw2);
        process_mic(raw1, BUF_SAMPLES, mic1); process_mic(raw2, BUF_SAMPLES, mic2);
    }

    float sR1 = 0, sR2 = 0, sD1 = 0, sD2 = 0, mP1 = 0, mP2 = 0;
    for (int i = 0; i < CAL_ROUNDS; i++) {
        size_t n1 = read_mic(I2S_NUM_0, raw1);
        size_t n2 = read_mic(I2S_NUM_1, raw2);
        process_mic(raw1, n1, mic1); process_mic(raw2, n2, mic2);
        sR1 += mic1.acRMS; sR2 += mic2.acRMS;
        sD1 += mic1.rawDC;  sD2 += mic2.rawDC;
        if (mic1.acPeak > mP1) mP1 = mic1.acPeak;
        if (mic2.acPeak > mP2) mP2 = mic2.acPeak;
        if (i % 20 == 0)
            serial_printf("  [%2d/%d] M1: RMS=%5.0f DC=%6.0f | M2: RMS=%5.0f DC=%6.0f\n",
                           i, CAL_ROUNDS, mic1.acRMS, mic1.rawDC, mic2.acRMS, mic2.rawDC);
    }

    mic1.calNoiseRMS  = sR1 / CAL_ROUNDS; mic2.calNoiseRMS  = sR2 / CAL_ROUNDS;
    mic1.calNoisePeak = mP1;               mic2.calNoisePeak = mP2;
    mic1.calDC        = sD1 / CAL_ROUNDS;  mic2.calDC        = sD2 / CAL_ROUNDS;
    mic1.gateThreshold = mic1.calNoiseRMS * NOISE_MARGIN;
    mic2.gateThreshold = mic2.calNoiseRMS * NOISE_MARGIN;
    mic1.gain = 1.0f; mic2.gain = 1.0f;
    mic1.calibrated = true; mic2.calibrated = true;

    serial_printf("\n  M1: NoiseRMS=%.0f  Peak=%.0f  DC=%.0f  Gate=%.0f\n",
                  mic1.calNoiseRMS, mP1, mic1.calDC, mic1.gateThreshold);
    serial_printf("  M2: NoiseRMS=%.0f  Peak=%.0f  DC=%.0f  Gate=%.0f\n",
                  mic2.calNoiseRMS, mP2, mic2.calDC, mic2.gateThreshold);

    if (save_to_nvs) {
        CalData cal = {
            mic1.calNoiseRMS, mic1.calNoisePeak, mic1.calDC, mic1.gateThreshold,
            mic2.calNoiseRMS, mic2.calNoisePeak, mic2.calDC, mic2.gateThreshold
        };
        cal_store_save(cal);
        serial_print("  Calibration saved to NVS.\n");
    }

    serial_print("\n");

    if (was_running) {
        afe_running = true;
    }
}

static bool try_load_calibration() {
    CalData cal;
    if (!cal_store_load(cal)) return false;

    mic1.calNoiseRMS   = cal.m1_noiseRMS;
    mic1.calNoisePeak  = cal.m1_noisePeak;
    mic1.calDC         = cal.m1_dc;
    mic1.gateThreshold = cal.m1_gate;
    mic1.gain          = 1.0f;
    mic1.calibrated    = true;

    mic2.calNoiseRMS   = cal.m2_noiseRMS;
    mic2.calNoisePeak  = cal.m2_noisePeak;
    mic2.calDC         = cal.m2_dc;
    mic2.gateThreshold = cal.m2_gate;
    mic2.gain          = 1.0f;
    mic2.calibrated    = true;

    serial_printf("  Loaded cal from NVS: M1 gate=%.0f  M2 gate=%.0f\n",
                  mic1.gateThreshold, mic2.gateThreshold);
    return true;
}

// ══════════════════════════════════════
// AFE FEED TASK (Core 1)
// ══════════════════════════════════════

static void afe_feed_task(void *arg) {
    int total_feed = afe_feed_chunksize * afe_feed_nch;
    int16_t *feed_buf = (int16_t *)heap_caps_malloc(
        total_feed * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int32_t *i2s_buf1 = (int32_t *)heap_caps_malloc(
        afe_feed_chunksize * 2 * sizeof(int32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int32_t *i2s_buf2 = (int32_t *)heap_caps_malloc(
        afe_feed_chunksize * 2 * sizeof(int32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    // Buffers for raw mic samples (for coherence filter)
    int16_t *raw_m1_buf = (int16_t *)heap_caps_malloc(
        afe_feed_chunksize * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *raw_m2_buf = (int16_t *)heap_caps_malloc(
        afe_feed_chunksize * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!feed_buf || !i2s_buf1 || !i2s_buf2 || !raw_m1_buf || !raw_m2_buf) {
        ESP_LOGE(TAG, "Failed to allocate feed buffers!");
        vTaskDelete(NULL);
        return;
    }

    // Store pointers for snapshot access
    snap_raw_m1 = raw_m1_buf;
    snap_raw_m2 = raw_m2_buf;

    ESP_LOGI(TAG, "Feed task started: chunk=%d nch=%d", afe_feed_chunksize, afe_feed_nch);

    while (afe_running) {
        size_t bytes1 = 0, bytes2 = 0;
        i2s_read(I2S_NUM_0, i2s_buf1, afe_feed_chunksize * 2 * sizeof(int32_t),
                 &bytes1, pdMS_TO_TICKS(500));
        i2s_read(I2S_NUM_1, i2s_buf2, afe_feed_chunksize * 2 * sizeof(int32_t),
                 &bytes2, pdMS_TO_TICKS(500));

        int samples1 = bytes1 / sizeof(int32_t);
        int samples2 = bytes2 / sizeof(int32_t);

        float dc1 = 0, dc2 = 0, sq1 = 0, sq2 = 0, pk1 = 0, pk2 = 0;
        int16_t mn1 = 32767, mx1 = -32768, mn2 = 32767, mx2 = -32768;
        int cnt = 0;

        for (int i = 0; i < afe_feed_chunksize; i++) {
            int idx = i * 2;
            int16_t s1 = (idx < samples1) ? (int16_t)(i2s_buf1[idx] >> 16) : 0;
            int16_t s2 = (idx < samples2) ? (int16_t)(i2s_buf2[idx] >> 16) : 0;

            feed_buf[i * afe_feed_nch]     = s1;
            feed_buf[i * afe_feed_nch + 1] = s2;

            // Save raw samples for coherence filter
            raw_m1_buf[i] = s1;
            raw_m2_buf[i] = s2;

            dc1 += s1; dc2 += s2;
            sq1 += (float)s1 * s1; sq2 += (float)s2 * s2;
            float a1 = fabsf((float)s1), a2 = fabsf((float)s2);
            if (a1 > pk1) pk1 = a1;
            if (a2 > pk2) pk2 = a2;
            if (s1 < mn1) mn1 = s1;
            if (s1 > mx1) mx1 = s1;
            if (s2 < mn2) mn2 = s2;
            if (s2 > mx2) mx2 = s2;
            cnt++;
        }

        if (cnt > 0) {
            snap_m1_dc = dc1 / cnt; snap_m2_dc = dc2 / cnt;
            snap_m1_rms = sqrtf(sq1 / cnt); snap_m2_rms = sqrtf(sq2 / cnt);
            snap_m1_peak = pk1; snap_m2_peak = pk2;
            snap_m1_min = mn1; snap_m1_max = mx1;
            snap_m2_min = mn2; snap_m2_max = mx2;
            snap_raw_len = afe_feed_chunksize;
        }

        // Write raw mic1 to raw ring buffer (for A/B comparison)
        rb_raw_write(raw_m1_buf, afe_feed_chunksize);

        // Feed coherence filter with raw dual-mic data
        coh_feed(raw_m1_buf, raw_m2_buf, afe_feed_chunksize);

        // Feed AFE
        afe_handle->feed(afe_data, feed_buf);
    }

    heap_caps_free(feed_buf);
    heap_caps_free(i2s_buf1);
    heap_caps_free(i2s_buf2);
    heap_caps_free(raw_m1_buf);
    heap_caps_free(raw_m2_buf);
    snap_raw_m1 = NULL;
    snap_raw_m2 = NULL;
    ESP_LOGI(TAG, "Feed task stopped");
    vTaskDelete(NULL);
}

// ══════════════════════════════════════
// AFE FETCH TASK (Core 1)
// Gets clean audio, applies post-processing, writes to ring buffer
// ══════════════════════════════════════

static void afe_fetch_task(void *arg) {
    ESP_LOGI(TAG, "Fetch task started: chunk=%d", afe_fetch_chunksize);

    while (afe_running) {
        afe_fetch_result_t *res = afe_handle->fetch(afe_data);
        if (!res || res->ret_value == ESP_FAIL || !res->data)
            continue;

        int nsamples = afe_fetch_chunksize;

        // Update VAD state machine from AFE VAD output
        vad_fsm_update(res->vad_state);

        // Always write to pre-roll buffer (for VAD auto-record)
        vad_preroll_write(res->data, nsamples);

        // Apply coherence-based Wiener filter (uses raw dual-mic gain mask)
        coh_apply(res->data, nsamples);

        // Apply post-processing pipeline (EQ + MMSE-LSA + formant)
        post_process(res->data, nsamples);

        // Write to ring buffer
        rb_write(res->data, nsamples);
    }

    ESP_LOGI(TAG, "Fetch task stopped");
    vTaskDelete(NULL);
}

// ══════════════════════════════════════
// AFE INITIALIZATION (HIGH PERFORMANCE)
// ══════════════════════════════════════

static void init_afe() {
    serial_print("  Initializing ESP-SR AFE (HIGH_PERF)...\n");

    // Ring buffers (processed + raw for A/B comparison)
    rb_init(CLEAN_RB_SAMPLES);
    rb_raw_init(CLEAN_RB_SAMPLES);

    // Post-processing pipeline
    post_init();
    coh_init();
    vad_fsm_init();

    // Load models from flash
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) {
        ESP_LOGE(TAG, "Failed to load SR models from 'model' partition!");
        serial_print("  [ERROR] Model partition empty — flash models first!\n");
        return;
    }

    // ╔══════════════════════════════════════════════════════════════╗
    // ║  AFE Configuration: HIGH PERFORMANCE Voice Communication    ║
    // ║  BSS + NSNet + VADNet + AGC — maximum voice quality         ║
    // ╚══════════════════════════════════════════════════════════════╝

    // AFE_TYPE_VC: 1MIC + NSNet (neural noise suppression, ~15-20 dB)
    // Note: 2MIC SR mode has BSS but no NSNet — net worse.
    // VC selects first mic channel from "MM" input.
    afe_config_t *cfg = afe_config_init("MM", models, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    if (!cfg) {
        ESP_LOGE(TAG, "Failed to create AFE config!");
        return;
    }

    // --- Noise Suppression: NSNet (neural, ~15-20 dB suppression) ---
    cfg->ns_init = true;
    cfg->afe_ns_mode = AFE_NS_MODE_NET;
    cfg->ns_model_name = esp_srmodel_filter(models, "nsnet", NULL);
    ESP_LOGI(TAG, "NS model selected: %s", cfg->ns_model_name ? cfg->ns_model_name : "NONE");

    // --- Speech Enhancement / BSS: disabled (VC mode is 1MIC, BSS not available) ---
    cfg->se_init = false;

    // --- VAD: DNN-based (VADNet, trained on 15k hours) ---
    cfg->vad_init = true;
    cfg->vad_mode = VAD_MODE_2;             // Very aggressive noise rejection
    cfg->vad_min_speech_ms = 200;           // 200ms minimum speech
    cfg->vad_min_noise_ms = 800;            // 800ms minimum silence
    cfg->vad_delay_ms = 128;                // Pre-roll cache in AFE

    // --- AGC: DISABLED for clean A/B comparison ---
    // AGC undoes noise suppression by boosting everything (incl. residual noise)
    // Re-enable after A/B testing is validated
    cfg->agc_init = false;

    // --- WakeNet: disabled (not needed for voice recording) ---
    cfg->wakenet_init = false;

    // --- AEC: disabled (no speaker playback in recording device) ---
    cfg->aec_init = false;

    // --- General ---
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg->afe_perferred_core = 1;
    cfg->afe_linear_gain = 1.0f;            // Unity gain (AGC handles levels)

    // Print BEFORE afe_config_check
    serial_printf("  [PRE-CHECK]  NS:%s mode:%d  SE:%s  VAD:%s  AGC:%s\n",
                  cfg->ns_init ? "ON" : "off",
                  cfg->afe_ns_mode,
                  cfg->se_init ? "ON" : "off",
                  cfg->vad_init ? "ON" : "off",
                  cfg->agc_init ? "ON" : "off");
    if (cfg->ns_model_name)
        serial_printf("  [PRE-CHECK]  NS model: %s\n", cfg->ns_model_name);

    // Validate config (auto-resolves conflicts — MAY OVERRIDE SETTINGS!)
    cfg = afe_config_check(cfg);

    // Print AFTER afe_config_check — see what changed
    serial_printf("  [POST-CHECK] NS:%s mode:%d  SE:%s  VAD:%s  AGC:%s\n",
                  cfg->ns_init ? "ON" : "off",
                  cfg->afe_ns_mode,
                  cfg->se_init ? "ON" : "off",
                  cfg->vad_init ? "ON" : "off",
                  cfg->agc_init ? "ON" : "off");
    if (cfg->ns_model_name)
        serial_printf("  [POST-CHECK] NS model: %s\n", cfg->ns_model_name);
    else
        serial_print("  [POST-CHECK] NS model: NULL (no model loaded!)\n");

    // Create AFE
    afe_handle = esp_afe_handle_from_config(cfg);
    if (!afe_handle) {
        ESP_LOGE(TAG, "Failed to get AFE handle!");
        afe_config_free(cfg);
        return;
    }

    afe_data = afe_handle->create_from_config(cfg);
    if (!afe_data) {
        ESP_LOGE(TAG, "Failed to create AFE data!");
        afe_config_free(cfg);
        return;
    }

    afe_feed_chunksize  = afe_handle->get_feed_chunksize(afe_data);
    afe_feed_nch        = afe_handle->get_feed_channel_num(afe_data);
    afe_fetch_chunksize = afe_handle->get_fetch_chunksize(afe_data);

    serial_printf("  AFE ready: feed_chunk=%d nch=%d fetch_chunk=%d\n",
                  afe_feed_chunksize, afe_feed_nch, afe_fetch_chunksize);

    // Debug: print what's actually enabled in the pipeline
    serial_printf("  NS: %s (mode=%d)  SE: %s  VAD: %s  AGC: %s  AEC: %s\n",
                  cfg->ns_init ? "ON" : "off",
                  cfg->afe_ns_mode,
                  cfg->se_init ? "ON" : "off",
                  cfg->vad_init ? "ON" : "off",
                  cfg->agc_init ? "ON" : "off",
                  cfg->aec_init ? "ON" : "off");
    if (cfg->ns_model_name)
        serial_printf("  NS model: %s\n", cfg->ns_model_name);
    else
        serial_print("  NS model: (auto/default)\n");

    afe_config_free(cfg);

    serial_print("  VC mode: NSNet neural noise suppression active\n");

    // Print the actual pipeline order (uses afe_data, not cfg)
    afe_handle->print_pipeline(afe_data);

    // Start tasks on Core 1
    afe_running = true;
    xTaskCreatePinnedToCore(afe_feed_task, "afe_feed", 8 * 1024, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(afe_fetch_task, "afe_fetch", 8 * 1024, NULL, 5, NULL, 1);

    serial_print("  AFE + post-processing tasks started on Core 1\n");
}

// ══════════════════════════════════════
// DASHBOARD
// ══════════════════════════════════════

static void print_bar(float value, float maxVal, int width) {
    int f = (maxVal > 0) ? (int)((value * width) / maxVal) : 0;
    if (f < 0) f = 0;
    if (f > width) f = width;
    char bar[32];
    for (int i = 0; i < width && i < 31; i++)
        bar[i] = (i < f) ? '#' : '-';
    bar[width] = '\0';
    serial_print(bar);
}

static const char* vad_state_str() {
    switch (vad_fsm_get_state()) {
        case VAD_STATE_IDLE:        return "IDLE";
        case VAD_STATE_PRE_SPEECH:  return "PRE ";
        case VAD_STATE_SPEECH:      return "TALK";
        case VAD_STATE_POST_SPEECH: return "HOLD";
        default:                    return "????";
    }
}

static void mode_dashboard() {
    mic1.rawDC = snap_m1_dc; mic1.acRMS = snap_m1_rms; mic1.acPeak = snap_m1_peak;
    mic1.rawMin = snap_m1_min; mic1.rawMax = snap_m1_max;
    mic2.rawDC = snap_m2_dc; mic2.acRMS = snap_m2_rms; mic2.acPeak = snap_m2_peak;
    mic2.rawMin = snap_m2_min; mic2.rawMax = snap_m2_max;

    if (mic1.smoothAC == 0) mic1.smoothAC = mic1.acRMS;
    else mic1.smoothAC = SMOOTH_ALPHA * mic1.smoothAC + (1.0f - SMOOTH_ALPHA) * mic1.acRMS;
    if (mic2.smoothAC == 0) mic2.smoothAC = mic2.acRMS;
    else mic2.smoothAC = SMOOTH_ALPHA * mic2.smoothAC + (1.0f - SMOOTH_ALPHA) * mic2.acRMS;

    if (mic1.calibrated) {
        mic1.active = (mic1.smoothAC > mic1.gateThreshold);
        mic1.snr = (mic1.calNoiseRMS > 0) ? 20.0f * log10f(mic1.acRMS / mic1.calNoiseRMS) : 0;
    }
    if (mic2.calibrated) {
        mic2.active = (mic2.smoothAC > mic2.gateThreshold);
        mic2.snr = (mic2.calNoiseRMS > 0) ? 20.0f * log10f(mic2.acRMS / mic2.calNoiseRMS) : 0;
    }

    bool s1 = (mode == 'B' || mode == '1'), s2 = (mode == 'B' || mode == '2');

    if (printCount % 25 == 0) {
        const char *mn = (mode == 'B') ? "BOTH" : (mode == '1') ? "MIC1" : "MIC2";
        serial_printf("\n=== %s === [AFE HIGH_PERF + NSNet + VADNet + AGC + Post]  VAD:%s\n", mn, vad_state_str());
        serial_printf("  EQ:%s  MMSE:%s  Formant:%s  Coherence:%s\n",
                      post_get_eq_enabled() ? "ON" : "off",
                      post_get_mmse_enabled() ? "ON" : "off",
                      post_get_formant_enabled() ? "ON" : "off",
                      coh_get_enabled() ? "ON" : "off");
        serial_print("Mic | Gate | DC(raw)| HP RMS|Smooth|  SNR  | Min~Max | Level\n");
        serial_print("----+------+--------+-------+------+-------+---------+---------\n");
    }

    float dm1 = mic1.gateThreshold * 10.0f;
    if (dm1 < 3000.0f) dm1 = 3000.0f;
    float dm2 = mic2.gateThreshold * 10.0f;
    if (dm2 < 3000.0f) dm2 = 3000.0f;

    if (s1) {
        serial_printf(" M1 |%s |%6.0f  |%5.0f  |%5.0f |%5.1fdB|%6d~%5d| ",
                      mic1.active ? " OPEN" : " shut", mic1.rawDC, mic1.acRMS,
                      mic1.smoothAC, mic1.snr, mic1.rawMin, mic1.rawMax);
        print_bar(mic1.smoothAC, dm1, 9);
        serial_print("\n");
    }
    if (s2) {
        serial_printf(" M2 |%s |%6.0f  |%5.0f  |%5.0f |%5.1fdB|%6d~%5d| ",
                      mic2.active ? " OPEN" : " shut", mic2.rawDC, mic2.acRMS,
                      mic2.smoothAC, mic2.snr, mic2.rawMin, mic2.rawMax);
        print_bar(mic2.smoothAC, dm2, 9);
        serial_print("\n");
    }

    printCount++;
    vTaskDelay(pdMS_TO_TICKS(100));
}

// ══════════════════════════════════════
// PLOTTER (clean audio from post-processing)
// ══════════════════════════════════════

static void mode_plotter() {
    int16_t buf[256];
    int n = rb_read(buf, 256);
    if (n > 0) {
        int step = 8;
        for (int i = 0; i < n; i += step)
            serial_printf("%d\n", (int)buf[i]);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
}

// ══════════════════════════════════════
// WAV STREAM
// ══════════════════════════════════════

static void start_wav_stream() {
    wavActive = true;
    wavStartUs = esp_timer_get_time();
    rb_reset();
    rb_raw_reset();
    serial_print("WAV_START\n");
    serial_printf("RATE:%d\n", SAMPLE_RATE);
    serial_print("BITS:16\n");
    serial_printf("CHANNELS:%d\n", wavABMode ? 2 : 1);
    serial_printf("DURATION:%d\n", WAV_DURATION_S);
    if (wavABMode) serial_print("MODE:AB\n");
    serial_print("DATA_BEGIN\n");
    serial_flush();
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void mode_wav_stream() {
    if (!wavActive) return;

    int64_t elapsed_us = esp_timer_get_time() - wavStartUs;
    if (elapsed_us > (int64_t)WAV_DURATION_S * 1000000LL) {
        serial_print("\nDATA_END\n");
        serial_flush();
        vTaskDelay(pdMS_TO_TICKS(50));
        wavActive = false;
        wavABMode = false;
        viewMode = 'D';
        serial_printf("\n  WAV done (%d sec). Back to dashboard.\n\n", WAV_DURATION_S);
        return;
    }

    if (wavABMode) {
        // A/B stereo: interleave raw (ch1) + processed (ch2)
        // Processed buffer is the bottleneck (AFE latency) — let it drive
        int16_t raw_buf[256], proc_buf[256], stereo_buf[512];
        int n_proc = rb_read(proc_buf, 256);
        if (n_proc > 0) {
            int n_raw = rb_raw_read(raw_buf, n_proc);
            // Pad raw if it hasn't caught up yet
            for (int i = n_raw; i < n_proc; i++) raw_buf[i] = 0;
            for (int i = 0; i < n_proc; i++) {
                stereo_buf[2 * i]     = raw_buf[i];   // Left = raw
                stereo_buf[2 * i + 1] = proc_buf[i];  // Right = processed
            }
            serial_write_bytes(stereo_buf, n_proc * 2 * sizeof(int16_t));
        } else {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    } else {
        // Normal mono processed output
        int16_t send_buf[512];
        int n = rb_read(send_buf, 512);
        if (n > 0) {
            serial_write_bytes(send_buf, n * sizeof(int16_t));
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

// ══════════════════════════════════════
// COMMANDS
// ══════════════════════════════════════

static void handle_commands() {
    while (serial_available()) {
        int raw = serial_read();
        if (raw < 0) break;
        char c = (char)raw;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c <= ' ') continue;

        switch (c) {
            case '1':
                mode = '1'; printCount = 0;
                if (viewMode == 'D') serial_print("\n  >> Mic1\n\n");
                break;
            case '2':
                mode = '2'; printCount = 0;
                if (viewMode == 'D') serial_print("\n  >> Mic2\n\n");
                break;
            case 'B':
                mode = 'B'; printCount = 0;
                if (viewMode == 'D') serial_print("\n  >> Both\n\n");
                break;
            case 'C':
                viewMode = 'D';
                calibrate(true);
                printCount = 0;
                break;
            case 'D':
                viewMode = 'D'; printCount = 0; wavActive = false;
                serial_print("\n  >> Dashboard mode\n\n");
                break;
            case 'P':
                viewMode = 'P'; wavActive = false;
                serial_print("\n  >> Plotter mode (post-processed AFE output)\n");
                serial_print("  >> Send D to return to dashboard\n\n");
                vTaskDelay(pdMS_TO_TICKS(500));
                break;
            case 'W':
                viewMode = 'W';
                wavABMode = false;
                serial_printf("\n  >> WAV stream: %d sec, %d Hz, mono (processed)\n", WAV_DURATION_S, SAMPLE_RATE);
                serial_print("  >> Pipeline: AFE HIGH_PERF → Coherence → MMSE-LSA → EQ → Formant\n");
                serial_print("  >> Start Python recorder, then send any key...\n\n");
                while (!serial_available()) vTaskDelay(pdMS_TO_TICKS(10));
                serial_read();
                start_wav_stream();
                break;
            case 'A':
                viewMode = 'W';
                wavABMode = true;
                serial_printf("\n  >> A/B WAV: %d sec, %d Hz, STEREO\n", WAV_DURATION_S, SAMPLE_RATE);
                serial_print("  >> Left=RAW mic  Right=PROCESSED (AFE+post)\n");
                serial_print("  >> Start Python recorder, then send any key...\n\n");
                while (!serial_available()) vTaskDelay(pdMS_TO_TICKS(10));
                serial_read();
                start_wav_stream();
                break;
            case 'V': {
                bool en = !vad_fsm_get_auto_record();
                vad_fsm_set_auto_record(en);
                serial_printf("\n  >> VAD auto-record: %s\n\n", en ? "ON" : "OFF");
                break;
            }
            case 'E': {
                bool en = !post_get_eq_enabled();
                post_set_eq_enabled(en);
                serial_printf("\n  >> Spectral tilt EQ: %s\n\n", en ? "ON" : "OFF");
                printCount = 0;
                break;
            }
            case 'M': {
                bool en = !post_get_mmse_enabled();
                post_set_mmse_enabled(en);
                serial_printf("\n  >> MMSE-LSA: %s\n\n", en ? "ON" : "OFF");
                printCount = 0;
                break;
            }
            case 'F': {
                bool en = !post_get_formant_enabled();
                post_set_formant_enabled(en);
                serial_printf("\n  >> Formant enhancement: %s\n\n", en ? "ON" : "OFF");
                printCount = 0;
                break;
            }
            case 'G': {
                bool en = !coh_get_enabled();
                coh_set_enabled(en);
                serial_printf("\n  >> Coherence filter: %s\n\n", en ? "ON" : "OFF");
                printCount = 0;
                break;
            }
            case 'H':
                serial_print("\n  +═══════════════════════════════════════════════+\n");
                serial_print("  | MOSA Tier 2 — High-Quality Voice Engine      |\n");
                serial_print("  +───────────────────────────────────────────────+\n");
                serial_print("  | VIEW:  D=Dashboard  P=Plotter  W=WAV  A=A/B  |\n");
                serial_print("  | MIC:   1=Mic1  2=Mic2  B=Both                |\n");
                serial_print("  | AUDIO: C=Calibrate  V=VAD auto-record toggle |\n");
                serial_print("  | POST:  E=EQ  M=MMSE  F=Formant  G=Coherence  |\n");
                serial_print("  | INFO:  H=Help                                |\n");
                serial_print("  +───────────────────────────────────────────────+\n");
                serial_print("  | Pipeline: AFE_VC HIGH_PERF                   |\n");
                serial_print("  |   BSS + NSNet DNN + VADNet + AGC             |\n");
                serial_print("  |   → Coherence Wiener → MMSE-LSA             |\n");
                serial_print("  |   → Spectral Tilt EQ → Formant Enhance      |\n");
                serial_print("  +═══════════════════════════════════════════════+\n\n");
                break;
        }
    }
}

// ══════════════════════════════════════
// MAIN
// ══════════════════════════════════════

extern "C" void app_main(void) {
    serial_init();
    vTaskDelay(pdMS_TO_TICKS(500));

    // NVS init (for calibration persistence)
    cal_store_init();

    // GPIO: L/R pins LOW for left channel
    gpio_set_direction(M1_LR, GPIO_MODE_OUTPUT);
    gpio_set_level(M1_LR, 0);
    gpio_set_direction(M2_LR, GPIO_MODE_OUTPUT);
    gpio_set_level(M2_LR, 0);

    // I2S at 16 kHz
    setup_i2s(I2S_NUM_0, M1_SCK, M1_WS, M1_SD);
    setup_i2s(I2S_NUM_1, M2_SCK, M2_WS, M2_SD);

    serial_print("\n  +═══════════════════════════════════════════════+\n");
    serial_print("  |  MOSA Tier 2 — High-Quality Voice Engine      |\n");
    serial_print("  |  Dual INMP441 + ESP-SR AFE HIGH_PERF          |\n");
    serial_print("  |  BSS + NSNet + VADNet + AGC                   |\n");
    serial_print("  |  + Coherence Wiener + MMSE-LSA + EQ + Formant |\n");
    serial_print("  |  M1: SCK=5 WS=43 SD=6   (I2S0)               |\n");
    serial_print("  |  M2: SCK=9 WS=7  SD=8   (I2S1)               |\n");
    serial_print("  |  16kHz | Neural + Classical DSP pipeline      |\n");
    serial_print("  |  [D]ash [P]lot [W]av [C]al [V]AD [H]elp     |\n");
    serial_print("  +═══════════════════════════════════════════════+\n");

    // Try loading calibration from NVS; if not available, run fresh calibration
    if (!try_load_calibration()) {
        calibrate(true);
    }

    // Initialize AFE and start pipeline
    init_afe();

    // Main loop on Core 0
    while (1) {
        handle_commands();
        switch (viewMode) {
            case 'D': mode_dashboard(); break;
            case 'P': mode_plotter();   break;
            case 'W': mode_wav_stream(); break;
        }
    }
}
