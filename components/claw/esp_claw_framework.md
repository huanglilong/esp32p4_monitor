# ESP-Claw 框架架构深度分析

> **分析日期**: 2026-07-12  
> **代码版本**: 基于 https://github.com/espressif/esp-claw (HEAD)  
> **参考文档**: https://esp-claw.com/zh-cn/reference-project/

---

## 一、项目概述

**ESP-Claw** 是乐鑫推出的面向物联网设备的 **Chat Coding「聊天造物」** 式 AI Agent 框架，版本 v0.1.0。它以对话定义设备行为，在 ESP32 系列芯片上本地完成感知、决策与执行的完整闭环。ESP-Claw 用 C 语言从 OpenClaw 理念重新实现，具备轻盈、事件驱动、结构化记忆、MCP 协议互通等核心特性。

---

## 二、整体架构 — 四层分层设计

项目采用 **"应用示例 + 通用组件"** 组织方式，代码分为四个逻辑层：

```
┌─────────────────────────────────────────────────────────┐
│  第1层: 应用装配层 (application/ + components/common/)    │
│  main.c → app_claw → wifi/http/config/emote              │
├─────────────────────────────────────────────────────────┤
│  第2层: 能力层 (components/claw_capabilities/)            │
│  IM平台 | MCP Client/Server | Lua | 调度器 | 文件 | ...  │
├─────────────────────────────────────────────────────────┤
│  第3层: 运行时核心层 (components/claw_modules/)           │
│  claw_core | claw_cap | claw_event_router | claw_memory  │
│  claw_skill | claw_manager                               │
├─────────────────────────────────────────────────────────┤
│  第4层: 设备与脚本扩展层 (components/lua_modules/)         │
│  显示屏/摄像头/音频/GPIO/PWM/传感器/存储/网络/线程/...    │
└─────────────────────────────────────────────────────────┘
```

### 分层职责

| 层 | 职责 | 关键模块 |
|---|---|---|
| **应用装配层** | 启动入口、WiFi连接、HTTP配置服务、能力注册组装 | `main.c`, `app_claw`, `wifi_manager`, `http_server` |
| **能力层** | 封装面向LLM的可调用工具（Function Calling） | 20+个 `cap_*` 模块 |
| **运行时核心层** | Agent Loop、上下文组装、事件路由、记忆管理、技能管理 | 7 个 `claw_*` 模块 |
| **设备扩展层** | 将硬件外设能力通过Lua脚本暴露给上层Agent | 30+个 `lua_module_*` / `lua_driver_*` 模块 |

---

## 三、核心模块详解 — 实现层面深度分析

### 3.1 `claw_core` — Agent 核心引擎实现

#### 3.1.1 内部状态结构

```c
struct claw_core_state {
    bool initialized;
    bool started;
    volatile bool stop_requested;
    uint32_t instance_id;
    // LLM 后端配置
    claw_core_llm_config_t llm_config;
    claw_llm_runtime_t *llm_runtime;
    SemaphoreHandle_t llm_lock;      // 保护 LLM 调用的互斥锁
    // 上下文提供者插件数组
    claw_core_context_provider_t *context_providers;
    size_t context_provider_count;
    size_t context_provider_capacity;  // 支持动态扩展
    // FreeRTOS 通信原语
    QueueHandle_t request_queue;      // 请求队列 (默认4个槽位)
    QueueHandle_t response_queue;     // 响应队列 (默认4个槽位)
    TaskHandle_t task_handle;
    SemaphoreHandle_t inflight_lock;  // 保护当前飞行请求状态
    // 当前飞行请求追踪
    uint32_t inflight_request_id;
    char inflight_session_id[128];
    claw_core_agent_loop_phase_t agent_loop_phase; // 8阶段状态机
    volatile bool inflight_abort;     // 原子中断标志
    // 用户中断插入队列
    claw_core_request_item_t insert_queue[4]; // 固定4个槽位
    // 完成观察者
    struct {
        claw_core_completion_observer_fn fn;
        void *user_ctx;
    } completion_observers[4];
};
```

#### 3.1.2 Agent Loop 主循环实现

这是整个 ESP-Claw 最核心的函数——单个 FreeRTOS 任务中运行的完整 Agent 推理循环：

```
claw_core_agent_loop_task(void *arg):
  while (!stop_requested):
    1. xQueueReceive(request_queue, &request, portMAX_DELAY)  // 阻塞等待请求
    2. 设置inflight状态 (request_id, session_id, phase=BEFORE_BUILD)
    3. 检查LLM配置是否就绪 (claw_core_llm_config_ready)
    4. 调用request_gate回调 — 门禁检查 (如速率限制)
    5. 调用on_request_start回调
    6. 持久化用户消息到会话历史
    7. 收集REQUEST_START_ONLY上下文提供者
    8. 进入Agent Loop迭代:
       while (true):
         a. 检查用户中断 → 插入新文本 → continue 重启循环
         b. BUILDING_CONTEXT: 构建系统提示词 + 消息列表 + 工具JSON
         c. BEFORE_LLM_HTTP: 再次检查用户中断
         d. IN_LLM_HTTP: claw_core_llm_chat_messages() — HTTP调用LLM
         e. 如果HTTP失败且是用户中断 → handle_pending_user_interrupts
         f. 如果无工具调用:
            FINALIZING → 从纯文本构建响应 → break
         g. AFTER_LLM_BEFORE_TOOL: 检查用户中断
         h. RUNNING_TOOL: 
            - 将assistant(tool_calls)消息追加到runtime_messages
            - 逐个调用core->call_cap(cap_name, args, request, &output)
            - 将tool_result消息追加到runtime_messages
            - 持久化工具调用轮次
         i. iteration++ → 检查max_tool_iterations上限
         j. continue回到循环开头 (重新构建上下文+调LLM)
    9. FINALIZING: 清理inflight状态，发布out_message事件
   10. 推送响应到response_queue或直接发布
```

**关键设计点**:

- **用户中断处理**: 在4个关键阶段(BUILD/BEFORE_LLM/AFTER_LLM/FINALIZE)都检查 `insert_queue`，如果有中断插入的文本，就把新文本追加到 `runtime_messages`，然后 `continue` 重新开始迭代循环——这是一个优雅的“热重载上下文”机制。

- **上下文构建分层**: 
  - `REQUEST_START_ONLY` 标志: 带此标志的 provider 只在请求开始时收集一次并缓存，后续迭代复用缓存结果(避免重复IO)。典型用途: 会话历史、Profile。
  - 每次迭代: 非 REQUEST_START_ONLY 的 provider 重新收集(如当前工具列表、技能目录)。

- **工具调用循环**: 逐个调用 `core->call_cap()`，将每个 tool_result 消息以 `{"role":"tool", "tool_call_id":..., "content":..., "is_error":...}` 格式追加到 runtime_messages，同时构建 tool_summary 用于失败追踪。

#### 3.1.3 上下文提供者机制

```c
// 定义在 claw_core.h
typedef esp_err_t (*claw_core_context_provider_collect_fn)(
    const claw_core_request_t *request,
    claw_core_context_t *out_context,    // {kind, content}
    void *user_ctx);
```

**三种上下文类型**:
- `SYSTEM_PROMPT` → 追加到 system prompt (带 `## ProviderName` 标题)
- `MESSAGES` → 解析为 JSON 数组追加到 messages
- `TOOLS` → 解析为 JSON 数组追加到 tools

**上下文组装流程**:
1. 从 `core->system_prompt` 复制基础 system prompt
2. 遍历所有 context_providers:
   - REQUEST_START_ONLY 的用缓存结果
   - 否则调用 `provider->collect()` 获取实时内容
   - 按 kind 类型分派到 system_prompt/messages/tools
3. 注入 `Current Turn Context` (source_channel, source_chat_id) 到 system prompt
4. 注入当前用户消息到 messages
5. 追加 runtime_messages (工具调用历史)

#### 3.1.4 请求与响应队列

**请求入队**:
- 普通请求: `xQueueSend(request_queue, ...)` 带超时
- 用户中断: 不走 request_queue，直接写入 `insert_queue`（`CLAW_CORE_REQUEST_FLAG_USER_INTERRUPT` 标志）

**响应出队**:
- 先检查 pending 链表 (按 request_id 匹配)
- 再等待 response_queue
- 不匹配的响应暂存到 pending 链表

---

### 3.2 `claw_cap` — 能力注册与调度中心实现

#### 3.2.1 内部数据结构

```c
typedef struct {
    bool occupied;
    claw_cap_descriptor_t descriptor; // 能力描述符副本
    size_t group_slot_index;
    claw_cap_state_t state;           // REGISTERED/STARTED/DISABLED/DRAINING/UNLOADING
    bool init_called;
    uint32_t active_calls;            // 当前活跃调用数(用于安全卸载)
} claw_cap_descriptor_slot_t;

typedef struct {
    bool occupied;
    const claw_cap_group_t *group;
    claw_cap_state_t state;
    size_t *member_slots;             // 该组包含的描述符槽位索引
    size_t member_count;
    bool group_init_called;
} claw_cap_group_slot_t;

// 会话级可见性 (实现 Skill 的 cap_groups 绑定)
typedef struct {
    char *session_id;
    char **group_ids;                 // 该 session 可见的 group_id 列表
    size_t group_count;
} claw_cap_session_visibility_t;

typedef struct {
    SemaphoreHandle_t mutex;
    claw_cap_descriptor_slot_t *descriptor_slots;  // 动态扩容
    claw_cap_group_slot_t *group_slots;
    char **llm_visible_group_ids;     // 全局LLM可见组
    claw_cap_session_visibility_t *session_visibilities; // 会话级可见性
} claw_cap_runtime_t;
```

#### 3.2.2 LLM 工具调用的鉴权链

`claw_cap_authorize_llm_tool_locked()` 实现6级检查:

```
1. CLAW_CAP_AUTH_NOT_AVAILABLE   → 描述符未注册或未 STARTED
2. CLAW_CAP_AUTH_NOT_EXECUTABLE  → execute 函数指针为空
3. CLAW_CAP_AUTH_NOT_CALLABLE_KIND → kind != CALLABLE && kind != HYBRID
4. CLAW_CAP_AUTH_NOT_LLM_CALLABLE → 未设置 CALLABLE_BY_LLM 标志
5. CLAW_CAP_AUTH_ROOT_ONLY       → 设置了 ROOT_AGENT_ONLY 但 caller 是 SUB_AGENT
6. CLAW_CAP_AUTH_GROUP_NOT_VISIBLE → group 既不在全局可见列表，也不在会话可见列表
```

#### 3.2.3 可见性管理逻辑

```
claw_cap_set_llm_visible_groups(group_ids, count):
  mutex_lock
  清空 llm_visible_group_ids
  深拷贝新的 group_ids 列表
  mutex_unlock

claw_cap_set_session_llm_visible_groups(session_id, group_ids, count):
  mutex_lock
  查找或创建 session_visibility 槽位
  替换该 session 的 group_ids
  mutex_unlock

claw_cap_group_is_llm_visible_locked(slot_index, session_id):
  if group_id 在全局可见列表 → true
  if session_id 在 session_visibilities 中
     && group_id 在其 group_ids 中 → true
  → false
```

#### 3.2.4 `claw_cap_call_from_core` — LLM 工具调用的入口

```c
claw_cap_call_from_core(cap_name, input_json, request, out_output, user_ctx):
  1. 从 user_ctx 恢复 caller(CALLER_AGENT)
  2. 构建 claw_cap_call_context_t:
     - session_id, agent_id, channel, chat_id 从 request 提取
     - caller = CLAW_CAP_CALLER_AGENT
  3. claw_cap_call(cap_name, input_json, ctx, output, 32KB)
  4. 返回动态分配的 output 字符串
```

#### 3.2.5 工具 JSON 生成

`claw_cap_build_llm_tools_json(ctx, wrap_for_responses_api)`:
- 遍历所有 descriptor_slots
- 对每个 CALLABLE_BY_LLM 且通过鉴权的描述符:
  - 构建 OpenAI function 格式: `{"type":"function", "function":{"name":..., "description":..., "parameters":...}}`
  - 如果 wrap_for_responses_api (Anthropic格式): 包装为 `{"type":"function", ...}`

---

### 3.3 `claw_event_router` — 事件驱动调度器实现

#### 3.3.1 事件结构

```c
typedef struct {
    char event_id[48];          // UUID
    char source_cap[32];        // 来源能力名
    char event_type[32];        // "message", "startup", "boot_completed"...
    char source_channel[16];    // "qq", "feishu", "telegram", "web"...
    char target_channel[16];    // 目标渠道
    char source_endpoint[64];   // 来源端点
    char target_endpoint[96];   // 目标端点
    char chat_id[96];           // 会话标识
    char sender_id[96];         // 发送者ID
    char message_id[96];        // 消息ID
    char correlation_id[96];    // 关联ID(用于追踪)
    char content_type[24];      // "text", "image"...
    int64_t timestamp_ms;       // 毫秒时间戳
    claw_session_policy_t session_policy;  // CHAT/TRIGGER/GLOBAL/EPHEMERAL/NOSAVE
    char *text;                 // 堆分配的文本内容
    char *payload_json;         // 堆分配的结构化负载
} claw_event_t;
```

#### 3.3.2 规则结构

```c
typedef struct {
    bool enabled;                   // 是否启用
    bool consume_on_match;          // 命中后停止后续规则匹配
    char id[64];                    // 唯一ID
    char description[160];
    char ack[256];                  // 命中后的确认消息
    char *vars_json;                // 规则变量
    claw_event_router_match_t match; // 匹配条件(6个字段+文本匹配模式)
    claw_event_router_action_t *actions; // 动作列表
    size_t action_count;
} claw_event_router_rule_t;
```

**匹配条件** (`claw_event_router_match_t`):
- `event_type`, `event_key`, `source_cap`, `channel`, `chat_id`, `content_type` — 精确匹配
- `text` + `text_match_rule` (EXACT/PREFIX) — 文本匹配

#### 3.3.3 6种动作类型

| 动作 | 核心实现 | 关键逻辑 |
|------|---------|---------|
| **call_cap** | 渲染 input_json 模板 → `claw_cap_call()` → 捕获 output 到 ctx |
| **run_agent** | 渲染模板 → `claw_agent_mgr_submit_root()` 带4个标志 (PUBLISH_OUT_MESSAGE, SKIP_RESPONSE_QUEUE, USER_INTERRUPT, PUBLISH_STAGE_MESSAGE) |
| **send_message** | 查找 outbound binding → 构建 payload → `claw_cap_call(send_cap_name, ...)` |
| **run_script** | 渲染模板 → Lua 脚本执行 |
| **emit_event** | 渲染模板 → 构建新 `claw_event_t` → `claw_event_router_handle_event()` (递归) |
| **drop** | 直接返回，丢弃事件 |

#### 3.3.4 模板渲染系统

Event Router 内置了一个模板渲染引擎:

- 输入: action 的 `input_json` (包含 `{{path.to.value}}` 占位符)
- 上下文变量: 
  - `event.*` — 事件的所有字段
  - `vars.*` — 规则的 vars_json
  - `last.output` / `last.error` — 上一个 capture_output 动作的结果
- 模板语法: `{{event.source_channel}}`, `{{last.output}}`

#### 3.3.5 FreeRTOS 任务架构

```c
typedef struct {
    QueueHandle_t event_queue;   // 事件队列 (默认6个槽位)
    TaskHandle_t task_handle;    // 任务句柄
    SemaphoreHandle_t mutex;     // 递归互斥锁
    claw_event_router_rule_t *rules;  // 规则数组
    size_t rule_count;
    claw_event_router_pending_t pending[6]; // 待处理事件追踪表
} claw_event_router_runtime_t;
```

**事件处理流程**:
1. IM 平台调用 `claw_event_router_handle_event()`
2. 事件加入 `event_queue`
3. Event Router 任务从队列取出事件
4. 按顺序匹配规则: 字段精确匹配 + text 匹配 (exact/prefix)
5. 命中规则后执行所有 actions
6. 如果 `consume_on_match` → 停止匹配
7. 结果通过 `claw_event_router_get_last_result()` 查询

---

### 3.4 `claw_memory` — 结构化记忆管理实现

#### 3.4.1 记忆条目结构

```c
typedef struct {
    char id[40];                 // UUID 格式的记忆ID
    char source[16];             // "manual" / "auto_llm"
    char content[256];           // 归一化后的记忆正文 (最多256字节)
    uint16_t summary_ids[3];     // 关联的摘要标签ID (最多3个)
    uint8_t summary_id_count;    // 实际摘要标签数量
    char tags[96];               // 标签文本
    char keywords[128];          // 关键词 (换行分隔)
    uint32_t created_at;         // Unix 时间戳
    uint32_t updated_at;
    uint16_t access_count;       // 访问计数 (用于优先级排序)
    uint8_t deleted;             // 软删除标记
} claw_memory_item_t;
```

#### 3.4.2 文件布局与索引结构

```
/fatfs/memory/
├── memory_index.json      // 索引: {version, next_summary_id, summaries[], keyword_index{}}
├── memory_records.jsonl   // JSONL 格式的记忆记录
├── memory_digest.log      // 操作日志 (用于调试和压缩)
├── MEMORY.md              // 人类可读视图 (自动生成)
├── user.md                // 用户画像
├── soul.md                // Agent 灵魂/价值观
└── identity.md            // Agent 身份卡
```

**memory_index.json 结构**:
```json
{
  "version": 3,
  "next_summary_id": 5,
  "summaries": [
    {"summary_id": 1, "label": "用户偏好", "ref_count": 3},
    {"summary_id": 2, "label": "项目信息", "ref_count": 5}
  ],
  "keyword_index": {
    "esp32": [1, 3],
    "mqtt": [2]
  }
}
```

#### 3.4.3 摘要标签检索机制

这是关键的轻量级设计——不使用向量数据库，而是:

**存储时**:
1. 解析 item 的 tags 和 keywords
2. 对每个 tag 在 `memory_index.json` 的 summaries 中查找或创建 summary_id
3. 将 summary_ids 写入 item
4. 更新 summaries 的 ref_count
5. 更新 keyword_index (关键词 → summary_id 映射)
6. 追加 JSONL 记录 + 更新 MEMORY.md

**检索时**:
1. 根据 query 的 summary_labels 查找 summary_ids
2. 加载所有非删除记录
3. 按 summary_ids 匹配过滤
4. 按 access_count 降序排列 (越常用越靠前)
5. 返回 limit 条记录的 JSON 数组

#### 3.4.4 自动记忆抽取

- 使用独立的 `claw_llm_runtime_t` 实例(独立于 core 的 LLM 连接)
- 用专门的系统提示词让 LLM 从用户消息中提取长期记忆
- 支持三种 message_intent: NONE / FORGET / REPLACE
- 结果解析为 JSON 结构 → 调用 `claw_memory_store_with_result()`
- **去重**: `claw_memory_items_semantically_match()` 比较 content 语义相似度

#### 3.4.5 四种 Context Provider

| Provider | 类型 | 内容 |
|----------|------|------|
| `claw_memory_profile_provider` | SYSTEM_PROMPT | 注入 user.md + soul.md + identity.md |
| `claw_memory_long_term_provider` | SYSTEM_PROMPT | 注入摘要标签目录 (summary labels) |
| `claw_memory_long_term_lightweight_provider` | SYSTEM_PROMPT | 注入 MEMORY.md 全文 (轻量模式) |
| `claw_memory_session_history_provider` | MESSAGES | 注入最近的对话历史 |

#### 3.4.6 会话历史持久化

`claw_memory_persist_context_callback()` 注册给 `claw_core` 的 `persist_context` 回调:
- 接收 `claw_core_context_persist_batch_t` 批量记录
- 每条记录类型: USER / ASSISTANT_FINAL / ASSISTANT_TOOL / TOOL_RESULT
- 写入 `{session_root_dir}/{session_id}.jsonl` 文件
- 字节大小限制: 150KB
- 消息文本截断: max_message_chars (默认4096)

---

### 3.5 `claw_skill` — 技能系统实现

#### 3.5.1 SKILL.md 格式

```markdown
---
name: web-search
summary: 使用 Brave Search API 搜索网页
manage_mode: runtime
metadata:
  cap_groups:
    - cap_web_search
---

# Web Search Skill

当用户需要搜索网页时，使用 `web_search` 工具...
```

**frontmatter 关键字段**:
- `name` → skill_id (唯一标识)
- `summary` → 在技能目录中显示的简介
- `manage_mode` → `readonly` (只读) / `runtime` (运行时可注册)
- `metadata.cap_groups` → 激活此技能时开放的 Capability Group

#### 3.5.2 目录扫描与优先级

`claw_skill_add_directory(dir)` 注册技能目录:
- 递归扫描 `{dir}/{skill_id}/SKILL.md`
- 先注册的目录优先级更高 (同名 skill_id 后者被覆盖)
- 典型调用顺序: `/system/skills/` (固件) → `/fatfs/skills/` (用户)

#### 3.5.3 激活机制

`claw_skill_activate_for_session(session_id, skill_id)`:

1. 从注册表查找 skill_id 的 catalog entry
2. 将 `{session_id}: {skill_id}` 写入会话状态文件
3. **不自动设置 cap_groups 可见性** — 这由上层(如 cap_skill_mgr 工具)协调:
   - `cap_skill_mgr` 的 `activate_skill` 工具实现中:
     1. 调用 `claw_skill_activate_for_session()` 持久化
     2. 调用 `claw_skill_load_active_cap_groups()` 获取该 session 所有活跃 skill 的 cap_groups 并集
     3. 调用 `claw_cap_set_session_llm_visible_groups()` 开放工具可见性
     4. 读取 SKILL.md 正文作为工具返回值注入会话上下文

#### 3.5.4 技能目录 Context Provider

输出格式 (注入到 system prompt):
```
## Skills Catalog
- **web-search**: 使用 Brave Search API 搜索网页
- **gpio-control**: 通过 Lua 脚本控制 GPIO 引脚
```
LLM 看到此目录后，可以用 `activate_skill` 工具按需激活。

---

### 3.6 `claw_manager` — Agent 管理器实现

#### 3.6.1 Agent Manager 架构

```c
typedef struct {
    const claw_core_config_t *core_config;
    const claw_core_context_provider_t *base_context_providers;
    const char *root_agent_system_prompt;    // Root Agent 角色叠加
    const char *subagent_system_prompt;       // Sub-agent 角色叠加
    const claw_agent_mgr_subagent_type_prompt_t *subagent_type_prompts; // 类型特定提示词
} claw_agent_mgr_config_t;
```

**Root Agent** (`agent_id = "0"`):
- `claw_agent_mgr_create_root_agent()`: 创建独立的 `claw_core` 实例 + `claw_core_start()`
- 添加 base_context_providers + cap tools providers

**Sub-agent 派生**:
- `claw_agent_mgr_spawn_subagent(parent_ctx, prompt, agent_type, background)`:
  1. 从 parent_ctx 继承 session 信息
  2. `claw_session_mgr_alloc_subagent_session_id()` 生成新 session_id
  3. 创建独立的 `claw_core` 实例
  4. 添加 base_providers + sub_agent tools provider (排除 ROOT_AGENT_ONLY 工具)
  5. `claw_core_submit(prompt)` 启动推理
  6. 如果 `background=false`，父 Agent 阻塞等待完成

#### 3.6.2 Session Manager — 会话策略实现

| 策略 | `chat_key` 行为 | session_id 行为 |
|------|----------------|----------------|
| **CHAT** | `{channel}:{chat_id}` | 持久化，同一 chat_key 复用 session_id |
| **TRIGGER** | 同上 | 每次生成新 UUID |
| **GLOBAL** | 固定 "global" | 全局唯一 session |
| **EPHEMERAL** | 同上 | 生成但不持久化到文件 |
| **NOSAVE** | 同上 | 生成但不保存会话历史 |

**Alias 机制**: 同一 chat_key 下可以有多个 alias (会话分支)，支持 `new_chat_session` / `switch_chat_session` / `delete_chat_session`。

---

## 四、模块间协作全景图

```
                    ┌──────────────────────────────────────┐
                    │         cap_im_platform              │
                    │  (QQ/飞书/Telegram/微信 接收消息)      │
                    └──────────┬───────────────────────────┘
                               │ claw_event_t
                               ▼
                    ┌──────────────────────────────────────┐
                    │       claw_event_router              │
                    │  ┌─────────────────────────────┐     │
                    │  │ 规则匹配 → 动作执行           │     │
                    │  │ run_agent / call_cap / ...  │     │
                    │  └──────────┬──────────────────┘     │
                    └─────────────┼────────────────────────┘
                                  │ submit_root()
                                  ▼
                    ┌──────────────────────────────────────┐
                    │        claw_agent_mgr               │
                    │  ┌──────────────────────────────┐    │
                    │  │ Root Agent (id="0")          │    │
                    │  │  ├─ claw_core 实例           │    │
                    │  │  └─ 可 spawn Sub-agents      │    │
                    │  └──────────────────────────────┘    │
                    └──────────┬───────────────────────────┘
                               │ claw_core_submit()
                               ▼
    ┌──────────────────────────────────────────────────────────┐
    │                     claw_core (Agent Loop)               │
    │                                                          │
    │  Context Building ──────────────────────────────────┐    │
    │  ├─ claw_memory_profile_provider → Profile          │    │
    │  ├─ claw_memory_long_term_provider → 摘要标签目录    │    │
    │  ├─ claw_memory_session_history_provider → 对话历史  │    │
    │  ├─ claw_skill_skills_list_provider → 技能目录       │    │
    │  └─ claw_cap_tools_provider → 可用工具JSON           │    │
    │                                                      │    │
    │  LLM HTTP Call ──────────────────────────────────    │    │
    │  Tool Call Loop ─────────────────────────────────    │    │
    │    └─ call_cap → claw_cap_call_from_core()           │    │
    │         ├─ claw_cap 鉴权 (6级检查)                   │    │
    │         ├─ cap_skill_mgr (activate_skill)            │    │
    │         │    ├─ claw_skill_activate_for_session()    │    │
    │         │    └─ claw_cap_set_session_llm_visible()   │    │
    │         ├─ cap_memory (memory_store/memory_recall)   │    │
    │         ├─ cap_files / cap_web_search / cap_lua ...  │    │
    │         └─ cap_agent_mgr (spawn_subagent)            │    │
    │                                                      │    │
    │  Response → PUBLISH_OUT_MESSAGE 事件                 │    │
    └──────────────────────┬───────────────────────────────────┘
                           │ out_message event
                           ▼
                    ┌──────────────────────────────────────┐
                    │       claw_event_router              │
                    │  outbound binding → send_message    │
                    └──────────┬───────────────────────────┘
                               │ claw_cap_call()
                               ▼
                    ┌──────────────────────────────────────┐
                    │         cap_im_platform              │
                    │  (发送回复消息到对应IM平台)             │
                    └──────────────────────────────────────┘
```

---

## 五、核心设计模式总结

### 5.1 事件驱动松耦合
所有模块间的通信以 `claw_event_t` 为载体，通过 `claw_event_router` 集中调度。IM平台、Lua脚本、定时任务、CLI命令等任意来源都可以产生事件，由统一的规则引擎决定如何处理。

### 5.2 Context Provider 插件模式
`claw_core` 通过回调接口支持动态注册上下文提供者。Memory、Skills、Tools 都通过这个机制注入上下文，易于扩展。支持 `REQUEST_START_ONLY` 标志实现缓存优化。

### 5.3 Skill-Capability 关注点分离
Capability 提供"能做什么"(工具)，Skill 提供"怎么做"(知识文档)，两者通过 `metadata.cap_groups` 绑定。LLM 先看到 Skill 摘要，按需激活后获得完整文档+工具。

### 5.4 Root Agent + Sub-agent 分层代理
复杂任务可由 Root Agent 委托给 Sub-agent 执行，每个 Sub-agent 有独立的 session 和 claw_core 实例，支持并行和隔离。

### 5.5 C 风格面向对象
所有核心模块采用 ESP-IDF 风格的 C 面向对象设计：不透明句柄 `xxx_t`、`xxx_create/delete/start/stop` 生命周期函数、回调注册机制、FreeRTOS 互斥锁保护共享状态。

### 5.6 摘要标签检索 (轻量级向量检索替代)
不依赖向量数据库，使用结构化 tags + 摘要标签目录实现记忆检索，通过 `access_count` 做简单的热度排序。适合嵌入式设备资源约束。

---

## 六、支持的平台与生态

- **LLM后端**: OpenAI (GPT)、阿里云百炼 (Qwen)、Anthropic (Claude)、DeepSeek，支持自定义 Endpoint
- **IM平台**: Telegram、QQ、飞书、微信 (4大平台，可扩展)
- **硬件**: ESP32-S3、ESP32-P4、ESP32-C5、ESP32-S31 等多款开发板
- **外设**: 30+ Lua模块覆盖显示屏、摄像头、音频、GPIO、PWM、I2C、UART、BLE HID、传感器、LVGL等
- **MCP协议**: 同时具备 Server/Client 双重身份

---

## 七、对 ESP32P4 Monitor 项目的参考价值

可以参考 esp-claw 的:
- **`claw_event_router`** 事件驱动调度模式 — 将传感器数据、用户输入、定时任务统一为事件流
- **`claw_core` 的 Context Provider 模式** — 灵活组装 LLM 上下文，支持缓存优化
- **`claw_cap` 的能力抽象** — 统一管理设备能力(摄像头、麦克风、传感器等)，支持可见性管理
- **`claw_memory` 的轻量级记忆** — 不依赖向量数据库的摘要标签检索方案，150KB 会话限制
- **Lua 扩展层** — 将硬件外设通过脚本暴露给 Agent 控制
- **多Agent架构** — Root/Sub-agent 分层用于任务隔离
- **Event Router 的模板渲染** — `{{event.*}}`, `{{last.output}}` 模式的动态规则

---

> 整个系统以 `claw_core`(LLM循环)为"大脑"、`claw_event_router`(事件路由)为"神经系统"、`claw_memory`(记忆)为"海马体"、`claw_skill`(技能)为"知识库"、`claw_cap`(能力)为"四肢"，构成了一套完整的边缘AI Agent运行时。
