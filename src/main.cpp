/*
 * MOSA_MIC_PROJ — ESP-IDF Native
 * ================================================================
 * Dual INMP441 Beamformed Noise Reduction System
 * XIAO ESP32-S3 with hardware-accelerated DSP
 *
 * Pipeline (WAV mode):
 *   Mic1 + Mic2 → HP filter → Beamform (average) →
 *   FFT (esp-dsp HW accel) → Spectral Subtraction →
 *   IFFT → Overlap-Add → Clean mono WAV
 *
 * Mic1 (I2S0): SCK=GPIO5  WS=GPIO43 SD=GPIO6  L/R=GPIO44
 * Mic2 (I2S1): SCK=GPIO9  WS=GPIO7  SD=GPIO8  L/R=GPIO4
 *
 * Framework: ESP-IDF (PlatformIO)
 * DSP:       esp-dsp (hardware-accelerated FFT on ESP32-S3)
 */

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cmath>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_timer.h"

// esp-dsp: hardware-accelerated FFT on ESP32-S3
#include "dsps_fft2r.h"
#include "dsps_wind_hann.h"

static const char *TAG = "MOSA";

// ══════════════════════════════════════
// PIN DEFINITIONS
// ══════════════════════════════════════

// Mic1 (I2S0)
#define M1_SCK  GPIO_NUM_5
#define M1_WS   GPIO_NUM_43
#define M1_SD   GPIO_NUM_6
#define M1_LR   GPIO_NUM_44

// Mic2 (I2S1)
#define M2_SCK  GPIO_NUM_9
#define M2_WS   GPIO_NUM_7
#define M2_SD   GPIO_NUM_8
#define M2_LR   GPIO_NUM_4

// ══════════════════════════════════════
// AUDIO CONFIGURATION
// ══════════════════════════════════════

#define SAMPLE_RATE     44100
#define DMA_BUF_COUNT   8
#define DMA_BUF_LEN     512
#define BUF_SAMPLES     1024

// ══════════════════════════════════════
// CALIBRATION
// ══════════════════════════════════════

#define CAL_ROUNDS      80
#define CAL_SKIP        20
#define NOISE_MARGIN    1.5f
#define DC_HP_ALPHA     0.995f
#define SMOOTH_ALPHA    0.85f

// ══════════════════════════════════════
// FFT NOISE REDUCTION (esp-dsp accelerated)
// ══════════════════════════════════════

#define FFT_SIZE        512
#define FFT_HOP         (FFT_SIZE / 2)      // 256 samples, 50% overlap
#define FFT_BINS        (FFT_SIZE / 2 + 1)  // 257 unique magnitude bins
#define OVERSUB_FACTOR  2.0f                // Noise oversubtraction (1.0=gentle, 4.0=aggressive)
#define SPECTRAL_FLOOR  0.02f               // Minimum gain per bin (prevents musical noise)

// ══════════════════════════════════════
// SERIAL / TIMING
// ══════════════════════════════════════

#define WAV_DURATION_S  10

// ══════════════════════════════════════
// I2S RAW BUFFERS
// ══════════════════════════════════════

static int32_t raw1[BUF_SAMPLES];
static int32_t raw2[BUF_SAMPLES];

// ══════════════════════════════════════
// PER-MIC STATE
// ══════════════════════════════════════

struct Mic {
    float hpPrev, hpOut;
    bool  hpReady;
    // WAV-mode HP filter (separate from dashboard HP)
    float wavHpPrev, wavHpOut;
    bool  wavHpReady;
    float calNoiseRMS, calNoisePeak, calDC, gateThreshold, gain;
    bool  calibrated;
    float rawDC, acRMS, acPeak, smoothAC;
    int16_t rawMin, rawMax;
    bool  active;
    float snr;
};

static Mic mic1 = {}, mic2 = {};
static char mode = 'B';       // 1, 2, B
static char viewMode = 'D';   // D=Dashboard, P=Plotter, W=WAV
static int printCount = 0;
static bool wavActive = false;
static int64_t wavStartUs = 0;

// ══════════════════════════════════════
// FFT NOISE REDUCTION STATE
// ══════════════════════════════════════

// esp-dsp uses interleaved complex: [re0, im0, re1, im1, ...]
static float __attribute__((aligned(16))) fftData[FFT_SIZE * 2];
static float hannWindow[FFT_SIZE];
static float noiseSpectrum[FFT_BINS];
static bool  noiseSpectrumReady = false;
static float inputRing[FFT_SIZE];
static int   inputRingPos = 0;
static float olaBuffer[FFT_HOP];
static bool  olaFirstFrame = true;

// ══════════════════════════════════════
// SERIAL I/O (USB Serial/JTAG)
// ══════════════════════════════════════

static int s_peek = -1;

static void serial_init() {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 32768;
    cfg.rx_buffer_size = 1024;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
}

static void serial_print(const char *str) {
    usb_serial_jtag_write_bytes(str, strlen(str), pdMS_TO_TICKS(200));
}

static void serial_printf(const char *fmt, ...) {
    static char buf[512];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0)
        usb_serial_jtag_write_bytes(buf, len, pdMS_TO_TICKS(200));
}

static void serial_write_bytes(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        int sent = usb_serial_jtag_write_bytes(p, len, pdMS_TO_TICKS(1000));
        if (sent > 0) { p += sent; len -= sent; }
        else vTaskDelay(1);
    }
}

static void serial_flush() {
    vTaskDelay(pdMS_TO_TICKS(20));
}

static bool serial_available() {
    if (s_peek >= 0) return true;
    uint8_t c;
    int n = usb_serial_jtag_read_bytes(&c, 1, 0);
    if (n > 0) { s_peek = c; return true; }
    return false;
}

static int serial_read() {
    if (s_peek >= 0) { int c = s_peek; s_peek = -1; return c; }
    uint8_t c;
    int n = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(10));
    return (n > 0) ? (int)c : -1;
}

// ══════════════════════════════════════
// MILLIS HELPER
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
    cfg.use_apll             = true;
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
// HP FILTER
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
        m.snr    = (m.calNoiseRMS > 0) ? 20.0f * log10f(m.acRMS / m.calNoiseRMS) : 0;
    } else {
        m.active = (m.acRMS > 100);
        m.snr    = 0;
    }
}

// WAV-mode HP filter — returns float for FFT pipeline
static inline float apply_wav_hp(float x, Mic &m) {
    if (!m.wavHpReady) {
        m.wavHpPrev = x; m.wavHpOut = 0; m.wavHpReady = true;
        return 0;
    }
    float hp = DC_HP_ALPHA * (m.wavHpOut + x - m.wavHpPrev);
    m.wavHpPrev = x; m.wavHpOut = hp;
    return hp;
}

// ══════════════════════════════════════
// FFT UTILITIES (esp-dsp hardware accelerated)
// ══════════════════════════════════════

static void init_dsp() {
    // Initialize esp-dsp FFT twiddle factors for our FFT size
    esp_err_t ret = dsps_fft2r_init_fc32(NULL, FFT_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FFT init failed: %s", esp_err_to_name(ret));
    }

    // Generate Hann window using esp-dsp
    dsps_wind_hann_f32(hannWindow, FFT_SIZE);
}

// Forward FFT using esp-dsp (hardware accelerated on ESP32-S3)
static void fft_forward(float *data) {
    dsps_fft2r_fc32(data, FFT_SIZE);
    dsps_bit_rev_fc32(data, FFT_SIZE);
}

// Inverse FFT: conjugate → forward FFT → conjugate + normalize
static void fft_inverse(float *data) {
    // Conjugate (negate imaginary parts)
    for (int i = 0; i < FFT_SIZE; i++)
        data[2 * i + 1] = -data[2 * i + 1];

    // Forward FFT
    dsps_fft2r_fc32(data, FFT_SIZE);
    dsps_bit_rev_fc32(data, FFT_SIZE);

    // Conjugate and normalize by 1/N
    float inv_n = 1.0f / FFT_SIZE;
    for (int i = 0; i < FFT_SIZE; i++) {
        data[2 * i]     *=  inv_n;
        data[2 * i + 1] *= -inv_n;
    }
}

// Process one FFT frame: window → FFT → spectral subtraction → IFFT → overlap-add
// Returns number of output samples written to outBuf
static int process_fft_frame(float *frame, int16_t *outBuf) {
    // Pack into interleaved complex: [re, im, re, im, ...]
    for (int i = 0; i < FFT_SIZE; i++) {
        fftData[2 * i]     = frame[i] * hannWindow[i];  // real
        fftData[2 * i + 1] = 0;                         // imaginary
    }

    // Forward FFT (hardware accelerated)
    fft_forward(fftData);

    // Spectral subtraction using calibrated noise profile
    if (noiseSpectrumReady) {
        // DC bin (index 0, no mirror)
        {
            float mag = fabsf(fftData[0]);
            if (mag > 0) {
                float clean = mag - OVERSUB_FACTOR * noiseSpectrum[0];
                if (clean < mag * SPECTRAL_FLOOR) clean = mag * SPECTRAL_FLOOR;
                fftData[0] *= clean / mag;
            }
        }
        // Nyquist bin (index N/2, no mirror)
        {
            int idx = FFT_SIZE / 2;
            float re = fftData[2 * idx], im = fftData[2 * idx + 1];
            float mag = sqrtf(re * re + im * im);
            if (mag > 0) {
                float clean = mag - OVERSUB_FACTOR * noiseSpectrum[idx];
                if (clean < mag * SPECTRAL_FLOOR) clean = mag * SPECTRAL_FLOOR;
                float g = clean / mag;
                fftData[2 * idx]     *= g;
                fftData[2 * idx + 1] *= g;
            }
        }
        // Bins 1..N/2-1 with conjugate mirrors
        for (int b = 1; b < FFT_SIZE / 2; b++) {
            float re = fftData[2 * b], im = fftData[2 * b + 1];
            float mag = sqrtf(re * re + im * im);
            if (mag > 0) {
                float clean = mag - OVERSUB_FACTOR * noiseSpectrum[b];
                if (clean < mag * SPECTRAL_FLOOR) clean = mag * SPECTRAL_FLOOR;
                float g = clean / mag;
                // Apply gain
                fftData[2 * b]     *= g;
                fftData[2 * b + 1] *= g;
                // Mirror (conjugate symmetry for real signal)
                int m = FFT_SIZE - b;
                fftData[2 * m]     =  fftData[2 * b];
                fftData[2 * m + 1] = -fftData[2 * b + 1];
            }
        }
    }

    // Inverse FFT (hardware accelerated)
    fft_inverse(fftData);

    // Overlap-add: combine first half with previous frame's tail
    int outCount = 0;
    if (!olaFirstFrame) {
        for (int i = 0; i < FFT_HOP; i++) {
            float s = fftData[2 * i] + olaBuffer[i];  // real part only
            if (s >  32767.0f) s =  32767.0f;
            if (s < -32768.0f) s = -32768.0f;
            outBuf[outCount++] = (int16_t)s;
        }
    } else {
        olaFirstFrame = false;
    }

    // Save current frame's tail for next overlap
    for (int i = 0; i < FFT_HOP; i++)
        olaBuffer[i] = fftData[2 * (i + FFT_HOP)];  // real part

    return outCount;
}

// ══════════════════════════════════════
// CALIBRATION
// ══════════════════════════════════════

static void calibrate() {
    serial_print("\n  Calibrating — SILENCE PLEASE!\n");
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

    float ratio = (mic2.calNoiseRMS > 0) ? mic1.calNoiseRMS / mic2.calNoiseRMS : 999;
    if (ratio > 5) serial_printf("  [WARN] M1 is %.0fx noisier than M2\n", ratio);

    // ── Build noise magnitude spectrum (esp-dsp accelerated FFT) ──
    serial_print("  Computing noise spectrum...\n");
    for (int b = 0; b < FFT_BINS; b++) noiseSpectrum[b] = 0;
    int specFrames = 0;
    int specPos = 0;

    mic1.wavHpPrev = 0; mic1.wavHpOut = 0; mic1.wavHpReady = false;
    mic2.wavHpPrev = 0; mic2.wavHpOut = 0; mic2.wavHpReady = false;

    // Let HP filter settle
    for (int r = 0; r < 5; r++) {
        size_t n1 = read_mic(I2S_NUM_0, raw1);
        size_t n2 = read_mic(I2S_NUM_1, raw2);
        size_t maxN = (n1 > n2) ? n1 : n2;
        for (size_t i = 0; i < maxN; i += 2) {
            if (i < n1) apply_wav_hp((float)(raw1[i] >> 16), mic1);
            if (i < n2) apply_wav_hp((float)(raw2[i] >> 16), mic2);
        }
    }

    // Collect noise frames
    for (int r = 0; r < 25; r++) {
        size_t n1 = read_mic(I2S_NUM_0, raw1);
        size_t n2 = read_mic(I2S_NUM_1, raw2);
        size_t maxN = (n1 > n2) ? n1 : n2;

        for (size_t i = 0; i < maxN; i += 2) {
            float hp1 = apply_wav_hp((float)(raw1[i] >> 16), mic1);
            float hp2 = (i < n2) ? apply_wav_hp((float)(raw2[i] >> 16), mic2) : hp1;
            float mono = (hp1 + hp2) * 0.5f;

            inputRing[specPos++] = mono;

            if (specPos == FFT_SIZE) {
                // Pack and window
                for (int k = 0; k < FFT_SIZE; k++) {
                    fftData[2 * k]     = inputRing[k] * hannWindow[k];
                    fftData[2 * k + 1] = 0;
                }
                // Hardware-accelerated FFT
                fft_forward(fftData);

                // Accumulate magnitudes
                for (int b = 0; b < FFT_BINS; b++) {
                    float re = fftData[2 * b], im = fftData[2 * b + 1];
                    noiseSpectrum[b] += sqrtf(re * re + im * im);
                }
                specFrames++;

                // 50% overlap shift
                for (int k = 0; k < FFT_HOP; k++)
                    inputRing[k] = inputRing[k + FFT_HOP];
                specPos = FFT_HOP;
            }
        }
    }

    if (specFrames > 0) {
        for (int b = 0; b < FFT_BINS; b++)
            noiseSpectrum[b] /= specFrames;
        noiseSpectrumReady = true;
        serial_printf("  Noise spectrum: %d frames (esp-dsp HW FFT)\n", specFrames);
    }
    serial_print("\n");
}

// ══════════════════════════════════════
// MODE D: DASHBOARD
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

static void mode_dashboard() {
    size_t n1 = read_mic(I2S_NUM_0, raw1);
    size_t n2 = read_mic(I2S_NUM_1, raw2);
    process_mic(raw1, n1, mic1);
    process_mic(raw2, n2, mic2);

    bool s1 = (mode == 'B' || mode == '1'), s2 = (mode == 'B' || mode == '2');

    if (printCount % 25 == 0) {
        const char *mn = (mode == 'B') ? "BOTH" : (mode == '1') ? "MIC1" : "MIC2";
        serial_printf("\n=== %s ===  [1][2][B] [C]al [P]lot [W]av [H]elp\n", mn);
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
// MODE P: SERIAL PLOTTER
// ══════════════════════════════════════

static void mode_plotter() {
    size_t n1 = read_mic(I2S_NUM_0, raw1);
    size_t n2 = read_mic(I2S_NUM_1, raw2);

    static float hp1_prev = 0, hp1_out = 0;
    static bool  hp1_ready = false;
    static float hp2_prev = 0, hp2_out = 0;
    static bool  hp2_ready = false;

    bool s1 = (mode == 'B' || mode == '1'), s2 = (mode == 'B' || mode == '2');
    int step = 8;

    size_t minN = (n1 < n2) ? n1 : n2;
    for (size_t i = 0; i < minN; i += 2 * step) {
        int16_t s1_raw = (int16_t)(raw1[i] >> 16);
        int16_t s2_raw = (int16_t)(raw2[i] >> 16);

        float x1 = (float)s1_raw;
        float h1;
        if (!hp1_ready) { hp1_prev = x1; hp1_out = 0; hp1_ready = true; h1 = 0; }
        else { h1 = DC_HP_ALPHA * (hp1_out + x1 - hp1_prev); hp1_prev = x1; hp1_out = h1; }

        float x2 = (float)s2_raw;
        float h2;
        if (!hp2_ready) { hp2_prev = x2; hp2_out = 0; hp2_ready = true; h2 = 0; }
        else { h2 = DC_HP_ALPHA * (hp2_out + x2 - hp2_prev); hp2_prev = x2; hp2_out = h2; }

        if (s1 && s2)
            serial_printf("%d,%d\n", (int)h1, (int)h2);
        else if (s1)
            serial_printf("%d\n", (int)h1);
        else
            serial_printf("%d\n", (int)h2);
    }
}

// ══════════════════════════════════════
// MODE W: WAV STREAM (Beamformed + NR)
// ══════════════════════════════════════

static void start_wav_stream() {
    wavActive = true;
    wavStartUs = esp_timer_get_time();

    // Reset HP filter state
    mic1.wavHpPrev = 0; mic1.wavHpOut = 0; mic1.wavHpReady = false;
    mic2.wavHpPrev = 0; mic2.wavHpOut = 0; mic2.wavHpReady = false;

    // Reset FFT / OLA state
    inputRingPos = 0;
    olaFirstFrame = true;
    memset(olaBuffer, 0, sizeof(olaBuffer));
    memset(inputRing, 0, sizeof(inputRing));

    serial_print("WAV_START\n");
    serial_printf("RATE:%d\n", SAMPLE_RATE);
    serial_print("BITS:16\n");
    serial_print("CHANNELS:1\n");  // Always mono (beamformed + NR)
    serial_printf("DURATION:%d\n", WAV_DURATION_S);
    serial_print("DATA_BEGIN\n");
    serial_flush();
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void mode_wav_stream() {
    if (!wavActive) return;

    // Check timeout
    int64_t elapsed_us = esp_timer_get_time() - wavStartUs;
    if (elapsed_us > (int64_t)WAV_DURATION_S * 1000000LL) {
        serial_print("\nDATA_END\n");
        serial_flush();
        vTaskDelay(pdMS_TO_TICKS(50));
        wavActive = false;
        viewMode = 'D';
        serial_printf("\n  WAV done (%d sec). Back to dashboard.\n\n", WAV_DURATION_S);
        return;
    }

    // Always read both mics for beamforming
    size_t n1 = read_mic(I2S_NUM_0, raw1);
    size_t n2 = read_mic(I2S_NUM_1, raw2);

    static int16_t sendBuf[FFT_HOP];
    size_t maxN = (n1 > n2) ? n1 : n2;

    for (size_t i = 0; i < maxN; i += 2) {
        // HP filter each mic
        float hp1 = apply_wav_hp((float)(raw1[i] >> 16), mic1);
        float hp2 = (i < n2) ? apply_wav_hp((float)(raw2[i] >> 16), mic2) : hp1;

        // Beamform: average (coherent speech +6dB, incoherent noise +3dB)
        float mono = (hp1 + hp2) * 0.5f;

        // Accumulate into frame buffer
        inputRing[inputRingPos++] = mono;

        // When frame is full → FFT noise reduction
        if (inputRingPos == FFT_SIZE) {
            int nOut = process_fft_frame(inputRing, sendBuf);
            if (nOut > 0)
                serial_write_bytes(sendBuf, nOut * 2);

            // 50% overlap shift
            for (int k = 0; k < FFT_HOP; k++)
                inputRing[k] = inputRing[k + FFT_HOP];
            inputRingPos = FFT_HOP;
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
        if (c >= 'a' && c <= 'z') c -= 32;  // toupper
        if (c <= ' ') continue;

        if (c == '1') { mode = '1'; printCount = 0; if (viewMode == 'D') serial_print("\n  >> Mic1\n\n"); }
        if (c == '2') { mode = '2'; printCount = 0; if (viewMode == 'D') serial_print("\n  >> Mic2\n\n"); }
        if (c == 'B') { mode = 'B'; printCount = 0; if (viewMode == 'D') serial_print("\n  >> Both\n\n"); }
        if (c == 'C') { viewMode = 'D'; calibrate(); printCount = 0; }
        if (c == 'D') {
            viewMode = 'D'; printCount = 0; wavActive = false;
            serial_print("\n  >> Dashboard mode\n\n");
        }
        if (c == 'P') {
            viewMode = 'P'; wavActive = false;
            serial_print("\n  >> Plotter mode\n");
            serial_print("  >> Send D to return to dashboard\n\n");
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (c == 'W') {
            viewMode = 'W';
            serial_printf("\n  >> WAV stream: %d sec, %d Hz, mono (beamformed + NR, esp-dsp)\n",
                          WAV_DURATION_S, SAMPLE_RATE);
            serial_print("  >> Start Python recorder, then send any key...\n\n");
            while (!serial_available()) vTaskDelay(pdMS_TO_TICKS(10));
            serial_read();  // consume key
            start_wav_stream();
        }
        if (c == 'H') {
            serial_print("\n  +---------------------------------------+\n");
            serial_print("  | 1=Mic1  2=Mic2  B=Both                |\n");
            serial_print("  | D=Dashboard  P=Plotter  W=WAV record  |\n");
            serial_print("  | C=Calibrate  H=Help                   |\n");
            serial_print("  +---------------------------------------+\n\n");
        }
    }
}

// ══════════════════════════════════════
// MAIN (ESP-IDF entry point)
// ══════════════════════════════════════

extern "C" void app_main(void) {
    // USB Serial
    serial_init();
    vTaskDelay(pdMS_TO_TICKS(500));

    // GPIO: L/R pins LOW for left channel
    gpio_set_direction(M1_LR, GPIO_MODE_OUTPUT);
    gpio_set_level(M1_LR, 0);
    gpio_set_direction(M2_LR, GPIO_MODE_OUTPUT);
    gpio_set_level(M2_LR, 0);

    // I2S
    setup_i2s(I2S_NUM_0, M1_SCK, M1_WS, M1_SD);
    setup_i2s(I2S_NUM_1, M2_SCK, M2_WS, M2_SD);

    // DSP: init hardware-accelerated FFT + Hann window
    init_dsp();

    serial_print("\n  +===============================================+\n");
    serial_print("  |  MOSA_MIC_PROJ — ESP-IDF + esp-dsp            |\n");
    serial_print("  |  Dual INMP441 Beamformed Noise Reduction      |\n");
    serial_print("  |  M1: SCK=5 WS=43 SD=6   (I2S0)               |\n");
    serial_print("  |  M2: SCK=9 WS=7  SD=8   (I2S1)               |\n");
    serial_print("  |  FFT: esp-dsp HW accelerated on ESP32-S3      |\n");
    serial_print("  |  [D]ash [P]lot [W]av [C]al [1][2][B] [H]elp  |\n");
    serial_print("  +===============================================+\n");

    // Calibrate on boot
    calibrate();

    // Main loop
    while (1) {
        handle_commands();

        switch (viewMode) {
            case 'D': mode_dashboard(); break;
            case 'P': mode_plotter();   break;
            case 'W': mode_wav_stream(); break;
        }
    }
}
