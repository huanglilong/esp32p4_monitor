/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#include "system_stats.h"
#include <uorb.h>

ORB_TOPIC_DEFINE(system_stats, system_stats_s, 3, "system_stats:uint64_t timestamp;uint32_t free_internal;uint32_t free_psram;uint32_t min_free_internal;uint32_t min_free_psram;uint32_t task_count;uint32_t total_cpu_pct;char[16] task_name_0;uint32_t task_cpu_pct_0;uint32_t task_stack_hwm_0;char[16] task_name_1;uint32_t task_cpu_pct_1;uint32_t task_stack_hwm_1;char[16] task_name_2;uint32_t task_cpu_pct_2;uint32_t task_stack_hwm_2;char[16] task_name_3;uint32_t task_cpu_pct_3;uint32_t task_stack_hwm_3;char[16] task_name_4;uint32_t task_cpu_pct_4;uint32_t task_stack_hwm_4;char[16] task_name_5;uint32_t task_cpu_pct_5;uint32_t task_stack_hwm_5;");
