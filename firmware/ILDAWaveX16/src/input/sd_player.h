/**
 * @file sd_player.h
 * @brief ILDA file player from SD card
 * 
 * Supports ILDA file formats 0, 1, 2, 4, 5
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize SD player
 * @return ESP_OK on success
 */
esp_err_t sd_player_init(void);

/**
 * @brief Start playing a file
 * @param filename Path to ILDA file (relative to SD mount point)
 * @return ESP_OK on success
 */
esp_err_t sd_player_play(const char* filename);

/**
 * @brief Stop playback
 */
void sd_player_stop(void);

/**
 * @brief Pause playback
 */
void sd_player_pause(void);

/**
 * @brief Resume playback
 */
void sd_player_resume(void);

/**
 * @brief Check if playing
 * @return true if currently playing
 */
bool sd_player_is_playing(void);

/**
 * @brief Set loop mode
 * @param loop true to loop file, false to stop at end
 */
void sd_player_set_loop(bool loop);

/**
 * @brief Set target frame rate
 * @param fps Frames per second (1-120)
 */
void sd_player_set_fps(uint32_t fps);

/**
 * @brief SD player main loop (call from task)
 */
void sd_player_loop(void);

/**
 * @brief List available ILDA files
 * @param files Array to store filenames
 * @param max_files Maximum number of files
 * @return Number of files found
 */
int sd_player_list_files(char files[][64], int max_files);

#ifdef __cplusplus
}
#endif
