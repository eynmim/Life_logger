/*
 * Calibration Persistence via NVS
 */
#include "cal_store.h"
#include "device_config.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "CAL_STORE";

// NVS stores 32-bit values — we use memcpy to store floats as uint32_t
static void put_float(nvs_handle_t h, const char *key, float val) {
    uint32_t raw;
    memcpy(&raw, &val, sizeof(raw));
    nvs_set_u32(h, key, raw);
}

static bool get_float(nvs_handle_t h, const char *key, float &val) {
    uint32_t raw;
    if (nvs_get_u32(h, key, &raw) != ESP_OK) return false;
    memcpy(&val, &raw, sizeof(val));
    return true;
}

void cal_store_init() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
}

bool cal_store_save(const CalData &cal) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;

    put_float(h, NVS_KEY_M1_NRMS,  cal.m1_noiseRMS);
    put_float(h, NVS_KEY_M1_NPEAK, cal.m1_noisePeak);
    put_float(h, NVS_KEY_M1_DC,    cal.m1_dc);
    put_float(h, NVS_KEY_M1_GATE,  cal.m1_gate);
    put_float(h, NVS_KEY_M2_NRMS,  cal.m2_noiseRMS);
    put_float(h, NVS_KEY_M2_NPEAK, cal.m2_noisePeak);
    put_float(h, NVS_KEY_M2_DC,    cal.m2_dc);
    put_float(h, NVS_KEY_M2_GATE,  cal.m2_gate);

    nvs_set_u8(h, NVS_KEY_VALID, 1);
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "Calibration saved to NVS");
    return true;
}

bool cal_store_load(CalData &cal) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

    uint8_t valid = 0;
    nvs_get_u8(h, NVS_KEY_VALID, &valid);
    if (!valid) { nvs_close(h); return false; }

    bool ok = true;
    ok &= get_float(h, NVS_KEY_M1_NRMS,  cal.m1_noiseRMS);
    ok &= get_float(h, NVS_KEY_M1_NPEAK, cal.m1_noisePeak);
    ok &= get_float(h, NVS_KEY_M1_DC,    cal.m1_dc);
    ok &= get_float(h, NVS_KEY_M1_GATE,  cal.m1_gate);
    ok &= get_float(h, NVS_KEY_M2_NRMS,  cal.m2_noiseRMS);
    ok &= get_float(h, NVS_KEY_M2_NPEAK, cal.m2_noisePeak);
    ok &= get_float(h, NVS_KEY_M2_DC,    cal.m2_dc);
    ok &= get_float(h, NVS_KEY_M2_GATE,  cal.m2_gate);

    nvs_close(h);

    if (ok) ESP_LOGI(TAG, "Calibration loaded from NVS");
    return ok;
}

void cal_store_clear() {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_VALID, 0);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Calibration cleared");
}
