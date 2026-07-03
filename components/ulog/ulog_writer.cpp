/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ULogWriter implementation — uORB topic logging to SD card in ULog format.
 *
 * Architecture:
 *   ULogWriter singleton
 *     ├─ Topic registry (subscribed uORB topics + intervals)
 *     ├─ Ring buffer (lock-free circular byte buffer)
 *     └─ Writer task (poll → format → ring → flush → file)
 */

#include "ulog_writer.h"
#include "ulog_messages.h"

#include <cstring>
#include <cstdio>
#include <ctime>
#include <cerrno>

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "esp_log.h"

static const char *TAG = "ULog";

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

/** Maximum number of topic subscriptions. */
#define MAX_TOPICS  ULOG_MAX_TOPICS

/** Maximum characters in a file path. */
#define MAX_PATH    ULOG_MAX_PATH

/** Max session number digits. */
#define MAX_SESSION_NUM 999

/** Log subdirectory under the SD card mount point. */
#define LOG_DIR_NAME "log"

/** File name format. */
#define FILE_NAME_FMT "session_%03u.ulg"

#define MS_TO_US(ms)  ((ms) * 1000ULL)

/* ------------------------------------------------------------------ */
/*  Ring buffer                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *buffer;
    size_t   size;
    volatile size_t write_pos;
    volatile size_t read_pos;
    SemaphoreHandle_t mutex;
} ringbuf_t;

static bool ringbuf_init(ringbuf_t *rb, size_t size)
{
    rb->buffer = (uint8_t *)malloc(size);
    if (!rb->buffer) return false;
    rb->size = size;
    rb->write_pos = 0;
    rb->read_pos = 0;
    rb->mutex = xSemaphoreCreateMutex();
    return rb->mutex != NULL;
}

static void ringbuf_deinit(ringbuf_t *rb)
{
    if (rb->mutex) vSemaphoreDelete(rb->mutex);
    free(rb->buffer);
    memset(rb, 0, sizeof(*rb));
}

static size_t ringbuf_available(const ringbuf_t *rb)
{
    size_t w = rb->write_pos;
    size_t r = rb->read_pos;
    if (w >= r) return w - r;
    return rb->size - (r - w);
}

static size_t ringbuf_free_space(const ringbuf_t *rb)
{
    /* Leave one byte gap to distinguish full vs empty */
    return rb->size - ringbuf_available(rb) - 1;
}

static bool ringbuf_write(ringbuf_t *rb, const uint8_t *data, size_t len)
{
    if (len == 0) return true;
    if (len > ringbuf_free_space(rb)) return false;

    xSemaphoreTake(rb->mutex, portMAX_DELAY);

    size_t w = rb->write_pos;
    size_t end = w + len;

    if (end <= rb->size) {
        memcpy(rb->buffer + w, data, len);
    } else {
        size_t first = rb->size - w;
        memcpy(rb->buffer + w, data, first);
        memcpy(rb->buffer, data + first, len - first);
    }

    /* Ensure write_pos is updated atomically (size_t is word-aligned on P4) */
    rb->write_pos = end % rb->size;

    xSemaphoreGive(rb->mutex);
    return true;
}

static size_t ringbuf_read(ringbuf_t *rb, uint8_t *dst, size_t max_len)
{
    size_t avail = ringbuf_available(rb);
    if (avail == 0) return 0;

    size_t to_read = (avail < max_len) ? avail : max_len;

    xSemaphoreTake(rb->mutex, portMAX_DELAY);

    size_t r = rb->read_pos;
    size_t end = r + to_read;

    if (end <= rb->size) {
        memcpy(dst, rb->buffer + r, to_read);
    } else {
        size_t first = rb->size - r;
        memcpy(dst, rb->buffer + r, first);
        memcpy(dst + first, rb->buffer, to_read - first);
    }

    rb->read_pos = end % rb->size;

    xSemaphoreGive(rb->mutex);
    return to_read;
}

/* ------------------------------------------------------------------ */
/*  Topic subscription entry                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    orb_id_t     meta;          /**< Topic metadata */
    orb_sub_t    sub_handle;    /**< uORB subscriber handle */
    uint32_t     interval_ms;   /**< Minimum sampling interval */
    uint64_t     last_poll_us;  /**< Last poll timestamp (esp_timer) */
    uint16_t     msg_id;        /**< ULog-internal message ID */
    bool         active;        /**< True if this slot is in use */
} topic_entry_t;

/* ------------------------------------------------------------------ */
/*  ULogWriter instance                                                */
/* ------------------------------------------------------------------ */

typedef struct ulog_writer {
    /* Configuration */
    char sd_mount_path[MAX_PATH];

    /* State */
    ulog_state_t state;

    /* File I/O */
    int          fd;                /**< Current log file descriptor, -1 if none */
    char         filepath[MAX_PATH]; /**< Current log file path */
    size_t       bytes_written;     /**< Total bytes written to current file */

    /* Topics */
    topic_entry_t topics[MAX_TOPICS];
    int           num_topics;
    uint16_t      next_msg_id;      /**< Monotonic msg_id counter */

    /* Ring buffer */
    ringbuf_t     ringbuf;

    /* Writer task */
    TaskHandle_t  task_handle;
    bool          task_should_run;

    /* Timing */
    uint64_t      last_flush_us;
    uint64_t      last_sync_us;
    uint64_t      last_fsync_us;
    uint64_t      start_time_us;    /**< When logging started (esp_timer) */

    /* Info fields */
    char          sys_name[32];     /**< System name, e.g. "esp32p4_monitor" */
    char          ver_sw[32];       /**< Software version string */
} ulog_writer_t;

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */

static void writer_task_func(void *arg);
static esp_err_t write_file_header(ulog_writer_t *writer);
static esp_err_t write_flag_bits(ulog_writer_t *writer);
static esp_err_t write_info_messages(ulog_writer_t *writer);
static esp_err_t write_format_messages(ulog_writer_t *writer);
static esp_err_t write_subscription_messages(ulog_writer_t *writer);
static void find_next_session(const char *dir, char *out_path, size_t out_size);
static void ensure_log_dir(const char *dir);

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

ulog_writer_t *ulog_writer_get(void)
{
    static ulog_writer_t s_instance;
    return &s_instance;
}

esp_err_t ulog_writer_init(ulog_writer_t *writer, const char *sd_mount_path)
{
    if (!writer || !sd_mount_path) return ESP_ERR_INVALID_ARG;

    memset(writer, 0, sizeof(*writer));
    writer->fd = -1;
    writer->state = ULOG_STATE_IDLE;
    strlcpy(writer->sd_mount_path, sd_mount_path, sizeof(writer->sd_mount_path));
    snprintf(writer->sys_name, sizeof(writer->sys_name), "esp32p4_monitor");
    snprintf(writer->ver_sw, sizeof(writer->ver_sw), "IDF %s", esp_get_idf_version());

    if (!ringbuf_init(&writer->ringbuf, ULOG_RINGBUF_SIZE)) {
        ESP_LOGE(TAG, "Failed to allocate ring buffer");
        writer->state = ULOG_STATE_ERROR;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Initialized (SD: %s, ringbuf: %u bytes)",
             sd_mount_path, ULOG_RINGBUF_SIZE);
    return ESP_OK;
}

esp_err_t ulog_writer_add_topic(ulog_writer_t *writer, orb_id_t meta,
                                uint32_t interval_ms)
{
    if (!writer || !meta) return ESP_ERR_INVALID_ARG;
    if (writer->state == ULOG_STATE_RUNNING) {
        ESP_LOGW(TAG, "Cannot add topics while logging is active");
        return ESP_ERR_INVALID_STATE;
    }
    if (writer->num_topics >= MAX_TOPICS) {
        ESP_LOGE(TAG, "Max topics reached (%d)", MAX_TOPICS);
        return ESP_ERR_NO_MEM;
    }

    topic_entry_t *entry = &writer->topics[writer->num_topics];
    entry->meta = meta;
    entry->interval_ms = (interval_ms > 0) ? interval_ms : ULOG_DEFAULT_INTERVAL_MS;
    entry->last_poll_us = 0;
    entry->msg_id = writer->next_msg_id++;
    entry->active = true;

    writer->num_topics++;

    ESP_LOGI(TAG, "Added topic %s (msg_id=%u, interval=%ums)",
             meta->o_name, entry->msg_id, entry->interval_ms);
    return ESP_OK;
}

esp_err_t ulog_writer_start(ulog_writer_t *writer)
{
    if (!writer) return ESP_ERR_INVALID_ARG;
    if (writer->state == ULOG_STATE_RUNNING) {
        ESP_LOGW(TAG, "Already logging");
        return ESP_OK;
    }
    if (writer->num_topics == 0) {
        ESP_LOGW(TAG, "No topics registered, nothing to log");
        return ESP_ERR_INVALID_STATE;
    }

    /* Ensure log directory exists */
    char log_dir[MAX_PATH + 8];
    snprintf(log_dir, sizeof(log_dir), "%s/%s", writer->sd_mount_path, LOG_DIR_NAME);
    ensure_log_dir(log_dir);

    /* Find next available session number */
    find_next_session(log_dir, writer->filepath, sizeof(writer->filepath));

    /* Open the file */
    writer->fd = open(writer->filepath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (writer->fd < 0) {
        ESP_LOGE(TAG, "Failed to create %s", writer->filepath);
        writer->state = ULOG_STATE_ERROR;
        return ESP_FAIL;
    }

    /* Subscribe to all topics */
    for (int i = 0; i < writer->num_topics; i++) {
        topic_entry_t *entry = &writer->topics[i];
        entry->sub_handle = orb_subscribe(entry->meta);
        if (entry->sub_handle < 0) {
            ESP_LOGW(TAG, "Failed to subscribe to %s", entry->meta->o_name);
        }
    }

    writer->bytes_written = 0;
    writer->start_time_us = esp_timer_get_time();
    writer->last_flush_us = writer->start_time_us;
    writer->last_sync_us  = writer->start_time_us;
    writer->last_fsync_us = writer->start_time_us;

    /* Write file header + definition section */
    esp_err_t err;

    err = write_file_header(writer);
    if (err != ESP_OK) { close(writer->fd); writer->fd = -1; writer->state = ULOG_STATE_ERROR; return err; }

    err = write_flag_bits(writer);
    if (err != ESP_OK) { close(writer->fd); writer->fd = -1; writer->state = ULOG_STATE_ERROR; return err; }

    err = write_info_messages(writer);
    if (err != ESP_OK) { close(writer->fd); writer->fd = -1; writer->state = ULOG_STATE_ERROR; return err; }

    err = write_format_messages(writer);
    if (err != ESP_OK) { close(writer->fd); writer->fd = -1; writer->state = ULOG_STATE_ERROR; return err; }

    err = write_subscription_messages(writer);
    if (err != ESP_OK) { close(writer->fd); writer->fd = -1; writer->state = ULOG_STATE_ERROR; return err; }

    /* Everything from now on goes through the ring buffer (non-reliable) */

    /* Create the writer task */
    writer->task_should_run = true;
    BaseType_t ret = xTaskCreate(
        writer_task_func,
        "ulog_writer",
        4096,        /* stack size */
        writer,      /* arg */
        5,           /* priority */
        &writer->task_handle
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create writer task");
        close(writer->fd);
        writer->fd = -1;
        writer->state = ULOG_STATE_ERROR;
        return ESP_FAIL;
    }

    writer->state = ULOG_STATE_RUNNING;
    ESP_LOGI(TAG, "Logging started: %s (%d topics)", writer->filepath, writer->num_topics);
    return ESP_OK;
}

esp_err_t ulog_writer_stop(ulog_writer_t *writer)
{
    if (!writer) return ESP_ERR_INVALID_ARG;
    if (writer->state != ULOG_STATE_RUNNING) {
        return ESP_OK;
    }

    /* Stop the writer task: signal and wait for it to drain */
    writer->task_should_run = false;
    if (writer->task_handle) {
        /* Wait for the task to process its last iteration and drain data */
        TickType_t timeout = pdMS_TO_TICKS(100);
        uint32_t notify;
        if (xTaskNotifyWait(0, 0, &notify, timeout) != pdTRUE) {
            /* Task didn't notify within timeout, force delete */
            vTaskDelete(writer->task_handle);
        }
        writer->task_handle = NULL;
    }

    /* Drain remaining ring buffer data to file */
    uint8_t buf[512];
    size_t n;
    while ((n = ringbuf_read(&writer->ringbuf, buf, sizeof(buf))) > 0) {
        write(writer->fd, buf, n);
        writer->bytes_written += n;
    }

    /* Close the file */
    if (writer->fd >= 0) {
        fsync(writer->fd);
        close(writer->fd);
        writer->fd = -1;
    }

    /* Unsubscribe */
    for (int i = 0; i < writer->num_topics; i++) {
        topic_entry_t *entry = &writer->topics[i];
        if (entry->sub_handle >= 0) {
            orb_unsubscribe(entry->sub_handle);
            entry->sub_handle = -1;
        }
    }

    /* Report statistics */
    uint64_t elapsed_us = esp_timer_get_time() - writer->start_time_us;
    uint32_t elapsed_ms = (uint32_t)(elapsed_us / 1000);
    double rate = (elapsed_ms > 0)
        ? (double)writer->bytes_written * 1000.0 / (double)elapsed_ms
        : 0.0;

    ESP_LOGI(TAG, "Logging stopped: %s (%u bytes in %ums, %.1f B/s)",
             writer->filepath, (unsigned)writer->bytes_written,
             elapsed_ms, rate);

    writer->state = ULOG_STATE_IDLE;
    return ESP_OK;
}

ulog_state_t ulog_writer_get_state(const ulog_writer_t *writer)
{
    return writer ? writer->state : ULOG_STATE_UNINIT;
}

const char *ulog_writer_get_filepath(const ulog_writer_t *writer)
{
    return writer ? writer->filepath : "";
}

size_t ulog_writer_get_bytes_written(const ulog_writer_t *writer)
{
    return writer ? writer->bytes_written : 0;
}

esp_err_t ulog_writer_write_message(ulog_writer_t *writer, uint8_t level,
                                    const char *message)
{
    if (!writer || writer->state != ULOG_STATE_RUNNING) return ESP_ERR_INVALID_STATE;

    size_t msg_len = strlen(message);
    if (msg_len > 127) msg_len = 127;

    ulog_message_logging_s msg;
    size_t total_size = ULOG_MSG_HEADER_LEN + 9 + msg_len + 1; /* msg_size field value */
    msg.msg_size = (uint16_t)(total_size - ULOG_MSG_HEADER_LEN);
    msg.msg_type = ULOG_MSG_TYPE_LOGGING;
    msg.log_level = level;
    msg.timestamp = esp_timer_get_time();
    memcpy(msg.message, message, msg_len);
    msg.message[msg_len] = '\0';

    if (!ringbuf_write(&writer->ringbuf, (const uint8_t *)&msg,
                       ULOG_MSG_HEADER_LEN + 9 + msg_len + 1)) {
        ESP_LOGW(TAG, "Ring buffer full, dropping log message");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ulog_writer_deinit(ulog_writer_t *writer)
{
    if (!writer) return;
    if (writer->state == ULOG_STATE_RUNNING) {
        ulog_writer_stop(writer);
    }
    ringbuf_deinit(&writer->ringbuf);
    writer->state = ULOG_STATE_UNINIT;
    ESP_LOGI(TAG, "Deinitialized");
}

/* ------------------------------------------------------------------ */
/*  Writer task                                                        */
/* ------------------------------------------------------------------ */

static void writer_task_func(void *arg)
{
    ulog_writer_t *writer = (ulog_writer_t *)arg;

    /* Scratch buffer for formatting one DATA message.
     * 3 (header) + 2 (msg_id) + max topic size (~hundred bytes).
     * 512 bytes is sufficient for all current topics. */
    uint8_t data_buf[512];
    uint8_t flush_buf[1024];

    while (writer->task_should_run) {
        uint64_t now_us = esp_timer_get_time();

        /* ── Poll each topic ── */
        for (int i = 0; i < writer->num_topics; i++) {
            topic_entry_t *entry = &writer->topics[i];
            if (!entry->active || entry->sub_handle < 0) continue;

            uint64_t interval_us = MS_TO_US(entry->interval_ms);
            if ((now_us - entry->last_poll_us) < interval_us) continue;
            entry->last_poll_us = now_us;

            bool updated = false;
            if (orb_check(entry->sub_handle, &updated) != 0 || !updated) continue;

            /* Copy the topic data */
            void *payload = data_buf + ULOG_MSG_HEADER_LEN + sizeof(uint16_t);
            if (orb_copy(entry->meta, entry->sub_handle, payload) != 0) continue;

            /* Build ULog DATA message header */
            uint16_t payload_size = (uint16_t)entry->meta->o_size;
            uint16_t msg_total = (uint16_t)(sizeof(uint16_t) + payload_size); /* excl. 3-byte header */

            data_buf[0] = (uint8_t)(msg_total & 0xFF);
            data_buf[1] = (uint8_t)((msg_total >> 8) & 0xFF);
            data_buf[2] = ULOG_MSG_TYPE_DATA;
            data_buf[3] = (uint8_t)(entry->msg_id & 0xFF);
            data_buf[4] = (uint8_t)((entry->msg_id >> 8) & 0xFF);

            size_t total = ULOG_MSG_HEADER_LEN + msg_total;

            if (!ringbuf_write(&writer->ringbuf, data_buf, total)) {
                ESP_LOGW(TAG, "Ring buffer full, dropping %s data", entry->meta->o_name);
            }
        }

        /* ── Periodic flush to file ── */
        uint64_t flush_interval_us = MS_TO_US(ULOG_FLUSH_INTERVAL_MS);
        if ((now_us - writer->last_flush_us) >= flush_interval_us) {
            writer->last_flush_us = now_us;

            /* Check if it's time for a sync marker */
            uint64_t sync_interval_us = MS_TO_US(ULOG_SYNC_INTERVAL_MS);
            if ((now_us - writer->last_sync_us) >= sync_interval_us) {
                writer->last_sync_us = now_us;
                /* Insert sync marker into the ring buffer (so it's in data section) */
                uint8_t sync_buf[ULOG_MSG_HEADER_LEN + 8];
                sync_buf[0] = 8;  /* msg_size = 8 */
                sync_buf[1] = 0;
                sync_buf[2] = ULOG_MSG_TYPE_SYNC;
                const uint8_t sync_magic[8] = ULOG_SYNC_MAGIC;
                memcpy(sync_buf + 3, sync_magic, 8);
                ringbuf_write(&writer->ringbuf, sync_buf, sizeof(sync_buf));
            }

            /* Drain ring buffer to file */
            size_t total_flushed = 0;
            size_t n;
            while ((n = ringbuf_read(&writer->ringbuf, flush_buf, sizeof(flush_buf))) > 0) {
                ssize_t written = write(writer->fd, flush_buf, n);
                if (written > 0) {
                    writer->bytes_written += (size_t)written;
                    total_flushed += (size_t)written;
                } else {
                    ESP_LOGE(TAG, "File write error");
                    break;
                }
            }

            /* Periodic fsync */
            uint64_t fsync_interval_us = MS_TO_US(ULOG_FSYNC_INTERVAL_MS);
            if ((now_us - writer->last_fsync_us) >= fsync_interval_us) {
                writer->last_fsync_us = now_us;
                fsync(writer->fd);
            }

            if (total_flushed > 0) {
                ESP_LOGV(TAG, "Flushed %u bytes to %s",
                         (unsigned)total_flushed, writer->filepath);
            }
        }

        /* Sleep a short time */
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* Task self-delete — notify the parent first */
    if (writer->task_handle) {
        xTaskNotifyGive(writer->task_handle); /* unused, but signals readiness */
    }
    writer->task_handle = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  File I/O helpers — write individual ULog sections                  */
/* ------------------------------------------------------------------ */

static esp_err_t write_all(ulog_writer_t *writer, const void *data, size_t len)
{
    if (writer->fd < 0) return ESP_ERR_INVALID_STATE;
    const uint8_t *ptr = (const uint8_t *)data;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(writer->fd, ptr, remaining);
        if (n <= 0) {
            ESP_LOGE(TAG, "Write error: errno=%d", errno);
            return ESP_FAIL;
        }
        ptr += n;
        remaining -= (size_t)n;
    }
    writer->bytes_written += len;
    return ESP_OK;
}

/** Write file header: magic + timestamp. */
static esp_err_t write_file_header(ulog_writer_t *writer)
{
    ulog_file_header_s hdr;
    memcpy(hdr.magic, ULOG_MAGIC, ULOG_MAGIC_LEN);
    hdr.timestamp = esp_timer_get_time() + (uint64_t)1700000000ULL * 1000000ULL;
    /* ^ Rough Unix epoch: esp_timer is relative to boot, add 1700000000 = ~Oct 2023
     * for approximate wall clock. Users can correct with a utc_offset topic if needed. */
    return write_all(writer, &hdr, sizeof(hdr));
}

/** Write flag bits message. */
static esp_err_t write_flag_bits(ulog_writer_t *writer)
{
    ulog_message_flag_bits_s msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_size = sizeof(msg) - ULOG_MSG_HEADER_LEN;
    msg.msg_type = ULOG_MSG_TYPE_FLAG_BITS;
    /* No special flags set */
    return write_all(writer, &msg, sizeof(msg));
}

/** Write info messages: sys_name, version, etc. */
static esp_err_t write_info_messages(ulog_writer_t *writer)
{
    /* Helper to write one info key-value.
     * Format: "type key_name value" or just "char[%d] key_name%s" for strings. */
    auto write_info = [writer](const char *type, const char *key, const char *value) -> esp_err_t {
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "%s %s %s", type, key, value);
        if (n < 0 || (size_t)n >= sizeof(buf)) return ESP_ERR_INVALID_SIZE;

        uint8_t key_len = (uint8_t)strlen(key);
        uint16_t total = (uint16_t)(ULOG_MSG_HEADER_LEN + 1 + (uint16_t)n + 1);

        uint8_t msg[512];
        size_t pos = 0;
        msg[pos++] = (uint8_t)((total - ULOG_MSG_HEADER_LEN) & 0xFF);
        msg[pos++] = (uint8_t)(((total - ULOG_MSG_HEADER_LEN) >> 8) & 0xFF);
        msg[pos++] = ULOG_MSG_TYPE_INFO;
        msg[pos++] = key_len;
        memcpy(msg + pos, buf, (size_t)n + 1); /* include NUL */
        pos += (size_t)n + 1;

        return write_all(writer, msg, pos);
    };

    esp_err_t err;

    err = write_info("char[%d]", "sys_name", writer->sys_name);
    if (err != ESP_OK) return err;

    err = write_info("char[%d]", "ver_sw", writer->ver_sw);
    if (err != ESP_OK) return err;

    err = write_info("char[%d]", "arch", "esp32p4");
    if (err != ESP_OK) return err;

    return ESP_OK;
}

/** Write format messages for all subscribed topics. */
static esp_err_t write_format_messages(ulog_writer_t *writer)
{
    for (int i = 0; i < writer->num_topics; i++) {
        topic_entry_t *entry = &writer->topics[i];
        const char *fmt = entry->meta->o_format;
        size_t fmt_len = strlen(fmt);
        if (fmt_len == 0) continue;

        uint16_t msg_total = (uint16_t)(fmt_len + 1); /* +1 for NUL */
        uint8_t header[ULOG_MSG_HEADER_LEN];
        header[0] = (uint8_t)(msg_total & 0xFF);
        header[1] = (uint8_t)((msg_total >> 8) & 0xFF);
        header[2] = ULOG_MSG_TYPE_FORMAT;

        esp_err_t err = write_all(writer, header, sizeof(header));
        if (err != ESP_OK) return err;
        err = write_all(writer, fmt, fmt_len + 1);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

/** Write subscription (ADD_LOGGED_MSG) messages. */
static esp_err_t write_subscription_messages(ulog_writer_t *writer)
{
    for (int i = 0; i < writer->num_topics; i++) {
        topic_entry_t *entry = &writer->topics[i];
        const char *name = entry->meta->o_name;
        size_t name_len = strlen(name);

        uint16_t payload_size = (uint16_t)(1 + 2 + name_len + 1); /* multi_id + msg_id + name + NUL */
        uint8_t header[ULOG_MSG_HEADER_LEN];
        header[0] = (uint8_t)(payload_size & 0xFF);
        header[1] = (uint8_t)((payload_size >> 8) & 0xFF);
        header[2] = ULOG_MSG_TYPE_ADD_LOGGED_MSG;

        esp_err_t err = write_all(writer, header, sizeof(header));
        if (err != ESP_OK) return err;

        uint8_t body[256];
        body[0] = 0; /* multi_id = 0 */
        body[1] = (uint8_t)(entry->msg_id & 0xFF);
        body[2] = (uint8_t)((entry->msg_id >> 8) & 0xFF);
        memcpy(body + 3, name, name_len + 1);

        err = write_all(writer, body, payload_size);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  File system helpers                                                */
/* ------------------------------------------------------------------ */

static void ensure_log_dir(const char *dir)
{
    struct stat st;
    if (stat(dir, &st) != 0) {
        mkdir(dir, 0755);
        ESP_LOGI(TAG, "Created log directory: %s", dir);
    }
}

static void find_next_session(const char *dir, char *out_path, size_t out_size)
{
    unsigned int session_num = 1;
    char test_path[MAX_PATH + 32]; /* extra space for path + filename */

    for (; session_num <= MAX_SESSION_NUM; session_num++) {
        snprintf(test_path, sizeof(test_path), "%s/" FILE_NAME_FMT, dir, session_num);
        struct stat st;
        if (stat(test_path, &st) != 0) {
            /* File doesn't exist, use this name */
            strlcpy(out_path, test_path, out_size);
            return;
        }
    }

    /* All slots taken, overwrite the oldest (session_001) */
    snprintf(test_path, sizeof(test_path), "%s/" FILE_NAME_FMT, dir, 1);
    strlcpy(out_path, test_path, out_size);
    ESP_LOGW(TAG, "Max sessions reached, overwriting %s", out_path);
}
