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
| S2 | **SD init-once + SDSPI 统一** | boot 时挂载永不下电, 两板统一 SDSPI, sd_pwr_ctrl + _has_lcd 区分, VFS_MAX_COUNT=16 | ✅ |
| S2b | **SD 防御检查** | Audio/Music App SD 文件操作前调用 init_sdcard() 确保挂载 | ✅ |
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
| S81 | **example_video_init VFS 泄漏修复** | `esp_video_init()` 失败时强制调用 `esp_video_deinit()` + 双重清理 `/dev/video0`(CSI) + `/dev/video20`(ISP1) + 诊断日志 + 100ms 延迟重试 | ✅ |
| S82 | **main.cpp nothrow 分配** | 所有 `new` 分配改为 `new(std::nothrow)` 防止 OOM 时 `std::bad_alloc` 崩溃（ESP-IDF 禁用 C++ 异常） | ✅ |
| S83 | **Camera Stream stop 死锁修复** | `_running=false` 移到 `httpd_stop()` 之前，MJPEG stream handler 先退出 `while(isRunning())` 循环，避免 `httpd_stop()` 永久等待活跃连接 | ✅ |
| S84 | **VFS_MAX_COUNT 扩容** | 8→16 (sdkconfig.defaults)，SD 常驻挂载占用 1 槽位 + Camera ISP/CSI 需要 2 槽位，原 8 不足 | ✅ |
| S84 | **Flutter 设备列表排序** | 新扫描设备排在前面，历史（已保存）设备排在后面，分区显示 "Scanned Devices" / "History" 标题 | ✅ |
| S85 | **Flutter 设备可达性状态** | 设备卡片显示状态徽章：Connected(绿) / Reachable(蓝) / Offline(橙) / History(灰)，TCP 端口 80/8080 探测 | ✅ |
| R18 | **Web File Manager** | web_config_server 新增 SD 卡文件管理器：递归浏览目录、Download 文件到浏览器、Delete 文件/空目录；与 Audio Recorder 互斥（模式切换），路径穿越防护 | ✅ |
| R19 | **Flutter File Manager** | Flutter App Settings 页新增文件管理：目录浏览/导航、下载、删除，含确认弹窗 | ✅ |
| R20 | **CameraStream JPEG 快照** | `/api/capture_image` 端点返回最新 JPEG 帧 (stream handler 每帧缓存到 `_last_jpeg_buf`, mutex 保护) | ✅ |
| R21 | **CameraStream 内联人体检测** | MJPEG stream_handler 每 3 帧运行 COCODetect 推理，检测框绘制在 JPEG 帧上，模型后台 task 加载 | ✅ |
| S86 | **web h_rec_start audio_unlock 泄漏** | fopen 失败路径缺少 `audio_unlock()` 导致永久死锁 | ✅ |
| S87 | **AudioDriver deinit mutex 删除竞态** | `deinit()` 先置空 codec handles + yield 让 in-flight 操作退出，再安全删除 `_codec_mutex`；同时重置 `_vol_pub` 防止重启用 stale handle | ✅ |
| S88 | **AudioDriver set_volume stale publish** | codec mutex 超时时跳过 uORB publish，避免发布未实际应用的音量值 | ✅ |
| S89 | **AudioDriver set_mic_gain WIFI6** | `_codec_mic_handle` 为 NULL 时回退到 `_codec_handle`，WIFI6 板 mic gain 不再静默失效 | ✅ |
| S90 | **mDNS mutex TOCTOU 修复** | `_mdns_mutex_get()` 懒创建改为 `shared_mdns_mutex_init()` 在 app_main 中提前创建，消除并发双重创建竞态 | ✅ |
| S91 | **monitor_init_brookesia 显示锁死锁** | `ESP_BROOKESIA_CHECK_*_EXIT` 改为 if/goto-cleanup 模式，所有退出路径均调用 `bsp_display_unlock()`；installApp 失败时 delete 已分配的 app 对象 | ✅ |
| S92 | **g_has_lcd volatile** | `g_has_lcd` 改为 `volatile bool`，确保跨核写入对其他 task 可见 | ✅ |
| S93 | **app_main 任务回收** | idle loop 改为 `vTaskDelete(NULL)`，回收 ~4KB 栈和 TCB | ✅ |
| S94 | **ULog SD 卡检查** | 仅在 SD 卡成功挂载时初始化 ULog writer，防止写入不存在的挂载点 | ✅ |
| S95 | **web uORB wifi_state 订阅泄漏** | `web_config_server_stop()` 和 task 退出路径添加 `orb_unsubscribe(s_wifi_state_sub)` | ✅ |
| S96 | **ULog PX4 双模式命名** | 参考 PX4 ULog 规范：有 SNTP 时用日期目录 `YYYY-MM-DD/HH_MM_SS.ulg`，无 RTC 时用 session 目录 `sessNNN/logNNN.ulg`；扩展名改为 `.ulg`；NVS 持久化 session 计数器 | ✅ |
| S97 | **ULog Info 消息扩展** | 添加 PX4 标准 Info keys: `ver_hw`, `sys_uuid` (MAC), `sys_os_name`, `sys_os_ver`, `sys_mcu`, `ver_data_format`, `boot_time_utc_us`, `time_ref_utc` | ✅ |
| S98 | **ULog 文件轮转** | 单文件超过 `ULOG_MAX_FILE_SIZE` (默认 100MB) 时自动关闭并创建新文件，同目录递增编号 | ✅ |
| S99 | **ULog 目录级清理** | 清理策略改为按子目录 (日期/session) 整体删除，替代 flat 文件扫描；自动迁移旧格式 `.ulog` flat 文件 | ✅ |
| S100 | **SNTP 时间同步** | WiFi 获取 IP 后自动启动 SNTP (`pool.ntp.org`)，为 ULog 日期命名提供 wall-clock 时间 | ✅ |
| S101 | **ULog RTC 时间戳** | 文件头和 `boot_time_utc_us` 在 SNTP 同步后使用真实 UTC 时间，无 SNTP 时回退近似值 | ✅ |
| S102 | **JPEG 编码器 OOM 修复 (LCD-4B)** | HW JPEG 编码器 DMA 描述符需内部 SRAM，LCD-4B 因 LVGL 绘制缓冲占用 ~72KB 内部 SRAM 导致分配失败。修复: ① LVGL 绘制缓冲改用 PSRAM (`buff_spiram=true`, P4 PSRAM 支持 DMA) ② JPEG 编码器延迟初始化 (首个 MJPEG 客户端连接时才创建, 3 次重试+退避) ③ `CONFIG_SPIRAM_TRY_ALLOCATE_DMA_BUFFER=y` | ✅ |
| S103 | **Settings close() 事件组 use-after-free** | `close()` 删除 `_wifi_event_group` 但事件处理器仍注册 → 处理器访问已释放句柄崩溃。修复: `close()` 在删除事件组之前先注销事件处理器，匹配析构函数已有的安全模式 | ✅ |
| S104 | **Camera 检测任务 mutex 删除竞态** | `_detect_task_handle = nullptr` 在 `vTaskDelete(NULL)` 之前设置，`close()` 可能在任务仍持有 mutex 时删除它。修复: 任务句柄为空后加 50ms yield 等待 idle task 回收 TCB | ✅ |
| S105 | **CameraStream FPS 追踪非功能** | `_fps_frame_count` 和 `_fps_total_bytes` 在 stream_handler 中从未更新，FPS 始终为 0.0f。修复: 每帧递增计数器，每 FPS_LOG_INTERVAL_S (2s) 计算并发布实际 FPS | ✅ |
| S106 | **V4L2 DQBUF 失败队列饥饿** | DQBUF 持续失败时两个缓冲区保持 dequeued → 队列饥饿 → 流永久挂起。修复: 加连续失败计数器，达 100 次后退出循环 | ✅ |
| S107 | **AudioDriver assert() 崩溃** | `init()` 中多个 `assert()` 在 codec 接口创建失败时调用 `abort()` 崩溃设备。修复: 替换为 null 检查 + `init_ok` 标志 + 分阶段 rollback，设备继续运行但不提供音频 | ✅ |
| S108 | **s_fps_pub uORB 发布者生命周期** | `s_fps_pub` 是 stream_handler 中的 static 局部变量，stop/start 周期后旧句柄残留。修复: 移至 `CameraStream` 类成员 `_fps_pub`，`stop()` 中重置为 `ORB_ADVERT_INVALID` | ✅ |
| S109 | **s_wifi_pub 懒初始化竞态** | `s_wifi_pub` 在 `publish_wifi_state()` 中懒初始化，WiFi 事件并发触发可双重 `orb_advertise()`。修复: 改为 `std::atomic<orb_advert_t>` + `compare_exchange_strong()` | ✅ |
| S110 | **Camera 检测帧撕裂** | 检测任务释放 mutex 后推理读取 `_cam_buffer`，LVGL timer 可在预处理器复制前写入新帧。修复: 分配私有 `_detect_in_buf`，mutex 内 memcpy 后释放 mutex 再推理 | ✅ |
| S111 | **V4L2 buf.index 越界** | `VIDIOC_DQBUF` 返回的 `buf.index` 未校验直接索引 `_v4l2_bufs[]`。修复: 加 `buf.index >= _v4l2_buf_count` 防御检查，越界时 requeue 并跳过 | ✅ |
| S112 | **h_list 不必要的音频初始化** | `/api/audio/list` 调用 `__audio_init()` 初始化 I2S codec，但仅需 SD 卡文件列表。修复: 改用 `__sd_ensure()` 避免 I2S 资源冲突 | ✅ |
| S113 | **Content-Disposition 头注入** | SD 卡文件名含 `"` 或 `\` 可破坏 HTTP 头格式。修复: 过滤危险字符后再嵌入 header | ✅ |
| S114 | **s_detect_pub 清理** | `s_detect_pub` 在 `_deinit_detection()` 中未重置，跨 App 开关周期残留。修复: deinit 时重置为 `ORB_ADVERT_INVALID` | ✅ |
| S115 | **s_audio_task 跨核可见性** | 音频任务在 core 0 设置 `s_audio_task = NULL`，httpd 在其他核心读取。修复: 清除前加 `__sync_synchronize()` 内存屏障 | ✅ |
| S116 | **h_rec_status 数据竞态** | 读取 `s_rec_bytes`/`s_rec_start_ms` 未持 mutex，录音任务并发更新可读不一致值。修复: 加 `audio_lock()/audio_unlock()` 获取一致性快照 | ✅ |
| S117 | **std::map 堆分配优化** | `_nvs_param_map` 使用 `std::map<std::string, int32_t>` 每次 lookup 产生堆分配。修复: 替换为包含 3 个 int32_t 字段的扁平 struct `_nvs` | ✅ |
| S118 | **Music 切歌崩溃** | `_play()` 切歌时 `stop()` + `vTaskDelay(200ms)` + `destroy()`，但 GMF 任务可能仍在处理。`destroy()` 释放管道资源时 GMF 任务仍在执行回调 → 崩溃。修复: 轮询 `esp_audio_simple_player_get_state()` 等待 STOPPED 状态后再 destroy()。同时修复 web_config_server 的 `h_play()`/`h_rec_start()`/`web_config_server_stop()` 中的相同问题 | ✅ |
| S119 | **FPS 发布每帧执行** | `camera_stream.cpp` FPS 追踪代码花括号错位，`orb_publish()` 在 `if (elapsed >= FPS_LOG_INTERVAL_S)` 块外 → 每帧执行而非每 2s。修复: 修正花括号缩进 | ✅ |
| S120 | **AudioDriver codec close 未打开句柄** | codec open 失败 rollback 时对未 open 的 handle 调用 `esp_codec_dev_close()`。修复: 跟踪 `codec_dac_opened`/`codec_mic_opened` 标志，仅 close 已打开的 handle | ✅ |
| S121 | **ULog Writer NUL 终止符修复** | ULog 字符串字段不应包含 NUL 终止符（ULog 规范: "Strings do not contain the termination NULL character"）。Format/Subscription/Info/Logging 消息均错误包含 `\0`，导致 pyulog `KeyError: 'fps_stats\x00'`。修复: 移除所有字符串字段 NUL 终止符，与 PX4 参考实现一致 | ✅ |
| S122 | **ULog Git 版本信息** | 新增 `ulog_git_info_t` + `ulog_writer_set_git_info()` API，ULog Info 消息写入 `ver_sw_branch`（PX4 标准键）、`ver_sw_commit`、`ver_sw_author`、`ver_sw_date`、`ver_sw_msg`，从 `git_info.h` 编译时宏获取 | ✅ |
| S123 | **orb_init() 幂等竞态修复** | `orb_init()` 的 `if (s_mutex != NULL) return` 检查本身不是原子的，并发调用可双重创建 mutex。修复: 移除幂等检查改为 `assert(s_mutex == NULL)`，文档说明必须在 app_main 中任务创建前调用一次 | ✅ |
| S124 | **CameraStream _fps_pub 双重广播竞态** | `_fps_pub` 是普通 `orb_advert_t`，`if (_fps_pub < 0) _fps_pub = orb_advertise()` 非原子 check-then-set。修复: 改为 `std::atomic<orb_advert_t>` + `compare_exchange_strong()`，匹配 AudioDriver::_vol_pub 模式 | ✅ |
| S125 | **CameraStream _fps_frame_count 一致性** | `_fps_frame_count` 为普通 uint32_t 而 `_fps_total_bytes` 为 `std::atomic<uint32_t>`，两者在同一代码路径递增/重置。修复: `_fps_frame_count` 也改为 `std::atomic<uint32_t>` | ✅ |
| S126 | **AudioDriver _volume 隐式原子加载** | `_volume` 为 `std::atomic<int>`，但 init() 中用隐式 `operator int()` 而非显式 `.load()`。修复: 统一使用 `_volume.load(std::memory_order_relaxed)` (写均在 mutex 内) | ✅ |
| S127 | **CameraDriver owner-tracked claim/release** | `claim()` 不区分重入（同模块）和争用（不同模块），CameraStream 运行时 Camera App `claim()` 返回 true → V4L2 操作冲突 → WDT 崩溃。修复: `claim(caller_id)/release(caller_id)` 支持 owner tracking，同 caller_id 可重入，不同 caller_id 互斥。PhoneAppCamera close() 中 `example_video_deinit()` 仅在 `_video_initialized` 时调用 | ✅ |
| S128 | **Music 播放失败清理** | `PhoneAppMusic::_play()` 在 `esp_audio_simple_player_run()` 失败时销毁半初始化的 ASP handle，并把播放状态/UI 复位，避免 GMF handle 泄漏和按钮状态残留 | ✅ |
| S129 | **Brookesia 初始化失败清理** | `monitor_init_brookesia()` 在任一步骤失败时删除临时 `ESP_Brookesia_Phone` 对象，避免部分安装成功的 App 在失败退出路径中泄漏 | ✅ |
| S130 | **SDCardDriver _initialized atomic** | `_initialized` 从 `bool` 改为 `std::atomic<bool>`，`available()` 无锁读取线程安全 | ✅ |
| S131 | **AudioDriver _volume atomic** | `_volume` 从 `std::atomic<int>` 隐式加载改为显式 `.load(std::memory_order_relaxed)`，`volume()` 无锁读取线程安全 | ✅ |
| S132 | **CameraDriver mutable _sub** | 移除 `const_cast`，`_sub` 改为 `mutable` 成员，保持 `available()` const 正确性 | ✅ |
| S133 | **JPEG encoder init race** | 两个并发 MJPEG 客户端可同时触发延迟初始化导致 double-init。修复: `_encoder_init_in_progress` atomic flag + `_encoder_initialized` atomic 双阶段保护 | ✅ |
| S134 | **uORB orb_init() 幂等竞态** | `orb_init()` 的 `if (s_mutex != NULL) return` 检查本身不是原子的。修复: 移除幂等检查改为 `assert(s_mutex == NULL)`，必须在 app_main 任务创建前调用一次 | ✅ |
| S135 | **uORB subscriber ABA 保护** | 订阅者 slot 复用后可能收到错误 topic 的消息。修复: generation counter 防止消息投递到错误订阅者 | ✅ |
| S136 | **Settings bool atomic migration** | `_wifi_scanning`/`_wifi_connecting`/`_is_ui_del` 从 `volatile bool` 改为 `std::atomic<bool>`，跨 task 安全 | ✅ |
| S137 | **AudioDriver _vol_pub atomic** | `_vol_pub` 从普通 `orb_advert_t` 改为 `std::atomic<orb_advert_t>` + `compare_exchange_strong()`，防止双重 `orb_advertise()` | ✅ |
| S138 | **CameraStream FPS fields atomic** | `_frame_count`/`_fps_frame_count`/`_fps_total_bytes` 从 `volatile` 改为 `std::atomic<uint32_t>`，正确跨核内存序 | ✅ |
| S139 | **AudioDriver deinit 竞态** | deinit() 释放 `_lifecycle_mutex` 10ms 等待 in-flight 操作退出期间，并发 init() 可创建孤儿资源。修复: 始终持有锁，`_codec_mutex` 置 null 后 in-flight op 跳过 | ✅ |
| S140 | **NVS cache 数据竞争** | `s_nvs_cache_count++` 在多 task 间无同步访问。修复: 新增 `s_nvs_cache_mutex` (FreeRTOS mutex) | ✅ |
| S141 | **Logger ring buffer 活锁** | 缓冲区满时 producer retry loop 检测到空间但不发 `data_sem`。修复: retry loop 退出后 `xSemaphoreGive(data_sem)` 唤醒 writer | ✅ |
| S142 | **V4L2 triple buffer** | CameraStream V4L2 使用 3 个 buffer (原 2 个)，减少 DQBUF 队列饥饿风险 | ✅ |
| S143 | **ULog storage path** | `.ulg` 文件存储路径从 `/sdcard/log` 改为 `/sdcard/data` | ✅ |
| S144 | **ULog session counter reset** | session counter 超过 60000 时重置，防止 NVS uint16 溢出 | ✅ |
| S145 | **ULog refactor: hardware-independent** | `ulog_writer` 移除硬件依赖，通过 `ulog_init_config_t` 从应用层接收配置（session_counter/has_wall_clock/sys_name 等），组件可移植 | ✅ |
| S146 | **nvs_set_i32 rename** | 静态函数 `nvs_set_i32` 重命名为 `nvs_write_i32`，避免与 ESP-IDF API 名称冲突 (shadow warning) | ✅ |
| S147 | **MJPEG stream + detection fix** | 检测帧绘制时 MJPEG 流暂停 + FPS 统计未发布。修复: 检测和流处理顺序正确化 | ✅ |
| S148 | **Camera info JSON parsing** | Camera Stream Web UI JSON 解析错误修复 | ✅ |
| S149 | **SystemMonitor CPU% 双核修正** | `uxTaskGetSystemState()` 返回 wall-clock 而非任务运行时间总和，导致双核 ESP32-P4 上 `total_cpu_pct` 始终≈50%。修复: 汇总各任务 `ulRunTimeCounter` delta 作为实际 CPU 时间，除以 `wall_delta × 2` | ✅ |
| S150 | **SystemMonitor TOP_N 编译错误** | `fields[TOP_N]` 数组用 6 个初始化器但 TOP_N 可配置为 1-6，<6 时编译错误。修复: 改为固定 `fields[6]` | ✅ |
| S151 | **SystemMonitor _task_handle 竞态** | `stop()` 和 `_monitor_task_func` 都写 `_task_handle`，快速 stop→start 可使新任务句柄被旧任务清空。修复: 任务不再写 `_task_handle`，由 `stop()` 独占管理 | ✅ |
| S152 | **SystemMonitor _prev_tasks 泄漏** | 最后一次 `_sample()` 分配的 `_prev_tasks` 在任务退出时未释放。修复: `_monitor_task_func` 退出前 free | ✅ |
| S153 | **SystemMonitor IDLE 任务排除** | IDLE0/IDLE1 占用高表示系统空闲而非负载高，导致 `total_cpu_pct` 始终≈100% 且 CPU 告警误触发。修复: `total_cpu_pct` 仅统计非 IDLE 任务 (busy CPU%)，top-N 排序也排除 IDLE 任务 | ✅ |
| S154 | **SystemMonitor 单核兼容** | CPU% 计算硬编码 `×2`，单核平台无法使用。修复: 改用 `configNUMBER_OF_CORES` | ✅ |
| P1 | **PPA 硬件加速检测预处理** | `PPAPreprocessor` 类: PPA SRM client 执行 RGB565LE→BGR888 resize (800×800→300×300, PPA 4-bit frac 量化 0.4→0.375, pic_w=actual_w 确保行步长连续), COCODetect 内部 BGR888→letterbox→RGB888_QINT8 (含 R↔B swap); PhoneAppCamera + CameraStream 均启用; 自动降级 CPU fallback; 检测框按 actual_width/height rescale | ✅ |

### 2.3 系统性能监控

| # | 需求 | 说明 | 状态 |
|---|------|------|:----:|
| M1 | **SystemMonitor 驱动** | `drivers/system_monitor/` — 周期性采样 FreeRTOS 任务 CPU% (`uxTaskGetSystemState`) + heap/PSRAM 内存，发布 uORB `system_stats` topic | ✅ |
| M2 | **system_stats uORB topic** | `proto/system_stats.msg` — 内存 (free/min internal+PSRAM)、CPU%、任务数、Top-6 任务 CPU%/栈高水位 | ✅ |
| M3 | **ESP_LOG 周期摘要** | 可配置间隔 (默认 60s) 输出 Top-6 CPU 消耗任务 + 内存摘要到 UART | ✅ |
| M4 | **ULog 持久化** | system_stats 注册到 ULog writer，SD 卡 `.ulg` 文件记录完整性能历史 | ✅ |
| M5 | **Web API `/api/system_stats`** | HTTP JSON 端点返回当前 CPU/内存/任务快照，供 Flutter App 或浏览器远程监控 | ✅ |
| M6 | **Kconfig 可配置** | `CONFIG_APP_SYS_MONITOR_INTERVAL_MS` (采样间隔)、`LOG_INTERVAL` (日志频率)、`TOP_N`、`TASK_STACK` | ✅ |
| M7 | **异常告警 — CPU 高负载** | CPU > 90% 时发布 `system_alert` uORB (CPU_HIGH)，ESP_LOGW 输出告警，包含 Top-1 任务名和 CPU%（绝对 CPU%，0-100% 单核） | ✅ |
| M8 | **异常告警 — 内存高占用** | Internal SRAM 或 PSRAM 使用率 > 80% 时发布 `system_alert` uORB (MEM_INTERNAL_HIGH / MEM_PSRAM_HIGH) | ✅ |
| M9 | **告警分级** | WARNING (阈值~+5%) / CRITICAL (阈值+10%) 两级严重度 | ✅ |
| M10 | **告警冷却** | 同类型告警最小间隔 30s (可配置)，防止告警洪泛 | ✅ |
| M11 | **告警 uORB 持久化** | `system_alert` 注册到 ULog writer，SD 卡 `.ulg` 文件记录告警事件 | ✅ |
| M12 | **告警 Web API** | `GET /api/system_alerts` 返回当前 CPU/内存/PSRAM 告警状态 + 阈值配置 | ✅ |
| M13 | **告警 Kconfig** | `CONFIG_APP_SYS_MONITOR_CPU_ALERT_PCT` (90%)、`MEM_ALERT_PCT` (80%)、`ALERT_COOLDOWN_S` (30) | ✅ |

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
| L4 | **Camera Stream 帧率 ~4fps** | 受限于 sensor VTS=9840 (~5fps) + HW JPEG 编码 + 内联检测 |
| L5 | **720×720 无预设样式表** | ESP-Brookesia 默认回退方案 |
| L6 | **PPA client 注册未使用** | 307KB PSRAM 占用，可优化 |
| L7 | **esp-dl mbedtls/sha256.h 兼容 shim** | ESP-IDF v6.x (mbedtls 4.x) 将 `sha256.h` 移至 `mbedtls/private/`，`managed_components/espressif__esp-dl` 未更新。当前方案: `main/compat/mbedtls/sha256.h` 转发头 + CMake include path。下次升级 esp-dl 后应移除 |

### 4.3 已知未修复问题（来自 project_design.md 分析）

| # | 严重度 | 问题 | 原因 |
|---|--------|------|------|
| K1 | ✅ 已修复 | **Camera 帧缓冲跨核并发** | 分配私有 `_detect_in_buf`, mutex 内 memcpy 后释放再推理 (S110) |
| K2 | 🟢 低 | Audio 无用的 PSRAM 分配 (1920B) | 开销极小 |
| K3 | 🟢 低 | Music 部分低优先级边界情况 | 见 project_design.md §4.10 |
| K4 | ✅ 已修复 | **Web 音频 Camera Stream 互斥已生效** | 所有 6 个端点均已加 `__cam_running()` 检查 (2026-07-02) |
| K5 | 🟢 低 | CameraStream stream_handler 代码重复 | 检测和流处理逻辑混合，可提取为独立函数 |
| K6 | 🟢 低 | Logger 重入风险 | esp_log_set_vprintf 回调中调用 ESP_LOG 可能递归 |
| K7 | 🟢 低 | Web `s_audio_running` 跨核竞态 | HTTP handler 和 audio task 并发访问，非原子 bool |

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
| Camera Stream | 🌐 | MJPEG WiFi 推流 + 人体检测 | LCD-4B + WiFi |
| Settings | ⚙️ | 音量/亮度 + WiFi | LCD-4B |
| Squareline | 🎨 | 内置示例 | LCD-4B |

### 6.2 非 UI 功能

| 功能 | 访问方式 | 适用板子 |
|------|----------|----------|
| Web 配置 (:8080) | 浏览器 | LCD-4B + WIFI6 |
| Web 音频录制 (:8080) | 浏览器 Audio Recorder card | LCD-4B + WIFI6 |
| Web 音频播放 (:8080) | 浏览器 Audio Recorder card | LCD-4B + WIFI6 |
| Web 文件管理 (:8080) | 浏览器 Files card | LCD-4B + WIFI6 |
| Web ULog 控制 (:8080) | 浏览器 ULog card | LCD-4B + WIFI6 |
| JPEG 快照 (:80) | `/api/capture_image` | Camera Stream 运行时 |

> **注意**: Camera Stream 和 Audio 使用独立硬件 (MIPI CSI vs I2S), 可同时运行。

---

## 7. 变更记录

| 日期 | 变更 |
|------|------|
| 2026-07-04 | +S86~S95 Bug 与性能修复: web audio_unlock 泄漏(S86), AudioDriver deinit 竞态(S87), set_volume stale publish(S88), set_mic_gain WIFI6(S89), mDNS mutex TOCTOU(S90), 显示锁死锁(S91), g_has_lcd volatile(S92), app_main 任务回收(S93), ULog SD 检查(S94), web uORB 订阅泄漏(S95) |
| 2026-07-04 | +R18 Web File Manager: SD 卡文件浏览器 (list/download/delete)，与 Audio Recorder 模式互斥，路径穿越防护 |
| 2026-07-04 | +S84 S85 Flutter 设备列表排序 (新扫描优先/历史在后) + 设备可达性状态徽章 (Connected/Reachable/Offline/History) |
| 2026-07-03 | +S68 mDNS 双主机名: 保留 `esp-web` 便捷名 + 新增 `esp-web-XXXXXX` 委托主机名（MAC后3字节），单设备零配置 + 多设备精确定位 |
| 2026-07-03 | +S50~S56 线程安全与性能修复: CameraStream 竞态, CameraDriver 互斥, uORB publish 锁优化, AudioDriver publisher 线程安全, PhoneAppCamera 帧缓冲同步, CameraStream 检测结果互斥, Web 音频操作互斥 |
| 2026-07-05 | +S103~S117 代码审计修复: Settings 事件组 use-after-free (S103), Camera mutex 删除竞态 (S104), FPS 追踪非功能 (S105), V4L2 DQBUF 队列饥饿 (S106), AudioDriver assert 崩溃 (S107), uORB 发布者生命周期 (S108/S109/S114), 检测帧撕裂 (S110), V4L2 越界 (S111), h_list I2S 冲突 (S112), HTTP 头注入 (S113), 跨核可见性 (S115), h_rec_status 竞态 (S116), std::map 堆分配 (S117) |
| 2026-07-03 | +S57 **CameraStream VFS 设备泄漏修复**: `_deinit_video()` 未调用 `example_video_deinit()` 导致 "video20" 设备未注销；`_init_video()` 早返 / fail 路径同样缺少；`goto fail` 跨初始化编译错误已修复 |
| 2026-07-03 | +S49 Driver 模块拆分: PeripheralManager→thin facade + AudioDriver + SDCardDriver + CameraDriver (claim/release API) |
| 2026-07-03 | +S44~S48 Bug 修复 (gettimeofday overflow, Music _stop ASP 泄漏, SD LDO 检查, web task 干净退出) +PeripheralManager facade 模块化重构 (消除 extern 全局变量) |
| 2026-07-03 | +S42 uORB 消息总线（FreeRTOS Queue 实现） +S43 .msg 自动生成 pipeline +P11 IPC 迁移计划 |
| 2026-07-06 | +R20 CameraStream JPEG 快照 (`/api/capture_image`), +R21 CameraStream 内联人体检测 (每3帧 COCODetect, 检测框绘制在 JPEG 帧上) |
| 2026-07-06 | +S130~S148 线程安全审查修复: SDCardDriver/AudioDriver atomic (S130-S131), CameraDriver mutable (S132), JPEG encoder init race (S133), uORB orb_init/ABA (S134-S135), Settings bool atomic (S136), AudioDriver _vol_pub atomic (S137), CameraStream FPS atomic (S138), AudioDriver deinit 竞态 (S139), NVS cache mutex (S140), Logger ring buffer 活锁 (S141), V4L2 triple buffer (S142), ULog storage path/session/refactor (S143-S145), nvs_write_i32 rename (S146), MJPEG+detection fix (S147), Camera info JSON (S148) |
| 2026-07-06 | +K1 已修复 (Camera 帧缓冲跨核并发 → _detect_in_buf 私有缓冲), +K5~K7 已知低优先级问题 |
| 2026-07-06 | VTS 勘误: 4920(~10fps)→9840(~5fps), Camera Stream 帧率 ~6.9fps→~4fps |
| 2026-07-06 | +S123 JPEG encoder init race fix: two-phase init with _encoder_init_in_progress flag prevents null handle dereference |
| 2026-07-06 | +S124 uORB orb_init(): eager mutex creation at boot eliminates dual-create race in lock() |
| 2026-07-06 | +S125 uORB subscriber ABA protection: generation counter prevents message delivery to wrong subscriber after slot reuse |
| 2026-07-06 | +S126 Settings bool atomic migration: _wifi_scanning/_wifi_connecting/_is_ui_del → std::atomic for cross-task safety |
| 2026-07-06 | +S127 AudioDriver _vol_pub atomic: single-advertise via compare_exchange_strong prevents double uORB publisher |
| 2026-07-06 | +S128 CameraStream _frame_count/_fps_total_bytes volatile→atomic: proper cross-core memory ordering |
| 2026-07-06 | +S129 SDCardDriver _initialized atomic: available() reads without mutex now thread-safe |
| 2026-07-06 | +S130 AudioDriver _volume atomic: volume() read without lock now thread-safe |
| 2026-07-06 | +S131 CameraDriver mutable _sub: removes const_cast, preserves const-correctness |
| 2026-07-06 | +M1~M13 SystemMonitor 驱动: CPU/内存采样 + uORB (system_stats/system_alert) + ULog + HTTP API (/api/system_stats, /api/system_alerts) + Kconfig + 告警 |
| 2026-07-06 | +S149~S152 SystemMonitor 代码审查修复: CPU% 双核修正 (wall-clock→sum_task_delta), TOP_N 编译错误 (fields[6]), _task_handle 竞态, _prev_tasks 泄漏 |
| 2026-07-06 | +S153~S154 SystemMonitor IDLE 排除 + 单核兼容: total_cpu_pct 改为 busy CPU% (排除 IDLE 任务), top-N 排除 IDLE, ×2→configNUMBER_OF_CORES |
| 2026-07-06 | +K5~K7 Code duplication in stream_handler (LOW), logger reentrancy (LOW), web s_audio_running race (LOW) — deferred |
| 2026-07-02 | 初始创建，汇总 PROJECT.md + project_design.md 中所有需求和问题 |
| 2026-07-02 | +R15 R16 R17 Web 音频录制/播放 + Camera Stream 互斥需求 |
| 2026-07-02 | +S28~S34 Web 音频稳定性修复 (懒加载、URL解码、JS修复、ASP生命周期等) |
| 2026-07-02 | +S35 WIFI6 音频修复: I2C地址修正 0x18→0x30, 单handle (BOTH+IN_OUT) 匹配参考固件 |
| 2026-07-02 | +S36 Music App ASP 生命周期同步, 匹配 web S31 |
| 2026-07-02 | +S37 Flutter Settings 页面: WiFi/音量/Camera Stream + 录音/播放 |
| 2026-07-02 | +K4 Web 音频 Camera Stream 互斥未生效 (诊断中，已加 noinline + debug log) |
| 2026-07-02 | **修复**: K4/R17 全部 6 个 Web 音频端点均加 `__cam_running()` 检查；CameraStream `_detector`/`_frame_count`/`_fps_total_bytes` 加 volatile；SD LDO re-acquire 返回值检查；`localtime()`→`localtime_r()`；`vTaskDelete` 自删除防护 |
