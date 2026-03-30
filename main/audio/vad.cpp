/*
 * VAD State Machine with Pre-Roll Buffer
 */
#include "vad.h"
#include "../config/device_config.h"

#include <cstring>
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

static vad_fsm_state_t state = VAD_STATE_IDLE;
static bool auto_record = false;
static int64_t state_enter_us = 0;

// Pre-roll circular buffer
#define PREROLL_SAMPLES  (SAMPLE_RATE * VAD_PRE_ROLL_MS / 1000)  // 8000 samples for 500ms
static int16_t *preroll_buf = nullptr;
static int preroll_wr = 0;
static int preroll_count = 0;

static inline int64_t now_us() { return esp_timer_get_time(); }
static inline int64_t ms_since(int64_t t) { return (now_us() - t) / 1000; }

void vad_fsm_init() {
    state = VAD_STATE_IDLE;
    state_enter_us = now_us();
    preroll_buf = (int16_t *)heap_caps_calloc(PREROLL_SAMPLES, sizeof(int16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    preroll_wr = 0;
    preroll_count = 0;
}

vad_fsm_state_t vad_fsm_update(int afe_vad) {
    bool speech = (afe_vad == 1);

    switch (state) {
        case VAD_STATE_IDLE:
            if (speech) {
                state = VAD_STATE_PRE_SPEECH;
                state_enter_us = now_us();
            }
            break;

        case VAD_STATE_PRE_SPEECH:
            if (!speech) {
                // False trigger — go back to idle
                state = VAD_STATE_IDLE;
                state_enter_us = now_us();
            } else if (ms_since(state_enter_us) >= VAD_HOLD_ON_MS) {
                // Confirmed speech
                state = VAD_STATE_SPEECH;
                state_enter_us = now_us();
            }
            break;

        case VAD_STATE_SPEECH:
            if (!speech) {
                state = VAD_STATE_POST_SPEECH;
                state_enter_us = now_us();
            }
            break;

        case VAD_STATE_POST_SPEECH:
            if (speech) {
                // Speech resumed
                state = VAD_STATE_SPEECH;
                state_enter_us = now_us();
            } else if (ms_since(state_enter_us) >= VAD_HOLD_OFF_MS) {
                // Confirmed silence — done
                state = VAD_STATE_IDLE;
                state_enter_us = now_us();
            }
            break;
    }

    return state;
}

vad_fsm_state_t vad_fsm_get_state() { return state; }

bool vad_fsm_is_active() {
    return (state == VAD_STATE_PRE_SPEECH ||
            state == VAD_STATE_SPEECH ||
            state == VAD_STATE_POST_SPEECH);
}

void vad_fsm_set_auto_record(bool en) { auto_record = en; }
bool vad_fsm_get_auto_record()        { return auto_record; }

void vad_preroll_write(const int16_t *data, int nsamples) {
    if (!preroll_buf) return;
    for (int i = 0; i < nsamples; i++) {
        preroll_buf[preroll_wr] = data[i];
        preroll_wr = (preroll_wr + 1) % PREROLL_SAMPLES;
        if (preroll_count < PREROLL_SAMPLES) preroll_count++;
    }
}

int vad_preroll_read(int16_t *out, int max_samples) {
    if (!preroll_buf || preroll_count == 0) return 0;
    int n = (preroll_count < max_samples) ? preroll_count : max_samples;
    // Read from oldest data
    int rd = (preroll_wr - preroll_count + PREROLL_SAMPLES) % PREROLL_SAMPLES;
    for (int i = 0; i < n; i++) {
        out[i] = preroll_buf[rd];
        rd = (rd + 1) % PREROLL_SAMPLES;
    }
    return n;
}

void vad_preroll_reset() {
    preroll_wr = 0;
    preroll_count = 0;
}
