/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ULogWriter — Write uORB topics to SD card in ULog format.
 *
 * Design:
 *   - Singleton, initialized once at boot.
 *   - Topics are registered via add_topic() with per-topic sampling intervals.
 *   - start() creates a new .ulg file and writes header/format/subscription sections.
 *   - A background task polls topics and writes DATA messages to a ring buffer.
 *   - A consumer periodically flushes the ring buffer to the SD card file.
 *   - stop() closes the file cleanly.
 *
 * Usage:
 *   ULogWriter::instance().init("/sdcard");
 *   ULogWriter::instance().add_topic(ORB_ID(fps_stats), 100);   // 100ms interval
 *   ULogWriter::instance().add_topic(ORB_ID(wifi_state), 200);  // 200ms interval
 *   ULogWriter::instance().start();
 *   ...
 *   ULogWriter::instance().stop();
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "esp_err.h"
#include "uorb.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default sampling interval for topics that specify 0 (ms). */
#define ULOG_DEFAULT_INTERVAL_MS 100

/** Maximum number of logged topics. */
#define ULOG_MAX_TOPICS 16

/** Maximum log file path length. */
#define ULOG_MAX_PATH 128

/** Ring buffer size (bytes). */
#define ULOG_RINGBUF_SIZE (16 * 1024)

/** How often to flush ring buffer to file (ms). */
#define ULOG_FLUSH_INTERVAL_MS 100

/** How often to write sync markers (ms). */
#define ULOG_SYNC_INTERVAL_MS 500

/** How often to fsync the file (ms). */
#define ULOG_FSYNC_INTERVAL_MS 1000

/** ULog writer states. */
typedef enum {
    ULOG_STATE_UNINIT = 0,
    ULOG_STATE_IDLE,
    ULOG_STATE_RUNNING,
    ULOG_STATE_ERROR,
} ulog_state_t;

/**
 * ULogWriter singleton.
 */
typedef struct ulog_writer ulog_writer_t;

/**
 * Create / get the ULogWriter singleton instance.
 */
ulog_writer_t *ulog_writer_get(void);

/**
 * Initialize the ULogWriter.
 *
 * @param sd_mount_path  SD card mount path, e.g. "/sdcard"
 * @return ESP_OK on success
 */
esp_err_t ulog_writer_init(ulog_writer_t *writer, const char *sd_mount_path);

/**
 * Register a topic for logging.
 *
 * @param meta          Topic metadata (use ORB_ID(name))
 * @param interval_ms   Minimum interval between samples (ms). 0 = default.
 * @return ESP_OK on success
 */
esp_err_t ulog_writer_add_topic(ulog_writer_t *writer, orb_id_t meta,
                                uint32_t interval_ms);

/**
 * Start logging — creates a new .ulg file and begins data collection.
 *
 * @return ESP_OK on success
 */
esp_err_t ulog_writer_start(ulog_writer_t *writer);

/**
 * Stop logging — closes the current file.
 *
 * @return ESP_OK on success
 */
esp_err_t ulog_writer_stop(ulog_writer_t *writer);

/**
 * Get current writer state.
 */
ulog_state_t ulog_writer_get_state(const ulog_writer_t *writer);

/**
 * Get the current log file path (empty string if not logging).
 */
const char *ulog_writer_get_filepath(const ulog_writer_t *writer);

/**
 * Get the number of bytes written to the current log file.
 */
size_t ulog_writer_get_bytes_written(const ulog_writer_t *writer);

/**
 * Write a logging message to the ULog file (for debug text).
 *
 * @param level    Log level (0=emerg through 7=debug)
 * @param message  NUL-terminated string
 * @return ESP_OK on success
 */
esp_err_t ulog_writer_write_message(ulog_writer_t *writer, uint8_t level,
                                    const char *message);

/**
 * Deinitialize and free resources.
 */
void ulog_writer_deinit(ulog_writer_t *writer);

#ifdef __cplusplus
}
#endif
