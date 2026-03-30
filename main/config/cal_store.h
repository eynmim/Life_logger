/*
 * Calibration Persistence — save/load noise calibration to NVS
 */
#pragma once

struct CalData {
    float m1_noiseRMS, m1_noisePeak, m1_dc, m1_gate;
    float m2_noiseRMS, m2_noisePeak, m2_dc, m2_gate;
};

// Initialize NVS (call once at boot)
void cal_store_init();

// Save calibration data to NVS. Returns true on success.
bool cal_store_save(const CalData &cal);

// Load calibration data from NVS. Returns true if valid data exists.
bool cal_store_load(CalData &cal);

// Clear saved calibration
void cal_store_clear();
