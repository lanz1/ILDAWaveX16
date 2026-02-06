/**
 * @file sd_player.c
 * @brief ILDA file player from SD card
 */

#include "sd_player.h"
#include "hal/sd_card.h"
#include "core/frame_buffer.h"
#include "config.h"
#include "esp_log.h"
#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "SD_PLAYER";

// =============================================================================
// ILDA File Format Structures
// =============================================================================

#pragma pack(push, 1)

typedef struct {
    char signature[4];
    uint8_t reserved1[3];
    uint8_t format;
    char name[8];
    char company[8];
    uint16_t num_records;
    uint16_t frame_number;
    uint16_t total_frames;
    uint8_t projector;
    uint8_t reserved2;
} ilda_header_t;

typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
    uint8_t status;
    uint8_t color_index;
} ilda_point_3d_indexed_t;

typedef struct {
    int16_t x;
    int16_t y;
    uint8_t status;
    uint8_t color_index;
} ilda_point_2d_indexed_t;

typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
    uint8_t status;
    uint8_t blue;
    uint8_t green;
    uint8_t red;
} ilda_point_3d_true_t;

typedef struct {
    int16_t x;
    int16_t y;
    uint8_t status;
    uint8_t blue;
    uint8_t green;
    uint8_t red;
} ilda_point_2d_true_t;

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} ilda_color_t;

#pragma pack(pop)

static const uint8_t ILDA_PALETTE[64][3] = {
    {255, 0, 0},     {255, 16, 0},    {255, 32, 0},    {255, 48, 0},
    {255, 64, 0},    {255, 80, 0},    {255, 96, 0},    {255, 112, 0},
    {255, 128, 0},   {255, 144, 0},   {255, 160, 0},   {255, 176, 0},
    {255, 192, 0},   {255, 208, 0},   {255, 224, 0},   {255, 240, 0},
    {255, 255, 0},   {224, 255, 0},   {192, 255, 0},   {160, 255, 0},
    {128, 255, 0},   {96, 255, 0},    {64, 255, 0},    {32, 255, 0},
    {0, 255, 0},     {0, 255, 32},    {0, 255, 64},    {0, 255, 96},
    {0, 255, 128},   {0, 255, 160},   {0, 255, 192},   {0, 255, 224},
    {0, 255, 255},   {0, 224, 255},   {0, 192, 255},   {0, 160, 255},
    {0, 128, 255},   {0, 96, 255},    {0, 64, 255},    {0, 32, 255},
    {0, 0, 255},     {32, 0, 255},    {64, 0, 255},    {96, 0, 255},
    {128, 0, 255},   {160, 0, 255},   {192, 0, 255},   {224, 0, 255},
    {255, 0, 255},   {255, 32, 255},  {255, 64, 255},  {255, 96, 255},
    {255, 128, 255}, {255, 160, 255}, {255, 192, 255}, {255, 224, 255},
    {255, 255, 255}, {255, 224, 224}, {255, 192, 192}, {255, 160, 160},
    {255, 128, 128}, {255, 96, 96},   {255, 64, 64},   {255, 32, 32},
};

static FILE* s_file = NULL;
static volatile bool s_playing = false;
static volatile bool s_paused = false;
static volatile bool s_loop = true;
static char s_current_file[128] = {0};

static uint8_t s_palette[256][3];
static bool s_custom_palette = false;

static uint32_t s_target_fps = 30;
static uint32_t s_last_frame_time = 0;

static inline int16_t swap16(int16_t val) {
    return ((val & 0xFF) << 8) | ((val >> 8) & 0xFF);
}

static inline uint16_t swap16u(uint16_t val) {
    return ((val & 0xFF) << 8) | ((val >> 8) & 0xFF);
}

static void get_palette_color(uint8_t index, uint8_t* r, uint8_t* g, uint8_t* b) {
    if (s_custom_palette) {
        *r = s_palette[index][0];
        *g = s_palette[index][1];
        *b = s_palette[index][2];
    } else {
        uint8_t idx = index % 64;
        *r = ILDA_PALETTE[idx][0];
        *g = ILDA_PALETTE[idx][1];
        *b = ILDA_PALETTE[idx][2];
    }
}

static int read_frame(void) {
    if (!s_file) return -1;
    
    ilda_header_t header;
    size_t read = fread(&header, 1, sizeof(header), s_file);
    if (read != sizeof(header)) {
        return -1;
    }
    
    if (memcmp(header.signature, "ILDA", 4) != 0) {
        ESP_LOGE(TAG, "Invalid ILDA signature");
        return -1;
    }
    
    uint16_t num_records = swap16u(header.num_records);
    
    if (num_records == 0) {
        return 0;
    }
    
    ESP_LOGD(TAG, "Frame format=%d, points=%d", header.format, num_records);
    
    switch (header.format) {
        case 0: {
            size_t point_size = sizeof(ilda_point_3d_indexed_t);
            laser_point_t points[64];
            size_t batch = 0;
            
            for (int i = 0; i < num_records; i++) {
                ilda_point_3d_indexed_t pt;
                if (fread(&pt, 1, point_size, s_file) != point_size) break;
                
                points[batch].x = swap16(pt.x);
                points[batch].y = -swap16(pt.y);
                
                uint8_t r, g, b;
                get_palette_color(pt.color_index, &r, &g, &b);
                points[batch].r = (uint16_t)r << 8 | r;
                points[batch].g = (uint16_t)g << 8 | g;
                points[batch].b = (uint16_t)b << 8 | b;
                
                points[batch].flags = (pt.status & 0x40) ? POINT_FLAG_BLANK : 0;
                points[batch].user1 = 0;
                points[batch].user2 = 0;
                
                if (++batch >= 64) {
                    frame_buffer_add_points(points, batch);
                    batch = 0;
                }
            }
            if (batch > 0) frame_buffer_add_points(points, batch);
            break;
        }
        
        case 1: {
            size_t point_size = sizeof(ilda_point_2d_indexed_t);
            laser_point_t points[64];
            size_t batch = 0;
            
            for (int i = 0; i < num_records; i++) {
                ilda_point_2d_indexed_t pt;
                if (fread(&pt, 1, point_size, s_file) != point_size) break;
                
                points[batch].x = swap16(pt.x);
                points[batch].y = -swap16(pt.y);
                
                uint8_t r, g, b;
                get_palette_color(pt.color_index, &r, &g, &b);
                points[batch].r = (uint16_t)r << 8 | r;
                points[batch].g = (uint16_t)g << 8 | g;
                points[batch].b = (uint16_t)b << 8 | b;
                
                points[batch].flags = (pt.status & 0x40) ? POINT_FLAG_BLANK : 0;
                points[batch].user1 = 0;
                points[batch].user2 = 0;
                
                if (++batch >= 64) {
                    frame_buffer_add_points(points, batch);
                    batch = 0;
                }
            }
            if (batch > 0) frame_buffer_add_points(points, batch);
            break;
        }
        
        case 2: {
            ESP_LOGI(TAG, "Loading color palette (%d colors)", num_records);
            s_custom_palette = true;
            
            for (int i = 0; i < num_records && i < 256; i++) {
                ilda_color_t color;
                if (fread(&color, 1, sizeof(color), s_file) != sizeof(color)) break;
                s_palette[i][0] = color.red;
                s_palette[i][1] = color.green;
                s_palette[i][2] = color.blue;
            }
            for (int i = 256; i < num_records; i++) {
                ilda_color_t dummy;
                fread(&dummy, 1, sizeof(dummy), s_file);
            }
            return read_frame();
        }
        
        case 4: {
            size_t point_size = sizeof(ilda_point_3d_true_t);
            laser_point_t points[64];
            size_t batch = 0;
            
            for (int i = 0; i < num_records; i++) {
                ilda_point_3d_true_t pt;
                if (fread(&pt, 1, point_size, s_file) != point_size) break;
                
                points[batch].x = swap16(pt.x);
                points[batch].y = -swap16(pt.y);
                points[batch].r = (uint16_t)pt.red << 8 | pt.red;
                points[batch].g = (uint16_t)pt.green << 8 | pt.green;
                points[batch].b = (uint16_t)pt.blue << 8 | pt.blue;
                
                points[batch].flags = (pt.status & 0x40) ? POINT_FLAG_BLANK : 0;
                points[batch].user1 = 0;
                points[batch].user2 = 0;
                
                if (++batch >= 64) {
                    frame_buffer_add_points(points, batch);
                    batch = 0;
                }
            }
            if (batch > 0) frame_buffer_add_points(points, batch);
            break;
        }
        
        case 5: {
            size_t point_size = sizeof(ilda_point_2d_true_t);
            laser_point_t points[64];
            size_t batch = 0;
            
            for (int i = 0; i < num_records; i++) {
                ilda_point_2d_true_t pt;
                if (fread(&pt, 1, point_size, s_file) != point_size) break;
                
                points[batch].x = swap16(pt.x);
                points[batch].y = -swap16(pt.y);
                points[batch].r = (uint16_t)pt.red << 8 | pt.red;
                points[batch].g = (uint16_t)pt.green << 8 | pt.green;
                points[batch].b = (uint16_t)pt.blue << 8 | pt.blue;
                
                points[batch].flags = (pt.status & 0x40) ? POINT_FLAG_BLANK : 0;
                points[batch].user1 = 0;
                points[batch].user2 = 0;
                
                if (++batch >= 64) {
                    frame_buffer_add_points(points, batch);
                    batch = 0;
                }
            }
            if (batch > 0) frame_buffer_add_points(points, batch);
            break;
        }
        
        default:
            ESP_LOGW(TAG, "Unknown ILDA format: %d", header.format);
            fseek(s_file, num_records * 8, SEEK_CUR);
            break;
    }
    
    return num_records;
}

esp_err_t sd_player_init(void) {
    memset(s_palette, 0, sizeof(s_palette));
    s_custom_palette = false;
    s_target_fps = 30;
    
    ESP_LOGI(TAG, "SD player initialized (ILDA parser)");
    return ESP_OK;
}

esp_err_t sd_player_play(const char* filename) {
    if (!sd_card_is_mounted()) {
        ESP_LOGE(TAG, "SD card not mounted");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_file) {
        fclose(s_file);
        s_file = NULL;
    }
    
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", sd_card_get_mount_point(), filename);
    
    s_file = fopen(path, "rb");
    if (!s_file) {
        ESP_LOGE(TAG, "Failed to open: %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    
    s_custom_palette = false;
    
    strncpy(s_current_file, filename, sizeof(s_current_file) - 1);
    s_playing = true;
    s_paused = false;
    s_last_frame_time = 0;
    
    ESP_LOGI(TAG, "Playing: %s", filename);
    return ESP_OK;
}

void sd_player_stop(void) {
    s_playing = false;
    s_paused = false;
    
    if (s_file) {
        fclose(s_file);
        s_file = NULL;
    }
    
    s_current_file[0] = '\0';
    ESP_LOGI(TAG, "Playback stopped");
}

void sd_player_pause(void) {
    if (s_playing) {
        s_paused = true;
        ESP_LOGI(TAG, "Playback paused");
    }
}

void sd_player_resume(void) {
    if (s_playing && s_paused) {
        s_paused = false;
        ESP_LOGI(TAG, "Playback resumed");
    }
}

bool sd_player_is_playing(void) {
    return s_playing && !s_paused;
}

void sd_player_set_loop(bool loop) {
    s_loop = loop;
}

void sd_player_set_fps(uint32_t fps) {
    if (fps > 0 && fps <= 120) {
        s_target_fps = fps;
    }
}

void sd_player_loop(void) {
    if (!s_playing || s_paused || !s_file) return;
    
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t frame_interval = 1000 / s_target_fps;
    
    if (now - s_last_frame_time < frame_interval) {
        return;
    }
    
    if (frame_buffer_level() > FRAME_BUFFER_SIZE - 1000) {
        return;
    }
    
    int result = read_frame();
    
    if (result <= 0) {
        if (s_loop) {
            fseek(s_file, 0, SEEK_SET);
            s_custom_palette = false;
            ESP_LOGD(TAG, "Looping file");
        } else {
            sd_player_stop();
        }
        return;
    }
    
    s_last_frame_time = now;
}

int sd_player_list_files(char files[][64], int max_files) {
    if (!sd_card_is_mounted()) return 0;
    
    int count = 0;
    DIR* dir = opendir(sd_card_get_mount_point());
    
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL && count < max_files) {
            const char* ext = strrchr(entry->d_name, '.');
            if (ext && (strcasecmp(ext, ".ild") == 0 || 
                       strcasecmp(ext, ".ilda") == 0)) {
                strncpy(files[count], entry->d_name, 63);
                files[count][63] = '\0';
                count++;
            }
        }
        closedir(dir);
    }
    
    ESP_LOGI(TAG, "Found %d ILDA files", count);
    return count;
}
