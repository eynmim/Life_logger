/*
 * USB Serial/JTAG I/O — extracted from monolithic main.cpp
 */
#include "serial_io.h"

#include <cstdio>
#include <cstring>
#include <cstdarg>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"

static int s_peek = -1;

void serial_init() {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 32768;
    cfg.rx_buffer_size = 1024;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
}

void serial_print(const char *str) {
    usb_serial_jtag_write_bytes(str, strlen(str), pdMS_TO_TICKS(200));
}

void serial_printf(const char *fmt, ...) {
    static char buf[512];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0)
        usb_serial_jtag_write_bytes(buf, len, pdMS_TO_TICKS(200));
}

void serial_write_bytes(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        int sent = usb_serial_jtag_write_bytes(p, len, pdMS_TO_TICKS(1000));
        if (sent > 0) { p += sent; len -= sent; }
        else vTaskDelay(1);
    }
}

void serial_flush() {
    vTaskDelay(pdMS_TO_TICKS(20));
}

bool serial_available() {
    if (s_peek >= 0) return true;
    uint8_t c;
    int n = usb_serial_jtag_read_bytes(&c, 1, 0);
    if (n > 0) { s_peek = c; return true; }
    return false;
}

int serial_read() {
    if (s_peek >= 0) { int c = s_peek; s_peek = -1; return c; }
    uint8_t c;
    int n = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(10));
    return (n > 0) ? (int)c : -1;
}
