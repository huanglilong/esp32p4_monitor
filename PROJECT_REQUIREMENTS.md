# ESP32-P4 Monitor — 项目需求文档

> 生成日期: 2026-07-02 | 基于 PROJECT.md、project_design.md 及历史会话分析
> 代码仓库: [gitee.com/huanglilong/esp32p4_monitor](https://gitee.com/huanglilong/esp32p4_monitor)
>
> 本文档记录项目需求、用户想法、已知限制和未来计划。随着项目迭代持续更新。

---

## 1. 项目愿景

基于 Waveshare ESP32-P4-WiFi6-Touch-LCD-4B 开发板构建一个多功能监控终端，集成摄像头预览、音频录制/播放、WiFi 推流和系统设置，以单一固件适配多款开发板（LCD-4B / WIFI6 基板）。

---

## 2. 已完成需求

### 2.1 核心功能

| # | 需求 | 说明 | 状态 |
|---|------|------|:----:|
| R1 | **MIPI DSI 显示** | ST7703 720×720 LCD，LVGL v9.2.2 + ESP-Brookesia Phone UI | ✅ |
| R2 | **MIPI CSI 摄像头** | OV5647 RAW8 800×800，ISP RAW8→RGB565 实时预览 (~10fps) | ✅ |
| R3 | **Camera 人体检测** | ESP-DL + YOLO11n 320×320, ~1.8fps，绿色检测框 + 置信度标签 | ✅ |
| R4 | **Camera V4L2 迁移** | 统一 esp_video 接口，与 Camera Stream 共享 | ✅ |
| R5 | **Camera 红绿通道修正** | Bayer GBRG + byte_swap_en=1 修复颜色错误 | ✅ |
| R6 | **Camera Stream WiFi 推流** | HW JPEG MJPEG 流 (port 81) + Web UI (port 80), mDNS 发现 | ✅ |
| R7 | **Camera Stream 独立 App** | 从 Settings App 分离，独立开关，含 CPU/PSRAM 监控 | ✅ |
| R8 | **Audio 双 Mic 监控** | I2S RX 48kHz Stereo 直接读取，双声道电平表 (0–100%) | ✅ |
| R9 | **Audio MP3 录音** | Shine 定点编码器 128kbps stereo → SD 卡，录制/停止/文件列表 | ✅ |
| R10 | **Music MP3/WAV 播放器** | ESP-GMF 音频管道，SD 卡音源，音量滑条 | ✅ |
| R11 | **Settings 音量/亮度** | LVGL Slider (0–100 / 20–100), NVS 持久化，500ms 去抖写入 | ✅ |
| R12 | **Settings WiFi 管理** | SSID 扫描 + 密码输入 + 连接，后台保持，条件编译 | ✅ |
| R13 | **Web 配置服务器** | HTTP :8080，WiFi/音量远程设置，connect-before-save 验证 | ✅ |
| R14 | **多板自动检测** | GT911 I2C (0x5D) 探测，自动适配 LCD-4B / WIFI6，单一固件 | ✅ |
| R15 | **Web 音频录制** | WIFI6 无屏板通过 Web :8080 录制，Shine MP3 → SD 卡，Start/End 按钮 | ✅ |
| R16 | **Web 音频播放** | WIFI6 无屏板通过 Web :8080 播放 SD 卡 MP3，esp_audio_simple_player | ✅ |
| R17 | **Camera Stream 互斥保护** | Web 音频功能仅在 Camera Stream 未运行时可用，UI 隐藏 + API 阻断 | ✅ |

### 2.2 稳定性与性能

| # | 需求 | 说明 | 状态 |
|---|------|------|:----:|
| S1 | **esp_hosted 稳定性** | 固定 v2.12.7 + RX_STREAMING + Q_SIZE=20 + PSRAM mempool | ✅ |
| S2 | **SD/音频延迟初始化** | 按需 init/deinit + 引用计数，减少空闲 DMA 负载 | ✅ |
| S3 | **LVGL 线程安全** | Music (GMF cb + lvgl_port_lock), Settings (wifi task + bsp_display_lock) | ✅ |
| S4 | **检测框坐标修复** | COCODetect 内部缩放 → 移除手动 SCALE，修正双重缩放 | ✅ |
| S5 | **I2S MCLK 匹配** | 384×→256×，与 ES8311/ES7210 codec 一致 | ✅ |
| S6 | **跨核 volatile 保护** | `_wifi_scanning`, `_running`, `_volume`, `_detect_available` 加 volatile | ✅ |
| S7 | **V4L2 双开防护** | Camera App 初始化前检查 CameraStream::instance().isRunning() | ✅ |
| S8 | **全局 codec 互斥锁** | `s_codec_mutex` 保护 Settings/Music 并发 `set_out_vol()` | ✅ |
| S9 | **WiFi task/Event Group 清理** | close() + WiFi OFF 时 vTaskDelete + vEventGroupDelete | ✅ |
| S10 | **资源泄漏修复** | CSI/ISP 释放顺序, SD LDO handle, Camera buffer 错误路径 | ✅ |
| S11 | **Task 栈安全** | Audio task 4KB→12KB, detection task 16KB, 消除溢出风险 | ✅ |
| S12 | **NVS 去抖写入** | Settings/Music 音量滑块 500ms debounce timer | ✅ |
| S13 | **WiFi deinit 完善** | WiFi OFF 时 `esp_wifi_stop()`, 避免一次性 init 函数重复调用 | ✅ |
| S14 | **字体精简** | 禁用 10 个未使用字体，回收 ~560KB Flash | ✅ |
| S15 | **Web 安全** | 密码不再明文返回 (has_pass)，JSON 用 cJSON 安全构造 | ✅ |
| S16 | **CORS 预检** | Web Config Server 添加 OPTIONS 处理器 | ✅ |
| S17 | **WiFi 扫描降频** | 空闲轮询 200ms→500ms，节省 CPU | ✅ |
| S18 | **内存日志优化** | Main loop 改为 60s delay，移除 5s 日志 | ✅ |
| S19 | **Music 音量同步降频** | NVS 同步 timer 1s→5s | ✅ |
| S20 | **不兼容 Kconfig 清理** | 移除 sdkconfig.defaults 中 4 个未知符号 | ✅ |
| S21 | **-Wno-attributes 作用域** | PUBLIC→PRIVATE，仅对 LVGL 自身生效 | ✅ |
| S22 | **shine_encoder 头文件** | `<malloc.h>`→`<stdlib.h>`，跨平台兼容 | ✅ |
| S23 | **strdup null 检查** | Audio/Music 文件列表 OOM 保护 | ✅ |
| S24 | **V4L2 cache 一致性** | DQBUF 后 + JPEG 编码后 esp_cache_msync | ✅ |
| S25 | **Music GMF 重入修复** | `_asp_event_cb` 不再直接调 `_play()`，改用 `_auto_next` flag | ✅ |
| S26 | **Audio 录音停止竞态** | 等待时间 50ms→200ms，覆盖 I2S read 100ms 超时 | ✅ |
| S27 | **PPA 硬件启用** | `CONFIG_LVGL_PORT_ENABLE_PPA=y`，加速旋转/缩放 | ✅ |
| S28 | **Web Audio 懒加载** | SD 卡 + 音频首次 API 调用时初始化，减少空闲资源占用 | ✅ |
| S29 | **Web Audio URL 解码** | `_url_decode()` 处理 `encodeURIComponent` 产生的 `%XX` 序列 | ✅ |
| S30 | **Web Audio JS 语法修复** | `loadFiles` 改用 template literal 避免单引号冲突，修复整个 script 被丢弃 | ✅ |
| S31 | **Web Playback ASP 生命周期** | 每次 play 重建 ASP handle (stop→destroy→new→run)，防止管道状态残留崩溃 | ✅ |
| S32 | **Web Record 音频任务停止** | `h_rec_stop` 同时设 `s_audio_running=false`，释放 I2S RX 给 playback | ✅ |
| S33 | **Web 音频 API 全端点 guard** | 6 个端点均加 `__cam_running()` 检查 (list/status/stop 新增) | ✅ |
| S34 | **`max_uri_handlers`** | 11→16，容纳 14 个 handler (5 core + 6 audio + 3 CORS) | ✅ |
| S35 | **WIFI6 音频修复** | 修正 I2C 地址 0x18→0x30 (ES8311_CODEC_DEFAULT_ADDR)，单 handle (BOTH+IN_OUT) 匹配参考固件 | ✅ |
| S36 | **Music App ASP 生命周期同步** | `_play()` 改为 stop→destroy→new→run，匹配 web S31，防止 GMF 缓存状态崩溃 | ✅ |
| S37 | **Flutter Settings 页面** | 集成 WiFi/音量/Camera Stream 配置 + 录音/播放 + 恢复出厂设置 | ✅ |
| S38 | **CameraStream cross-core volatile** | `_detector` + `_frame_count` + `_fps_total_bytes` 加 volatile 防止跨核编译器缓存 | ✅ |
| S39 | **SD LDO re-acquire 检查** | `esp_ldo_acquire_channel` 返回值检查，防止脏句柄 | ✅ |
| S40 | **web_config_server 线程安全** | `localtime()`→`localtime_r()` + `vTaskDelete` 自删除保护 | ✅ |
| S41 | **Flutter macOS 端口 8080 连接修复** | HttpClient→Socket 切换 + Info.plist 补全 NSLocalNetworkUsageDescription | ✅ |
| S42 | **uORB 消息总线** | 引入仿 PX4 uORB 的 pub/sub 机制，FreeRTOS Queue 实现，零额外依赖 | ✅ |
| S43 | **uORB .msg 自动生成** | `proto/*.msg` → `tools/msg_gen.py` → `main/generated/`，`idf.py build` 自动触发 | ✅ |
| S44 | **gettimeofday overflow 修复** | 录音计时改用 `esp_timer_get_time()` (monotonic)，避免 tv_sec*1000 溢出 uint32_t | ✅ |
| S45 | **Music _stop() ASP handle 泄漏** | `_stop()` 增加 `esp_audio_simple_player_destroy()`，匹配 S31/S36 stop→destroy 模式 | ✅ |
| S46 | **SD LDO acquire 检查** | 首次 `esp_ldo_acquire_channel` 返回值检查，防止无效 handle 传入 release | ✅ |
| S47 | **web_config_server task 干净退出** | `s_running` volatile flag + 任务自行清理 HTTP/mDNS 后自删，替代 vTaskDelete 强杀 | ✅ |
| S48 | **PeripheralManager facade** | 提取 `peripherals.hpp/cpp` 单例，统一外设 init/deinit/refcount/mutex，消除所有 extern 全局变量 | ✅ |
| S49 | **Driver 模块拆分** | PeripheralManager 重构为 thin facade，拆分 AudioDriver (I2S+codec+volume uORB)、SDCardDriver (SDSPI+LDO)、CameraDriver (camera_state pub/sub+claim/release)。CameraStream/PhoneAppCamera 改用 CameraDriver::claim()/release() | ✅ |
| S50 | **CameraStream start/stop 竞态修复** | `_running` 提前设置防并发 start，claim() 在 `_running=true` 之前执行，stop() 在资源释放后才清 `_running` | ✅ |
| S51 | **CameraDriver 互斥保护** | 添加 `_mutex` 保护 claim/release/available 操作，消除 const_cast 竞态和 TOCTOU 间隙 | ✅ |
| S52 | **uORB publish 锁优化** | `orb_publish()` 仅在注册表查找时持锁，队列操作无锁执行，降低高频率 topic 发布延迟 | ✅ |
| S53 | **AudioDriver publisher 线程安全** | `set_volume()` 的 `orb_advert_t` 从函数局部 static 改为类成员 `_vol_pub`，消除并发初始化竞态 | ✅ |
| S54 | **PhoneAppCamera 帧缓冲同步** | LVGL timer 和 detection task 通过 `_detect_mutex` 同步 `_cam_buffer` 访问，消除画面撕裂 | ✅ |
| S55 | **CameraStream 检测结果互斥** | `_detect_results` 添加 `_detect_mutex` 保护，防止 stream handler 和 HTTP API 并发访问 | ✅ |
| S56 | **Web 音频操作互斥** | `web_config_server` 添加 `s_audio_mutex` 序列化音频 HTTP handler，防止并发客户端损坏音频状态 | ✅ |
| S57 | **audio_lock 竞态修复** | `s_audio_mutex` 在 task 启动时创建（不再懒初始化），防止并发 handler 看到 NULL 各创建独立 mutex | ✅ |
| S58 | **Camera claim TOCTOU 修复** | PhoneAppCamera `_init_camera()` 在 V4L2 操作前先 claim()，消除与 CameraStream 的 TOCTOU 竞态 | ✅ |
| S59 | **uORB 订阅者槽位复用** | `orb_unsubscribe()` 将释放的槽位加入 free-list，`orb_subscribe()` 优先复用，防止重复订阅/取消耗尽表 | ✅ |
| S60 | **NVS RAM 缓存** | web_config_server 添加 NVS 整数键 RAM 缓存层（读 O(1)，写穿透），减少每次 API 调用 ~1ms flash 访问 | ✅ |
| S61 | **mDNS 双初始化防护** | `shared_mdns_ensure()` 共享标志防止 CameraStream 和 web_config_server 双重 mdns_init() | ✅ |
| S62 | **Web audio task I2S null guard** | audio_task 在 `i2s_channel_read` 前检查 `rx_handle()` 非 null，防止 AudioDriver deinit 后崩溃 | ✅ |
| S63 | **CameraStream HTTP URI 精确匹配** | 移除 `httpd_uri_match_wildcard`，改用默认精确匹配（更快更安全） | ✅ |
| S64 | **PhoneAppCameraStream uORB unsubscribe** | close() 时 `orb_unsubscribe(s_fps_sub)`，防止永久占用订阅者槽位 | ✅ |
| S65 | **URL decode hex 验证** | `_hex_digit()` + `_url_decode()` 验证 %XX 后为合法 hex 字符，非法字符原样保留 | ✅ |
| S66 | **NVS 批量写入** | `_nvs_save_timer_cb` 合并 volume + brightness 在同一 nvs_open/close 会话写入 | ✅ |
| S67 | **CameraStream VFS 设备泄漏修复** | `_deinit_video()` 和 `_init_video()` fail 路径缺少 `example_video_deinit()`，导致 "video20" VFS 设备未注销，切换 App 后无法重新初始化 CSI/ISP pipeline | ✅ |
| S68 | **mDNS 双主机名** | 主主机名 `esp-web-XXXXXX`（MAC后3字节）确保 SRV 记录唯一稳定，委托主机名 `esp-web` 保留单设备便捷性 | ✅ |
| S69 | **ULog SD 卡日志** | 集成 PX4 ULog 文件格式，uORB topics 经 ring buffer 写入 SD 卡 `.ulog` 文件，兼容 Flight Review/PlotJuggler 分析；Web API 控制启停；自动文件轮转 | ✅ |
| S70 | **ULog format string 自动生成** | `msg_gen.py` 扩展支持，每个 topic 自动生成 ULog format 字符串（如 `"fps_stats:uint64_t timestamp;uint32_t frame_count;float fps;"`），嵌入 `orb_metadata_t::o_format` | ✅ |
| S71 | **ulog_state uORB topic** | 新增 `proto/ulog_state.msg`，记录日志状态（logging/filepath/bytes_written），供 UI/Web 查询 | ✅ |
| S72 | **mDNS 引用计数** | `shared_mdns_ensure/release()` 引用计数替代 `mdns_free()` 直接调用，CameraStream 停止不再破坏 web_config_server 的 mDNS | ✅ |
| S73 | **mDNS 初始化互斥** | `shared_mdns_ensure()` 加 mutex 防止并发调用双重初始化 | ✅ |
| S74 | **eTaskGetState 竞态修复** | 所有自删除任务改用 `_task_handle == nullptr` 检测（任务退出前清 handle），替代 `eTaskGetState()` 避免与 idle task 回收 TCB 竞态 | ✅ |
| S75 | **PhoneAppCamera 帧撕裂修复** | `_detect_mutex` 获取失败时真正跳过 memcpy，而非注释说跳过实际仍复制 | ✅ |
| S76 | **CameraStream 原子启动** | `_running` 改为 `std::atomic<bool>` + `compare_exchange_strong()` 防止并发 `start()` 双重初始化 | ✅ |
| S77 | **COCODetect nothrow** | `PhoneAppCamera` 的 `new COCODetect` 改为 `new(std::nothrow)` 防止 OOM 时 `std::bad_alloc` 崩溃 | ✅ |
| S78 | **cJSON_Print NULL 检查** | CameraStream 两个 JSON handler 添加 `cJSON_Print` 返回 NULL 检查，防止 `httpd_resp_sendstr(NULL)` 崩溃 | ✅ |
| S79 | **AudioDriver volume 线程安全** | `_volume = volume` 移入 `_codec_mutex` 保护区间内，消除跨核读写竞态 | ✅ |
| S80 | **CameraStream detector atomic** | `_detector` 从 `volatile COCODetect*` 改为 `std::atomic<COCODetect*>`，`_model_ready` 改为 `std::atomic<bool>`，提供正确的内存序保证 | ✅ |
| S81 | **example_video_init VFS 泄漏修复** | `esp_video_init()` 失败时强制调用 `esp_video_deinit()` 清理部分注册的 VFS 设备（如 video20），防止后续 `example_video_init()` 永远失败 | ✅ |
| S82 | **main.cpp nothrow 分配** | 所有 `new` 分配改为 `new(std::nothrow)` 防止 OOM 时 `std::bad_alloc` 崩溃（ESP-IDF 禁用 C++ 异常） | ✅ |
| S83 | **example_video_init VFS 强制清理重试** | `esp_video_init()` 失败时用 `esp_vfs_unregister()` 强制注销残留 `video20`，然后重试一次，修复因前次会话残留导致的永久失败 | ✅ |
| S84 | **Flutter 设备列表排序** | 新扫描设备排在前面，历史（已保存）设备排在后面，分区显示 "Scanned Devices" / "History" 标题 | ✅ |
| S85 | **Flutter 设备可达性状态** | 设备卡片显示状态徽章：Connected(绿) / Reachable(蓝) / Offline(橙) / History(灰)，TCP 端口 80/8080 探测 | ✅ |

---

## 3. 待完成需求

### 3.1 高优先级

| # | 需求 | 说明 | 阻塞因素 |
|---|------|------|----------|
| P1 | **720×720 自定义样式表** | 当前使用默认回退方案，UI 一致性不佳 | 需设计 ESP-Brookesia 样式 |
| P2 | **WIFI6 无屏配网** | 首次启动 NVS 为空时需要配网方案 | esp-hosted SDIO 不支持稳定 SoftAP (#197) |

### 3.2 中优先级

| # | 需求 | 说明 |
|---|------|------|
| P3 | **Camera App 录像功能** | 视频录制到 SD 卡 |
| P4 | **Camera 检测框平滑** | EMA 或 Kalman filter 减少检测框抖动 |
| P5 | **ROI 区域检测** | 只检测画面中心区域，减少误报 |

### 3.3 低优先级 / 长期优化

| # | 需求 | 说明 |
|---|------|------|
| P6 | **双缓冲 Camera 帧** | ISP DMA 交替写入两个 buffer，消除 detect memcpy |
| P7 | **SD 卡访问协调器** | 统一管理 Camera/Music/Audio 对 SD 卡的并发访问 |
| P8 | **Camera Canvas 800→720** | Canvas 裁剪到 720×720，节省 21% 像素处理量 |
| P9 | **I2S 按需启停** | Audio App 打开时启用 RX，Music 播放时启用 TX |
| P10 | **WiFi 管理重构** | WiFi 功能解耦为独立服务模块 |
| P11 | **IPC 迁移至 uORB** | 逐步将现有的 volatile/mutex/event_group IPC 替换为 uORB topic |

---

## 4. 已知限制与风险

### 4.1 硬件限制

| # | 限制 | 说明 |
|---|------|------|
| H1 | **SDSPI 1-bit 模式** | SDMMC Slot 0 被 C6 SDIO 占用，SD 卡仅 1-bit SPI 20MHz |
| H2 | **MIPI CSI 与 SDMMC 无物理冲突** | 已勘误：CSI (pin 42-48，专用引脚) 与 SD (pin 80-86，GPIO) 完全独立 |
| H3 | **DMA 竞争** | MIPI CSI ISP pipeline + SDSPI + SDIO WiFi 共享 DMA/PSRAM 带宽 |
| H4 | **WiFi 依赖 C6 SDIO** | P4 无内置 WiFi，C6 SDIO 高负载下可能死锁 |

### 4.2 软件限制

| # | 限制 | 说明 |
|---|------|------|
| L1 | **esp-hosted v2.12.7 锁定** | v2.12.8+ 在 Waveshare 硬件上验证更快死锁 (#197) |
| L2 | **无 SoftAP 配网** | SDIO 缓冲区溢出导致 C6 崩溃，首次启动需预置 WiFi 凭据 |
| L3 | **Camera 检测帧率 ~1.8fps** | YOLO11n NPU 推理 ~560ms/帧 |
| L4 | **Camera Stream 帧率 ~6.9fps** | 受限于 sensor VTS=4920 (~10fps) + HW JPEG 编码 |
| L5 | **720×720 无预设样式表** | ESP-Brookesia 默认回退方案 |
| L6 | **PPA client 注册未使用** | 307KB PSRAM 占用，可优化 |
| L7 | **esp-dl mbedtls/sha256.h 兼容 shim** | ESP-IDF v6.x (mbedtls 4.x) 将 `sha256.h` 移至 `mbedtls/private/`，`managed_components/espressif__esp-dl` 未更新。当前方案: `main/compat/mbedtls/sha256.h` 转发头 + CMake include path。下次升级 esp-dl 后应移除 |

### 4.3 已知未修复问题（来自 project_design.md 分析）

| # | 严重度 | 问题 | 原因 |
|---|--------|------|------|
| K1 | 🟢 低 | Camera 帧缓冲跨核并发 (timer core1 / detect core0) 画面撕裂 | 非关键，影响单帧 |
| K2 | 🟢 低 | Audio 无用的 PSRAM 分配 (1920B) | 开销极小 |
| K3 | 🟢 低 | Music 部分低优先级边界情况 | 见 project_design.md §4.10 |
| K4 | ✅ 已修复 | **Web 音频 Camera Stream 互斥已生效** | 所有 6 个端点均已加 `__cam_running()` 检查 (2026-07-02) |

---

## 5. 技术约束

### 5.1 不可修改项

- `managed_components/` — ESP-IDF 管理，会被覆盖
- `components/` — BSP 组件，谨慎修改
- `sdkconfig` — 由 sdkconfig.defaults 生成，不直接编辑

### 5.2 编译约束

- C++ 指定初始化器顺序必须与结构体声明一致
- `gpio_num_t` / `i2s_mclk_multiple_t` 需要显式类型转换
- ESL 头文件需 `extern "C"` 包裹
- MIPI DSI/CSI 使用专用接口引脚，不是 GPIO

### 5.3 依赖版本锁定

| 组件 | 版本 | 锁定原因 |
|------|------|----------|
| `espressif/esp_hosted` | 2.12.7 | v2.12.8+ 在 Waveshare 上验证死锁 (#197) |
| `espressif/esp-brookesia` | 0.5.0 | 当前功能稳定 |
| `espressif/esp_lvgl_port` | 2.8.0~1 (本地补丁) | 修复 LVGL 9.2.2 兼容性 |
| `lvgl/lvgl` | 9.2.2 | ESP-Brookesia 依赖 |

---

## 6. 用户界面需求

### 6.1 App 列表

| App | 图标 | 功能 | 入口条件 |
|-----|:----:|------|----------|
| Camera | 📷 | OV5647 预览 + 人体检测 | LCD-4B (有屏幕) |
| Audio | 🎤 | 双 Mic 电平 + MP3 录音 | LCD-4B + Audio codec |
| Music | 🎵 | MP3/WAV 播放 | LCD-4B + Audio codec |
| Camera Stream | 🌐 | MJPEG WiFi 推流 | LCD-4B + WiFi |
| Settings | ⚙️ | 音量/亮度 + WiFi | LCD-4B |
| Squareline | 🎨 | 内置示例 | LCD-4B |

### 6.2 非 UI 功能

| 功能 | 访问方式 | 适用板子 |
|------|----------|----------|
| Web 配置 (:8080) | 浏览器 | LCD-4B + WIFI6 |
| Web 音频录制 (:8080) | 浏览器 Audio Recorder card | WIFI6 (Camera Stream OFF 时可用) |
| Web 音频播放 (:8080) | 浏览器 Audio Recorder card | WIFI6 (Camera Stream OFF 时可用) |

---

## 7. 变更记录

| 日期 | 变更 |
|------|------|
| 2026-07-04 | +S84 S85 Flutter 设备列表排序 (新扫描优先/历史在后) + 设备可达性状态徽章 (Connected/Reachable/Offline/History) |
| 2026-07-03 | +S68 mDNS 双主机名: 保留 `esp-web` 便捷名 + 新增 `esp-web-XXXXXX` 委托主机名（MAC后3字节），单设备零配置 + 多设备精确定位 |
| 2026-07-03 | +S50~S56 线程安全与性能修复: CameraStream 竞态, CameraDriver 互斥, uORB publish 锁优化, AudioDriver publisher 线程安全, PhoneAppCamera 帧缓冲同步, CameraStream 检测结果互斥, Web 音频操作互斥 |
| 2026-07-03 | +S57 **CameraStream VFS 设备泄漏修复**: `_deinit_video()` 未调用 `example_video_deinit()` 导致 "video20" 设备未注销；`_init_video()` 早返 / fail 路径同样缺少；`goto fail` 跨初始化编译错误已修复 |
| 2026-07-03 | +S49 Driver 模块拆分: PeripheralManager→thin facade + AudioDriver + SDCardDriver + CameraDriver (claim/release API) |
| 2026-07-03 | +S44~S48 Bug 修复 (gettimeofday overflow, Music _stop ASP 泄漏, SD LDO 检查, web task 干净退出) +PeripheralManager facade 模块化重构 (消除 extern 全局变量) |
| 2026-07-03 | +S42 uORB 消息总线（FreeRTOS Queue 实现） +S43 .msg 自动生成 pipeline +P11 IPC 迁移计划 |
| 2026-07-02 | 初始创建，汇总 PROJECT.md + project_design.md 中所有需求和问题 |
| 2026-07-02 | +R15 R16 R17 Web 音频录制/播放 + Camera Stream 互斥需求 |
| 2026-07-02 | +S28~S34 Web 音频稳定性修复 (懒加载、URL解码、JS修复、ASP生命周期等) |
| 2026-07-02 | +S35 WIFI6 音频修复: I2C地址修正 0x18→0x30, 单handle (BOTH+IN_OUT) 匹配参考固件 |
| 2026-07-02 | +S36 Music App ASP 生命周期同步, 匹配 web S31 |
| 2026-07-02 | +S37 Flutter Settings 页面: WiFi/音量/Camera Stream + 录音/播放 |
| 2026-07-02 | +K4 Web 音频 Camera Stream 互斥未生效 (诊断中，已加 noinline + debug log) |
| 2026-07-02 | **修复**: K4/R17 全部 6 个 Web 音频端点均加 `__cam_running()` 检查；CameraStream `_detector`/`_frame_count`/`_fps_total_bytes` 加 volatile；SD LDO re-acquire 返回值检查；`localtime()`→`localtime_r()`；`vTaskDelete` 自删除防护 |
