/*
 * SystemMonitor — Implementation.
 *
 * Samples FreeRTOS task CPU usage and heap memory at a configurable
 * interval, publishes results via uORB system_stats topic.
 */

#include "system_monitor.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "topics.h"
#include <cstring>
#include <algorithm>
#include <cstdlib>

/* Kconfig defaults — can be overridden in Kconfig.projbuild */
#ifndef CONFIG_APP_SYS_MONITOR_INTERVAL_MS
#define CONFIG_APP_SYS_MONITOR_INTERVAL_MS 5000
#endif

#ifndef CONFIG_APP_SYS_MONITOR_LOG_INTERVAL
#define CONFIG_APP_SYS_MONITOR_LOG_INTERVAL 12
#endif

#ifndef CONFIG_APP_SYS_MONITOR_TOP_N
#define CONFIG_APP_SYS_MONITOR_TOP_N 6
#endif

#ifndef CONFIG_APP_SYS_MONITOR_TASK_STACK
#define CONFIG_APP_SYS_MONITOR_TASK_STACK 4096
#endif

/* Alert thresholds — default CPU > 90%, memory usage > 80% */
#ifndef CONFIG_APP_SYS_MONITOR_CPU_ALERT_PCT
#define CONFIG_APP_SYS_MONITOR_CPU_ALERT_PCT 90
#endif

#ifndef CONFIG_APP_SYS_MONITOR_MEM_ALERT_PCT
#define CONFIG_APP_SYS_MONITOR_MEM_ALERT_PCT 80
#endif

#ifndef CONFIG_APP_SYS_MONITOR_ALERT_COOLDOWN_S
#define CONFIG_APP_SYS_MONITOR_ALERT_COOLDOWN_S 10
#endif

/* Number of top tasks to track (must match system_stats.msg structure) */
static constexpr int TOP_N = CONFIG_APP_SYS_MONITOR_TOP_N;

/*============================================================================
 * Singleton
 *============================================================================*/
SystemMonitor& SystemMonitor::instance(void)
{
    static SystemMonitor inst;
    return inst;
}

SystemMonitor::SystemMonitor()
{
    memset(&_latest, 0, sizeof(_latest));
}

/*============================================================================
 * Init / Start / Stop
 *============================================================================*/
bool SystemMonitor::init(void)
{
    if (_initialized.load(std::memory_order_relaxed)) {
        return true;
    }

    _latest_mutex = xSemaphoreCreateMutex();
    if (!_latest_mutex) {
        ESP_LOGE(TAG, "Failed to create latest_mutex");
        return false;
    }

    _alert_mutex = xSemaphoreCreateMutex();
    if (!_alert_mutex) {
        ESP_LOGE(TAG, "Failed to create alert_mutex");
        return false;
    }

    /* Track minimum free heap from boot */
    uint32_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    _min_free_internal.store(free_int, std::memory_order_relaxed);
    _min_free_psram.store(free_psram, std::memory_order_relaxed);

    _initialized.store(true, std::memory_order_release);

    /* Print detailed heap region info once at init for debugging SRAM allocation */
    ESP_LOGI(TAG, "── Heap Region Details ──");
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "Initialized (interval=%dms, log_every=%d samples, top_n=%d, "
             "cpu_alert=%d%%, mem_alert=%d%%, cooldown=%ds)",
             CONFIG_APP_SYS_MONITOR_INTERVAL_MS,
             CONFIG_APP_SYS_MONITOR_LOG_INTERVAL,
             TOP_N,
             CONFIG_APP_SYS_MONITOR_CPU_ALERT_PCT,
             CONFIG_APP_SYS_MONITOR_MEM_ALERT_PCT,
             CONFIG_APP_SYS_MONITOR_ALERT_COOLDOWN_S);
    return true;
}

bool SystemMonitor::start(void)
{
    if (!_initialized.load(std::memory_order_relaxed)) {
        ESP_LOGE(TAG, "Not initialized, call init() first");
        return false;
    }

    /* Atomic compare-exchange to prevent double-start */
    bool expected = false;
    if (!_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        ESP_LOGW(TAG, "Already running");
        return true;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        _monitor_task_func,
        "sys_monitor",
        CONFIG_APP_SYS_MONITOR_TASK_STACK,
        this,
        1,  /* Low priority — must not interfere with real-time tasks */
        &_task_handle,
        0   /* Pin to core 0 (core 1 runs LVGL) */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create monitor task");
        _running.store(false, std::memory_order_relaxed);
        return false;
    }

    ESP_LOGI(TAG, "Started on core 0");
    return true;
}

void SystemMonitor::stop(void)
{
    bool expected = true;
    if (!_running.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        return;  /* Not running */
    }

    /* Wait for task to exit. The monitor task checks _running each cycle
     * and self-deletes via vTaskDelete(NULL). We wait one full interval
     * plus a margin, then verify the task is truly gone via eTaskGetState(). */
    if (_task_handle) {
        TaskHandle_t handle = _task_handle;
        vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_SYS_MONITOR_INTERVAL_MS + 500));
        /* If the task still exists (shouldn't happen), force-delete it */
        if (eTaskGetState(handle) != eDeleted) {
            ESP_LOGW(TAG, "Monitor task did not exit gracefully, force-deleting");
            vTaskDelete(handle);
        }
        _task_handle = nullptr;
    }

    ESP_LOGI(TAG, "Stopped");
}

/*============================================================================
 * Background Task
 *============================================================================*/
void SystemMonitor::_monitor_task_func(void *arg)
{
    SystemMonitor *self = static_cast<SystemMonitor *>(arg);

    ESP_LOGI(TAG, "Monitor task started");

    while (self->_running.load(std::memory_order_relaxed)) {
        self->_sample();
        vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_SYS_MONITOR_INTERVAL_MS));
    }

    /* Clean up per-task snapshot and self-delete.
     * Do NOT write _task_handle — the owner (stop()) manages it exclusively
     * to avoid racing with a subsequent start() call. */
    if (self->_prev_tasks) {
        free(self->_prev_tasks);
        self->_prev_tasks = nullptr;
    }
    ESP_LOGI(TAG, "Monitor task exiting");
    vTaskDelete(NULL);
}

/*============================================================================
 * Sampling
 *============================================================================*/
void SystemMonitor::_sample(void)
{
    /* ── 1. Heap memory stats ── */
    uint32_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t min_free_int = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t min_free_psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

    /* Update historical minimums */
    uint32_t prev_min = _min_free_internal.load(std::memory_order_relaxed);
    if (min_free_int < prev_min) {
        _min_free_internal.store(min_free_int, std::memory_order_relaxed);
    }
    prev_min = _min_free_psram.load(std::memory_order_relaxed);
    if (min_free_psram < prev_min) {
        _min_free_psram.store(min_free_psram, std::memory_order_relaxed);
    }

    /* ── 2. Task CPU stats ── */
    UBaseType_t task_count = uxTaskGetNumberOfTasks();

    /* Allocate array for task status. Use PSRAM if available to save SRAM. */
    size_t array_size = sizeof(TaskStatus_t) * task_count;
    TaskStatus_t *task_array = nullptr;

    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > array_size + 4096) {
        task_array = static_cast<TaskStatus_t *>(
            heap_caps_malloc(array_size, MALLOC_CAP_SPIRAM));
    }
    if (!task_array) {
        task_array = static_cast<TaskStatus_t *>(
            heap_caps_malloc(array_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (!task_array) {
        ESP_LOGW(TAG, "Cannot allocate task array (%u tasks)", (unsigned)task_count);
        return;
    }

    uint32_t total_run_time = 0;
    UBaseType_t actual_count = uxTaskGetSystemState(task_array, task_count, &total_run_time);

    /* ── 3. Build system_stats_s ── */
    system_stats_s stats = {};
    stats.timestamp = (uint64_t)esp_timer_get_time();
    stats.free_internal = free_internal;
    stats.free_psram = free_psram;
    stats.min_free_internal = min_free_int;
    stats.min_free_psram = min_free_psram;
    stats.task_count = (uint32_t)actual_count;

    /* Compute total CPU% as delta from previous sample.
     *
     * On ESP-IDF with SMP FreeRTOS, uxTaskGetSystemState() stores
     * esp_timer_get_time() (wall-clock) into total_run_time — NOT the
     * sum of per-task runtimes. The sum of all tasks' ulRunTimeCounter
     * ≈ wall-clock × num_cores.
     *
     * We compute "busy CPU%" = time spent on non-IDLE tasks:
     *   busy_cpu_pct = sum(non_idle_task_delta) / (wall_delta × num_cores)
     * This correctly represents system load: IDLE high = system free.
     */
    int64_t now_us = esp_timer_get_time();
    uint32_t delta_wall = 0;
    if (_prev_total_run_time > 0 && total_run_time > _prev_total_run_time) {
        delta_wall = total_run_time - _prev_total_run_time;
    }

    /* Sum per-task runtime deltas, separating IDLE from busy tasks */
    uint32_t sum_idle_delta = 0;
    uint32_t sum_task_delta = 0;
    if (delta_wall > 0 && _prev_tasks && _prev_task_count > 0) {
        for (UBaseType_t i = 0; i < actual_count; i++) {
            uint32_t prev_rt = _find_prev_runtime(task_array[i].pcTaskName);
            uint32_t cur_rt = task_array[i].ulRunTimeCounter;
            if (cur_rt > prev_rt) {
                uint32_t delta = cur_rt - prev_rt;
                sum_task_delta += delta;
                if (_is_idle_task(task_array[i].pcTaskName)) {
                    sum_idle_delta += delta;
                }
            }
        }
    }

    uint32_t busy_delta = sum_task_delta - sum_idle_delta;

    if (sum_task_delta > 0 && _prev_timestamp_us > 0 && now_us > _prev_timestamp_us) {
        int64_t delta_us = now_us - _prev_timestamp_us;
        /* Busy CPU% = non-idle runtime / available runtime × 10000
         * Available runtime = wall_delta × num_cores */
        int64_t max_runtime = delta_us * configNUMBER_OF_CORES;
        if (max_runtime > 0) {
            stats.total_cpu_pct = (uint32_t)((uint64_t)busy_delta * 10000 / (uint64_t)max_runtime);
            if (stats.total_cpu_pct > 10000) {
                stats.total_cpu_pct = 10000;
            }
        }
    }
    _prev_total_run_time = total_run_time;
    /* Note: _prev_timestamp_us updated AFTER _fill_top_tasks uses delta_us */

    /* ── 4. Fill top-N tasks by delta CPU% ── */
    int64_t delta_us = (_prev_timestamp_us > 0 && now_us > _prev_timestamp_us)
                        ? (now_us - _prev_timestamp_us) : 0;
    _fill_top_tasks(stats, task_array, actual_count, sum_task_delta, delta_us);

    _prev_timestamp_us = now_us;

    /* ── 5. Store per-task snapshot for next delta (name-matched) ── */
    if (_prev_tasks) {
        free(_prev_tasks);
        _prev_tasks = nullptr;
    }
    _prev_tasks = static_cast<TaskSnapshot *>(
        malloc(sizeof(TaskSnapshot) * actual_count));
    if (_prev_tasks) {
        for (UBaseType_t i = 0; i < actual_count; i++) {
            strncpy(_prev_tasks[i].name, task_array[i].pcTaskName,
                    configMAX_TASK_NAME_LEN - 1);
            _prev_tasks[i].name[configMAX_TASK_NAME_LEN - 1] = '\0';
            _prev_tasks[i].run_time = task_array[i].ulRunTimeCounter;
        }
        _prev_task_count = actual_count;
    }

    free(task_array);

    /* ── 6. Update latest snapshot (mutex-protected) ── */
    if (_latest_mutex && xSemaphoreTake(_latest_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        _latest = stats;
        xSemaphoreGive(_latest_mutex);
    }

    /* ── 7. Publish via uORB ── */
    orb_advert_t pub = _pub.load(std::memory_order_acquire);
    if (pub == ORB_ADVERT_INVALID) {
        orb_advert_t new_pub = orb_advertise(ORB_ID(system_stats));
        orb_advert_t expected = ORB_ADVERT_INVALID;
        if (!_pub.compare_exchange_strong(expected, new_pub, std::memory_order_acq_rel)) {
            /* Another thread beat us — use the existing publisher */
            /* new_pub is leaked but this is a one-time event on a singleton */
        }
    }
    pub = _pub.load(std::memory_order_acquire);
    if (pub >= 0) {
        orb_publish(ORB_ID(system_stats), pub, &stats);
    }

    /* ── 8. Periodic ESP_LOG summary ── */
    _sample_count++;
    if (CONFIG_APP_SYS_MONITOR_LOG_INTERVAL > 0 &&
        (_sample_count % CONFIG_APP_SYS_MONITOR_LOG_INTERVAL) == 0) {
        _log_summary(stats);
    }

    /* ── 9. Check alert thresholds ── */
    _check_alerts(stats);
}

/*============================================================================
 * Top-N task sorting — delta-based CPU%
 *============================================================================*/

bool SystemMonitor::_is_idle_task(const char *name) const
{
    if (!name) return false;
    /* FreeRTOS IDLE tasks: "IDLE" (single-core), "IDLE0", "IDLE1", etc. */
    return strncmp(name, "IDLE", 4) == 0;
}

uint32_t SystemMonitor::_find_prev_runtime(const char *name) const
{
    if (!_prev_tasks || _prev_task_count == 0 || !name) {
        return 0;
    }
    for (UBaseType_t i = 0; i < _prev_task_count; i++) {
        if (strncmp(_prev_tasks[i].name, name, configMAX_TASK_NAME_LEN) == 0) {
            return _prev_tasks[i].run_time;
        }
    }
    return 0;  /* Task not in previous snapshot (newly created) */
}

void SystemMonitor::_fill_top_tasks(system_stats_s &stats,
                                     TaskStatus_t *task_array,
                                     UBaseType_t task_count,
                                     uint32_t sum_task_delta,
                                     int64_t delta_us)
{
    struct TaskCpu {
        const char *name;
        uint32_t cpu_pct;      /* 0-10000 = 0-100.00% */
        uint32_t stack_hwm;    /* bytes */
    };

    const int MAX_SORT = 32;
    TaskCpu cpu_list[MAX_SORT];
    int sort_count = 0;  /* non-IDLE tasks only */

    for (UBaseType_t i = 0; i < task_count && sort_count < MAX_SORT; i++) {
        /* Skip IDLE tasks — they represent free CPU, not load */
        if (_is_idle_task(task_array[i].pcTaskName)) {
            continue;
        }

        cpu_list[sort_count].name = task_array[i].pcTaskName;
        cpu_list[sort_count].stack_hwm = task_array[i].usStackHighWaterMark * sizeof(StackType_t);

        if (delta_us > 0 && sum_task_delta > 0) {
            /* Absolute CPU%: task_delta / (wall_delta × num_cores) × 10000
             * This represents actual CPU utilization (0-100% of one core). */
            uint32_t prev_rt = _find_prev_runtime(task_array[i].pcTaskName);
            uint32_t cur_rt = task_array[i].ulRunTimeCounter;
            if (cur_rt > prev_rt) {
                cpu_list[sort_count].cpu_pct = (uint32_t)(
                    (uint64_t)(cur_rt - prev_rt) * 10000ULL / ((uint64_t)delta_us * configNUMBER_OF_CORES));
            } else {
                cpu_list[sort_count].cpu_pct = 0;
            }
        } else {
            /* First sample — no previous data */
            cpu_list[sort_count].cpu_pct = 0;
        }
        sort_count++;
    }

    /* Sort descending by CPU% (simple selection sort for small N) */
    for (int i = 0; i < sort_count - 1; i++) {
        int max_idx = i;
        for (int j = i + 1; j < sort_count; j++) {
            if (cpu_list[j].cpu_pct > cpu_list[max_idx].cpu_pct) {
                max_idx = j;
            }
        }
        if (max_idx != i) {
            TaskCpu tmp = cpu_list[i];
            cpu_list[i] = cpu_list[max_idx];
            cpu_list[max_idx] = tmp;
        }
    }

    /* Fill top-N into stats struct (always 6 slots in system_stats_s) */
    struct {
        char *name;
        uint32_t *cpu_pct;
        uint32_t *stack_hwm;
    } fields[6] = {
        { stats.task_name_0, &stats.task_cpu_pct_0, &stats.task_stack_hwm_0 },
        { stats.task_name_1, &stats.task_cpu_pct_1, &stats.task_stack_hwm_1 },
        { stats.task_name_2, &stats.task_cpu_pct_2, &stats.task_stack_hwm_2 },
        { stats.task_name_3, &stats.task_cpu_pct_3, &stats.task_stack_hwm_3 },
        { stats.task_name_4, &stats.task_cpu_pct_4, &stats.task_stack_hwm_4 },
        { stats.task_name_5, &stats.task_cpu_pct_5, &stats.task_stack_hwm_5 },
    };

    for (int i = 0; i < TOP_N; i++) {
        if (i < sort_count) {
            strncpy(fields[i].name, cpu_list[i].name ? cpu_list[i].name : "???", 15);
            fields[i].name[15] = '\0';
            *(fields[i].cpu_pct) = cpu_list[i].cpu_pct;
            *(fields[i].stack_hwm) = cpu_list[i].stack_hwm;
        } else {
            memset(fields[i].name, 0, 16);
            *(fields[i].cpu_pct) = 0;
            *(fields[i].stack_hwm) = 0;
        }
    }
}

/*============================================================================
 * Log Summary
 *============================================================================*/
void SystemMonitor::_log_summary(const system_stats_s &stats)
{
    ESP_LOGI(TAG, "───── System Stats ─────");
    uint32_t total_int = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    uint32_t used_pct_int = total_int > 0 ? (uint32_t)((uint64_t)(total_int - stats.free_internal) * 100 / total_int) : 0;
    uint32_t used_pct_psram = total_psram > 0 ? (uint32_t)((uint64_t)(total_psram - stats.free_psram) * 100 / total_psram) : 0;
    ESP_LOGI(TAG, "  Internal SRAM: %u KB free / %u KB total (%u%% used, min %u KB)",
             stats.free_internal / 1024, total_int / 1024,
             used_pct_int, stats.min_free_internal / 1024);
    ESP_LOGI(TAG, "  PSRAM:         %u KB free / %u KB total (%u%% used, min %u KB)",
             stats.free_psram / 1024, total_psram / 1024,
             used_pct_psram, stats.min_free_psram / 1024);
    ESP_LOGI(TAG, "  Tasks: %u   CPU: %u.%02u%%",
             stats.task_count,
             stats.total_cpu_pct / 100,
             stats.total_cpu_pct % 100);

    /* Log top-N tasks (always 6 slots in system_stats_s) */
    struct {
        const char *name;
        uint32_t cpu_pct;
        uint32_t stack_hwm;
    } fields[6] = {
        { stats.task_name_0, stats.task_cpu_pct_0, stats.task_stack_hwm_0 },
        { stats.task_name_1, stats.task_cpu_pct_1, stats.task_stack_hwm_1 },
        { stats.task_name_2, stats.task_cpu_pct_2, stats.task_stack_hwm_2 },
        { stats.task_name_3, stats.task_cpu_pct_3, stats.task_stack_hwm_3 },
        { stats.task_name_4, stats.task_cpu_pct_4, stats.task_stack_hwm_4 },
        { stats.task_name_5, stats.task_cpu_pct_5, stats.task_stack_hwm_5 },
    };

    for (int i = 0; i < TOP_N; i++) {
        if (fields[i].name[0] != '\0') {
            ESP_LOGI(TAG, "  #%d %-15s  CPU %u.%02u%%  stack HWM %u B",
                     i + 1,
                     fields[i].name,
                     fields[i].cpu_pct / 100,
                     fields[i].cpu_pct % 100,
                     fields[i].stack_hwm);
        }
    }
}

/*============================================================================
 * Alert Checking
 *============================================================================*/
void SystemMonitor::_check_alerts(const system_stats_s &stats)
{
    int64_t now_us = esp_timer_get_time();
    int64_t cooldown_us = (int64_t)CONFIG_APP_SYS_MONITOR_ALERT_COOLDOWN_S * 1000000LL;

    /* ── CPU alert ── */
    uint32_t cpu_threshold = (uint32_t)CONFIG_APP_SYS_MONITOR_CPU_ALERT_PCT * 100;  /* 90% → 9000 */
    if (stats.total_cpu_pct >= cpu_threshold) {
        if ((now_us - _last_alert_cpu_us) >= cooldown_us) {
            /* Determine severity: >95% = CRITICAL, else WARNING */
            uint8_t severity = (stats.total_cpu_pct >= cpu_threshold + 500)  /* 95%+ */
                               ? SYS_ALERT_SEVERITY_CRITICAL
                               : SYS_ALERT_SEVERITY_WARNING;

            /* Identify the top CPU-consuming task */
            const char *top_task = stats.task_name_0;
            uint32_t top_cpu = stats.task_cpu_pct_0;

            _publish_alert(SYS_ALERT_CPU_HIGH, severity,
                           stats.total_cpu_pct, cpu_threshold,
                           top_task, top_cpu,
                           stats.free_internal, stats.free_psram);

            _last_alert_cpu_us = now_us;

            ESP_LOGW(TAG, "⚠ CPU ALERT: %u.%02u%% exceeds %d%% threshold (top task: %s %u.%02u%%)",
                     stats.total_cpu_pct / 100, stats.total_cpu_pct % 100,
                     CONFIG_APP_SYS_MONITOR_CPU_ALERT_PCT,
                     top_task, top_cpu / 100, top_cpu % 100);
        }
    }

    /* ── Internal SRAM alert ── */
    uint32_t total_internal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (total_internal > 0) {
        uint32_t used_pct = (uint32_t)((uint64_t)(total_internal - stats.free_internal) * 10000 / total_internal);
        uint32_t mem_threshold = (uint32_t)CONFIG_APP_SYS_MONITOR_MEM_ALERT_PCT * 100;  /* 80% → 8000 */
        if (used_pct >= mem_threshold) {
            if ((now_us - _last_alert_mem_int_us) >= cooldown_us) {
                uint8_t severity = (used_pct >= mem_threshold + 1000)  /* 90%+ */
                                   ? SYS_ALERT_SEVERITY_CRITICAL
                                   : SYS_ALERT_SEVERITY_WARNING;

                _publish_alert(SYS_ALERT_MEM_INTERNAL_HIGH, severity,
                               used_pct, mem_threshold,
                               "", 0,
                               stats.free_internal, stats.free_psram);

                _last_alert_mem_int_us = now_us;

                ESP_LOGW(TAG, "⚠ MEM(internal) ALERT: %u.%02u%% used exceeds %d%% threshold (%u KB free of %u KB)",
                         used_pct / 100, used_pct % 100,
                         CONFIG_APP_SYS_MONITOR_MEM_ALERT_PCT,
                         stats.free_internal / 1024, total_internal / 1024);
            }
        }
    }

    /* ── PSRAM alert ── */
    uint32_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (total_psram > 0) {
        uint32_t used_pct = (uint32_t)((uint64_t)(total_psram - stats.free_psram) * 10000 / total_psram);
        uint32_t mem_threshold = (uint32_t)CONFIG_APP_SYS_MONITOR_MEM_ALERT_PCT * 100;  /* 80% → 8000 */
        if (used_pct >= mem_threshold) {
            if ((now_us - _last_alert_mem_psram_us) >= cooldown_us) {
                uint8_t severity = (used_pct >= mem_threshold + 1000)  /* 90%+ */
                                   ? SYS_ALERT_SEVERITY_CRITICAL
                                   : SYS_ALERT_SEVERITY_WARNING;

                _publish_alert(SYS_ALERT_MEM_PSRAM_HIGH, severity,
                               used_pct, mem_threshold,
                               "", 0,
                               stats.free_internal, stats.free_psram);

                _last_alert_mem_psram_us = now_us;

                ESP_LOGW(TAG, "⚠ MEM(PSRAM) ALERT: %u.%02u%% used exceeds %d%% threshold (%u KB free of %u KB)",
                         used_pct / 100, used_pct % 100,
                         CONFIG_APP_SYS_MONITOR_MEM_ALERT_PCT,
                         stats.free_psram / 1024, total_psram / 1024);
            }
        }
    }
}

void SystemMonitor::_publish_alert(uint8_t alert_type, uint8_t severity,
                                    uint32_t current_value, uint32_t threshold,
                                    const char *task_name, uint32_t task_cpu_pct,
                                    uint32_t free_internal, uint32_t free_psram)
{
    system_alert_s alert = {};
    alert.timestamp = (uint64_t)esp_timer_get_time();
    alert.alert_type = alert_type;
    alert.severity = severity;
    alert.current_value = current_value;
    alert.threshold = threshold;
    if (task_name) {
        strncpy(alert.task_name, task_name, 15);
        alert.task_name[15] = '\0';
    }
    alert.task_cpu_pct = task_cpu_pct;
    alert.free_internal = free_internal;
    alert.free_psram = free_psram;

    /* Update latest alert snapshot (mutex-protected) */
    if (_alert_mutex && xSemaphoreTake(_alert_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        switch (alert_type) {
        case SYS_ALERT_CPU_HIGH:       _alert_cpu = alert; break;
        case SYS_ALERT_MEM_INTERNAL_HIGH: _alert_mem_int = alert; break;
        case SYS_ALERT_MEM_PSRAM_HIGH:   _alert_mem_psram = alert; break;
        default: break;
        }
        xSemaphoreGive(_alert_mutex);
    }

    /* Publish via uORB */
    orb_advert_t pub = _alert_pub.load(std::memory_order_acquire);
    if (pub == ORB_ADVERT_INVALID) {
        orb_advert_t new_pub = orb_advertise(ORB_ID(system_alert));
        orb_advert_t expected = ORB_ADVERT_INVALID;
        _alert_pub.compare_exchange_strong(expected, new_pub, std::memory_order_acq_rel);
    }
    pub = _alert_pub.load(std::memory_order_acquire);
    if (pub >= 0) {
        orb_publish(ORB_ID(system_alert), pub, &alert);
    }
}

/*============================================================================
 * Get latest snapshot (thread-safe)
 *============================================================================*/
system_stats_s SystemMonitor::get_latest(void) const
{
    system_stats_s result = {};
    if (_latest_mutex && xSemaphoreTake(_latest_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        result = _latest;
        xSemaphoreGive(_latest_mutex);
    }
    return result;
}

void SystemMonitor::get_alerts(system_alert_s *cpu_alert,
                                system_alert_s *mem_int_alert,
                                system_alert_s *mem_psram_alert) const
{
    if (!cpu_alert || !mem_int_alert || !mem_psram_alert) return;

    if (_alert_mutex && xSemaphoreTake(_alert_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        *cpu_alert = _alert_cpu;
        *mem_int_alert = _alert_mem_int;
        *mem_psram_alert = _alert_mem_psram;
        xSemaphoreGive(_alert_mutex);
    } else {
        memset(cpu_alert, 0, sizeof(system_alert_s));
        memset(mem_int_alert, 0, sizeof(system_alert_s));
        memset(mem_psram_alert, 0, sizeof(system_alert_s));
    }
}
