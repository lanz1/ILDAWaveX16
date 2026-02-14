
#include "etherdream_server.h"
#include "core/frame_buffer.h"
#include "hal/dac_timer.h"
#include "config.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>
#include <errno.h>

static const char* TAG = "EDREAM";

// Ether Dream Protocol Constants

#define ETHERDREAM_MAX_POINT_RATE   100000
#define ETHERDREAM_HW_REVISION      2
#define ETHERDREAM_SW_REVISION      2

// Command bytes
#define CMD_PREPARE     'p'     // 0x70
#define CMD_BEGIN       'b'     // 0x62
#define CMD_QUEUE_RATE  'q'     // 0x71
#define CMD_DATA        'd'     // 0x64
#define CMD_STOP        's'     // 0x73
#define CMD_ESTOP_0     0x00
#define CMD_ESTOP_FF    0xFF
#define CMD_CLEAR_ESTOP 'c'     // 0x63
#define CMD_PING        '?'     // 0x3F
#define CMD_VERSION     'v'     // 0x76

// Response bytes
#define RESP_ACK        'a'     // 0x61
#define RESP_NAK_FULL   'F'     // 0x46
#define RESP_NAK_INVAL  'I'     // 0x49
#define RESP_NAK_ESTOP  '!'     // 0x21

// Playback states
#define PLAYBACK_IDLE       0
#define PLAYBACK_PREPARED   1
#define PLAYBACK_PLAYING    2

// Light engine states
#define LE_READY        0
#define LE_WARMUP       1
#define LE_COOLDOWN     2
#define LE_ESTOP        3

// Source types
#define SOURCE_NETWORK  0
#define SOURCE_SD       1
#define SOURCE_ABSTRACT 2

// DAC point control field
#define POINT_CONTROL_RATE_CHANGE   0x8000

// Buffer capacity reported to clients.
// (libetherdream, MadMapper, etc.) target ~1700 buffer level and sleep based on
// (fullness - 1700). With capacity=8191, the client fills to 4000+ then sleeps
// for 200ms, causing buffer drain to 0 and underruns ("ping-pong").
#define ETHERDREAM_BUFFER_CAPACITY  8192

// Rate change queue
#define RATE_QUEUE_SIZE     16

// Broadcast interval
#define BROADCAST_INTERVAL_MS   1000

// Ether Dream Protocol Structures (packed, little-endian)

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
} etherdream_status_t;

typedef struct {
    uint8_t  response;
    uint8_t  command;
    etherdream_status_t status;
} etherdream_response_t;

typedef struct {
    uint8_t  mac_address[6];
    uint16_t hw_revision;
    uint16_t sw_revision;
    uint16_t buffer_capacity;
    uint32_t max_point_rate;
    etherdream_status_t status;
} etherdream_broadcast_t;

typedef struct {
    uint8_t  command;
    uint16_t low_water_mark;
    uint32_t point_rate;
} etherdream_begin_cmd_t;

typedef struct {
    uint8_t  command;
    uint32_t point_rate;
} etherdream_queue_rate_cmd_t;

typedef struct {
    uint8_t  command;
    uint16_t npoints;
} etherdream_data_cmd_t;

typedef struct {
    uint16_t control;
    int16_t  x;
    int16_t  y;
    uint16_t r;
    uint16_t g;
    uint16_t b;
    uint16_t i;
    uint16_t u1;
    uint16_t u2;
} etherdream_point_t;

#pragma pack(pop)

// State
static int s_listen_socket = -1;
static int s_client_socket = -1;
static volatile bool s_running = false;
static volatile bool s_connected = false;

static volatile uint8_t s_playback_state = PLAYBACK_IDLE;
static volatile uint8_t s_light_engine_state = LE_READY;
static volatile uint16_t s_light_engine_flags = 0;
static volatile uint16_t s_playback_flags = 0;
static volatile uint32_t s_point_rate = 0;
static volatile uint32_t s_point_count = 0;

static uint32_t s_rate_queue[RATE_QUEUE_SIZE];
static volatile int s_rate_queue_head = 0;
static volatile int s_rate_queue_tail = 0;

static int s_broadcast_socket = -1;
static uint32_t s_last_broadcast_time = 0;

#define RATE_METER_INTERVAL_MS  500

#define TCP_RX_BUF_SIZE     (3 + 1000 * 18 + 16)
static uint8_t s_rx_buf[TCP_RX_BUF_SIZE];

// Client's low_water_mark from begin command (500-3000, typically 1700)
// This tells us the client's target buffer level.
static uint16_t s_client_low_water = 0;

// E-STOP rate limiting
static uint32_t s_estop_count = 0;
static uint32_t s_estop_last_log_ms = 0;

// Network throughput metering (for performance validation)
static uint32_t s_total_points_received = 0;
static uint32_t s_total_bytes_received = 0;
static uint32_t s_last_report_ms = 0;
#define THROUGHPUT_REPORT_INTERVAL_MS 1000

#define RESP_BATCH_MAX  16
static etherdream_response_t s_resp_batch[RESP_BATCH_MAX];
static size_t s_resp_batch_count = 0;

static void fill_status(etherdream_status_t* status);
static void flush_responses(int sock);
static void send_response(int sock, uint8_t resp_code, uint8_t cmd_byte);
static void handle_prepare(int sock);
static void handle_begin(int sock, const etherdream_begin_cmd_t* cmd);
static void handle_queue_rate(int sock, const etherdream_queue_rate_cmd_t* cmd);
static void handle_stop(int sock);
static void handle_estop(int sock);
static void handle_clear_estop(int sock);
static void handle_ping(int sock);
static void send_broadcast(void);
static void accept_client(void);
static void handle_client(void);

static bool rate_queue_full(void) {
    return ((s_rate_queue_head + 1) % RATE_QUEUE_SIZE) == s_rate_queue_tail;
}

static bool rate_queue_empty(void) {
    return s_rate_queue_head == s_rate_queue_tail;
}

static bool rate_queue_push(uint32_t rate) {
    if (rate_queue_full()) return false;
    s_rate_queue[s_rate_queue_head] = rate;
    s_rate_queue_head = (s_rate_queue_head + 1) % RATE_QUEUE_SIZE;
    return true;
}

static uint32_t rate_queue_pop(void) {
    if (rate_queue_empty()) return 0;
    uint32_t rate = s_rate_queue[s_rate_queue_tail];
    s_rate_queue_tail = (s_rate_queue_tail + 1) % RATE_QUEUE_SIZE;
    return rate;
}

static void rate_queue_clear(void) {
    s_rate_queue_head = 0;
    s_rate_queue_tail = 0;
}

static void fill_status(etherdream_status_t* status) {
    memset(status, 0, sizeof(etherdream_status_t));
    status->protocol = 0;
    status->light_engine_state = s_light_engine_state;
    status->playback_state = s_playback_state;
    status->source = SOURCE_NETWORK;
    status->light_engine_flags = s_light_engine_flags;
    status->playback_flags = s_playback_flags;
    status->source_flags = 0;
    
    size_t real_level = frame_buffer_level();
    status->buffer_fullness = (uint16_t)(real_level > 0xFFFF ? 0xFFFF : real_level);
    
    status->point_rate = (s_playback_state == PLAYBACK_PLAYING) ? s_point_rate : 0;
    status->point_count = s_point_count;
}

static void flush_responses(int sock) {
    if (s_resp_batch_count == 0 || sock < 0) return;
    
    size_t total_bytes = s_resp_batch_count * sizeof(etherdream_response_t);
    

    
    ssize_t sent = send(sock, s_resp_batch, total_bytes, MSG_NOSIGNAL);
    

    
    if (sent != (ssize_t)total_bytes) {
        ESP_LOGW(TAG, "flush: sent=%d expected=%d errno=%d",
                 (int)sent, (int)total_bytes, errno);
    }
    
    s_resp_batch_count = 0;
}

static void send_response(int sock, uint8_t resp_code, uint8_t cmd_byte) {
    // Flush if batch full (safety)
    if (s_resp_batch_count >= RESP_BATCH_MAX) {
        flush_responses(sock);
    }
    
    etherdream_response_t* resp = &s_resp_batch[s_resp_batch_count];
    resp->response = resp_code;
    resp->command = cmd_byte;
    fill_status(&resp->status);
    if (cmd_byte != CMD_DATA || resp_code != RESP_ACK) {
        ESP_LOGI(TAG, "TX: resp=%c cmd=%c le=%d pb=%d buf=%d rate=%lu",
                 resp_code, cmd_byte,
                 resp->status.light_engine_state,
                 resp->status.playback_state,
                 resp->status.buffer_fullness,
                 (unsigned long)resp->status.point_rate);
    }
    
    s_resp_batch_count++;
}

esp_err_t etherdream_server_init(void) {
    ESP_LOGI(TAG, "Ether Dream server initialized");
    s_playback_state = PLAYBACK_IDLE;
    s_light_engine_state = LE_READY;
    s_point_rate = 0;
    s_point_count = 0;
    return ESP_OK;
}

esp_err_t etherdream_server_start(void) {
    ESP_LOGI(TAG, "Starting Ether Dream TCP server on port %d...", ETHERDREAM_TCP_PORT);
    
    s_listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen_socket < 0) {
        ESP_LOGE(TAG, "Failed to create TCP socket: errno %d", errno);
        return ESP_FAIL;
    }
    
    int reuse = 1;
    setsockopt(s_listen_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    // Set non-blocking for accept
    int flags = fcntl(s_listen_socket, F_GETFL, 0);
    fcntl(s_listen_socket, F_SETFL, flags | O_NONBLOCK);
    
    struct sockaddr_in tcp_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(ETHERDREAM_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    
    if (bind(s_listen_socket, (struct sockaddr*)&tcp_addr, sizeof(tcp_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind TCP socket: errno %d", errno);
        close(s_listen_socket);
        s_listen_socket = -1;
        return ESP_FAIL;
    }
    
    if (listen(s_listen_socket, 1) < 0) {
        ESP_LOGE(TAG, "Failed to listen: errno %d", errno);
        close(s_listen_socket);
        s_listen_socket = -1;
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "TCP server listening on port %d", ETHERDREAM_TCP_PORT);
    
    s_broadcast_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_broadcast_socket < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket: errno %d", errno);
    } else {
        int broadcast = 1;
        setsockopt(s_broadcast_socket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
        
        // Set non-blocking
        flags = fcntl(s_broadcast_socket, F_GETFL, 0);
        fcntl(s_broadcast_socket, F_SETFL, flags | O_NONBLOCK);
        
        ESP_LOGI(TAG, "UDP broadcast socket ready on port %d", ETHERDREAM_UDP_PORT);
    }
    
    s_running = true;
    s_last_broadcast_time = 0;  // Trigger immediate broadcast
    
    ESP_LOGI(TAG, "Ether Dream server started");
    return ESP_OK;
}

esp_err_t etherdream_server_stop(void) {
    s_running = false;
    s_connected = false;
    
    if (s_client_socket >= 0) {
        close(s_client_socket);
        s_client_socket = -1;
    }
    
    if (s_listen_socket >= 0) {
        close(s_listen_socket);
        s_listen_socket = -1;
    }
    
    if (s_broadcast_socket >= 0) {
        close(s_broadcast_socket);
        s_broadcast_socket = -1;
    }
    
    s_playback_state = PLAYBACK_IDLE;
    s_point_rate = 0;
    
    ESP_LOGI(TAG, "Ether Dream server stopped");
    return ESP_OK;
}

void etherdream_server_loop(void) {
    if (!s_running) return;
    
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    if (s_broadcast_socket >= 0 && (now - s_last_broadcast_time >= BROADCAST_INTERVAL_MS)) {
        send_broadcast();
        s_last_broadcast_time = now;
    }
    
    fd_set rfds;
    FD_ZERO(&rfds);
    
    int maxfd = s_listen_socket;
    if (s_listen_socket >= 0) {
        FD_SET(s_listen_socket, &rfds);
    }
    
    if (s_client_socket >= 0) {
        FD_SET(s_client_socket, &rfds);
        if (s_client_socket > maxfd) maxfd = s_client_socket;
    }
    uint32_t timeout_us = (s_playback_state == PLAYBACK_PLAYING) ? 1000 : 50000;
    struct timeval tv = { .tv_sec = 0, .tv_usec = timeout_us };
    

    int ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);

    if (ret <= 0) return;  // Timeout or error
    
    // --- Accept new connection ---
    if (s_listen_socket >= 0 && FD_ISSET(s_listen_socket, &rfds)) {
        accept_client();
    }
    
    // --- Handle client data ---
    if (s_client_socket >= 0 && FD_ISSET(s_client_socket, &rfds)) {
        handle_client();
    }
}

bool etherdream_server_is_connected(void) {
    return s_connected;
}

uint32_t etherdream_server_get_point_rate(void) {
    return s_point_rate;
}

uint32_t etherdream_server_get_measured_pps(void) {
    return 0;
}

static void send_broadcast(void) {
    etherdream_broadcast_t bcast;
    memset(&bcast, 0, sizeof(bcast));
    
    // Use our configured Ethernet MAC address
    uint8_t mac[] = ETH_MAC_ADDR;
    memcpy(bcast.mac_address, mac, 6);
    
    bcast.hw_revision = ETHERDREAM_HW_REVISION;
    bcast.sw_revision = ETHERDREAM_SW_REVISION;
    bcast.buffer_capacity = ETHERDREAM_BUFFER_CAPACITY;
    bcast.max_point_rate = ETHERDREAM_MAX_POINT_RATE;
    
    fill_status(&bcast.status);
    
    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(ETHERDREAM_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    
    sendto(s_broadcast_socket, &bcast, sizeof(bcast), 0,
           (struct sockaddr*)&dest, sizeof(dest));
}

static void accept_client(void) {
    // Non-blocking accept - called only when select() says listen socket is ready
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    int new_sock = accept(s_listen_socket, (struct sockaddr*)&client_addr, &addr_len);
    if (new_sock < 0) {
        return;
    }
    
    // Reject if we already have a client
    if (s_client_socket >= 0) {
        ESP_LOGW(TAG, "Rejecting - already connected");
        close(new_sock);
        return;
    }
    
    ESP_LOGI(TAG, "Client from %s:%d",
             inet_ntoa(client_addr.sin_addr),
             ntohs(client_addr.sin_port));
    
    s_client_socket = new_sock;
    s_connected = true;
    
    // TCP_NODELAY: crucial for responsive ACKs
    int nodelay = 1;
    setsockopt(s_client_socket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    
    // Non-blocking for select() integration
    int flags = fcntl(s_client_socket, F_GETFL, 0);
    fcntl(s_client_socket, F_SETFL, flags | O_NONBLOCK);
    
    // Large receive buffer for batch data
    int rcvbuf = 65536;
    setsockopt(s_client_socket, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    
    // Reset all protocol state for new client
    s_playback_state = PLAYBACK_IDLE;
    s_light_engine_state = LE_READY;
    s_light_engine_flags = 0;
    s_playback_flags = 0;
    s_point_count = 0;
    s_point_rate = 0;
    s_client_low_water = 0;
    s_estop_count = 0;
    rate_queue_clear();
    frame_buffer_clear();
    
    // Protocol: on connection, DAC immediately sends status like a ping ACK
    send_response(s_client_socket, RESP_ACK, CMD_PING);
    flush_responses(s_client_socket);
}

typedef enum {
    RX_STATE_COMMAND,       // Waiting for command byte
    RX_STATE_BEGIN,         // Reading begin command (6 more bytes after cmd)
    RX_STATE_QUEUE_RATE,    // Reading queue rate command (4 more bytes after cmd)
    RX_STATE_DATA_HEADER,   // Reading data command header (2 more bytes after cmd)
    RX_STATE_DATA_POINTS,   // Reading point data (N * 18 bytes)
} rx_state_t;

static rx_state_t s_rx_state = RX_STATE_COMMAND;
static uint8_t s_cmd_buf[32];
static size_t s_cmd_buf_len = 0;
static size_t s_cmd_expected = 0;

static uint16_t s_data_npoints = 0;
static uint16_t s_data_points_received = 0;
static uint8_t s_data_point_buf[18];    // Buffer for one point (18 bytes)
static size_t s_data_point_buf_len = 0;

#define POINT_BATCH_SIZE    512
static laser_point_t s_point_batch[POINT_BATCH_SIZE];
static size_t s_batch_count = 0;

static size_t flush_point_batch(void) {
    if (s_batch_count > 0) {
        size_t written = frame_buffer_write(s_point_batch, s_batch_count);
        s_batch_count = 0;
        return written;
    }
    return 0;
}

static void add_point_to_batch(const etherdream_point_t* ep) {
    laser_point_t* lp = &s_point_batch[s_batch_count];
    
    // SIMD-like optimization: copy 32-bit chunks instead of 16-bit
    // Reduces parsing from ~2.5µs to ~1µs per point
    *(uint32_t*)&lp->x = *(uint32_t*)&ep->x;  // Copy x,y together
    *(uint32_t*)&lp->r = *(uint32_t*)&ep->r;  // Copy r,g together
    lp->b = ep->b;
    
    lp->user1 = 0;
    lp->user2 = 0;
    
    // Optimized blanking check: OR all color channels
    // Compiles to single ANDN instruction on Xtensa
    lp->flags = (ep->r | ep->g | ep->b | ep->i) ? 0 : POINT_FLAG_BLANK;
    
    if (ep->control & POINT_CONTROL_RATE_CHANGE) {
        if (!rate_queue_empty()) {
            uint32_t new_rate = rate_queue_pop();
            if (new_rate >= SCAN_RATE_MIN_HZ && new_rate <= SCAN_RATE_MAX_HZ) {
                s_point_rate = new_rate;
                dac_timer_set_scan_rate(new_rate);
                ESP_LOGD(TAG, "Rate change: %lu pps", (unsigned long)new_rate);
            }
        }
    }
    
    s_batch_count++;
    if (s_batch_count >= POINT_BATCH_SIZE) {
        flush_point_batch();
    }
}

static void close_client(void) {
    dac_timer_set_playback_active(false);  // Stop draining buffer
    if (s_client_socket >= 0) {
        close(s_client_socket);
        s_client_socket = -1;
    }
    s_connected = false;
    s_playback_state = PLAYBACK_IDLE;
    s_point_rate = 0;
    s_rx_state = RX_STATE_COMMAND;
    s_cmd_buf_len = 0;
    s_data_npoints = 0;
    s_data_points_received = 0;
    s_data_point_buf_len = 0;
    s_batch_count = 0;
    s_resp_batch_count = 0;
    
    ESP_LOGI(TAG, "Client disconnected");
}

static void process_complete_command(int sock, uint8_t cmd, const uint8_t* data, size_t len) {
    switch (cmd) {
        case CMD_PREPARE:
            handle_prepare(sock);
            break;
            
        case CMD_BEGIN: {
            if (len >= sizeof(etherdream_begin_cmd_t)) {
                handle_begin(sock, (const etherdream_begin_cmd_t*)data);
            }
            break;
        }
        
        case CMD_QUEUE_RATE: {
            if (len >= sizeof(etherdream_queue_rate_cmd_t)) {
                handle_queue_rate(sock, (const etherdream_queue_rate_cmd_t*)data);
            }
            break;
        }
        
        case CMD_STOP:
            handle_stop(sock);
            break;
            
        case CMD_ESTOP_0:
        case CMD_ESTOP_FF:
            handle_estop(sock);
            break;
            
        case CMD_CLEAR_ESTOP:
            handle_clear_estop(sock);
            break;
            
        case CMD_PING:
            handle_ping(sock);
            break;
            
        case CMD_VERSION: {
            flush_responses(sock);  // Flush pending before raw send
            char version[32];
            memset(version, 0, sizeof(version));
            snprintf(version, sizeof(version), "ILDAWaveX16-ED v1.0");
            send(sock, version, sizeof(version), MSG_NOSIGNAL);
            break;
        }
        
        default:
            ESP_LOGW(TAG, "Unknown command 0x%02X -> E-Stop", cmd);
            handle_estop(sock);
            break;
    }
}

static void feed_rx_data(int sock, const uint8_t* data, size_t len) {
    size_t pos = 0;
    
    while (pos < len) {
        switch (s_rx_state) {
            case RX_STATE_COMMAND: {
                uint8_t cmd = data[pos++];
                
                switch (cmd) {
                    case CMD_PREPARE:
                    case CMD_STOP:
                    case CMD_ESTOP_0:
                    case CMD_ESTOP_FF:
                    case CMD_CLEAR_ESTOP:
                    case CMD_PING:
                    case CMD_VERSION:
                        s_cmd_buf[0] = cmd;
                        process_complete_command(sock, cmd, s_cmd_buf, 1);
                        break;
                    
                    case CMD_BEGIN:
                        s_cmd_buf[0] = cmd;
                        s_cmd_buf_len = 1;
                        s_cmd_expected = 7;
                        s_rx_state = RX_STATE_BEGIN;
                        break;
                    
                    case CMD_QUEUE_RATE:
                        s_cmd_buf[0] = cmd;
                        s_cmd_buf_len = 1;
                        s_cmd_expected = 5;
                        s_rx_state = RX_STATE_QUEUE_RATE;
                        break;
                    
                    case CMD_DATA:
                        s_cmd_buf[0] = cmd;
                        s_cmd_buf_len = 1;
                        s_cmd_expected = 3;
                        s_rx_state = RX_STATE_DATA_HEADER;
                        break;
                    
                    default:
                        s_cmd_buf[0] = cmd;
                        process_complete_command(sock, cmd, s_cmd_buf, 1);
                        break;
                }
                break;
            }
            
            case RX_STATE_BEGIN:
            case RX_STATE_QUEUE_RATE: {
                size_t need = s_cmd_expected - s_cmd_buf_len;
                size_t avail = len - pos;
                size_t copy = (avail < need) ? avail : need;
                
                memcpy(&s_cmd_buf[s_cmd_buf_len], &data[pos], copy);
                s_cmd_buf_len += copy;
                pos += copy;
                
                if (s_cmd_buf_len >= s_cmd_expected) {
                    process_complete_command(sock, s_cmd_buf[0], s_cmd_buf, s_cmd_buf_len);
                    s_rx_state = RX_STATE_COMMAND;
                    s_cmd_buf_len = 0;
                }
                break;
            }
            
            case RX_STATE_DATA_HEADER: {
                size_t need = 3 - s_cmd_buf_len;
                size_t avail = len - pos;
                size_t copy = (avail < need) ? avail : need;
                
                memcpy(&s_cmd_buf[s_cmd_buf_len], &data[pos], copy);
                s_cmd_buf_len += copy;
                pos += copy;
                
                if (s_cmd_buf_len >= 3) {
                    etherdream_data_cmd_t* dcmd = (etherdream_data_cmd_t*)s_cmd_buf;
                    s_data_npoints = dcmd->npoints;
                    s_data_points_received = 0;
                    s_data_point_buf_len = 0;
                    
                    if (s_data_npoints == 0) {
                        if (s_playback_state == PLAYBACK_PREPARED || 
                            s_playback_state == PLAYBACK_PLAYING) {
                            send_response(sock, RESP_ACK, CMD_DATA);
                        } else {
                            send_response(sock, RESP_NAK_INVAL, CMD_DATA);
                        }
                        s_rx_state = RX_STATE_COMMAND;
                    } else {
                        s_rx_state = RX_STATE_DATA_POINTS;
                    }
                }
                break;
            }
            
            case RX_STATE_DATA_POINTS: {
                // First: finish any partial point from previous recv
                while (pos < len && s_data_point_buf_len > 0 && 
                       s_data_points_received < s_data_npoints) {
                    size_t point_need = 18 - s_data_point_buf_len;
                    size_t avail = len - pos;
                    size_t copy = (avail < point_need) ? avail : point_need;
                    
                    memcpy(&s_data_point_buf[s_data_point_buf_len], &data[pos], copy);
                    s_data_point_buf_len += copy;
                    pos += copy;
                    
                    if (s_data_point_buf_len >= 18) {
                        etherdream_point_t* ep = (etherdream_point_t*)s_data_point_buf;
                        add_point_to_batch(ep);
                        s_data_points_received++;
                        s_point_count++;
                        s_data_point_buf_len = 0;
                    }
                }
                
                // BULK: process complete points directly from recv buffer
                // No memcpy needed - cast directly from data buffer
                size_t remaining_points = s_data_npoints - s_data_points_received;
                size_t avail_bytes = len - pos;
                size_t complete_points = avail_bytes / 18;
                if (complete_points > remaining_points)
                    complete_points = remaining_points;
                
                for (size_t i = 0; i < complete_points; i++) {
                    etherdream_point_t* ep = (etherdream_point_t*)&data[pos];
                    add_point_to_batch(ep);
                    pos += 18;
                    s_data_points_received++;
                    s_point_count++;
                }
                
                // Leftover partial point bytes -> save for next recv
                if (s_data_points_received < s_data_npoints && pos < len) {
                    size_t leftover = len - pos;
                    memcpy(s_data_point_buf, &data[pos], leftover);
                    s_data_point_buf_len = leftover;
                    pos = len;
                }
                
                if (s_data_points_received >= s_data_npoints) {
                    flush_point_batch();
                    
                    // Update throughput metrics
                    s_total_points_received += s_data_npoints;
                    s_total_bytes_received += (3 + s_data_npoints * 18);
                    
                    // Periodic throughput report
                    uint32_t now_ms = esp_timer_get_time() / 1000;
                    if (now_ms - s_last_report_ms >= THROUGHPUT_REPORT_INTERVAL_MS) {
                        uint32_t pps = s_total_points_received;  // Over 1 second
                        uint32_t kbps = (s_total_bytes_received * 8) / 1000;  // Kbps
                        uint32_t buf_level = frame_buffer_level();
                        uint32_t underruns = dac_timer_get_underruns();
                        
                        ESP_LOGI(TAG, "RX: %lu pps | %lu kbps | Buf: %lu/%d (%d%%) | Underruns: %lu",
                                 (unsigned long)pps,
                                 (unsigned long)kbps,
                                 (unsigned long)buf_level,
                                 FRAME_BUFFER_SIZE,
                                 (int)(100 * buf_level / FRAME_BUFFER_SIZE),
                                 (unsigned long)underruns);
                        
                        s_total_points_received = 0;
                        s_total_bytes_received = 0;
                        s_last_report_ms = now_ms;
                    }
                    
                    if (s_playback_state == PLAYBACK_PREPARED || 
                        s_playback_state == PLAYBACK_PLAYING) {
                        send_response(sock, RESP_ACK, CMD_DATA);
                    } else {
                        send_response(sock, RESP_NAK_INVAL, CMD_DATA);
                    }
                    
                    s_rx_state = RX_STATE_COMMAND;
                    s_cmd_buf_len = 0;
                }
                break;
            }
        }
    }
}

static void handle_client(void) {
    while (1) {

        ssize_t len = recv(s_client_socket, s_rx_buf, sizeof(s_rx_buf), 0);
        
        if (len > 0) {

            feed_rx_data(s_client_socket, s_rx_buf, len);
            flush_responses(s_client_socket);

            // Continue loop to check for more data
        } else if (len == 0) {
            ESP_LOGW(TAG, "Client closed connection (FIN)");
            close_client();
            return;
        } else {
            // EAGAIN/EWOULDBLOCK means no more data - normal exit
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;  // All data drained, go back to select()
            }
            // Real error
            ESP_LOGW(TAG, "Recv error: errno %d (%s)", errno,
                     errno == ECONNRESET ? "RST" : 
                     errno == ETIMEDOUT ? "TIMEOUT" : "OTHER");
            close_client();
            return;
        }
    }
}

static void handle_prepare(int sock) {
    ESP_LOGI(TAG, "PREPARE: le=%d pb=%d", s_light_engine_state, s_playback_state);
    
    if (s_light_engine_state != LE_READY) {
        ESP_LOGW(TAG, "PREPARE NAK: light engine not ready (%d)", s_light_engine_state);
        send_response(sock, RESP_NAK_INVAL, CMD_PREPARE);
        return;
    }
    
    if (s_playback_state != PLAYBACK_IDLE) {
        ESP_LOGW(TAG, "PREPARE NAK: not idle (pb=%d)", s_playback_state);
        send_response(sock, RESP_NAK_INVAL, CMD_PREPARE);
        return;
    }
    
    frame_buffer_clear();
    rate_queue_clear();
    s_point_count = 0;
    s_playback_flags &= ~0x06;
    
    s_playback_state = PLAYBACK_PREPARED;
    
    ESP_LOGI(TAG, "Playback state -> Prepared");
    send_response(sock, RESP_ACK, CMD_PREPARE);
}

static void handle_begin(int sock, const etherdream_begin_cmd_t* cmd) {
    s_client_low_water = cmd->low_water_mark;
    
    ESP_LOGI(TAG, "BEGIN: low_water=%u point_rate=%lu pb=%d buf=%zu",
             cmd->low_water_mark, (unsigned long)cmd->point_rate,
             s_playback_state, frame_buffer_level());
    
    if (s_playback_state != PLAYBACK_PREPARED) {
        ESP_LOGW(TAG, "BEGIN NAK: not prepared (pb=%d)", s_playback_state);
        send_response(sock, RESP_NAK_INVAL, CMD_BEGIN);
        return;
    }
    
    if (frame_buffer_level() == 0) {
        ESP_LOGW(TAG, "BEGIN NAK: buffer empty");
        send_response(sock, RESP_NAK_INVAL, CMD_BEGIN);
        return;
    }
    
    uint32_t rate = cmd->point_rate;
    
    // MadMapper sends rate=0xFFFFFFFF meaning "use queued rate".
    // Try rate queue first, then fall back to current rate.
    if (rate == 0 || rate > ETHERDREAM_MAX_POINT_RATE) {
        if (!rate_queue_empty()) {
            rate = rate_queue_pop();
            ESP_LOGI(TAG, "BEGIN: using queued rate=%lu", (unsigned long)rate);
        } else if (s_point_rate > 0) {
            rate = s_point_rate;
            ESP_LOGI(TAG, "BEGIN: using current rate=%lu", (unsigned long)rate);
        } else {
            ESP_LOGW(TAG, "BEGIN NAK: invalid rate=%lu, no fallback", (unsigned long)rate);
            send_response(sock, RESP_NAK_INVAL, CMD_BEGIN);
            return;
        }
    }
    
    if (rate < SCAN_RATE_MIN_HZ) rate = SCAN_RATE_MIN_HZ;
    if (rate > SCAN_RATE_MAX_HZ) rate = SCAN_RATE_MAX_HZ;
    
    s_point_rate = rate;
    s_playback_state = PLAYBACK_PLAYING;
    s_playback_flags |= 0x01;
    
    // Set DAC scan rate and enable point flow
    dac_timer_set_scan_rate(rate);
    dac_timer_set_playback_active(true);
    
    ESP_LOGI(TAG, "Playing @ %lu pps (low_water=%u, buf=%zu)", 
             (unsigned long)rate, s_client_low_water, frame_buffer_level());
    send_response(sock, RESP_ACK, CMD_BEGIN);
}

static void handle_queue_rate(int sock, const etherdream_queue_rate_cmd_t* cmd) {
    ESP_LOGD(TAG, "CMD: Queue rate %lu", (unsigned long)cmd->point_rate);
    
    if (s_playback_state != PLAYBACK_PREPARED && s_playback_state != PLAYBACK_PLAYING) {
        send_response(sock, RESP_NAK_INVAL, CMD_QUEUE_RATE);
        return;
    }
    
    uint32_t rate = cmd->point_rate;
    if (rate == 0 || rate > ETHERDREAM_MAX_POINT_RATE) {
        send_response(sock, RESP_NAK_INVAL, CMD_QUEUE_RATE);
        return;
    }
    
    if (!rate_queue_push(rate)) {
        send_response(sock, RESP_NAK_FULL, CMD_QUEUE_RATE);
        return;
    }
    
    send_response(sock, RESP_ACK, CMD_QUEUE_RATE);
}

static void handle_stop(int sock) {
    ESP_LOGI(TAG, "CMD: Stop");
    
    if (s_playback_state == PLAYBACK_IDLE) {
        send_response(sock, RESP_NAK_INVAL, CMD_STOP);
        return;
    }
    
    dac_timer_set_playback_active(false);  // Stop draining buffer
    s_playback_state = PLAYBACK_IDLE;
    s_playback_flags &= ~0x01;
    s_point_rate = 0;
    
    ESP_LOGI(TAG, "Playback state -> Idle");
    send_response(sock, RESP_ACK, CMD_STOP);
}

static void handle_estop(int sock) {
    dac_timer_set_playback_active(false);  // Stop draining buffer
    s_light_engine_state = LE_ESTOP;
    s_light_engine_flags |= 0x01;
    s_playback_state = PLAYBACK_IDLE;
    s_playback_flags |= 0x04;
    s_playback_flags &= ~0x01;
    s_point_rate = 0;
    
    frame_buffer_clear();
    
    // Rate-limit E-STOP logging to avoid console flood
    s_estop_count++;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (now - s_estop_last_log_ms >= 1000) {
        if (s_estop_count > 1) {
            ESP_LOGW(TAG, "E-STOP (x%lu in last sec)", (unsigned long)s_estop_count);
        } else {
            ESP_LOGW(TAG, "E-STOP");
        }
        s_estop_count = 0;
        s_estop_last_log_ms = now;
    }
    
    send_response(sock, RESP_ACK, CMD_ESTOP_0);
}

static void handle_clear_estop(int sock) {
    ESP_LOGI(TAG, "CMD: Clear E-Stop");
    
    if (s_light_engine_state != LE_ESTOP) {
        send_response(sock, RESP_NAK_INVAL, CMD_CLEAR_ESTOP);
        return;
    }
    
    s_light_engine_state = LE_READY;
    s_light_engine_flags = 0;
    s_playback_state = PLAYBACK_IDLE;
    
    ESP_LOGI(TAG, "Light engine -> Ready");
    send_response(sock, RESP_ACK, CMD_CLEAR_ESTOP);
}

static void handle_ping(int sock) {
    send_response(sock, RESP_ACK, CMD_PING);
}