/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#ifndef UORB_TOPIC_SYSTEM_STATS_H_
#define UORB_TOPIC_SYSTEM_STATS_H_

#include <cstdint>
#include <cstddef>

#define ORB_QUEUE_LENGTH_SYSTEM_STATS 3

#define SYSTEM_STATS_FORMAT_STR "system_stats:uint64_t timestamp;uint32_t free_internal;uint32_t free_psram;uint32_t min_free_internal;uint32_t min_free_psram;uint32_t task_count;uint32_t total_cpu_pct;char[16] task_name_0;uint32_t task_cpu_pct_0;uint32_t task_stack_hwm_0;char[16] task_name_1;uint32_t task_cpu_pct_1;uint32_t task_stack_hwm_1;char[16] task_name_2;uint32_t task_cpu_pct_2;uint32_t task_stack_hwm_2;char[16] task_name_3;uint32_t task_cpu_pct_3;uint32_t task_stack_hwm_3;char[16] task_name_4;uint32_t task_cpu_pct_4;uint32_t task_stack_hwm_4;char[16] task_name_5;uint32_t task_cpu_pct_5;uint32_t task_stack_hwm_5;"

// NOLINTNEXTLINE(modernize-use-using)
typedef struct system_stats_s
{
    uint64_t                 timestamp;  ///< @brief
    uint32_t                 free_internal;  ///< @brief
    uint32_t                 free_psram;  ///< @brief
    uint32_t                 min_free_internal;  ///< @brief
    uint32_t                 min_free_psram;  ///< @brief
    uint32_t                 task_count;  ///< @brief
    uint32_t                 total_cpu_pct;  ///< @brief
    char                     task_name_0[16];  ///< @brief
    uint32_t                 task_cpu_pct_0;  ///< @brief
    uint32_t                 task_stack_hwm_0;  ///< @brief
    char                     task_name_1[16];  ///< @brief
    uint32_t                 task_cpu_pct_1;  ///< @brief
    uint32_t                 task_stack_hwm_1;  ///< @brief
    char                     task_name_2[16];  ///< @brief
    uint32_t                 task_cpu_pct_2;  ///< @brief
    uint32_t                 task_stack_hwm_2;  ///< @brief
    char                     task_name_3[16];  ///< @brief
    uint32_t                 task_cpu_pct_3;  ///< @brief
    uint32_t                 task_stack_hwm_3;  ///< @brief
    char                     task_name_4[16];  ///< @brief
    uint32_t                 task_cpu_pct_4;  ///< @brief
    uint32_t                 task_stack_hwm_4;  ///< @brief
    char                     task_name_5[16];  ///< @brief
    uint32_t                 task_cpu_pct_5;  ///< @brief
    uint32_t                 task_stack_hwm_5;  ///< @brief
} system_stats_s;

#define SYSTEM_STATS_SIZE sizeof(system_stats_s)

// NOLINTNEXTLINE
static constexpr size_t system_stats_SIZE_CONST { SYSTEM_STATS_SIZE };

#endif /* UORB_TOPIC_SYSTEM_STATS_H_ */
