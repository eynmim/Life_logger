/*
 * Thread-safe ring buffers backed by PSRAM
 * Two independent buffers: processed (clean) and raw (unprocessed)
 */
#include "audio_buffer.h"

#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"

// ═══ Processed audio ring buffer ═══

static int16_t *rb_buf = nullptr;
static int rb_cap = 0, rb_wr = 0, rb_rd = 0, rb_cnt = 0;
static SemaphoreHandle_t rb_mtx = nullptr;

void rb_init(int capacity_samples) {
    rb_cap = capacity_samples;
    rb_buf = (int16_t *)heap_caps_malloc(rb_cap * sizeof(int16_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rb_buf) memset(rb_buf, 0, rb_cap * sizeof(int16_t));
    rb_mtx = xSemaphoreCreateMutex();
    rb_wr = 0; rb_rd = 0; rb_cnt = 0;
}

void rb_write(const int16_t *data, int nsamples) {
    xSemaphoreTake(rb_mtx, portMAX_DELAY);
    for (int i = 0; i < nsamples; i++) {
        if (rb_cnt < rb_cap) {
            rb_buf[rb_wr] = data[i];
            rb_wr = (rb_wr + 1) % rb_cap;
            rb_cnt++;
        }
    }
    xSemaphoreGive(rb_mtx);
}

int rb_read(int16_t *out, int max_samples) {
    xSemaphoreTake(rb_mtx, portMAX_DELAY);
    int n = (rb_cnt < max_samples) ? rb_cnt : max_samples;
    for (int i = 0; i < n; i++) {
        out[i] = rb_buf[rb_rd];
        rb_rd = (rb_rd + 1) % rb_cap;
    }
    rb_cnt -= n;
    xSemaphoreGive(rb_mtx);
    return n;
}

void rb_reset() {
    xSemaphoreTake(rb_mtx, portMAX_DELAY);
    rb_wr = 0; rb_rd = 0; rb_cnt = 0;
    xSemaphoreGive(rb_mtx);
}

int rb_available() {
    xSemaphoreTake(rb_mtx, portMAX_DELAY);
    int n = rb_cnt;
    xSemaphoreGive(rb_mtx);
    return n;
}

// ═══ Raw audio ring buffer (for A/B comparison) ═══

static int16_t *rr_buf = nullptr;
static int rr_cap = 0, rr_wr = 0, rr_rd = 0, rr_cnt = 0;
static SemaphoreHandle_t rr_mtx = nullptr;

void rb_raw_init(int capacity_samples) {
    rr_cap = capacity_samples;
    rr_buf = (int16_t *)heap_caps_malloc(rr_cap * sizeof(int16_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rr_buf) memset(rr_buf, 0, rr_cap * sizeof(int16_t));
    rr_mtx = xSemaphoreCreateMutex();
    rr_wr = 0; rr_rd = 0; rr_cnt = 0;
}

void rb_raw_write(const int16_t *data, int nsamples) {
    xSemaphoreTake(rr_mtx, portMAX_DELAY);
    for (int i = 0; i < nsamples; i++) {
        if (rr_cnt < rr_cap) {
            rr_buf[rr_wr] = data[i];
            rr_wr = (rr_wr + 1) % rr_cap;
            rr_cnt++;
        }
    }
    xSemaphoreGive(rr_mtx);
}

int rb_raw_read(int16_t *out, int max_samples) {
    xSemaphoreTake(rr_mtx, portMAX_DELAY);
    int n = (rr_cnt < max_samples) ? rr_cnt : max_samples;
    for (int i = 0; i < n; i++) {
        out[i] = rr_buf[rr_rd];
        rr_rd = (rr_rd + 1) % rr_cap;
    }
    rr_cnt -= n;
    xSemaphoreGive(rr_mtx);
    return n;
}

void rb_raw_reset() {
    xSemaphoreTake(rr_mtx, portMAX_DELAY);
    rr_wr = 0; rr_rd = 0; rr_cnt = 0;
    xSemaphoreGive(rr_mtx);
}

int rb_raw_available() {
    xSemaphoreTake(rr_mtx, portMAX_DELAY);
    int n = rr_cnt;
    xSemaphoreGive(rr_mtx);
    return n;
}
