/*
 * VAD State Machine
 * =================
 * Wraps ESP-SR AFE VAD output into a state machine with
 * pre-roll buffer, hold-on, and hold-off timing.
 *
 * States:
 *   IDLE        → waiting for speech
 *   PRE_SPEECH  → VAD triggered, filling pre-roll
 *   SPEECH      → active speech detected
 *   POST_SPEECH → silence detected, hold-off timer running
 */
#pragma once

#include <cstdint>

typedef enum {
    VAD_STATE_IDLE = 0,
    VAD_STATE_PRE_SPEECH,
    VAD_STATE_SPEECH,
    VAD_STATE_POST_SPEECH
} vad_fsm_state_t;

// Initialize VAD state machine
void vad_fsm_init();

// Feed a new AFE VAD result (called every fetch cycle)
// afe_vad: 0 = silence, 1 = speech (from afe_fetch_result_t.vad_state)
// Returns the current FSM state
vad_fsm_state_t vad_fsm_update(int afe_vad);

// Get current state
vad_fsm_state_t vad_fsm_get_state();

// Is speech active (PRE_SPEECH, SPEECH, or POST_SPEECH)?
bool vad_fsm_is_active();

// Enable/disable auto-record on VAD
void vad_fsm_set_auto_record(bool en);
bool vad_fsm_get_auto_record();

// Pre-roll buffer: stores last N ms of audio before VAD triggers
void vad_preroll_write(const int16_t *data, int nsamples);
int  vad_preroll_read(int16_t *out, int max_samples);
void vad_preroll_reset();
