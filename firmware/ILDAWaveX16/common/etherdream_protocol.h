/**
 * Ether Dream Protocol Definitions
 * Common header for both ESP32-S3 and ESP32-C3
 */

#ifndef ETHERDREAM_PROTOCOL_H
#define ETHERDREAM_PROTOCOL_H

#include <stdint.h>

// Ether Dream point structure (18 bytes)
typedef struct __attribute__((packed)) {
    int16_t x;       // -32768 to 32767
    int16_t y;       // -32768 to 32767
    uint16_t r;      // 0 to 65535
    uint16_t g;      // 0 to 65535
    uint16_t b;      // 0 to 65535
    uint16_t i;      // Intensity (often unused)
    uint16_t u1;     // User 1
    uint16_t u2;     // User 2
} etherdream_point_t;

// Ether Dream commands
#define CMD_PREPARE     0x70  // 'p'
#define CMD_BEGIN       0x62  // 'b'
#define CMD_POINT_RATE  0x71  // 'q'
#define CMD_DATA        0x64  // 'd'
#define CMD_STOP        0x73  // 's'
#define CMD_ESTOP       0x00
#define CMD_CLEAR       0x63  // 'c'
#define CMD_PING        0x3F  // '?'

// UART Bridge protocol
#define UART_SYNC_BYTE  0xAA
#define UART_CMD_DATA   0x01
#define UART_CMD_STATUS 0x02

// UART frame structure
typedef struct __attribute__((packed)) {
    uint8_t sync;        // 0xAA
    uint8_t cmd;         // 0x01 = data, 0x02 = status
    uint16_t length;     // Big-endian length of payload
    // Followed by payload (etherdream_point_t array)
} uart_frame_header_t;

#endif // ETHERDREAM_PROTOCOL_H
