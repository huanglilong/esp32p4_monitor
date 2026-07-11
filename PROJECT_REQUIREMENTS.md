# ESP32-P4 Monitor — 项目需求文档

> 生成日期: 2026-07-02 | 基于 PROJECT.md 及历史会话分析
> 代码仓库: [gitee.com/huanglilong/esp32p4_monitor](https://gitee.com/huanglilong/esp32p4_monitor)
>
> 本文档是**需求、已修复问题登记 (R/S/M) 与变更记录**的唯一参考。软件/架构/实现细节见 [PROJECT.md](PROJECT.md)，硬件规格见 [README.md](README.md)。随项目迭代持续更新。

---

## 1. 项目愿景

基于 Waveshare ESP32-P4-WiFi6-Touch-LCD-4B 开发板构建一个多功能监控终端，集成摄像头预览、音频录制/播放、WiFi 推流和系统设置，以单一固件适配多款开发板（LCD-4B / WIFI6 基板）。

---

## 2. 已完成需求

### 2.1 核心功能

| # | 需求 | 说明 | 状态 |
|---|------|------|:----:|
| R1 | **MIPI DSI 显示** | ST7703 720×720 LCD，LVGL v9.2.2 + ESP-Brookesia Phone UI | ✅ |
| R2 | **MIPI CSI 摄像头** | OV5647 RAW8 800×800，ISP RAW8→RGB565 实时预览 (~5fps) | ✅ |
| R3 | ~~**Camera 人体检测**~~ | ESP-DL + YOLO11n 320×320, ~1.8fps — **已移除**: COCO detection 模型和 esp-dl 依赖已从项目中完全移除 | ❌→移除 |
| R4 | **Camera V4L2 迁移** | 统一 esp_video 接口，与 Camera Stream 共享 | ✅ |
| R5 | **Camera 红绿通道修正** | Bayer GBRG + byte_swap_en=1 修复颜色错误 | ✅ |
| R6 | **Camera Stream WiFi 推流** | HW JPEG MJPEG 流 (port 81) + Web UI (port 80), mDNS 发现 | ✅ |
| R7 | **Camera Stream 独立 App** | 从 Settings App 分离，独立开关，含 CPU/PSRAM 监控 | ✅ |
| R8 | **Audio 双 Mic 监控** | I2S RX 48kHz Stereo 直接读取，双声道电平表 (0–100%) | ✅ |
| R9 | **Audio MP3 录音** | Shine 定点编码器 128kbps stereo → SD 卡，录制/停止/文件列表 | ✅ |
| R10 | **Music MP3/WAV 播放器** | ESP-GMF 音频管道，SD 卡音源，音量滑条 | ✅ |
| R11 | **Settings 音量/亮度** | LVGL Slider (0–100 / 20–100), NVS 持久化，500ms 去抖写入 | ✅ |
| R12 | **Settings WiFi 管理** | SSID 扫描 + 密码输入 + 连接，后台保持，WiFi 始终启用不可禁用 | ✅ |
| R13 | **Web 配置服务器** | HTTP :8080，WiFi/音量远程设置，connect-before-save 验证 | ✅ |
| R14 | **多板自动检测** | GT911 I2C (0x5D) 探测，自动适配 LCD-4B / WIFI6，单一固件 | ✅ |
| R15 | **Web 音频录制** | WIFI6 无屏板通过 Web :8080 录制，Shine MP3 → SD 卡，Start/End 按钮 | ✅ |
| R16 | **Web 音频播放** | WIFI6 无屏板通过 Web :8080 播放 SD 卡 MP3，esp_audio_simple_player | ✅ |
| R17 | **Camera Stream 互斥保护** | ~~Web 音频功能仅在 Camera Stream 未运行时可用，UI 隐藏 + API 阻断~~ → **已移除**: Camera (MIPI CSI) 和 Audio (I2S) 使用独立硬件，无需互斥。所有 `__cam_running()` 检查已移除 | ✅ |

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
| R20 | ~~**CameraStream JPEG 快照**~~ | `/api/capture_image` 端点已移除，改为独立 capture task 架构 | ❌→移除 |
| R21 | ~~**CameraStream 内联人体检测**~~ | capture task 每 3 帧运行 COCODetect 推理，检测框绘制在 JPEG 帧上，模型后台 task 加载。**已移除**: coco_detect 依赖 + COCODetect/esp-dl + detection_result uORB topic + 模型 loader task 全部删除，PPA 继续用于 300×300 JPEG 编码 (无检测) | ❌→移除 |
| R22 | **Camera Frame ULog 录制** | JPEG 帧通过 `camera_frame` uORB topic 写入 `.ulg` 文件 (PPA 400×400, 默认 quality 30 下典型 8-11KB/帧；`jpeg_data` 缓冲 15360B (15KB) 覆盖最坏情况；uORB queue PSRAM 后移，不再占用 internal-SRAM)，2fps；**录制随 Camera Stream 启停自动控制** (start→recording on, stop→recording off)，不再有独立按键/API；ULog ring buffer 扩容 64KB，data_buf PSRAM 动态分配；`msg_gen.py` 自动计算 struct alignment padding（降序对齐重排 + tail `_padding`）；`orb_metadata.o_size_no_padding` (PX4 惯例) 使 ULog writer 不写尾部 padding 字节，修复 pyulog 兼容性；`tools/ulog_extract_frames.py` 提取 JPEG 帧 | ✅ |
| R57 | **WiFi 始终启用** | 移除 `wifi_en` NVS 键和所有 UI 开关 (Settings App switch, Web checkbox, Flutter toggle)，WiFi 不可禁用；`bootWifiAutoConnect` 跳过 wifi_en 检查直接连接；断线自动重连无条件触发；`_nvs` struct 移除 `wifi_en` 字段 | ✅ |
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
| S152 | **volatile → std::atomic 全面迁移** | PhoneAppAudio/Music/Camera 和 web_config_server 中 `volatile bool/int/uint32_t` 替换为 `std::atomic`，双核 ESP32-P4 上 volatile 不保证原子性和内存序 | ✅ |
| S153 | **CameraStream stop() CAS 保护** | `stop()` 使用 `compare_exchange_strong()` 防止并发双重清理，匹配 `start()` 的模式 | ✅ |
| S154 | **CameraStream _jpeg_quality atomic** | JPEG 质量从 `uint8_t` 改为 `std::atomic<uint8_t>`，HTTP API 和 stream handler 跨核访问 | ✅ |
| S155 | **PhoneAppCamera _cleanup_camera_init 防护** | 添加 `_video_initialized` 检查，避免未初始化时调用 `example_video_deinit()` | ✅ |
| S156 | **NVS 共享键定义** | NVS namespace 和 key 宏统一定义在 `example_config.h` (`NVS_NAMESPACE_SETTINGS`, `NVS_KEY_*`)，消除跨文件重复 | ✅ |
| S157 | **I2S/SD SPI 引脚宏化** | AudioDriver I2S GPIO 和 SDCardDriver SPI GPIO 改用 `example_config.h` 宏，消除硬编码 | ✅ |
| S158 | **Logger 丢弃策略优化** | 缓冲区满时立即丢弃日志行，替代 100ms 轮询等待，避免阻塞 LVGL/WiFi 任务 | ✅ |
| S159 | **CameraStream stream_handler 重构** | 提取辅助方法减少代码重复，改善可维护性 | ✅ |
| S152 | **SystemMonitor _prev_tasks 泄漏** | 最后一次 `_sample()` 分配的 `_prev_tasks` 在任务退出时未释放。修复: `_monitor_task_func` 退出前 free | ✅ |
| S153 | **SystemMonitor IDLE 任务排除** | IDLE0/IDLE1 占用高表示系统空闲而非负载高，导致 `total_cpu_pct` 始终≈100% 且 CPU 告警误触发。修复: `total_cpu_pct` 仅统计非 IDLE 任务 (busy CPU%)，top-N 排序也排除 IDLE 任务 | ✅ |
| S154 | **SystemMonitor 单核兼容** | CPU% 计算硬编码 `×2`，单核平台无法使用。修复: 改用 `configNUMBER_OF_CORES` | ✅ |
| P1 | **PPA 硬件加速检测预处理** | `PPAPreprocessor` 类: PPA SRM client 执行 RGB565LE→BGR888 resize (800×800→300×300, PPA 4-bit frac 量化 0.4→0.375, pic_w=actual_w 确保行步长连续), COCODetect 内部 BGR888→letterbox→RGB888_QINT8 (含 R↔B swap); PhoneAppCamera + CameraStream 均启用; 自动降级 CPU fallback; 检测框按 actual_width/height rescale | ✅ |
| S155 | **SystemMonitor 导致 LCD 闪烁** | 根因: `_sample()` 每 5s 调用 `uxTaskGetSystemState()` → `vTaskSuspendAll()` 暂停两核 FreeRTOS 调度器 → LVGL task 的 `ulTaskNotifyTake()` 无法 block → `lv_timer_handler()` 异常高频调用 + `xTaskResumeAll()` 恢复时上下文切换爆发 → 画面闪烁。修复: ① 移除 `heap_caps_print_heap_info()` ② **核心修复: 用 `xTaskGetIdleTaskHandleForCore()` + `vTaskGetInfo()` 替代 `uxTaskGetSystemState()`** — 前者仅读取每核 idle task 累计运行时间，不暂停调度器，无闪烁。per-core busy CPU% = 100% - idle%。③ 移除死代码 (静态 buffer、`_fill_top_tasks`、`_find_prev_runtime`、`_is_idle_task` 等 ~2.4KB SRAM 回收) ④ `system_stats.msg` 移除 top-6 task 字段，新增 `core0_cpu_pct` / `core1_cpu_pct` ⑤ 启用 `CONFIG_FREERTOS_INCLUDE_xTaskGetIdleTaskHandle=y` | ✅ |
| S156 | **音乐卡顿 — 内部 SRAM 不足** | Camera Stream + 音乐播放时 `ESP_GMF_ASMP_DEC: Not enough memory for out` + 内部 SRAM 85.76% (54KB free / 386KB)。根因: LVGL IRAM 占用 ~64KB 内部 SRAM + LWIP 22 sockets × 64KB TCP 缓冲区 pbuf 链头占 ~60KB。修复: ① 关闭 LVGL IRAM (`LV_ATTRIBUTE_FAST_MEM_USE_IRAM=n`) → 代码移至 PSRAM XIP, 释放 **~64KB** (主要修复) ② `task_stack_in_ext=true` 将 ASP 任务栈移至 PSRAM (-8KB) ③ LWIP `TCP_SND_BUF/WND` 65535→32768, httpd `max_open_sockets` 7→3/2/3 (-30KB pbuf 头部) | ✅ |
| S160 | **PhoneAppCameraStream FPS no-op** | `_fps = (_fps > 0) ? _fps : 0` 是空操作，uORB fps_stats 中的实际 FPS 值从未使用。修复: 当 uORB 有新数据时 `_fps = fps_data.fps` | ✅ |
| S161 | **_detect_in_buf null-free** | `_init_detection()` 失败路径 `heap_caps_free(_detect_in_buf)` 无 null 检查，PPA 路径下为 nullptr。修复: 添加 null 检查，匹配 `_deinit_detection()` 模式 | ✅ |
| S162 | **Music 冗余 ASP 清理** | `close()` 中 `_stop()` 已调用 `_destroy_player_handle()`，后续 `if(_asp_handle)` 块是死代码。修复: 移除冗余清理，保留注释说明依赖顺序 | ✅ |
| S163 | **SD 路径常量统一** | `RECORD_DIR`/`MUSIC_DIR`/`REC_DIR` 三个文件各自定义 `"/sdcard"`。修复: 统一使用 `example_config.h` 中的 `SDMMC_MOUNT_POINT` | ✅ |
| S164 | **VOLUME/BRIGHTNESS 常量统一** | `VOLUME_MIN/MAX/DEFAULT` 和 `BRIGHTNESS_MIN/MAX/DEFAULT` 在 `phone_app_settings.hpp` 和 `web_config_server.cpp` 重复定义。修复: 移至 `example_config.h` 共享宏 | ✅ |
| S165 | **WIFI_CONNECTED_BIT 共享** | `WIFI_CONNECTED_BIT` 在 `phone_app_settings.cpp` 和 `web_config_server.cpp` 重复定义。修复: 移至 `example_config.h` | ✅ |
| S166 | **Logger ring buffer memcpy 优化** | 字节逐个拷贝+取模运算改为 wrap-aware memcpy (1-2次 memcpy)，writer task 批量从 128B→512B | ✅ |
| S167 | **SystemMonitor heap 总大小缓存** | `heap_caps_get_total_size()` 每次采样都调用 (运行时常量)。修复: `init()` 中缓存一次 | ✅ |
| S168 | **camera_stream.hpp extern "C" 修复** | 整个 `CameraStream` C++ 类被 `extern "C"` 包裹，禁用 C++ name mangling。修复: 仅 `ov5647_set_vts_2fps()` C 函数使用 `extern "C"` | ✅ |
| S169 | **heap_caps_free 一致性** | `heap_caps_*` 分配的内存用 `free()` 释放。修复: 统一使用 `heap_caps_free()` | ✅ |
| S170 | **Logger volatile→atomic 迁移** | `writer_running`/`writer_exited` 使用 `volatile bool`，writer task (core 0) 写入，logger_deinit() (任意 core) 读取。修复: 迁移为 `std::atomic<bool>`，遵循 S152 标准 | ✅ |
| S171 | **web s_rec_bytes 数据竞态** | `s_rec_bytes` 在 audio_task 中无锁 `+=` (read-modify-write)，HTTP handler 持锁读取。修复: 改为 `std::atomic<uint32_t>`，writer 使用 `fetch_add()`，reader 使用 `load()` | ✅ |
| S172 | **SystemMonitor stop() eTaskGetState 竞态** | `stop()` 使用 `eTaskGetState(handle)` 检查已自删任务，与 idle task TCB 回收竞态。修复: 新增 `_task_exited` atomic flag，任务退出前设置，stop() 轮询替代 eTaskGetState (遵循 S74 模式)；轮询超时使用 `CONFIG_APP_SYS_MONITOR_INTERVAL_MS + 500` 确保覆盖一个完整任务周期 | ✅ |
| S173 | **bsp_i2c_get_handle extern 去重** | `audio_driver.cpp` 和 `camera_stream.cpp` 各自声明 `extern "C" bsp_i2c_get_handle()`。修复: 统一定义在 `example_config.h`，移除重复声明 | ✅ |
| S174 | **AudioDriver PA GPIO 硬编码** | `gpio_config_t` 的 `pin_bit_mask` 使用字面量 `(1ULL << 53)`，而 `AUDIO_PA_GPIO` 宏已存在。修复: 使用 `(1ULL << AUDIO_PA_GPIO)` 宏 | ✅ |
| S175 | **TCP 窗口/发送缓冲调整** | Camera MJPEG 流通过 SDIO 传输大量 TCP 数据，小窗口导致频繁小包传输与 C6 控制消息竞争。先从 32KB→64KB，后回退至 32KB。**注意**: 64KB 超出未开启 WND_SCALE 时 Kconfig 上限 65535，若需 >65535 必须开启 `CONFIG_LWIP_WND_SCALE=y` + `CONFIG_LWIP_TCP_RCV_SCALE≥1`，否则 clean rebuild 静默钳制为 5760。最终选择 32KB (32768)，无需 WND_SCALE | ✅ |
| S176 | **Core 1 负载过高** | Core1 占 27% vs Core0 占 8%。原因: httpd (3实例) 未绑核默认跑 Core1 与 LVGL 竞争；LVGL timer 5ms 过于频繁。修复: (1) httpd core_id=0 (2) LVGL timer_period 5→20ms | ✅ |
| S177 | **HTTP 服务器不可达修复** | 客户端断连后 HTTP 永久不可达。根因: `LWIP_MAX_SOCKETS=22` 太小，3 个 httpd 内部占 17 个 socket，socket 表耗尽 → `accept()` 返回 `ENOTSOCK`。修复: ① `LWIP_MAX_SOCKETS` 22→28 ② 所有 httpd 启用 TCP keep-alive ③ Web Config WiFi 断连自动重启 httpd + WiFi 恢复后重注册 URI ④ 提取 `_register_web_config_uris()` 复用 | ✅ |
| S178 | **SystemMonitor 内存告警阈值提升** | `CONFIG_APP_SYS_MONITOR_MEM_ALERT_PCT` 从 80% 提升至 85%，减少 Internal SRAM 误报 (LVGL + LWIP 常态占用 ~70%) | ✅ |
| S179 | **Flutter ULog 视频查看器** | 新增 `ulog_parser.dart` (移植 Python ULog 解析器) + `ulog_viewer_screen.dart` (下载/解析 .ulg 文件，camera_frame JPEG 帧缩略图网格，幻灯片播放，键盘导航，InteractiveViewer 缩放，单帧/全帧保存)；Settings 页 .ulg 可点击查看 + "Open Local .ulg" 按钮 | ✅ |
| S180 | **Flutter filesDownload 可靠性** | 移除 `_isChunkedBody` 自动检测 (非 chunked 误判)，仅 `Transfer-Encoding: chunked` 头存在时 dechunk (RFC 7230)；O(n²) `toBytes().length` → int 计数器；超时 30s→10s | ✅ |
| S181 | **Non-detection JPEG QBUF 延迟** | 非检测帧 JPEG sensor 路径 QBUF 在消费者完成前发出，V4L2 buffer 可能被驱动覆写。修复: 与检测帧路径统一，JPEG sensor 延迟 QBUF 至 `_send_mjpeg_part`/`_save_jpeg_snapshot`/`_publish_camera_frame` 完成后；CPU fallback 编码后立即 QBUF (jpeg_data 已独立) | ✅ |
| S182 | **AudioDriver deinit mutex 竞态** | `deinit()` 用 10ms 延时等待 in-flight codec 操作，但 `set_volume`/`set_mic_gain` 可持锁 100ms。修复: 新增 `_codec_ops_in_flight` 原子计数器，codec 操作入口递增/出口递减，`deinit()` 轮询等待计数归零 (最长 1s) 后再删除 mutex | ✅ |
| S183 | **Logger deinit mutex 竞态** | `logger_deinit()` 用 10ms 延时等待 in-flight `_logger_push`，但可持 `buf_mutex` 100ms。修复: 新增 `push_in_flight` 原子计数器，`_logger_push` 入口递增/出口递减，`deinit()` 轮询等待计数归零 (最长 1s) 后再删除 mutex | ✅ |
| S184 | **Logger snprintf 栈溢出** | `vsnprintf`/`snprintf` 返回值未截断到缓冲区大小，长日志行导致 `_strip_ansi`/`_logger_push` 越界读取栈缓冲区。修复: `raw_len` 和 `len` 截断到 `sizeof(buffer) - 1` | ✅ |
| S185 | **Logger data_sem UAF** | `_logger_push` 在 `fetch_sub(push_in_flight)` 后才 `xSemaphoreGive(data_sem)`，`deinit` 可在 fetch_sub 后、give 前 delete data_sem → UAF。修复: give data_sem 移到 fetch_sub 之前 | ✅ |
| S186 | **Logger writer force-kill 持锁** | writer task clean-exit 路径在 `buf_mutex` 内调用 `fwrite`，`deinit` force-kill 时孤儿化 mutex。修复: 先在 mutex 内 memcpy 到堆缓冲，释放 mutex 后再 fwrite | ✅ |
| S187 | **Logger 句柄悬挂** | `xTaskCreate` 失败时 `buf_mutex`/`data_sem` 被 delete 但未置 null，重试 init 时使用已释放句柄。修复: delete 后立即置 null | ✅ |
| S188 | **Logger sd_level 数据竞争** | `sd_level` 为普通 `logger_level_t`，跨核读写无同步。修复: 改为 `std::atomic<int>` | ✅ |
| S189 | **Web h_play 路径穿越** | `/api/audio/play?file=` 参数未经 `__path_sanitize`，可构造 `../../etc/passwd` 逃逸 `/sdcard`。修复: 经 `__path_sanitize` 校验后再构建 URI | ✅ |
| S190 | **Web cJSON NULL 解引用** | `status_handler`/`ulog_status_handler`/`h_files_list` 中 `cJSON_CreateObject()` 和 `cJSON_PrintUnformatted()` 返回 NULL 未检查，OOM 时崩溃。修复: 全部加 null 检查 + 500 响应 | ✅ |
| S191 | **Web s_running 停止竞态** | `s_running` 在 WiFi 连接 + httpd 启动后才置 true，stop() 在 WiFi 等待窗口调用时看到 false 直接返回，任务继续运行。修复: 任务入口立即置 true + WiFi 等待循环检查 s_running | ✅ |
| S192 | **Web mutex 泄漏** | `s_nvs_cache_mutex` 和 `s_audio_mutex` 在 stop/restart 周期中从不 delete，每次 task 创建新 mutex 孤儿旧句柄。修复: task cleanup 和 stop() fallback 中 delete 两个 mutex | ✅ |
| S193 | **Web httpd_start 失败泄漏** | `httpd_start` 失败时 `vTaskDelete(NULL)` 跳过 mutex/task 清理。修复: `goto cleanup` 统一清理路径 | ✅ |
| S194 | **Web NVS cache 竞态** | `factory_reset_handler` 无锁遍历 `s_nvs_cache` 失效条目，并发 handler 可撕裂读取。修复: 加 `s_nvs_cache_mutex` 保护 + 重置 count | ✅ |
| S195 | **Web free→cJSON_free** | `h_list` 用 `free()` 释放 cJSON 字符串，不一致。修复: 改用 `cJSON_free()` | ✅ |
| S196 | **CameraStream V4L2 buf 越界** | `VIDIOC_REQBUFS` 返回 count 可能 > 固定数组大小 2，导致 OOB 写。修复: 截断 `_v4l2_buf_count` 到 2 | ✅ |
| S197 | **CameraStream model-load force-kill** | `_deinit_detection()` 立即 `vTaskDelete` model-load task，可能在 `new`/`malloc` 中途被杀导致堆损坏。修复: 先等待 15s (覆盖 ~11s 模型加载) 再考虑 force-kill | ✅ |
| S198 | **CameraStream dummy_buf NULL** | 无 PPA 路径 `heap_caps_calloc` 失败时 NULL 传给 `run(img)` → NPE。修复: 加 null 检查 + 提前退出 | ✅ |
| S199 | **CameraStream snprintf 截断** | `_send_mjpeg_part` 中 `snprintf` 返回值未检查，负值或 >= buf_size 时越界。修复: 加 `hlen < 0 || >= part_buf_size` 检查 | ✅ |
| S200 | **CameraStream bytesused 越界** | `buf.bytesused` 可能 > `_v4l2_buf_len`，导致编码器越界读取。修复: 截断到 buf_len | ✅ |
| S201 | **CameraStream encoder dims 零值** | JPEG sensor 路径和 encoder 未初始化时 `_stream_enc_width/height` 为 0，`camera_info_handler` 返回 0×0。修复: JPEG 路径设值 + handler fallback 到 sensor dims | ✅ |
| S202 | **PhoneAppCamera V4L2 pipeline 泄漏** | `example_video_init()` 成功但后续步骤失败时 `_video_initialized` 仍为 false，cleanup 不 deinit pipeline。修复: init 成功后立即置 true | ✅ |
| S203 | **PhoneAppCamera buf.index 越界** | `buf.index` 未校验直接索引固定大小数组。修复: 加 `>= _v4l2_buf_count || >= 2` 检查 | ✅ |
| S204 | **PhoneAppSettings 事件组泄漏** | `run()` 无条件创建 `_wifi_event_group`，已存在时泄漏旧句柄。修复: 加 null 检查 | ✅ |
| S205 | **PhoneAppSettings WiFi OFF/ON abort** | OFF 路径重置 `_wifi_initialized = false`，下次 ON 时 `ESP_ERROR_CHECK(esp_netif_init())` 二次调用返回 `ESP_ERR_INVALID_STATE` → abort。修复: 不重置 `_wifi_initialized`，保留一次性 init + handler 注册 | ✅ |
| S206 | **PhoneAppSettings connect task UAF** | `wifiConnectTaskHandler` 创建时传 NULL handle，close()/析构无法等待，删除 event group 时 task 可能仍在 `xEventGroupWaitBits` → UAF。修复: 存储 `_wifi_connect_task` handle + 析构等待 `_wifi_connecting` 归零 | ✅ |
| S207 | **PhoneAppSettings _wifi_ip 未填充** | IP 事件 handler 日志输出 IP 但未写入 `_wifi_ip` 成员，UI 永远显示空 IP。修复: IP 事件中 `snprintf(_wifi_ip, ...)` | ✅ |
| S208 | **PhoneAppCameraStream 析构 handler UAF** | 析构函数只 `stop()`，未注销 WiFi event handler (引用 `this`)，handler 触发时 UAF。修复: 析构中防御性注销 handler + unsubscribe uORB | ✅ |
| S209 | **AudioDriver init rollback mutex UAF** | codec open 失败 rollback 时 `_codec_mutex` 用普通 load + delete，并发 codec op 可能持有已删 mutex。修复: mutex 创建移到 codec open 成功后，rollback 不再删 mutex | ✅ |
| S210 | **SystemMonitor force-kill 持锁** | `stop()` 超时后 `vTaskDelete` 任务，任务可能持有 `_latest_mutex`/`_alert_mutex` → 永久死锁。修复: 不再 force-kill，任务自行检查 `_running` 退出 | ✅ |
| S211 | **SDCardDriver _has_lcd 数据竞争** | `_has_lcd` 为普通 `bool`，跨核读写无同步。修复: 改为 `std::atomic<bool>` | ✅ |
| S212 | **main.cpp assert no-op** | `assert(*disp)` 在 release 构建中被移除，NULL disp 传入后续函数 → NPE。修复: 改为显式 null 检查 + log + return | ✅ |
| S213 | **main.cpp esp_read_mac 未检查** | `esp_read_mac` 失败时 mac 未初始化，ULog sys_uuid 含垃圾数据。修复: 检查返回值，失败时 memset 清零 | ✅ |
| S214 | **camera_frame JPEG 超出 ULog 缓冲** | 实测帧 JPEG 达 9210B，超过 `camera_frame_s.jpeg_data[8192]`，触发 `JPEG frame too large for ULog` 告警并丢帧。修复: `proto/camera_frame.msg` 的 `jpeg_data` 数组由 8192 扩至 10240（10KB，覆盖默认 quality 30 下实测 ~9210B 的帧并留余量）；`_publish_camera_frame` 的大小检查自动取 `sizeof(jpeg_data)`，无需改逻辑。注意: 每次发布 uORB 队列 (depth 2，默认单一 ULog 订阅) 在内部 SRAM 占用 2×10240B，较此前 +4KB；彻底消除上限且省 SRAM 的更优方案为 O1（改为 PSRAM 指针，见 review 建议） | ✅ |
| S215 | **CameraStream 独立 capture task 架构** | 帧采集/编码/uORB 发布从 stream_handler (HTTP callback) 分离到独立 `_capture_task_fn` (FreeRTOS task)。即使无 HTTP 客户端，capture task 仍持续 DQBUF→encode→publish (`fps_stats`, `camera_frame`)→store shared JPEG，确保 ULog 录制模块始终能接收到 uORB topic。stream_handler 变为纯消费者: `_frame_ready_sem` (counting sem, max 2 clients) → `_shared_jpeg_buf` (PSRAM, mutex) → `_send_mjpeg_part`。`_frame_generation` atomic counter 防止重复帧。移除 `/api/capture_image` 端点及 `_save_jpeg_snapshot`/`_last_jpeg_buf` 相关代码。JPEG encoder 改为 start() 时直接初始化 (不再 lazy init)。 | ✅ |
| S216 | **SRAM 优化: 禁用 EAP + DVP** | ① `CONFIG_ESP_WIFI_REMOTE_EAP_ENABLED=n` — 项目仅用 WPA2-PSK，EAP 未使用，禁用后节省 ~1.1 KB IRAM + ~21 KB PSRAM 代码 (tfpsacrypto) ② `CONFIG_ESP_VIDEO_ENABLE_DVP_VIDEO_DEVICE=n` — 此板仅用 MIPI CSI，DVP 驱动未使用。DIRAM 总节省 1,114 bytes (1.1 KB)，External RAM 节省 21,600 bytes (21.1 KB) | ✅ |
| S289 | **SRAM 优化: BSS 大对象迁移至 PSRAM (P0+P1)** | `s_agent_msgs[16]` (74.9KB) text 字段 PSRAM 动态分配 + `s_topics/s_subs/s_sub_free_list` (5.6KB) PSRAM calloc + `s_jobs[16]` cap_lua_async (5.6KB) PSRAM calloc + `s_mgr` claw_agent_mgr (5.0KB) PSRAM calloc。DIRAM 使用 227KB(51.5%)→136KB(30.8%), 释放 91.1KB 内部 SRAM, 可用 heap 207KB→298KB | ✅ |
| S217 | **Camera Frame Recording 绑定 Camera Stream** | 移除 Web UI 和 API 的独立 "Camera Frame Recording" 按键 (HTML card + JS + `/api/camera_record` POST/GET/OPTIONS 端点)，改为 Camera Stream enable/disable 自动控制 recording: `CameraStream::start()` 自动 `_recording_enabled=true`，`stop()` 已有 `_recording_enabled=false`；Web `camera_stream_handler` enable 时 `set_recording(true)`，disable 时先 `set_recording(false)` 再 stop。`/api/status` 新增 `cam_recording` 字段，Web UI cam_status 显示 "Streaming + Recording" 状态。`max_uri_handlers` 30→27 (移除 3 个 camera_record handler) | ✅ |
| S218 | **ULog 自动启动 (WiFi+SNTP)** | SNTP 初始化从 `PhoneAppSettings` 移至 `web_config_server` (两块板都运行，WIFI6 无 LCD 不启动 Settings App)；WiFi 连接后 web_config_task 启动 SNTP，SNTP 同步回调中 `ulog_writer_set_wall_clock(true)` + 检查 `ULOG_STATE_IDLE` 才 `ulog_writer_start()` (已录制时跳过)；Web/Flutter Start/Stop 控制保留 (手动 Stop 后不会自动重启，手动 Start 后 SNTP 同步不重复启动) | ✅ |
| S219 | **CameraStream model load task 早期退出信号** | `_model_load_task_fn` 中 COCODetect 创建失败时直接 `vTaskDelete(NULL)` 退出，未设置 `_model_load_task_exited` 或清除 `_model_load_task`，导致 `_deinit_detection()` 挂起 15s 后对已回收句柄调用 `vTaskDelete()`。修复: 早期退出路径添加 flag 清理 + `_detector` 改用显式 `.store()` + `_detect_results.clear()` 加 `_detect_mutex` 保护 | ✅ |
| S220 | **s_sntp_synced volatile→atomic** | `s_sntp_synced` 从 SNTP 回调 (lwIP tcpip task, 任意 core) 写入，从 `web_config_task` 读取。`volatile` 不保证双核原子性和内存序。修复: 改为 `std::atomic<bool>` + `.store(release)`/`.load(acquire)` | ✅ |
| S221 | **s_audio_task TaskHandle_t→atomic** | `s_audio_task` 由 audio_task (core 0) 写入，HTTP handler (任意 core) 读写，无同步。修复: 改为 `std::atomic<TaskHandle_t>` + 显式 load/store/exchange | ✅ |
| S222 | **_wifi_scan_task TaskHandle_t→atomic** | `_wifi_scan_task` 由 scan task (任意 core) 写入，UI/析构 (core 1) 读写，无同步。修复: 改为 `std::atomic<TaskHandle_t>` + 显式 load/store/exchange | ✅ |
| S223 | **PhoneAppAudio _task_handle→atomic** | `_task_handle` 由 audio task (core 0) 写入，析构/close (任意 core) 读取，无同步。修复: 改为 `std::atomic<TaskHandle_t>` + 显式 load/store/exchange | ✅ |
| S224 | **AudioDriver gpio_config 返回值检查** | `gpio_config(&pa_conf)` 返回值未检查，PA GPIO 配置失败时静默无输出。修复: 添加返回值检查 + 错误日志 | ✅ |
| S225 | **Logger vTaskDelete 过期句柄** | `logger_deinit()` 检测到 `writer_exited` 后立即 `vTaskDelete(s_log.writer_task)`，但 writer 可能刚自删而 idle task 尚未回收 TCB。修复: 添加 10ms yield + 重新检查 `writer_exited` 后再决定是否 force-kill | ✅ |
| S226 | **g_has_lcd volatile→atomic** | `g_has_lcd` 在 `app_main` (core 0) 写入，`web_config_server` task (任意 core) 读取。`volatile` 不保证双核内存序。修复: 改为 `std::atomic<bool>` + `.store(release)`/`.load(acquire)` | ✅ |
| S227 | **_wifi_connect_task TaskHandle_t→atomic** | `_wifi_connect_task` 由 connect task (任意 core) 写入，UI (core 1) 读取，无同步。修复: 改为 `std::atomic<TaskHandle_t>` + 显式 store(release)，匹配 S221/S222/S223 模式 | ✅ |
| S228 | **PeripheralManager _has_lcd→atomic** | `PeripheralManager::_has_lcd` 为普通 `bool`，跨核读写无同步。修复: 改为 `std::atomic<bool>` + `.store(release)`/`.load(acquire)`，匹配 AudioDriver/SDCardDriver 模式 | ✅ |
| S229 | **s_rec_pub orb_advert_t→atomic** | `s_rec_pub` 为普通 `orb_advert_t`，懒初始化。修复: 改为 `std::atomic<orb_advert_t>` + 显式 load/store，匹配项目中所有其他 publisher handle 模式 | ✅ |
| S230 | **h_play __audio_init 锁顺序** | `h_play()` 在 `audio_lock()` 之前调用 `__audio_init()`，并发请求可竞态 `s_audio_inited`。修复: 将 `audio_lock()` 移到 `__audio_init()` 之前，匹配 `h_rec_start` 锁序 | ✅ |
| S231 | **h_rec_stop s_rec_path 竞态** | `s_rec_path` 在 `audio_unlock()` 之后读取，并发 `h_rec_start` 可覆写。修复: 在释放锁前复制到本地缓冲区 | ✅ |
| S232 | **ULog writer task 内部 SRAM 不足** | `xTaskCreate` 从内部 SRAM 分配 8KB 栈，LVGL+LWIP 占用后不足导致创建失败。修复: 改用 `xTaskCreateStatic` + PSRAM 栈 + 内部 SRAM TCB，匹配 audio_task/model_load_task 模式 | ✅ |
| S233 | **h_rec_stop s_rec_path 拷贝位置错误** | S231 修复将 `strlcpy(saved_path)` 放在 `audio_unlock()` 之后，仍存在竞态。修复: 移到 `audio_unlock()` 之前 | ✅ |
| S234 | **ULog writer TCB 释放竞态** | writer task 自删后 `vTaskDelete(NULL)` 将 TCB 加入终止链表，立即 `heap_caps_free(task_tcb)` 可能破坏内核链表。修复: TCB 在 `init()` 预分配，stop 时只释放栈，`deinit()` 时释放 TCB。统一所有 `xTaskCreateStatic` 模块为相同模式 (PhoneAppAudio, web_config_server, CameraStream) | ✅ |
| S235 | **LLM API key 明文暴露** | `GET /api/llm/config` 返回完整 `api_key` 值，LAN 客户端可通过未加密 HTTP 获取 LLM API 密钥。修复: 改为 `has_api_key` 布尔标志，匹配 WiFi 密码保护模式 (S15) | ✅ |
| S236 | **IM/LLM handler Content-Length 无验证** | `h_llm_config_set`/`h_feishu_config`/`h_qq_config`/`h_tg_config` 直接 `calloc(req->content_len + 1)` 无大小验证，恶意客户端可发送 Content-Length: 4294967295 耗尽 PSRAM 导致 OOM 崩溃。修复: 添加 4096 字节上限检查，匹配 `h_files_delete_batch` 模式 | ✅ |
| S237 | **CameraStream _detector atomic UAF** | `_deinit_detection()` 先 `delete _detector` 再 `_detector = nullptr` 两步操作，并发读者可看到悬空指针。修复: 使用 `exchange(nullptr)` 原子置空后再释放，防止任何读者观察到悬空指针 | ✅ |
| S238 | **WeChat/LLM handler cJSON NULL + token 暴露** | 9 个 HTTP handler 将 `cJSON_PrintUnformatted()` 结果直接传给 `httpd_resp_sendstr()` 无 NULL 检查，OOM 时 NPE 崩溃。同时 `h_wx_login_status` 返回明文 WeChat auth token。修复: 全部添加 NULL 检查 + HTTP 500 fallback + `free()`→`cJSON_free()`；token 改为 `has_token` 布尔标志 | ✅ |
| S239 | **PhoneAppCamera _v4l2_buf_count 未截断** | `VIDIOC_REQBUFS` 返回的 `req.count` 直接赋值给 `_v4l2_buf_count`，未截断到固定数组大小 2。V4L2 驱动返回 >2 时，后续循环越界写入 `_v4l2_buffers[]`/`_v4l2_buf_len[]` 导致栈/堆损坏。修复: 添加截断检查，匹配 CameraStream 已有的相同防护 (S196) | ✅ |
| S240 | **PhoneAppAudio _stop_recording UAF** | `_stop_recording()` 用 `vTaskDelay(200ms)` 等待音频任务退出录制块后释放 `_pcm_buffer`/`_encoder`，但系统负载高时任务可能被抢占超过 200ms，导致 use-after-free。修复: 新增 `_recording_ops_in_flight` 原子计数器，任务进入录制块递增/退出递减，`_stop_recording()` 轮询等待计数归零 (最长 1s)，匹配 AudioDriver `_codec_ops_in_flight` 模式 (S182) | ✅ |
| S241 | **uORB 订阅者槽位泄漏 (xQueueCreate 失败)** | `orb_subscribe()` 消耗槽位后若 `xQueueCreate()` 失败 (OOM)，槽位永久丢失。修复: 失败时将槽位归还 free-list 或回退 `s_num_subs`，防止内存压力下耗尽 ORB_MAX_SUBS 池 | ✅ |
| S242 | **ULog 订阅泄漏 (start 失败)** | `ulog_writer_start()` 订阅所有 topic 后若写入/task 创建失败，订阅永不释放；`stop()` 仅处理 RUNNING 态，重试循环可耗尽 256 订阅者槽位。修复: `stop()` 改为仅跳过 IDLE 态，ERROR 态也执行完整清理 (unsubscribe + close + free) | ✅ |
| S243 | **orb_publish 无锁投递 use-after-free** | `orb_publish()` 快照订阅者后释放锁，并发 `orb_unsubscribe()` 可 `vQueueDelete` 释放队列，投递时写入已释放内存。修复: 整个 publish (查找 topic + 投递) 全程持锁，xQueue 操作非阻塞锁时间极短 | ✅ |
| S244 | **AudioDriver codec I2C 控制句柄 NULL** | `audio_codec_new_i2c_ctrl()` 分配失败可返回 NULL，但三个调用点 (DAC ctrl/ADC ctrl/WIFI6 ES8311 ctrl) 均未检查，传入 codec 构造函数导致崩溃。修复: 三处均添加 NULL 检查，失败时设 `init_ok=false` 跳过 codec 创建 | ✅ |
| S245 | **CameraStream PSRAM 缓冲区 free/alloc 不匹配** | 析构函数用 `free()` 释放 `_shared_jpeg_buf` (由 `heap_caps_realloc(..., MALLOC_CAP_SPIRAM)` 分配)，API 不匹配。修复: 改用 `heap_caps_free()` 匹配分配 API，避免分配器实现变更风险 | ✅ |
| S246 | **max_uri_handlers 不足 (ESP-Claw IM)** | 添加 WeChat (8 handlers) + LLM (3) + TG (2) 后实际需 40 个 handler slot，但 `max_uri_handlers` 仅设 29/30，导致 10+ 个 handler 注册失败 (`no slots left`)。修复: 29→42 / 30→42，覆盖 5 core + 6 audio + 4 file mgr + 5 CORS + 5 ULog + 2 system + 8 WeChat + 3 LLM + 2 TG + 2 spare | ✅ |
| S247 | **Camera Stream + Music 播放卡顿 (P0 回归)** | 提交 65c36ad 将帧采集/编码循环从 httpd stream_handler (绑定 Core 0, S176) 抽离为独立 `cam_capture` 任务，但错误地将其绑定到 **Core 1, priority 5**。Music 的 GMF/ASP 任务固定运行在 **Core 1, priority 3** (`phone_app_music.cpp`)，capture 任务 (DQBUF→PPA DMA→HW JPEG 编码→NPU 推理) 持续运行并以更高优先级抢占音乐解码器，导致 I2S TX DMA 欠载 → 音乐卡顿。v0.0.3 正常是因为采集循环在 Core 0 的 httpd 上下文中运行，Core 1 仅 LVGL(prio4)+Music ASP(prio3)，音乐从不被摄像头工作抢占。修复: 将 `cam_capture` 改绑 **Core 0** (恢复 v0.0.3 核亲和性)，不再与 Core 1 的 Music ASP 竞争 | ✅ (硬件验证: 固定频率卡顿已消失；但仍有偶发杂音/轻微卡顿，见 S248) |
| S248 | **Camera Stream + Music 偶发杂音/轻微卡顿 (S247 残留)** | 固定频率卡顿修复后 (S247)，硬件验证发现**偶发**杂音并轻微卡一下 (非周期、低频)。根因疑似 **跨核共享 PSRAM 总线带宽 + 共享 L2 cache 竞争** (非同核调度抢占): Core 0 的 `cam_capture`(prio5) 每帧产生大量 PSRAM 流量 (CSI DMA→PPA DMA→HW JPEG DMA→SDIO WiFi DMA + 每3帧 NPU 推理尖峰)，与 Core 1 的 Music I2S TX DMA (从 PSRAM 读 PCM) 争用同一外部 PSRAM 总线/L2 cache → 短时欠载。S249 将背景/轻量任务重新绑核验证调度抢占已消除。最终解决: S289 SRAM 优化将 91.1KB 的 BSS 大对象 (agent_msgs/uORB/lua_jobs/agent_mgr) 从内部 SRAM 迁移至 PSRAM，DIRAM 使用从 51.5%→30.8%，释放足够内部 SRAM 用于关键 DMA 缓冲，消除 PSRAM 总线争用瓶颈 | ✅ (S289 SRAM 优化) |
| S249 | **Web 设置 WiFi 字段完整性校验** | 需求: WiFi 子设置 **SSID + password 必须同时非空** (不考虑开放网络)。`settings_handler` 现校验: 若请求携带任意 WiFi 字段 (ssid/pass) 但 SSID 或 password 为空/缺失，则 **skip 整个 WiFi NVS 更新** (含连接尝试)，返回 `wifi_connected:false`；volume-only 请求 (无 WiFi 字段) 不触碰 WiFi。仍保持 **连接成功才写 NVS** (规则 #2)。`PhoneAppSettings::wifiConnectTaskHandler` 同步: SSID 或 password 任一为空则跳过连接且不写 NVS | ✅ |
| S250 | **子设置字段完整性校验 (LLM / IM)** | 需求: 每个子 Setting 的**所有字段必须非空**，任一为空则 **skip 整个子设置**的 NVS 更新，其余满足条件的子设置正常更新。`h_llm_config_set` 原逻辑逐字段独立写入，可保存不完整配置 (如仅有 provider 无 api_key)。修复: ① LLM 子设置 `{provider, api_key, model, base_url}` 全字段非空才整体写入 NVS，否则 `ok:false` 跳过；② Feishu/QQ 子设置 `{app_id, app_secret}` 全非空才写入；③ Telegram 子设置 `{token}` 非空才写入。各子设置独立校验，互相不干扰 | ✅ |
| S251 | **WeChat QR login TLS 握手失败 (-0x7200)** | `mbedtls_ssl_handshake` 连接 `ilinkai.weixin.qq.com` 返回 `MBEDTLS_ERR_SSL_INVALID_RECORD` (-0x7200)。根因: `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=4096` 过小，WeChat/Tencent 证书链 (leaf ~3.6KB + intermediate ~2KB) 在 TLS Certificate 消息中超过 4KB，触发 `ssl_msg.c:4013` 的 `rec->data_len > MBEDTLS_SSL_IN_CONTENT_LEN` 检查。即使启用 `CONFIG_MBEDTLS_DYNAMIC_BUFFER`，该硬编码上限仍生效。修复: ① 启用 `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y` — 握手时按需分配大缓冲区，握后释放/缩为静态；② 启用 `CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA=y` + `CONFIG_MBEDTLS_DYNAMIC_FREE_CA_CERT=y` — 握手后释放私钥/CA 证书内存；③ `SSL_IN_CONTENT_LEN` 从 4096→8192 — 覆盖 WeChat/Tencent 等大证书链服务器 | ✅ |
| S252 | **WeChat QR 码不显示** | 原 `<img id="wx_qr_img">` 直接设置 `src=qr_data_url`，但 WeChat API 返回的 `qr_data_url` 是 HTML 页面 (`liteapp.weixin.qq.com`)，不是图片 URL，导致 `<img>` 无法渲染。修复: ① `<img>` → `<canvas id="wx_qr_cv">`；② 内嵌 lean-qr v2.7.2 (MIT, nano build ~3.7KB) — 与 esp-claw 上游使用相同库；③ `qrGen(url)` 调用 `lean-qr.generate(url).toCanvas(canvas)` 将 URL 编码为 QR 码渲染到 canvas，微信扫码即可打开链接 | ✅ |
| S253 | **AudioDriver ESP_ERROR_CHECK I2S abort** | `AudioDriver::init()` 对 5 个 I2S API 调用使用 `ESP_ERROR_CHECK()` (i2s_new_channel, i2s_channel_init_std_mode x2, i2s_channel_enable x2)，失败时调用 `abort()` 崩溃设备。修复: 替换为显式返回值检查 + ESP_LOGE + 清理已创建资源 + early return，匹配函数内已有的 codec 失败 graceful rollback 模式 | ✅ |
| S254 | **PhoneAppSettings assert(sta_netif)** | 两处 `assert(sta_netif)` / `assert(sta)` 检查 `esp_netif_create_default_wifi_sta()` 返回值。`assert()` 在 release 构建 (NDEBUG) 中被移除，允许 null 指针静默传播。修复: 替换为显式 null 检查 + ESP_LOGE + graceful return | ✅ |
| S255 | **CameraStream _encoder_handle HTTP handler 竞态** | `set_quality_handler` 和 `set_camera_config_handler` 读取 `_encoder_handle` 并调用 `example_encoder_set_jpeg_quality()` 未获取 `_encoder_sem`，与 capture task 并发编码竞态。修复: 检查 `_encoder_initialized.load(acquire)` 后获取 `_encoder_sem` 再调用 `set_jpeg_quality()`，序列化硬件寄存器写入 | ✅ |
| S256 | **s_audio_inited/s_sntp_initialized 数据竞态** | `s_audio_inited` 和 `s_sntp_initialized` 为普通 `bool`，从多个执行上下文 (HTTP handler/任意 core, audio_task, lwIP tcpip task, web_config_task) 无同步读写，C++11 数据竞争未定义行为。修复: 改为 `std::atomic<bool>` + `.store(release)`/`.load(acquire)` | ✅ |
| S257 | **CameraStream cJSON_Print free→cJSON_free** | `camera_info_handler()` 用 `free()` 释放 `cJSON_PrintUnformatted()` 返回的字符串，违反 cJSON API 契约。若 cJSON 配置自定义分配器，`free()` 会破坏堆。修复: 改用 `cJSON_free()`，匹配项目中所有其他 27 处调用 | ✅ |
| S258 | **PhoneAppSettings _wifi_initialized 数据竞态** | `_wifi_initialized` 为普通 `bool`，从 `bootWifiAutoConnect()` (app_main, core 0) 和 `wifiInit()` (wifiScanTaskHandler, core 1) 无同步读写。修复: 改为 `std::atomic<bool>` + `.store(release)`/`.load(acquire)` | ✅ |
| S259 | **vTaskDelete(nullptr) 竞态 (CRITICAL)** | `vTaskDelete(handle.exchange(nullptr))` 模式在任务自删后 `exchange()` 返回 nullptr → `vTaskDelete(nullptr)` 删除调用者自身 (LVGL task 或析构 callers)。PhoneAppAudio (2 处) + PhoneAppSettings (2 处) 全部修复: 先 exchange 捕获值再判空调用 vTaskDelete | ✅ |
| S260 | **HTTP 错误响应缺失 (HIGH)** | h_llm_config_set/h_feishu_config/h_qq_config/h_tg_config 在 calloc/recv/cJSON_Parse 失败时 return ESP_FAIL/ESP_ERR_NO_MEM 未发送 HTTP 响应 → 客户端 TCP 连接保持但无响应，挂起直至超时。修复: 12 处早期返回前添加 httpd_resp_send_err | ✅ |
| S261 | **s_task_handle 非原子 (MEDIUM)** | web_config_server 中 s_task_handle 为普通 TaskHandle_t，web_config_task (core 1) 写入，web_config_server_stop() (任意 core) 读取，无同步。修复: 改为 `std::atomic<TaskHandle_t>`，xTaskCreatePinnedToCore 改用临时变量后原子存储 | ✅ |
| S262 | **Logger vsnprintf 未定义行为 (MEDIUM)** | ESP-IDF v6.x 将完全格式化日志行传给 vprintf hook (含时间戳/level/tag)，`vsnprintf` 将其作为格式字符串再格式化 → 若日志消息含 `%` 字符 (如 "CPU: 50%") 触发 UB。修复: 改用 `strlcpy` 直接复制，无需再格式化 | ✅ |
| S263 | **QR fetch_code 网络未就绪误判为致命错误 (HIGH)** | `cap_im_wechat_qr_task` 中两处 `cap_im_wechat_qr_fetch_code_locked()` 调用 (TTL 过期刷新 + poll 超时刷新) 将 `ESP_ERR_NOT_FOUND` (DNS/SNTP 未就绪) 当作致命错误处理 — 停用 QR 并退出任务。而 poll 路径已正确处理 `ESP_ERR_NOT_FOUND` 为瞬态重试。修复: 两处 fetch_code 路径添加 `ESP_ERR_NOT_FOUND` 特殊处理 — 回退 `refresh_count++` (因刷新未实际完成) + 2s 延迟重试，匹配 poll 路径模式 | ✅ |
| S264 | **s_pcm_count 数据竞态** | `s_pcm_count` 为普通 `int`，audio_task 无锁 read-modify-write (`++s_pcm_count`/`s_pcm_count=0`)，httpd handler 在 audio_mutex 下重置为 0。双核 ESP32-P4 上为真实数据竞态。修复: 改为 `std::atomic<int>` + `fetch_add(relaxed)`/`store(0, relaxed)` | ✅ |
| S265 | **s_rec_start_ms 非原子** | `s_rec_start_ms` 为普通 `uint32_t`，httpd task 写入，h_rec_status/h_rec_stop 读取。虽在 audio_mutex 下安全，但与项目 atomic 约定不一致。修复: 改为 `std::atomic<uint32_t>` + 显式 load/store | ✅ |
| S266 | **__sync_synchronize 冗余** | audio_task 退出时 `__sync_synchronize()` + `s_audio_task.store(release)`，GCC builtin 非标准 C++ 且 release store 已提供所需内存序。修复: 移除冗余 `__sync_synchronize()` | ✅ |
| S267 | **audio_task calloc→heap_caps_calloc** | `audio_task` 用 `calloc()` 分配 1920B 缓冲区，PhoneAppAudio 用 `heap_caps_calloc(PSRAM)`。不一致，浪费内部 SRAM。修复: 改用 `heap_caps_calloc(PSRAM)` + `heap_caps_free()` | ✅ |
| S268 | **WiFi scan task vTaskDelete 持锁风险** | `close()`/析构用 `vTaskDelete()` 强杀 scan task，但 `scanWifiAndUpdateUi()` 持有 `bsp_display_lock`，强杀可孤儿化锁导致 UI 死锁。修复: 添加 `_wifi_scan_exit` atomic flag 信号任务退出循环并自删，等待 1s 后 fallback vTaskDelete | ✅ |
| S269 | **CameraStream isRunning/is_recording 隐式 atomic** | `isRunning()`/`is_recording()` 用 `std::atomic<bool>` 隐式转换，不符合项目显式 `.load(acquire)` 约定。修复: 改为 `.load(acquire)` | ✅ |
| S270 | **AudioDriver volume() 隐式 atomic** | `volume()` 用 `std::atomic<int>` 隐式转换，不符合项目显式 `.load()` 约定。修复: 改为 `.load(relaxed)` | ✅ |
| S271 | **ulog_writer ringbuf volatile→atomic + free→heap_caps_free** | ringbuf `write_pos`/`read_pos` 为 `volatile size_t`，双核 ESP32-P4 上 volatile 不保证原子性和内存序。`data_buf`/`drain_buf` 用 `heap_caps_malloc(PSRAM)` 分配但 `free()` 释放。修复: 改为 `std::atomic<size_t>` + 显式 load/store + release/acquire 内存序；`free()`→`heap_caps_free()`；`memset(&ringbuf)`→显式字段初始化 (std::atomic 成员不可 memset) | ✅ |
| S272 | **uorb assert→explicit checks + orb_copy use-after-free** | `orb_init()` 用 `assert()` 检查 mutex 状态，release 构建被移除导致 NULL mutex 解引用。`orb_copy()`/`orb_check()` 无锁访问 subscriber entry，并发 `orb_unsubscribe()` 可在 topic_idx 检查和 xQueueReceive 间删除队列 → use-after-free。修复: assert→显式 null 检查 + ESP_LOGE；orb_copy/orb_check 持锁获取 queue 指针后释放锁再操作，generation counter 防止 ABA | ✅ |
| S273 | **JPEG encoder ref count 竞态 + free→heap_caps_free** | `s_jpeg_hw_ref_count`/`s_jpeg_hw_handle` 为普通变量，check-then-act 模式 (check NULL→create→assign) 为 TOCTOU 竞态。`jpeg_alloc_encoder_mem` 输出缓冲用 `free()` 释放。修复: 添加 `s_jpeg_hw_mutex` 保护共享状态 + double-check after lock；`free()`→`heap_caps_free()` | ✅ |
| S274 | **example_video_init s_is_init 竞态** | `s_is_init` 为普通 `bool`，并发 `example_video_init()` 调用可同时看到 false 并双重初始化视频系统。修复: 添加 `s_init_mutex` 保护 init/deinit 状态转换 | ✅ |
| S275 | **esp_lvgl_port assert→null checks + task_notify NULL guard** | `lvgl_port_lock/unlock` 用 `assert(lvgl_mux)` 检查，release 构建被移除导致 NULL mutex 解引用。`lvgl_port_task_notify` 未检查 `lvgl_task` 为 NULL，deinit 后 ISR 调用崩溃。修复: assert→null 检查 + ESP_LOGE + early return；task_notify 添加 NULL guard | ✅ |
| S276 | **esp_lvgl_port_disp assert→null checks + free→heap_caps_free** | `io_handle`/`ppa_handle` 用 `assert()` 检查，release 构建被移除。`draw_buffs`/`oled_buffer`/`disp_ctx` 用 `heap_caps_malloc`/`heap_caps_aligned_alloc` 分配但 `free()` 释放。修复: assert→null 检查 + error return/goto；`free()`→`heap_caps_free()` | ✅ |
| S277 | **lcd_ppa assert→null checks + free→heap_caps_free** | `cfg`/`buffer`/`ppa_ctx`/`rotate_cfg` 用 `assert()` 检查，release 构建被移除。`ppa_ctx->buffer` 用 `heap_caps_aligned_calloc` 分配但 `free()` 释放。修复: assert→null 检查 + ESP_LOGE + early return/ESP_ERR_INVALID_ARG；`free()`→`heap_caps_free()` | ✅ |
| S278 | **WeChat session timeout 后重启仍需重新扫码** | WeChat iLink 服务器 `getupdates` 返回 `errcode:-14, "session timeout"` 后，bot_token 服务端失效。虽然 token 已持久化到 NVS 并在重启时加载，但服务端不再接受该 token，导致每次重启后 poll 循环反复报错重试。修复: ① `cap_im_wechat_poll_once()` 检测 errcode=-14，标记 `configured=false` 停止轮询，清除 NVS 中过期 token；② poll_task 对 `ESP_ERR_INVALID_STATE` (session expired) 不重试；③ Web UI 添加 `checkWxSession()` 周期性检查 `configured` 状态，session 过期时显示 "⚠️ Session expired — re-scan QR"；④ `h_wx_login_status` 响应中添加 `configured` 字段 | ✅ |
| S281 | **Agent Chat 在 LLM 配置保存后失效 (CRITICAL)** | 根因: `h_llm_config_set` 仅写 NVS 未调用 `claw_agent_mgr_update_core_config()`，运行中的 agent 继续使用启动时的旧配置。冷启动路径未检查 `claw_agent_mgr_init`/`create_root_agent` 返回值，LLM 配置为空时静默失败。热初始化路径无条件重新初始化已运行的 event router 和 agent manager (`ESP_ERR_INVALID_STATE`)。共 5 个子 Bug: ① `max_tokens_field = "4096"` — JSON key name 被误设为数值，OpenAI-compatible 后端生成 `{"4096": 8192}` 而非 `{"max_tokens": 4096}`。② `supports_tools = false` 硬编码 — agent context providers 仍加载 14KB 工具定义，后端对非空 `tools_json` 返回 `ESP_ERR_NOT_SUPPORTED`，所有请求失败。③ `h_llm_config_set` 配置更新缺少 `supports_tools`/`supports_vision`/`timeout_ms`，`claw_agent_mgr_update_core_config` 全量结构体覆写将运行中 agent 的这些字段降级为 false/0。④ `h_agent_chat` 未设 `event.chat_id`，agent 回复的 `out_message` 事件因 `chat_id` 为空被 `build_out_message_event_common` 拒绝 (`ESP_ERR_INVALID_ARG`)。⑤ 冷/热初始化路径缺少 `system_prompt = ""` (被 `claw_agent_mgr_copy_core_config` 拒绝)。修复: `h_llm_config_set` 保存后调用 `update_core_config` + 懒创建 root agent；冷/热初始化路径设 `supports_tools = true`；`h_agent_chat` 设 `chat_id = "web_chat"`；`mgr_cfg` 改用 `memset` 避免 C++ 指定初始化器字段顺序问题；热初始化路径探测 event router/agent manager 状态跳过已初始化的组件 | ✅ |
| S282 | **xTaskCreateStatic 栈缓冲 4× 欠分配** | `phone_app_audio.cpp`/`web_config_server.cpp`(`w_audio` 任务)/`components/ulog/ulog_writer.cpp`(ULog writer 任务) 用 `heap_caps_malloc(N, ...)` 分配 PSRAM 栈，但 `xTaskCreateStatic(PinnedToCore)` 的 `ulStackDepth` 单位为 `StackType_t` **字**，内核期望 `N×4` 字节，实际只分配 `N` 字节 → 内核向相邻 PSRAM 越界写入 (静默堆损坏/偶发崩溃)。`camera_stream.cpp:241` 早已用 `8192 * sizeof(StackType_t)` 正确。修复: 三处 malloc 大小乘 `sizeof(StackType_t)`，深度值(字数)不变 | ✅ |
| S283 | ~~h_files_list opendir 失败 cJSON 泄漏~~ (误报，已撤销) | 审查误判: `root` 实际在 `opendir()` 成功之后 (L1667) 才创建，`opendir()` 失败路径根本无 cJSON 对象存在，不存在泄漏。原修复引用了未声明的 `root` 导致编译失败，已 `git revert` | ❌ 撤销 |
| S284 | **h_llm_config_set 错误路径缺失 HTTP 响应** | OOM (`cJSON_CreateObject` 失败) 与 `nvs_open` 失败两路径 `return ESP_FAIL/ESP_ERR_NO_MEM` 但未 `httpd_resp_send_err`，keep-alive 连接半开，客户端 (fetch) 挂起至超时。修复: 两路径 `httpd_resp_send_err(HTTPD_500_INTERNAL_SERVER_ERROR, ...)` 后再返回，符合项目"每个 handler 路径必须响应"规则 | ✅ |
| S285 | **ULog writer 自删前未置 task_exited** | `writer_task_func` 在 `data_buf` 分配失败时 `vTaskDelete(NULL)` 自删，但未置 `writer->task_exited`，导致 `ulog_writer_stop()` 等待超时后 `vTaskDelete` 已被 idle task 回收的 TCB (UB/损坏)。修复: 自删前 `task_exited.store(true)`，与正常退出路径一致 | ✅ |
| S286 | **CameraStream stop() force-kill 句柄失效** | `stop()` 在 5s 等待前捕获 capture task 句柄，等待期间若任务自删 (已清空 `_capture_task`)，则对已被回收的 TCB 调用 `vTaskDelete` (双重删除损坏)。修复: 用 `_capture_task.exchange(nullptr)` 原子独占句柄，`if (t) vTaskDelete(t)`，匹配 `start()` 失败路径模式 | ✅ |
| S287 | **CameraDriver::release(nullptr) 绕过 owner 检查** | `release()` 仅当 `caller_id && _owner_id` 均非 null 时才拒绝非 owner，null `caller_id` 会释放其他模块的 camera claim，破坏 owner-tracking 安全网。修复: `!caller_id || !_owner_id || strcmp(...) != 0` 均拒绝 | ✅ |
| S288 | **CameraStream 编码器生命周期竞态 (handler vs stop)** | `set_quality_handler`/`set_camera_config_handler` 先读 `_encoder_initialized` 再 `xSemaphoreTake(_encoder_sem)`/`set_jpeg_quality(_encoder_handle)`；`stop()`→`_deinit_encoder()` 在 handler 不持锁时删除 `_encoder_sem` 并置空 `_encoder_handle` → 并发请求可获取已释放信号量/空句柄 (hard fault)。修复: 新增 `_encoder_lock` (构造创建/析构释放)，`_deinit_encoder` 拆除时与两个 handler 的 check+use 互斥；`_encoder_sem` 为 null 时跳过内部 take (覆盖潜在 JPEG-sensor 路径) | ✅ |
| S289 | **Captive portal HTTP 未释放 port 80** | WiFi provisioning 启动 captive portal HTTP server (port 80)，STA 连接后仅停止 DNS 但未停止 HTTP server → CameraStream 启动 port 80 失败 (EADDRINUSE errno 112)。修复: STA 连接后同时停止 captive HTTP server，释放 port 80。**二次修复**: `httpd_stop()` 是阻塞调用 (等待 httpd 线程退出)，在 event loop task 中调用会阻塞事件循环 → 其他 httpd 实例的 `esp_event_post` 超时 (ESP_ERR_TIMEOUT 每 2s)。改用 `esp_timer` one-shot 延迟停止，从 timer 上下文调用 `httpd_stop()` | ✅ |
| S290 | **VFS FAT max_files 不足** | `esp_vfs_fat_sdspi_mount` 配置 `max_files=3`，ULog + logger + claw session 并发打开文件耗尽 → `open: no free file descriptors` (ENFILE)，claw session 回退默认 ID。修复: `max_files` 3→8，容纳 ULog(1-2) + logger(1) + session(1-2) + HTTP download(1) + audio(1) | ✅ |
| S291 | **h_play() / h_rec_start() TOCTOU 竞态 — I2S 共享冲突** | `h_play()` 和 `h_rec_start()` 为销毁旧播放器释放 `s_audio_mutex` 期间，另一方可启动冲突操作，导致录音和播放同时占用同一 I2S 硬件。修复: 重新获取锁后双方均二次检查对方状态标志 (`s_playing` / `s_is_recording`)，`h_rec_start()` 停止新启动的播放，`h_play()` 返回错误 | ✅ |
| S292 | **h_rec_start() 错误路径持锁调用 500ms delay** | `_stop_audio_task_if_running()` 在持 `s_audio_mutex` 时等待音频任务退出 (10×50ms)，阻塞其他 HTTP handler。修复: 3 个错误路径先将 `audio_unlock()` 移至 `_stop_audio_task_if_running()` 之前 | ✅ |
| S293 | **captive_dns s_dns_task 跨核数据竞争** | `s_dns_task` (TaskHandle_t) 无原子保护，DNS 任务写入 `NULL` 但 `captive_dns_start()` 从另一核读取时无内存屏障。修复: 改为 `_Atomic TaskHandle_t`，使用 `memory_order_release`/`memory_order_acquire` | ✅ |
| S294 | **httpd_register_uri_handler 返回值未检查** | 全部 ~51 次 `httpd_register_uri_handler()` 调用忽略返回值，handler 表满时端点静默返回 404。修复: `_register_web_config_uris()` 使用 `REG_URI` 宏，`_captive_httpd_start()` 使用内联检查，均 `ESP_LOGE` 记录失败 | ✅ |

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
| M8 | **异常告警 — 内存高占用** | Internal SRAM 或 PSRAM 使用率 > 85% (原 80%，提升以减少误报) 时发布 `system_alert` uORB (MEM_INTERNAL_HIGH / MEM_PSRAM_HIGH) | ✅ |
| M9 | **告警分级** | WARNING (阈值~+5%) / CRITICAL (阈值+10%) 两级严重度 | ✅ |
| M10 | **告警冷却** | 同类型告警最小间隔 30s (可配置)，防止告警洪泛 | ✅ |
| M11 | **告警 uORB 持久化** | `system_alert` 注册到 ULog writer，SD 卡 `.ulg` 文件记录告警事件 | ✅ |
| M12 | **告警 Web API** | `GET /api/system_alerts` 返回当前 CPU/内存/PSRAM 告警状态 + 阈值配置 | ✅ |
| M13 | **告警 Kconfig** | `CONFIG_APP_SYS_MONITOR_CPU_ALERT_PCT` (90%)、`MEM_ALERT_PCT` (85%)、`ALERT_COOLDOWN_S` (30) | ✅ |

---

## 3. 待完成需求

### 3.1 高优先级

| # | 需求 | 说明 | 阻塞因素 |
|---|------|------|----------|
| P0 | **Camera Stream + Music 播放卡顿** (S247) | Camera Stream 运行时 Music 播放卡顿。git bisect 定位根因为 commit 65c36ad: 独立 `cam_capture` 任务错误绑定 Core 1 (priority 5)，抢占 Core 1 上 priority 3 的 Music GMF/ASP 任务导致 I2S DMA 欠载。修复: `cam_capture` 改绑 Core 0 (恢复 v0.0.3 httpd 上下文核亲和性)。S247 固定频率卡顿已消除；S248 偶发杂音残留经 S289 SRAM 优化 (释放 91.1KB 内部 SRAM) 后已解决 | ✅ (已解决: S247 + S289) |
| P0b | **ESP-Claw 启动后 Music 播放异常** | 引入 ESP-Claw AI Agent 后 (commits 56fade6, 5e6d816)，Music 播放出现杂音/卡顿。已修复: ① S283: claw tasks 强制绑定 Core 0，防止抢占 Core 1 Music GMF/ASP ② S284: event_router 栈 8KB→16KB 防溢出 ③ S289: SRAM 优化释放 91.1KB 内部 SRAM (DIRAM 51.5%→30.8%)，消除 PSRAM 总线/L2 cache 带宽竞争导致的偶发杂音。硬件验证通过: Music 播放正常 | ✅ (已解决: S283 + S284 + S289) |
| P1 | **720×720 自定义样式表** | 当前使用默认回退方案，UI 一致性不佳 | 需设计 ESP-Brookesia 样式 |
| P2 | **WIFI6 无屏配网** | 首次启动 NVS 为空时需要配网方案 | esp-hosted SDIO 不支持稳定 SoftAP (#197) |
| PA | **ESP-Claw AI Agent 端到端验证** | ✅ 已验证: 15个设备 MCP tools (含 camera.capture_vision); Web Agent Chat UI (/api/agent/chat); Flutter Agent Chat 页面; LLM Vision 启用 (supports_vision=true)。硬件验证通过: Web Chat + WeChat IM → Agent → LLM (DeepSeek) → tool_call → 回复 (S281 修复后) | — |
| PB | **Flutter AI Agent UI** | ✅ 已完成: agent_chat_screen.dart — 聊天界面 + LLM 配置弹窗 + 消息气泡 (user/agent/tool) | — |
| PC | **Web Agent 对话界面** | ✅ 已完成: Web UI Agent Chat card + /api/agent/chat POST handler (claw_event_router) | — |
| PD | **Camera Vision 多模态** | ✅ 代码就绪: supports_vision=true + camera.capture_vision MCP tool; 实际 Vision 需 LLM 支持 image content | 需 Vision API 验证 |
| PE | **Audio Speech ASR/TTS** | 语音交互: 录音→ASR→LLM→TTS→播放 | 需 ASR/TTS 服务

### 3.2 中优先级

| # | 需求 | 说明 |
|---|------|------|
| P3 | **Camera App 录像功能** | 视频录制到 SD 卡（已通过 R22 ULog 录制部分实现） |
| P3b | **pyulog 兼容性修复** | ~~pyulog 1.2.3 在移除 trailing `_padding` 后计算 `max_data_size`，导致所有含 padding 的 topic 被标记为 corrupt。~~ ✅ 已修复：采用 PX4 `o_size_no_padding` 方案 — `orb_metadata` 新增 `o_size_no_padding` 字段，ULog writer DATA 消息写 `o_size_no_padding` 字节（不含尾部 padding），pyulog `max_data_size` 与 `o_size_no_padding` 一致 |
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

### 3.4 ESP-Claw 架构对齐 (G 系列)

> 参考上游: [esp-claw](https://github.com/espressif/esp-claw) — Espressif AI Agent Framework
> 详见 PROJECT.md §ESP-Claw 架构对比与补全计划

| # | 需求 | 说明 | 优先级 |
|---|------|------|:------:|
| G1 | **cap_im_local 集成** | 本地 IM 通道, 支持不依赖外部 IM 平台 (WeChat/TG/Feishu/QQ) 的 Web/Flutter Agent 对话。上游: `claw_capabilities/cap_im_local/` | ✅ 已完成 |
| G2 | **cap_llm_inspect 集成** | LLM 请求/响应检查 capability, 用于调试 Agent 行为。上游: `claw_capabilities/cap_llm_inspect/` | 中 |
| G3 | **wifi_manager 移植** | 独立 WiFi 管理组件 (AP+STA 模式 + 自动重连 + 状态回调), 解决 P2 无屏配网 + P10 WiFi 管理重构。上游: `common/wifi_manager/` | ✅ 已完成 (高) |
| G4 | **captive_dns 移植** | Captive Portal DNS, 配合 wifi_manager 实现无屏 WiFi 配网 (手机连接 AP 自动弹出配置页)。上游: `common/captive_dns/` | ✅ 已完成 (高) |
| G5 | **app_claw Shell 移植** | 模块化 capability/lua 注册 (app_capabilities.c + app_lua_modules.c + app_claw_cli.c), 替代 main.cpp 内联初始化。上游: `common/app_claw/` | 中 |
| G6 | **lua_modules 移植** | Lua 脚本子系统 — esp-claw 核心特性"对话即创建"。按需移植相关模块 (audio, camera, display, storage, system, json, thread, http_server, event_publisher, call_capability)。上游: `lua_modules/` (35 个模块) | 中 |
| G7 | **lua_module_builder** | Lua 模块文档/测试构建工具。上游: `common/lua_module_builder/` | 低 |
| G8 | **skill_builder** | Skill 编译工具。上游: `common/skill_builder/` | 低 |
| G9 | **display_arbiter** | 多显示客户端仲裁。上游: `common/display_arbiter/` | 低 |
| G10 | **emote** | 表情/情绪系统。上游: `common/emote/` | 低 |
| G11 | **esp_painter** | 图形绘画库。上游: `common/esp_painter/` | 低 |
| G12 | **esp_video** | 视频捕获组件 (当前使用 example_video_common)。上游: `common/esp_video/` | 低 |
| G13 | **Board Manager** | 结构化多板元数据 + 外设 YAML + setup code (当前用 GT911 I2C 探测)。上游: `application/edge_agent/boards/` | 低 |
| G14 | **FATFS Images** | 构建时 FATFS 镜像 (SYSTEM 只读 + DATA 种子), 用于 bundle skills/router_rules/recovery defaults。上游: `application/edge_agent/fatfs_image/` | 中 |
| G15 | **结构化 HTTP Server** | API 分域 + 嵌入式前端, 替代 web_config_server.cpp 内联 HTML。上游: `application/edge_agent/components/http_server/` | 中 |
| G16 | **内置 Skills 系统** | 创建设备专属 Skill 文档 (SKILL.md), 如 camera/audio/system monitor 使用指南。当前 claw_skill 已链接但无内置 skill | 中 |
| G17 | **Scheduler Rules** | 设置默认定时任务规则 (周期性系统统计、摄像头捕获计划等)。cap_scheduler 已链接但无默认规则 | 低 |
| G18 | **ASR/TTS 语音交互** | 语音交互: 录音→ASR→LLM→TTS→播放 (同 PE, 需外部 ASR/TTS 服务) | 中 |

### 3.5 集成优先级建议

```
P0 (阻塞)     → ✅ 已解决: S247 (cam_capture 绑核) + S289 (SRAM 优化 91.1KB)
高优先级       → G1 (cap_im_local) ✅ → G3 (wifi_manager) ✅ → G4 (captive_dns) ✅
中优先级       → G6 (lua_modules) → G16 (skills) → G5 (app_claw) → G15 (HTTP Server)
低优先级       → G2, G7-G14, G17 (按需集成)
```

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
| L7 | ~~**ESP-Claw 启动后 Music 播放异常**~~ ✅ 已解决 | 引入 ESP-Claw 后 (commits 56fade6, 5e6d816)，Music 播放出现杂音/卡顿。已完全修复: S283 (claw tasks 绑 Core 0) + S284 (event_router 栈 16KB) + S289 (SRAM 优化 91.1KB, 消除 PSRAM 总线竞争)。硬件验证通过: Music 播放正常 |
| L8 | **esp-dl mbedtls/sha256.h 兼容 shim** | ESP-IDF v6.x (mbedtls 4.x) 将 `sha256.h` 移至 `mbedtls/private/`，`managed_components/espressif__esp-dl` 未更新。当前方案: `main/compat/mbedtls/sha256.h` 转发头 + CMake include path。下次升级 esp-dl 后应移除 |

### 4.3 已知未修复问题（架构审查遗留）

| # | 严重度 | 问题 | 原因 |
|---|--------|------|------|
| K1 | ✅ 已修复 | **Camera 帧缓冲跨核并发** | 分配私有 `_detect_in_buf`, mutex 内 memcpy 后释放再推理 (S110) |
| K2 | 🟢 低 | Audio 无用的 PSRAM 分配 (1920B) | 开销极小 |
| K3 | 🟢 低 | Music 部分低优先级边界情况 | `_play()` 参数未验证、`run()` 中 UI 对象先于 player 创建等边界情况 |
| K4 | ✅ 已修复 | **Web 音频 Camera Stream 互斥已移除** | Camera (MIPI CSI) 和 Audio (I2S) 使用独立硬件，无需互斥。所有 `__cam_running()` 检查已移除 |
| K5 | 🟢 低 | CameraStream/web_config_server 大文件模块化 | 单文件超 1800/87KB，可拆分为独立模块 (V4L2Pipeline, AudioRecorder, WiFiManager 等)。部分已提取辅助方法 (S159) |
| K6 | 🟢 低 | Logger 重入风险 | esp_log_set_vprintf 回调中调用 ESP_LOG 可能递归 |
| K7 | ✅ 已修复 | Web `s_audio_running` 跨核竞态 | `volatile bool` → `std::atomic<bool>` (v2.4) |

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
| 2026-07-11 | **G3 wifi_manager 集成**: 移植 esp-claw 上游 `common/wifi_manager/` → `components/common/wifi_manager/`。新增 `WifiService` C++ facade (main/wifi_service.hpp/cpp) — 封装 wifi_manager C API, 自动发布 uORB wifi_state, 管理 SNTP 启动和 NVS 持久化。PhoneAppSettings 重构: 移除 bootWifiAutoConnect()/wifiInit()/wifiEventHandler()/wifiReconnectTimerCallback() (~200 行), scan/connect 改用 WifiService。web_config_server 重构: 移除 s_wifi_state_sub uORB 订阅, settings_handler WiFi connect 改用 WifiService::apply_sta_config()/wait_connected()。main.cpp boot WiFi 改用 WifiService::init()+start()。CMakeLists.txt 添加 EXTRA_COMPONENT_DIRS components/common, main REQUIRES wifi_manager。wifi_manager 配置: AP always on (ap_behavior=keep) + infinite auto-reconnect。idf.py build 验证通过 | | (`~/works/opensource/esp-claw`) 全部组件, 识别 18 个缺失项 (G1-G18)。PROJECT.md 新增 "ESP-Claw 架构对比与补全计划" 章节 (已集成组件清单 + 缺失组件表 + 架构差异对比 + 集成优先级)。PROJECT_REQUIREMENTS.md §3.4 新增 G 系列 TODO。已集成: 16/18 claw_capabilities, 7/7 claw_modules, 5/12 common, 15 device MCP tools, WeChat/Feishu/QQ/TG IM, Web+Flutter Agent Chat, LLM Vision。缺失关键项: cap_im_local (G1), wifi_manager+captive_dns (G3+G4, 解决 P2 无屏配网), lua_modules (G6, 35个模块), app_claw shell (G5), structured HTTP server (G15), FATFS images (G14), 内置 Skills (G16), ASR/TTS (G18) |
| 2026-07-12 | +S289+S290 **Captive portal 占用 port 80 + FAT max_files 不足**: WiFi provisioning captive HTTP server (port 80) 在 STA 连接后未停止，CameraStream 启动 port 80 失败 (EADDRINUSE)。修复: STA 连接后延迟停止 captive HTTP server (esp_timer one-shot，避免 httpd_stop 阻塞 event loop task)。VFS FAT `max_files=3` 不足 (ULog+logger+session 并发)，`open: no free file descriptors` 导致 claw session 回退默认 ID。修复: `max_files` 3→8 |
| 2026-07-11 | **P0/P0b/S248 关闭: SRAM 优化解决 PSRAM 带宽竞争**: commit 3dcdd25 (S289) 将 4 个 BSS 大对象 (agent_msgs/uORB/lua_jobs/agent_mgr, 共 91.1KB) 从内部 SRAM 迁移至 PSRAM，DIRAM 使用 51.5%→30.8%。释放足够内部 SRAM 用于关键 DMA 缓冲后，Camera Stream + Music 并发的 PSRAM 总线/L2 cache 带宽竞争消除，偶发杂音/卡顿彻底解决。P0/P0b/S248 状态更新为已解决；§3.5 集成优先级移除 P0 阻塞项 |
| 2026-07-11 | +S281 **Agent Chat 在 LLM 配置保存后失效 (CRITICAL)**: `h_llm_config_set` 仅写 NVS 未更新运行中 agent 的 core config，冷/热初始化路径多处缺陷导致 agent 无法启动/响应。7 个增量提交 squash 为一个: ① `h_llm_config_set` 保存后调用 `claw_agent_mgr_update_core_config` + 懒创建 root agent ② 冷初始化检查 `mgr_init`/`create_root_agent` 返回值 ③ 热初始化探测 event router/agent manager 状态跳过已初始化组件 ④ 修复 `max_tokens_field = "4096"` → `max_tokens = 4096` (JSON key name vs 数值混淆) ⑤ `supports_tools = false` → `true` (硬编码 false 导致所有含工具的请求被拒绝) ⑥ `h_agent_chat` 设 `chat_id = "web_chat"` (空 chat_id 导致 out_message 发布失败) ⑦ `h_llm_config_set` 补全 `supports_tools`/`supports_vision`/`timeout_ms` (缺失导致 update_core_config 全量覆写降级运行中 agent)。硬件验证通过: Web Chat + WeChat IM 双通道正常响应 |
| 2026-07-11 | +S282~S288 代码审查 (Round 6, 7 个 HIGH/MEDIUM): xTaskCreateStatic 栈缓冲 4× 欠分配 (S282: `phone_app_audio`/`web_config_server` `w_audio`/`ulog_writer` PSRAM 栈 malloc 乘 `sizeof(StackType_t)`，原仅分配字数对应字节导致内核越界写入相邻 PSRAM)，~~`h_files_list` opendir 失败 cJSON 泄漏 (S283 误报，已 revert: `root` 在 opendir 之后创建，无泄漏)~~，`h_llm_config_set` OOM/nvs_open 错误路径缺失 HTTP 500 响应 (S284)，ULog writer `data_buf` OOM 自删前未置 `task_exited` 致 stop() 强杀已回收 TCB (S285)，CameraStream `stop()` force-kill 用过期句柄 `vTaskDelete` 已回收 TCB (S286: 改 `exchange(nullptr)` 独占)，CameraDriver `release(nullptr)` 绕过 owner 检查 (S287)，CameraStream 编码器生命周期竞态 handler vs `stop()` (S288: 新增 `_encoder_lock` 互斥 `_deinit_encoder` 拆除与 handler 的 check+use)。`idf.py build` (IDF v6.0.2) 验证通过；S283 经构建验证为误报已 revert |
| 2026-07-11 | +S280 **ULog writer 自动启动竞态修复**: SNTP 同步早于 ULog 初始化完成时，`ulog_autostart_done` 在 state==UNINIT 时即被设为 true，导致 ULog 永远不会自动启动。修复: 仅在 state==IDLE (成功启动) 或 state!=UNINIT (无需重试) 时设置 ulog_autostart_done；state==UNINIT 时跳过本轮等待下一轮重试 |
| 2026-07-11 | +S279 **claw 组件 NVS 依赖解耦**: `components/claw/` 内 3 处直接依赖 `nvs_flash` (settings_store/cap_scheduler/cap_im_wechat)，违反组件应独立于外部存储的设计原则。参考 esp-claw 上游依赖反转模式修复: ① 新增 `claw_kv_backend` 纯接口组件 (函数指针表: init/get_str/set_str/has_key/erase_key/get_blob/set_blob/commit/deinit) ② 新增 `claw_kv_nvs` — 唯一依赖 nvs_flash 的 NVS 实现 ③ settings_store 重构为 backend 驱动 + 新增 blob API ④ cap_scheduler_store 替换 nvs_get_blob/set_blob/erase_key 为 backend 调用 ⑤ cap_im_wechat 替换 nvs_open/erase_key 为 backend 调用 (s_wechat_kv_backend) ⑥ main.cpp 创建 claw_kv_nvs 实例并注入各组件。二次修复: NVS handle 从共享 ctx 移至每操作局部变量，消除双核并发竞态；init() 返回值检查。构建通过，nvs_flash 依赖仅限 claw_kv_nvs 单一组件 |
| 2026-07-10 | +S263 **QR fetch_code 网络未就绪误判为致命错误 (CR)**: `cap_im_wechat_qr_task` 两处 `cap_im_wechat_qr_fetch_code_locked()` 调用将 `ESP_ERR_NOT_FOUND` (DNS/SNTP 未就绪) 当致命错误处理。修复: 回退 refresh_count + 2s 重试，匹配 poll 路径模式 |
| 2026-07-10 | **ESP-Claw AI Agent Phase 3 集成**: Web Agent Chat UI (/api/agent/chat POST → claw_event_router); Flutter Agent Chat 页面 (agent_chat_screen.dart + http_service getJson/postJson); LLM Vision 启用 (supports_vision=true) + camera.capture_vision MCP tool; 15个设备 MCP tools (新增 vision)。P3-4 ASR/TTS 待开发 (需外部服务) |
| 2026-07-10 | +S253~S258 代码审查 Round 4 修复 (6 个 HIGH/MEDIUM): AudioDriver ESP_ERROR_CHECK I2S abort(S253), PhoneAppSettings assert(sta_netif)(S254), CameraStream _encoder_handle HTTP handler 竞态(S255), s_audio_inited/s_sntp_initialized 数据竞态(S256), CameraStream cJSON_Print free→cJSON_free(S257), PhoneAppSettings _wifi_initialized 数据竞态(S258) |
| 2026-07-10 | +S259~S262 代码审查 Round 5 修复 (4 个 CRITICAL/HIGH/MEDIUM): **S259 (CRITICAL) vTaskDelete(nullptr) 竞态**: PhoneAppAudio/PhoneAppSettings 中 vTaskDelete(handle.exchange(nullptr)) 模式在任务自删后 exchange() 返回 nullptr 导致 vTaskDelete(nullptr) 删除调用者自身。修复: 先 exchange 捕获值再判空, 4 处全部修复。**S260 (HIGH) HTTP 错误响应缺失**: h_llm_config_set/h_feishu_config/h_qq_config/h_tg_config 在 calloc/recv/cJSON_Parse 失败时 return ESP_FAIL 未发送 HTTP 响应导致客户端挂起。修复: 12 处早期返回前添加 httpd_resp_send_err。**S261 (MEDIUM) s_task_handle 非原子**: web_config_server 中 s_task_handle 为普通 TaskHandle_t 跨核访问无同步。修复: 改为 std::atomic&lt;TaskHandle_t&gt; + xTaskCreatePinnedToCore 用临时变量。**S262 (MEDIUM) Logger vsnprintf 未定义行为**: ESP-IDF v6.x 传完全格式化日志行给 vprintf hook, vsnprintf 再格式化含 % 字符的消息导致 UB。修复: 改用 strlcpy |
| 2026-07-10 | **ESP-Claw AI Agent Phase 1-2 集成**: 对比 esp-claw 上游项目 (https://github.com/espressif/esp-claw)，识别并完成跑通 AI Agent 全链路所需的代码集成。Phase 1: 链接 cap_system/cap_files/cap_http_request + cap_mcp_server/client + mcp_mdns; 新增 device_mcp_tools.cpp (14个设备专属 MCP tools); 配置 claw_paths + mDNS + MCP server 启动。Phase 2: 链接 cap_lua/cap_web_search/cap_agent_mgr/cap_router_mgr/cap_skill_mgr + 注册; 扩展 MCP tools (audio/brightness/wifi)。待硬件验证: PA (端到端链路), PB (Flutter UI), PC (Web UI), PD (Vision), PE (ASR/TTS) |
| 2026-07-09 | **文档整合**: 合并 `project_design.md` 至 PROJECT.md (FreeRTOS 任务调度表 + 模块管道图) 并删除；消除三份文档间的架构/修复记录冗余。分工明确: README=硬件, PROJECT=软件/架构/实现, PROJECT_REQUIREMENTS=需求/修复登记/变更记录。P0 新增 Camera Stream + Music 卡顿 (S235) |
| 2026-07-08 | +S219~S232 代码审查 Round 2 修复 (14 个 CRITICAL/HIGH/MEDIUM): CameraStream model load 早期退出信号(S219), s_sntp_synced volatile→atomic(S220), s_audio_task TaskHandle→atomic(S221), _wifi_scan_task TaskHandle→atomic(S222), PhoneAppAudio _task_handle→atomic(S223), AudioDriver gpio_config 返回值检查(S224), Logger vTaskDelete 过期句柄(S225), g_has_lcd volatile→atomic(S226), _wifi_connect_task TaskHandle→atomic(S227), PeripheralManager _has_lcd→atomic(S228), s_rec_pub orb_advert_t→atomic(S229), h_play __audio_init 锁顺序(S230), h_rec_stop s_rec_path 竞态(S231), ULog writer task PSRAM 栈(S232) |
| 2026-07-09 | +S235~S240 代码审查 Round 3 修复 (6 个 HIGH/MEDIUM): LLM API key 明文暴露(S235), IM/LLM handler Content-Length 无验证(S236), CameraStream _detector atomic UAF(S237), WeChat/LLM handler cJSON NULL + token 暴露(S238), PhoneAppCamera _v4l2_buf_count 未截断(S239), PhoneAppAudio _stop_recording UAF(S240) |
| 2026-07-09 | +S236 **ULog header timestamp 修复**: `write_file_header()` 写入 UTC 绝对时间但 PX4 规范要求 µs since boot，导致 `ulog_info` 显示错误 start time (`495425:08:53`) 和 duration (`0:00:00`)。修复: header timestamp 改用 `esp_timer_get_time()`，移除 UTC 转换逻辑 |
| 2026-07-09 | +S247 **Camera Stream + Music 卡顿修复 (P0 回归根因)**: git bisect 确认 commit 65c36ad 引入回归。`cam_capture` 任务错误绑定 Core 1 (priority 5)，抢占同核 priority 3 的 Music GMF/ASP 任务 (I2S TX DMA 欠载→卡顿)。v0.0.3 正常因采集循环在 Core 0 的 httpd 上下文运行。修复: `cam_capture` 改绑 Core 0 (恢复 v0.0.3 核亲和性)。构建通过，待硬件验证 |
| 2026-07-10 | +S249 **任务重新绑核 (S248 残留缓解)**: 将背景/轻量任务按核重新分配 — Core 1 = `w_audio`(与 Music 互斥,prio1)/`sys_monitor`(1)/`web_config`(1)/`wifi_scan`(1)/`wifi_conn`(1)，均低于 Music prio3 绝不抢占；Core 0 = `ulog_writer`(5)/`logger_writer` 移离 Core 1 使其不再以 prio5 抢占 Music。硬件验证: 杂音**稍有改善但仍偶发** → 证实调度抢占已消除、残留为跨核共享 PSRAM 总线/L2 cache 带宽竞争 (S248) |
| 2026-07-10 | +S250 **CameraStream start()/stop() 竞态修复**: Settings App 的 cam_start/cam_stop 在独立 FreeRTOS task 中运行。stop() 未完成清理时 start() 被调用 → `_running` CAS 成功 (stop 已设 false) → CameraDriver re-entrant claim 成功 → httpd 端口 80 EADDRINUSE → start() 失败 → 与 stop() 并发清理 → V4L2/PPA/SPI double-free → SPI DMA memcpy 空指针 → Guru Meditation (Load access fault)。修复: ① `_start_stop_mutex` 互斥锁串行化 start()/stop() ② stop() 等待 capture task 加 5s timeout + force-kill ③ start() mutex take 加 20s timeout (防御层) ④ Settings App `_cam_start_stop_task` 跟踪防止孤儿任务泄漏内部 SRAM |
| 2026-07-08 | +S184~S213 代码审查 Round 1 修复 (30 个 CRITICAL/HIGH/MEDIUM): Logger snprintf 栈溢出(S184)/data_sem UAF(S185)/writer force-kill 持锁(S186)/句柄悬挂(S187)/sd_level 竞态(S188), Web 路径穿越(S189)/cJSON NULL(S190)/s_running 竞态(S191)/mutex 泄漏(S192)/httpd_start 泄漏(S193)/NVS cache 竞态(S194)/free→cJSON_free(S195), CameraStream V4L2 buf 越界(S196)/model-load force-kill(S197)/dummy_buf NULL(S198)/snprintf 截断(S199)/bytesused 越界(S200)/encoder dims 零值(S201), PhoneAppCamera pipeline 泄漏(S202)/buf.index 越界(S203), PhoneAppSettings 事件组泄漏(S204)/WiFi OFF/ON abort(S205)/connect task UAF(S206)/_wifi_ip 未填充(S207), PhoneAppCameraStream 析构 handler UAF(S208), AudioDriver init rollback mutex UAF(S209), SystemMonitor force-kill 持锁(S210), SDCardDriver _has_lcd 竞态(S211), main.cpp assert no-op(S212)/esp_read_mac 未检查(S213) |
| 2026-07-08 | +S177~S180 HTTP 服务器不可达修复(S177: LWIP_MAX_SOCKETS 22→28 + TCP keep-alive + WiFi 断连 httpd 重启), SystemMonitor 内存告警 80%→85%(S178), Flutter ULog 视频查看器(S179: ulog_parser.dart + ulog_viewer_screen.dart), Flutter filesDownload 可靠性(S180: chunked 检测修正 + O(n²)消除) |
| 2026-07-08 | R17 更新: Camera Stream 和 Audio 不再互斥 (硬件独立: MIPI CSI vs I2S)，所有 `__cam_running()` 检查已移除 |
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
| 2026-07-10 | -R21 -R3 人体检测移除: 删除 coco_detect 依赖 + COCODetect/esp-dl + detection_result uORB topic + 模型 loader task, PPA 继续用于 300×300 JPEG 编码 (无检测), _start_stop_mutex 修复 start/stop 竞态 |
| 2026-07-07 | +S160~S169 代码优化与清理: PhoneAppCameraStream FPS no-op修复(S160), _detect_in_buf null-check(S161), Music冗余ASP清理(S162), SD路径常量统一(S163), VOLUME/BRIGHTNESS常量统一(S164), WIFI_CONNECTED_BIT共享(S165), Logger ring buffer memcpy优化(S166), SystemMonitor heap总大小缓存(S167), camera_stream.hpp extern "C"修复(S168), heap_caps_free一致性(S169) |
| 2026-07-07 | +R22 Camera Frame ULog 录制: JPEG 帧通过 camera_frame uORB topic 写入 .ulg 文件, Web API /api/camera_record 控制, ULog ring buffer 64KB + PSRAM data_buf |
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
| 2026-07-02 | 初始创建，汇总 PROJECT.md 及历史架构分析中所有需求和问题 |
| 2026-07-02 | +R15 R16 R17 Web 音频录制/播放 + Camera Stream 互斥需求 |
| 2026-07-02 | +S28~S34 Web 音频稳定性修复 (懒加载、URL解码、JS修复、ASP生命周期等) |
| 2026-07-02 | +S35 WIFI6 音频修复: I2C地址修正 0x18→0x30, 单handle (BOTH+IN_OUT) 匹配参考固件 |
| 2026-07-02 | +S36 Music App ASP 生命周期同步, 匹配 web S31 |
| 2026-07-02 | +S37 Flutter Settings 页面: WiFi/音量/Camera Stream + 录音/播放 |
| 2026-07-02 | +K4 Web 音频 Camera Stream 互斥未生效 (诊断中，已加 noinline + debug log) |
| 2026-07-02 | **修复**: K4/R17 全部 6 个 Web 音频端点均加 `__cam_running()` 检查；CameraStream `_detector`/`_frame_count`/`_fps_total_bytes` 加 volatile；SD LDO re-acquire 返回值检查；`localtime()`→`localtime_r()`；`vTaskDelete` 自删除防护 |
| 2026-07-08 | +S216 SRAM 优化: 禁用 EAP (WPA2-Enterprise, 省约1.1KB IRAM+21KB PSRAM) + DVP (此板仅用MIPI CSI); S175 TCP 窗口 64KB→32KB (65536超Kconfig range被钳制为5760, 32768无需WND_SCALE) |
| 2026-07-09 | +R57 WiFi 始终启用: 移除 wifi_en NVS 键和 UI 开关, WiFi 不可禁用 (Settings App 移除 switch, Web 移除 checkbox, Flutter 移除 toggle, bootWifiAutoConnect 跳过 wifi_en 检查, 断线自动重连无条件触发) |
| 2026-07-10 | +S264~S274 全项目代码审查 (Round 1): S264 s_pcm_count 数据竞态→atomic; S265 s_rec_start_ms→atomic; S266 __sync_synchronize 冗余移除; S267 audio_task calloc→heap_caps_calloc(PSRAM); S268 WiFi scan task vTaskDelete→self-delete pattern; S269 CameraStream isRunning/is_recording 显式 atomic load; S270 AudioDriver volume() 显式 atomic load; S271 ulog_writer ringbuf volatile→atomic + free→heap_caps_free; S272 uorb assert→explicit checks + orb_copy/orb_check use-after-free 修复; S273 JPEG encoder ref count 竞态→mutex + free→heap_caps_free; S274 example_video_init s_is_init 竞态→mutex; S275 esp_lvgl_port assert→null checks + task_notify NULL guard; S276 esp_lvgl_port_disp assert→null checks + free→heap_caps_free; S277 lcd_ppa assert→null checks + free→heap_caps_free |
| 2026-07-11 | **SRAM 优化: BSS 大对象迁移至 PSRAM (P0+P1)**: 分析 linker map 识别 DIRAM BSS 占用 top-4: ① `s_agent_msgs[16]` (74,880B, 67.6% of BSS) — `agent_msg_t.text` 从 `char[4096]` 静态数组改为 `char*` PSRAM 动态分配 (`heap_caps_malloc`)，数组本身改为 `heap_caps_calloc(PSRAM)`，节省 **74.9 KB** ② uORB `s_topics[32]`+`s_subs[256]`+`s_sub_free_list[256]` (5,632B) — 改为 PSRAM `heap_caps_calloc`，节省 **5.6 KB** ③ cap_lua_async `s_jobs[16]` (5,632B) — `EXT_RAM_BSS_ATTR` 在未启用 `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` 时无效，改为 PSRAM `heap_caps_calloc`，节省 **5.6 KB** ④ claw_agent_mgr `s_mgr` (4,980B) — 改为 PSRAM `heap_caps_calloc` 指针，`s_mgr.` → `s_mgr->` 全部重写，节省 **5.0 KB**。总计 DIRAM 使用从 227,139B (51.47%) 降至 136,027B (30.82%)，释放 **91.1 KB** 内部 SRAM，可用 heap 从 ~207KB 增至 ~298KB。`idf.py build` 验证通过 |
| 2026-07-11 | +S283 **claw tasks 核心绑定 Core 0**: Agent Chat 发消息后音乐立即播放异常。根因: claw 高优先级 tasks (event_router prio 5, agent loop prio 5, IM platform prio 5) 配置 `tskNO_AFFINITY`，可能被调度到 Core 1 抢占 prio 3 的 Music GMF/ASP task → I2S TX DMA 欠载。修复: ① main.cpp/web_config_server.cpp cold/hot-init 路径: event_router `task_core=0`, core_cfg `task_core=0` ② `claw_task.c` resolve_config: `tskNO_AFFINITY` 自动绑定 Core 0 |
| 2026-07-11 | +S285 **event_router out_message 默认路由**: Agent 回复在 terminal log 中出现但不显示在网页 Agent Chat 中。根因: `out_message` 事件需要 `router_rules.json` 中的显式规则才能触发 `send_message` action，`default_route_messages_to_agent` 仅处理 `message` 事件类型。修复: 在 `claw_event_router_process_event` 中添加 `out_message` fallback — 当无规则匹配时，查找 outbound binding (web_chat→local_send_message) 并直接调用 `claw_cap_call`，与默认 agent 路由逻辑对称 |
