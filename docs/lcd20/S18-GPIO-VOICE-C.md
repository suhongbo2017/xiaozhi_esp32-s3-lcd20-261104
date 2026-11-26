# P7-C：GPIO 开关接入小智语音（路线 C：通用工具 + 每路独立工具）

## 语音怎么用

配好开关后直接说：

| 说法 | 模型调用的工具 |
|---|---|
| "打开打印机灯光" | `self.gpio.sw1_on` |
| "把打印机灯光关了" | `self.gpio.sw1_off` |
| "打印机灯光开着吗" | `self.gpio.sw1_state` |
| "把灯翻转一下" / "全部关掉" | `self.gpio.set`（通用兜底） |
| "我有哪些开关" | `self.gpio.list` |

**每路三个工具**，工具名是 ASCII（`self.gpio.sw<N>_on/_off/_state`），
**中文名写在 description 里**，所以模型不需要先查就能一步调用：

```
self.gpio.sw1_on   打开「打印机灯光」（GPIO12，当前关）。用户说"打开打印机灯光"…时调用。
self.gpio.sw1_off  关闭「打印机灯光」（GPIO12，当前关）。…
self.gpio.sw1_state 查询「打印机灯光」（GPIO12）当前是开还是关。
```

## 关键实现

### 1. MCP 工具要能动态重建（以前只增不删）

`McpServer` 原来用裸指针 + 无锁，且 `DoToolCall` 会把 `McpTool*` 捕获到主线程调度里执行——
如果那时工具被删掉就是 **use-after-free**。改造：

```cpp
std::vector<std::shared_ptr<McpTool>> tools_;   // 共享所有权
std::mutex tools_mutex_;                        // 注册/删除/列举加锁
void RemoveTool(name);                          // 单个删除
int  RemoveToolsWithPrefix("self.gpio.sw");     // 前缀批量删
```

调度时捕获 `shared_ptr` → 即使刚被移除，回调也能安全执行完。

### 2. 配置变化即时生效（无需重启）

```
POST /api/gpio {devices:[...]}
  → GpioControl::Save() 校验+落盘+重配引脚
  → RebuildPerDeviceTools(): 先 RemoveToolsWithPrefix("self.gpio.sw")，再按新配置注册
```

实测（`/api/mcp_tools` 是这次新加的诊断接口）：

```
加到 2 路 → 34 个工具（gpio: list,set,sw1_on/off/state,sw2_on/off/state）
改第 2 路引脚 → 仍是 34 个（重建，无重复）
删掉第 2 路 → 31 个
全部清空   → 28 个（只剩 list/set）
```

### 3. 名称匹配放宽（模型传话不一定一字不差）

`FindIndex()` 依次尝试：
1. 精确匹配
2. 归一化后匹配（去空格/引号、忽略大小写）
3. 互相包含（"打印机灯" ↔ "打印机灯光"）—— 但**多个都能对上时宁可不猜**
4. 序号 `"1"`~`"4"`

### 4. 网页改成卡片 + 删除二次确认

```
┌──────────────────────────────┐
│ 打印机灯光            ( ●)   │  ← 整行卡片，右侧大开关（绿色=开）
│ GPIO12 · 高电平触发           │
│ ● 已打开            [ 删除 ]  │  ← 删除弹确认框，不再直接删
└──────────────────────────────┘
```

删除走独立确认弹窗（不是浏览器原生 confirm），确认文案带上开关名。

## 顺手修掉的两个坑

### ① `max_uri_handlers = 8` 把新接口挤掉了
加了 `/api/gpio`、`/api/mcp_tools` 之后，`POST /api/gpio` 返回 **405**、`POST /api/image` 变 404。
原因是 `AdminServer::Start()` 里写死了 `config.max_uri_handlers = 8`，而现在有 **11 个 URI**
（GET/POST 分开算），第 9 个之后注册失败但**没人检查返回值**。

修法：`config.max_uri_handlers = 20`，并把所有 `httpd_register_uri_handler` 换成会打日志的 `reg()` 包装。

> IDF 6.0 里这个上限是**运行时 config 字段**，不是 Kconfig（`CONFIG_HTTPD_MAX_URI_HANDLERS` 不存在）。

### ② 工具注册用了 shared_ptr 之后要记得不能 delete
`AddTool` 遇到重名会 `delete tool`（调用者交出的所有权），避免泄漏。

## 实测记录

```
POST /api/gpio {devices:[打印机灯光/GPIO12, 桌面风扇/GPIO13]}  → ok
  /api/mcp_tools: 34 个工具，含 sw1_*, sw2_*
改 GPIO13→14、低电平、上电开                              → ok，工具重建后仍 34 个
删除第二路                                                → 31 个
清空                                                      → 28 个（只剩 list/set）
所有接口状态码：GET /api/status 200、GET/POST /api/gpio 200、POST /api/image 200
固件 3.01MB / 3.94MB（76%）
```

## 限制 / 注意

- 工具数上限取决于服务器：4 路 = 2 + 12 个工具，`tools/list` 是 8KB 分页的，没问题
- 模型是否调用仍取决于服务端大模型；如果发现它"口头答应但不执行"，可以把
  `self.gpio.list` 的描述再强化（例如"任何开关操作都必须调用本工具"）
- 语音回复用的是工具返回的字符串，所以话术在 `SetByState()` 里统一生成
- 卡片开关点击是**乐观更新**（点完立即重拉 `/api/gpio` 校对）
