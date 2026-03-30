/*
 * USB Serial/JTAG I/O abstraction
 */
#pragma once

#include <cstddef>
#include <cstdint>

void serial_init();
void serial_print(const char *str);
void serial_printf(const char *fmt, ...);
void serial_write_bytes(const void *data, size_t len);
void serial_flush();
bool serial_available();
int  serial_read();
