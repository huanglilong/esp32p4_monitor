# System performance statistics
uint64  timestamp
uint32  free_internal        # Free internal SRAM (bytes)
uint32  free_psram           # Free PSRAM (bytes)
uint32  min_free_internal    # Historical minimum free internal SRAM (bytes)
uint32  min_free_psram       # Historical minimum free PSRAM (bytes)
uint32  task_count           # Current number of FreeRTOS tasks
uint32  total_cpu_pct        # Total CPU utilization (0-10000 = 0-100.00%)

# Top-6 task CPU/stack snapshot (sorted by CPU usage descending)
char[16] task_name_0         # Task name (0)
uint32  task_cpu_pct_0       # CPU% (0-10000 = 0-100.00%)
uint32  task_stack_hwm_0     # Stack high-water mark (bytes)
char[16] task_name_1         # Task name (1)
uint32  task_cpu_pct_1       # CPU% (0-10000 = 0-100.00%)
uint32  task_stack_hwm_1     # Stack high-water mark (bytes)
char[16] task_name_2         # Task name (2)
uint32  task_cpu_pct_2       # CPU% (0-10000 = 0-100.00%)
uint32  task_stack_hwm_2     # Stack high-water mark (bytes)
char[16] task_name_3         # Task name (3)
uint32  task_cpu_pct_3       # CPU% (0-10000 = 0-100.00%)
uint32  task_stack_hwm_3     # Stack high-water mark (bytes)
char[16] task_name_4         # Task name (4)
uint32  task_cpu_pct_4       # CPU% (0-10000 = 0-100.00%)
uint32  task_stack_hwm_4     # Stack high-water mark (bytes)
char[16] task_name_5         # Task name (5)
uint32  task_cpu_pct_5       # CPU% (0-10000 = 0-100.00%)
uint32  task_stack_hwm_5     # Stack high-water mark (bytes)

uint8 ORB_QUEUE_LENGTH = 3
