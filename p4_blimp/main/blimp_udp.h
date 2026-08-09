#pragma once

#include <stddef.h>
#include <stdint.h>

void blimp_udp_start(void);
void blimp_udp_send_jpeg(const uint8_t *jpeg, size_t length);
