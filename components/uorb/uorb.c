/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * uORB for FreeRTOS — implementation
 *
 * Design summary:
 *   - A global registry holds one entry per topic.
 *   - Each subscriber gets its own FreeRTOS queue (depth = topic's o_depth).
 *   - orb_publish() writes to every subscriber queue.
 *   - Thread safety: registry writes are serialised by a mutex.
 *     Queue operations themselves are FreeRTOS thread-safe.
 */

#include "uorb.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Internal types                                                     */
/* ------------------------------------------------------------------ */

/** Subscriber entry — maps a subscriber handle to its queue + topic. */
typedef struct {
    QueueHandle_t queue;      /**< FreeRTOS queue handle. */
    int           topic_idx;  /**< Index into s_topics[]; < 0 if inactive. */
} orb_sub_entry_t;

/** Topic registry entry. */
typedef struct {
    orb_id_t  meta;               /**< Topic metadata pointer. */
    int       sub_indices[ORB_MAX_SUBSCRIBERS]; /**< Indices into s_subs[]. */
    int       num_subscribers;    /**< Current subscriber count. */
    int       num_publishers;     /**< Current publisher count. */
    bool      active;             /**< True once topic has been registered. */
} orb_topic_reg_t;

/* ------------------------------------------------------------------ */
/*  Global state                                                       */
/* ------------------------------------------------------------------ */

/** Topic registry (one per unique topic). */
static orb_topic_reg_t s_topics[ORB_MAX_TOPICS];

/** Subscriber table (flat, handles are indices into this array). */
#define ORB_MAX_SUBS  (ORB_MAX_TOPICS * ORB_MAX_SUBSCRIBERS)
static orb_sub_entry_t s_subs[ORB_MAX_SUBS];
static int s_num_subs;         /**< Next free index in s_subs[]. */

/** Protects the registry and subscriber table. */
static SemaphoreHandle_t s_mutex;

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static inline void lock(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static inline void unlock(void)
{
    xSemaphoreGive(s_mutex);
}

/**
 * Find a topic in the registry by its metadata pointer.
 * Returns the index, or -1 if not found.
 */
static int topic_find(orb_id_t meta)
{
    for (int i = 0; i < ORB_MAX_TOPICS; i++) {
        if (s_topics[i].active && s_topics[i].meta == meta) {
            return i;
        }
    }
    return -1;
}

/**
 * Find or create a topic in the registry.
 * Returns the index, or -1 if the registry is full.
 * Caller must hold s_mutex.
 */
static int topic_find_or_create(orb_id_t meta)
{
    int idx = topic_find(meta);
    if (idx >= 0) {
        return idx;
    }

    /* Find the first inactive slot */
    for (int i = 0; i < ORB_MAX_TOPICS; i++) {
        if (!s_topics[i].active) {
            memset(&s_topics[i], 0, sizeof(s_topics[i]));
            s_topics[i].meta   = meta;
            s_topics[i].active = true;
            return i;
        }
    }

    return -1; /* Registry full */
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

orb_advert_t orb_advertise(orb_id_t meta)
{
    if (meta == NULL) {
        return -1;
    }

    lock();
    int idx = topic_find_or_create(meta);
    if (idx >= 0) {
        s_topics[idx].num_publishers++;
    }
    unlock();

    return (idx >= 0) ? idx : -1;
}

orb_sub_t orb_subscribe(orb_id_t meta)
{
    if (meta == NULL) {
        return -1;
    }

    lock();

    int t_idx = topic_find_or_create(meta);
    if (t_idx < 0) {
        unlock();
        return -1;
    }

    orb_topic_reg_t *topic = &s_topics[t_idx];

    if (topic->num_subscribers >= ORB_MAX_SUBSCRIBERS) {
        unlock();
        return -1;
    }

    if (s_num_subs >= ORB_MAX_SUBS) {
        unlock();
        return -1;
    }

    /* Create the per-subscriber queue */
    QueueHandle_t q = xQueueCreate(meta->o_depth, meta->o_size);
    if (q == NULL) {
        unlock();
        return -1;
    }

    int s_idx = s_num_subs++;
    s_subs[s_idx].queue     = q;
    s_subs[s_idx].topic_idx = t_idx;

    int pos = topic->num_subscribers++;
    topic->sub_indices[pos] = s_idx;

    unlock();
    return s_idx;
}

int orb_unsubscribe(orb_sub_t handle)
{
    if (handle < 0 || handle >= s_num_subs) {
        return -1;
    }

    lock();

    orb_sub_entry_t *sub = &s_subs[handle];
    if (sub->topic_idx < 0) {
        unlock();
        return -1; /* Already unsubscribed */
    }

    int t_idx = sub->topic_idx;
    orb_topic_reg_t *topic = &s_topics[t_idx];

    /* Remove this subscriber from the topic's subscriber list */
    for (int i = 0; i < topic->num_subscribers; i++) {
        if (topic->sub_indices[i] == handle) {
            topic->num_subscribers--;
            if (i < topic->num_subscribers) {
                topic->sub_indices[i] = topic->sub_indices[topic->num_subscribers];
            }
            break;
        }
    }

    /* Free the queue and mark inactive */
    if (sub->queue) {
        vQueueDelete(sub->queue);
    }
    sub->queue     = NULL;
    sub->topic_idx = -1;

    unlock();
    return 0;
}

int orb_publish(orb_id_t meta, orb_advert_t handle, const void *data)
{
    (void)handle;

    if (meta == NULL || data == NULL) {
        return -1;
    }

    lock();
    int t_idx = topic_find(meta);
    if (t_idx < 0) {
        unlock();
        return -1;
    }

    orb_topic_reg_t *topic = &s_topics[t_idx];
    const bool overwrite = (meta->o_depth == 1);

    for (int i = 0; i < topic->num_subscribers; i++) {
        int s_idx = topic->sub_indices[i];
        QueueHandle_t q = s_subs[s_idx].queue;
        if (q == NULL) {
            continue;
        }

        if (overwrite) {
            /* Latest-only: always keep the newest message */
            xQueueOverwrite(q, data);
        } else {
            /* Multi-depth: enqueue, drop if full */
            xQueueSend(q, data, 0);
        }
    }

    unlock();
    return 0;
}

int orb_copy(orb_id_t meta, orb_sub_t handle, void *buffer)
{
    (void)meta;

    if (handle < 0 || handle >= s_num_subs || buffer == NULL) {
        return -1;
    }

    /* We access the subscriber entry lock-free because:
     * - s_num_subs only grows (entries are never removed from the array)
     * - sub->queue is written once at creation and only cleared on
     *   unsubscribe (which also requires the handle to be valid)
     * - xQueueReceive is itself thread-safe.
     */
    orb_sub_entry_t *sub = &s_subs[handle];
    if (sub->topic_idx < 0) {
        return -1; /* Unsubscribed */
    }

    BaseType_t ret = xQueueReceive(sub->queue, buffer, portMAX_DELAY);
    return (ret == pdTRUE) ? 0 : -1;
}

int orb_check(orb_sub_t handle, bool *updated)
{
    if (handle < 0 || handle >= s_num_subs || updated == NULL) {
        return -1;
    }

    orb_sub_entry_t *sub = &s_subs[handle];
    if (sub->topic_idx < 0) {
        return -1; /* Unsubscribed */
    }

    *updated = (uxQueueMessagesWaiting(sub->queue) > 0);
    return 0;
}
