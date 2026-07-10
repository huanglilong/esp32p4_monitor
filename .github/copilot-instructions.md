# Copilot Instructions — ESP32 Project

> **Framework**: ESP-IDF v6.x | **Language**: C++ (app) + C/C++ (components)
> **Docs**: [README.md](../README.md) (hardware) | [PROJECT.md](../PROJECT.md) (software) | [PROJECT_REQUIREMENTS.md](../PROJECT_REQUIREMENTS.md) (requirements)

---

## 1. Startup Procedure (CRITICAL)

**ALWAYS** at the start of every session:

1. **Read** `README.md` — hardware info
2. **Read** `PROJECT.md` — software architecture, known issues
3. **Read** `PROJECT_REQUIREMENTS.md` — requirements, pending items, change log
4. **Plan** before starting any work
5. **After finishing all tasks**:
   - Update `README.md` / `PROJECT.md` / `PROJECT_REQUIREMENTS.md` as needed
   - Update this file if workflow/conventions change
   - Provide a summary of what was done, why, and remaining TODOs
6. **If the project crashes**: check logs → find root cause → add diagnostic logs if unclear

---

## 2. Project Structure

```
├── main/                    # Application code (C++) — EDIT FREELY
│   ├── drivers/             # Peripheral driver modules
│   ├── generated/           # ⚠️ AUTO-GENERATED — DO NOT EDIT
│   └── compat/              # Third-party compatibility shims
├── proto/                   # uORB .msg definitions
├── tools/                   # Code generators (e.g., msg_gen.py)
├── components/              # ⚠️ Local/BSP components — edit with caution
├── managed_components/      # ⚠️ ESP-IDF managed — DO NOT EDIT
├── sdkconfig.defaults       # Default Kconfig — edit this, NOT sdkconfig
└── partitions.csv           # Partition table
```

---

## 3. Code Conventions

### 3.1 Style

- Follow **Project C/C++ Style** for formatting and naming
- **C++** for `main/`, **C/C++** for `components/`
- C++ designated initializer field order MUST match struct declaration order
- Enum types require explicit casts from int (e.g., `gpio_num_t`)
- C headers included from C++ need `extern "C"` wrapper

### 3.2 Thread Safety (Dual-Core ESP32)

- Use `std::atomic<T>` for ALL cross-core/cross-task shared variables — **never** `volatile`
- Use `std::atomic<bool>` with `.store(release)` / `.load(acquire)` for flags
- Use `compare_exchange_strong()` for lazy one-time init of shared handles
- Use FreeRTOS Mutex for multi-step critical sections
- Use atomic counter (`_ops_in_flight`) to wait for in-flight ops before deinit
- Use atomic flag (`_task_exited`) for safe task cleanup — avoid `eTaskGetState()` on self-deleting tasks
- Let tasks self-delete after releasing resources — never `vTaskDelete()` on tasks holding mutexes
- Large-stack tasks (>4KB): use `xTaskCreateStatic` with PSRAM-allocated stack

### 3.3 Error Handling

- **Never** use `assert()` for runtime checks — compiled out in release builds
- Use explicit null checks + `ESP_LOGE` + graceful degradation
- **Always** check return values of ESP-IDF API calls
- **Always** validate array indices from hardware/driver responses
- **Always** validate `Content-Length` before `calloc()` in HTTP handlers
- **Always** sanitize file paths in HTTP handlers — prevent path traversal
- **Never** expose secrets in HTTP responses — use `has_*` boolean flags

### 3.4 Memory

- Use `new(std::nothrow)` — ESP-IDF disables C++ exceptions, `bad_alloc` calls `abort()`
- Use `heap_caps_free()` for `heap_caps_malloc`/`heap_caps_realloc` allocations
- Use `cJSON_free()` for `cJSON_Print*` results
- Use `esp_timer_get_time()` (monotonic) for durations — `gettimeofday()` can overflow

---

## 4. Build, Flash & Monitor

```bash
source ~/.espressif/v6.x/esp-idf/export.sh      # Setup environment
idf.py build                                    # Build
idf.py fullclean                                # Full clean (only when config changed)
```

**macOS**: `idf.py flash -b 1500000 -p $(ls /dev/cu.usbmodem*) monitor`
**Linux**: `idf.py flash -b 1500000 -p /dev/ttyACM0 monitor`

After code changes, rebuild. If `sdkconfig.defaults` changed, `fullclean` first.

**Flutter app** (`flutter_app/`): if any code under `flutter_app/lib/` changes, verify with:
```bash
cd flutter_app && flutter build macos
```
This must pass before committing Flutter app changes.

---

## 5. Protected Files

| Path | Rule | Reason |
|------|------|--------|
| `managed_components/` | **DO NOT EDIT** | ESP-IDF managed, overwritten on build |
| `main/generated/` | **DO NOT EDIT** | Auto-generated from `proto/*.msg` |
| `sdkconfig` | **DO NOT EDIT** | Generated from `sdkconfig.defaults` |
| `components/` | **Edit with caution** | Only if necessary and approved |

---

## 6. Git Rules

1. **Every commit must be approved by user**
2. **git push is forbidden** — never push without explicit request
3. **One issue, one commit** — each commit addresses exactly ONE issue/feature/bugfix
4. **Commit messages**: clear, concise, in English, with **root cause and summary of what was done and why**
5. **Before committing**: verify build passes (`idf.py build` for firmware, `cd flutter_app && flutter build macos` for Flutter app)

---

## 7. Code Review & Fix Workflow

1. Review for issues and improvements
2. **One issue, one commit** — fix each in its own commit
3. After fixing, run another review; repeat up to **2 rounds max**
4. If issues remain after 2 rounds, report to user
5. Update all relevant `*.md` docs
6. Flash to device and monitor logs to verify

---

## 8. Reference Projects

| Project | Reference |
|---------|-----------|
| [PX4-Autopilot](https://github.com/PX4/PX4-Autopilot) | uORB, ULog, architecture |
| [ESP-Claw](https://github.com/espressif/esp-claw) | AI agent, IM, LLM, MCP |

When modifying `components/` code, follow its existing coding style and architecture.

---

## 9. Common Pitfalls

| Pitfall | Correct Approach |
|---------|-----------------|
| `volatile` for cross-core vars | `std::atomic<T>` with explicit memory ordering |
| Bare `new` | `new(std::nothrow)` + null check |
| `free()` for `heap_caps_*` allocs | `heap_caps_free()` |
| `free()` for cJSON strings | `cJSON_free()` |
| `assert()` for runtime checks | Explicit null/error checks + `ESP_LOGE` |
| `eTaskGetState()` on self-deleting tasks | `_task_exited` atomic flag |
| `vTaskDelete()` on mutex-holding tasks | Let task self-delete after release |
| `gettimeofday()` for durations | `esp_timer_get_time()` (monotonic) |
| Editing `sdkconfig` | Edit `sdkconfig.defaults` |
| Editing `main/generated/` | Edit `proto/*.msg` + regenerate |
| Unchecked HTTP `Content-Length` | Cap before `calloc()` |
| Exposing secrets in HTTP | Use `has_*` boolean flags |
