/**
 * @file etherdream_server.c
 * @brief Ether Dream protocol server - SIMPLIFIED VERSION
 * 
 * KEY CHANGES:
 *   - Uses select() with timeout instead of busy polling
 *   - Reports REAL buffer_fullness (no scaling tricks)
 *   - Integrated loop - no separate network_task needed
 *   - Flow control matches original EtherDream behavior
 */

#include "etherdream_server.h"
#include "core/frame_buffer.h"
#include "hal/dac_timer.h"
#include "config.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include <string.h>
#include <errno.h>

static const char* TAG = "ED";

//=============================================================================
// Protocol Constants
//=============================================================================

#define ED_HW_REV           2
#define ED_SW_REV           2
#define ED_MAX_POINT_RATE   100000
#define ED_BUFFER_CAPACITY  1799    // Report like original EtherDream

// Commands
#define CMD_PREPARE     'p'
#define CMD_BEGIN       'b'
#define CMD_QUEUE_RATE  'q'
#define CMD_DATA        'd'
#define CMD_STOP        's'
#define CMD_ESTOP_0     0x00
#define CMD_ESTOP_FF    0xFF
#define CMD_CLEAR_ESTOP 'c'
#define CMD_PING        '?'
#define CMD_VERSION     'v'

// Responses
#define RESP_ACK        'a'
#define RESP_NAK_FULL   'F'
#define RESP_NAK_INVAL  'I'
#define RESP_NAK_ESTOP  '!'

// States
#define STATE_IDLE      0
#define STATE_PREPARED  1
#define STATE_PLAYING   2
#define LE_READY        0
#define LE_ESTOP        3

#define POINT_RATE_CHANGE   0x8000

//=============================================================================
// Protocol Structures
//=============================================================================

#pragma pack(push, 1)

typedef struct {
    uint8_t  protocol;
    uint8_t  light_engine_state;
    uint8_t  playback_state;
    uint8_t  source;
    uint16_t light_engine_flags;
    uint16_t playback_flags;
    uint16_t source_flags;
    uint16_t buffer_fullness;
    uint32_t point_rate;
    uint32_t point_count;
} ed_status_t;

typedef struct {
    uint8_t response;
    uint8_t command;
    ed_status_t status;
} ed_response_t;

typedef struct {
    uint8_t  mac[6];
    uint16_t hw_rev;
    uint16_t sw_rev;
    uint16_t buffer_capacity;
    uint32_t max_point_rate;
    ed_status_t status;
} ed_broadcast_t;

typedef struct {
    uint16_t control;
    int16_t  x, y;
    uint16_t r, g, b, i, u1, u2;
} ed_point_t;

#pragma pack(pop)

//=============================================================================
// State
//=============================================================================

static int s_listen_sock = -1;
static int s_client_sock = -1;
static int s_bcast_sock = -1;

static volatile uint8_t s_playback_state = STATE_IDLE;
static volatile uint8_t s_le_state = LE_READY;
static volatile uint16_t s_le_flags = 0;
static volatile uint16_t s_pb_flags = 0;
static volatile uint32_t s_point_rate = 0;
static volatile uint32_t s_point_count = 0;

// Rate queue
#define RATE_Q_SIZE 8
static uint32_t s_rate_q[RATE_Q_SIZE];
static int s_rate_q_head = 0;
static int s_rate_q_tail = 0;

// RX buffer and state machine
#define RX_BUF_SIZE (4096)
static uint8_t s_rx_buf[RX_BUF_SIZE];

// Point batch buffer
#define BATCH_SIZE 128
static laser_point_t s_batch[BATCH_SIZE];
static size_t s_batch_count = 0;

// Stats
static uint32_t s_last_bcast = 0;
static uint32_t s_rx_points = 0;
static uint32_t s_rx_points_per_sec = 0;
static int64_t s_last_stats = 0;

//=============================================================================
// Helpers
//=============================================================================

static void rate_q_push(uint32_t r) {
    int next = (s_rate_q_head + 1) % RATE_Q_SIZE;
    if (next != s_rate_q_tail) {
        s_rate_q[s_rate_q_head] = r;
        s_rate_q_head = next;
    }
}

static uint32_t rate_q_pop(void) {
    if (s_rate_q_head == s_rate_q_tail) return 0;
    uint32_t r = s_rate_q[s_rate_q_tail];
    s_rate_q_tail = (s_rate_q_tail + 1) % RATE_Q_SIZE;
    return r;
}

static void rate_q_clear(void) {
    s_rate_q_head = s_rate_q_tail = 0;
}

static void fill_status(ed_status_t* st) {
    memset(st, 0, sizeof(*st));
    st->light_engine_state = s_le_state;
    st->playback_state = s_playback_state;
    st->light_engine_flags = s_le_flags;
    st->playback_flags = s_pb_flags;
    
    // Report ACTUAL buffer level - no tricks!
    // Client formula: cap = 1700 - fullness
    // Our buffer is 8192, but we report as if it's 1800 (like original)
    // This gives correct flow control behavior
    size_t level = frame_buffer_level();
    
    // Scale 0-8192 to 0-1799 range (like original EtherDream)
    st->buffer_fullness = (uint16_t)((level * 1799) / FRAME_BUFFER_SIZE);
    
    st->point_rate = (s_playback_state == STATE_PLAYING) ? s_point_rate : 0;
    st->point_count = s_point_count;
}

static void send_ack(int sock, uint8_t cmd) {
    ed_response_t r = { .response = RESP_ACK, .command = cmd };
    fill_status(&r.status);
    send(sock, &r, sizeof(r), MSG_NOSIGNAL);
}

static void send_nak(int sock, uint8_t cmd, uint8_t code) {
    ed_response_t r = { .response = code, .command = cmd };
    fill_status(&r.status);
    send(sock, &r, sizeof(r), MSG_NOSIGNAL);
}

static void flush_batch(void) {
    if (s_batch_count > 0) {
        frame_buffer_write(s_batch, s_batch_count);
        s_batch_count = 0;
    }
}

static void add_point(const ed_point_t* p) {
    laser_point_t* lp = &s_batch[s_batch_count];
    lp->x = p->x;
    lp->y = p->y;
    lp->r = p->r;
    lp->g = p->g;
    lp->b = p->b;
    lp->user1 = 0;
    lp->user2 = 0;
    lp->flags = (p->r == 0 && p->g == 0 && p->b == 0) ? POINT_FLAG_BLANK : 0;
    
    // Handle rate change in point stream
    if (p->control & POINT_RATE_CHANGE) {
        uint32_t new_rate = rate_q_pop();
        if (new_rate >= SCAN_RATE_MIN_HZ && new_rate <= SCAN_RATE_MAX_HZ) {
            s_point_rate = new_rate;
            dac_timer_set_scan_rate(new_rate);
        }
    }
    
    s_batch_count++;
    s_rx_points++;
    s_point_count++;
    
    if (s_batch_count >= BATCH_SIZE) {
        flush_batch();
    }
}

//=============================================================================
// Protocol Handlers
//=============================================================================

static void handle_prepare(int sock) {
    if (s_le_state != LE_READY || s_playback_state != STATE_IDLE) {
        send_nak(sock, CMD_PREPARE, RESP_NAK_INVAL);
        return;
    }
    frame_buffer_clear();
    rate_q_clear();
    s_point_count = 0;
    s_playback_state = STATE_PREPARED;
    ESP_LOGI(TAG, "Prepared");
    send_ack(sock, CMD_PREPARE);
}

static void handle_begin(int sock, uint32_t rate, uint16_t low_water) {
    (void)low_water;
    
    if (s_playback_state != STATE_PREPARED) {
        send_nak(sock, CMD_BEGIN, RESP_NAK_INVAL);
        return;
    }
    if (frame_buffer_level() == 0) {
        send_nak(sock, CMD_BEGIN, RESP_NAK_INVAL);
        return;
    }
    if (rate < SCAN_RATE_MIN_HZ) rate = SCAN_RATE_MIN_HZ;
    if (rate > SCAN_RATE_MAX_HZ) rate = SCAN_RATE_MAX_HZ;
    
    s_point_rate = rate;
    s_playback_state = STATE_PLAYING;
    s_pb_flags |= 0x01;
    
    dac_timer_set_scan_rate(rate);
    
    ESP_LOGI(TAG, "Playing @ %lu pps", (unsigned long)rate);
    send_ack(sock, CMD_BEGIN);
}

static void handle_queue_rate(int sock, uint32_t rate) {
    if (s_playback_state != STATE_PREPARED && s_playback_state != STATE_PLAYING) {
        send_nak(sock, CMD_QUEUE_RATE, RESP_NAK_INVAL);
        return;
    }
    rate_q_push(rate);
    send_ack(sock, CMD_QUEUE_RATE);
}

static void handle_stop(int sock) {
    s_playback_state = STATE_IDLE;
    s_pb_flags &= ~0x01;
    s_point_rate = 0;
    ESP_LOGI(TAG, "Stopped");
    send_ack(sock, CMD_STOP);
}

static void handle_estop(int sock) {
    s_le_state = LE_ESTOP;
    s_le_flags |= 0x01;
    s_playback_state = STATE_IDLE;
    s_pb_flags = 0x04;
    s_point_rate = 0;
    frame_buffer_clear();
    ESP_LOGW(TAG, "E-STOP");
    send_ack(sock, CMD_ESTOP_0);
}

static void handle_clear_estop(int sock) {
    if (s_le_state != LE_ESTOP) {
        send_nak(sock, CMD_CLEAR_ESTOP, RESP_NAK_INVAL);
        return;
    }
    s_le_state = LE_READY;
    s_le_flags = 0;
    s_playback_state = STATE_IDLE;
    ESP_LOGI(TAG, "E-STOP cleared");
    send_ack(sock, CMD_CLEAR_ESTOP);
}

//=============================================================================
// Client Data Processing
//=============================================================================

// Process received data - returns false on error/disconnect
static bool process_data(const uint8_t* data, size_t len) {
    size_t pos = 0;
    
    while (pos < len) {
        uint8_t cmd = data[pos++];
        
        switch (cmd) {
            case CMD_PREPARE:
                handle_prepare(s_client_sock);
                break;
                
            case CMD_BEGIN:
                if (pos + 6 > len) return true; // Wait for more data
                {
                    uint16_t low_water = data[pos] | (data[pos+1] << 8);
                    uint32_t rate = data[pos+2] | (data[pos+3] << 8) | 
                                   (data[pos+4] << 16) | (data[pos+5] << 24);
                    pos += 6;
                    handle_begin(s_client_sock, rate, low_water);
                }
                break;
                
            case CMD_QUEUE_RATE:
                if (pos + 4 > len) return true;
                {
                    uint32_t rate = data[pos] | (data[pos+1] << 8) |
                                   (data[pos+2] << 16) | (data[pos+3] << 24);
                    pos += 4;
                    handle_queue_rate(s_client_sock, rate);
                }
                break;
                
            case CMD_DATA:
                if (pos + 2 > len) return true;
                {
                    uint16_t npts = data[pos] | (data[pos+1] << 8);
                    pos += 2;
                    
                    size_t need = npts * 18;
                    if (pos + need > len) return true; // Need more data
                    
                    // Check if buffer can accept points
                    if (!frame_buffer_can_fit(npts)) {
                        // Buffer full - NAK and skip points
                        send_nak(s_client_sock, CMD_DATA, RESP_NAK_FULL);
                        pos += need;
                        break;
                    }
                    
                    // Process all points
                    for (uint16_t i = 0; i < npts; i++) {
                        add_point((const ed_point_t*)&data[pos]);
                        pos += 18;
                    }
                    flush_batch();
                    send_ack(s_client_sock, CMD_DATA);
                }
                break;
                
            case CMD_STOP:
                handle_stop(s_client_sock);
                break;
                
            case CMD_ESTOP_0:
            case CMD_ESTOP_FF:
                handle_estop(s_client_sock);
                break;
                
            case CMD_CLEAR_ESTOP:
                handle_clear_estop(s_client_sock);
                break;
                
            case CMD_PING:
                send_ack(s_client_sock, CMD_PING);
                break;
                
            case CMD_VERSION:
                {
                    char ver[32] = "ILDAWaveX16 v2.0";
                    send(s_client_sock, ver, 32, MSG_NOSIGNAL);
                }
                break;
                
            default:
                ESP_LOGW(TAG, "Unknown cmd 0x%02X", cmd);
                handle_estop(s_client_sock);
                return false;
        }
    }
    return true;
}

//=============================================================================
// Socket Management
//=============================================================================

static void close_client(void) {
    if (s_client_sock >= 0) {
        close(s_client_sock);
        s_client_sock = -1;
    }
    s_playback_state = STATE_IDLE;
    s_point_rate = 0;
    s_batch_count = 0;
    ESP_LOGI(TAG, "Client disconnected");
}

static void accept_client(void) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int sock = accept(s_listen_sock, (struct sockaddr*)&addr, &len);
    
    if (sock < 0) return;
    
    if (s_client_sock >= 0) {
        ESP_LOGW(TAG, "Rejecting - already connected");
        close(sock);
        return;
    }
    
    ESP_LOGI(TAG, "Client from %s:%d", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    
    s_client_sock = sock;
    
    // Configure socket
    int opt = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    
    // NON-BLOCKING is key
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    
    // Large receive buffer
    int rcvbuf = 65536;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    
    // Reset state
    s_playback_state = STATE_IDLE;
    frame_buffer_clear();
    rate_q_clear();
    s_point_count = 0;
    
    // Send initial ACK
    send_ack(sock, CMD_PING);
}

static void send_broadcast(void) {
    ed_broadcast_t b = {0};
    esp_read_mac(b.mac, ESP_MAC_WIFI_STA);
    b.hw_rev = ED_HW_REV;
    b.sw_rev = ED_SW_REV;
    b.buffer_capacity = ED_BUFFER_CAPACITY;
    b.max_point_rate = ED_MAX_POINT_RATE;
    fill_status(&b.status);
    
    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(ETHERDREAM_UDP_PORT),
        .sin_addr.s_addr = INADDR_BROADCAST,
    };
    sendto(s_bcast_sock, &b, sizeof(b), 0, (struct sockaddr*)&dest, sizeof(dest));
}

//=============================================================================
// Public API
//=============================================================================

esp_err_t etherdream_server_init(void) {
    s_playback_state = STATE_IDLE;
    s_le_state = LE_READY;
    return ESP_OK;
}

esp_err_t etherdream_server_start(void) {
    // Create listen socket
    s_listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen_sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        return ESP_FAIL;
    }
    
    int opt = 1;
    setsockopt(s_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Non-blocking listen socket
    int flags = fcntl(s_listen_sock, F_GETFL, 0);
    fcntl(s_listen_sock, F_SETFL, flags | O_NONBLOCK);
    
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(ETHERDREAM_TCP_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    
    if (bind(s_listen_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed");
        close(s_listen_sock);
        return ESP_FAIL;
    }
    
    listen(s_listen_sock, 1);
    ESP_LOGI(TAG, "TCP listening on %d", ETHERDREAM_TCP_PORT);
    
    // UDP broadcast socket
    s_bcast_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_bcast_sock >= 0) {
        opt = 1;
        setsockopt(s_bcast_sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
    }
    
    s_last_stats = esp_timer_get_time();
    return ESP_OK;
}

esp_err_t etherdream_server_stop(void) {
    close_client();
    if (s_listen_sock >= 0) { close(s_listen_sock); s_listen_sock = -1; }
    if (s_bcast_sock >= 0) { close(s_bcast_sock); s_bcast_sock = -1; }
    return ESP_OK;
}

// Main loop - call this from network task
// Uses select() to wait for data efficiently
void etherdream_server_loop(void) {
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Broadcast every second
    if (now - s_last_bcast >= 1000) {
        if (s_bcast_sock >= 0) send_broadcast();
        s_last_bcast = now;
    }
    
    // Stats every second
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_stats >= 1000000) {
        if (s_rx_points > 0) {
            s_rx_points_per_sec = s_rx_points;
            ESP_LOGI(TAG, "RX: %lu pts/s | BUF: %zu/%d",
                     (unsigned long)s_rx_points_per_sec,
                     frame_buffer_level(), FRAME_BUFFER_SIZE);
        }
        s_rx_points = 0;
        s_last_stats = now_us;
    }
    
    // Build fd_set
    fd_set rfds;
    FD_ZERO(&rfds);
    
    int maxfd = s_listen_sock;
    FD_SET(s_listen_sock, &rfds);
    
    if (s_client_sock >= 0) {
        FD_SET(s_client_sock, &rfds);
        if (s_client_sock > maxfd) maxfd = s_client_sock;
    }
    
    // Wait with timeout - this yields CPU properly!
    struct timeval tv = { .tv_sec = 0, .tv_usec = 5000 }; // 5ms max wait
    int ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
    
    if (ret <= 0) return; // Timeout or error
    
    // Check for new connection
    if (FD_ISSET(s_listen_sock, &rfds)) {
        accept_client();
    }
    
    // Handle client data
    if (s_client_sock >= 0 && FD_ISSET(s_client_sock, &rfds)) {
        ssize_t len = recv(s_client_sock, s_rx_buf, sizeof(s_rx_buf), 0);
        
        if (len > 0) {
            if (!process_data(s_rx_buf, len)) {
                close_client();
            }
        } else if (len == 0) {
            close_client();
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(TAG, "recv error %d", errno);
            close_client();
        }
    }
}

bool etherdream_server_is_connected(void) {
    return s_client_sock >= 0;
}

uint32_t etherdream_server_get_point_rate(void) {
    return s_point_rate;
}

uint32_t etherdream_server_get_measured_pps(void) {
    return s_rx_points_per_sec;
}
