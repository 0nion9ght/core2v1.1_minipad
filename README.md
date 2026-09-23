# Core2v1.1 MiniPad

基于 **M5Stack Core2 v1.1（ESP32）** 的小型桌面设备固件：LVGL 深色界面、时钟、下拉控制面板与开机动画，
各功能以**组件 + 总线**的方式解耦，方便继续扩展（触摸交互、联网取数据、图片显示等）。

> 仓库只包含固件源码：第三方库（`external/`）与设计素材（`reference/`）不入库，
> 克隆后按 [构建与烧录](#构建与烧录) 还原即可。

---

## 硬件

| 项 | 规格 |
|---|---|
| 主控 | ESP32-D0WDQ6-V3，双核 Xtensa LX6 @ 240MHz |
| 存储 | 16MB Flash / 8MB PSRAM（ESP32 经典款仅可映射 4MB） |
| 屏幕 | 2.0" IPS 320×240，ILI9342C / ILI9342E，SPI（40MHz） |
| 触摸 | FT6336U（I2C 0x38）+ 屏下 3 个电容虚拟键 |
| RTC | BM8563（I2C 0x51，带后备电池） |
| 电源 | AXP2101 + INA3221，震动马达，USB 桥 CH9102F（接 G1/G3） |
| 参考文档 | <https://docs.m5stack.com/zh_CN/core/Core2%20v1.1> |

---

## 功能

- **开机动画**：`Shallwe` 逐笔"写出"效果（路径数据 1.5KB，运行时逐帧绘制抗锯齿笔迹，播完淡出到时钟页）
- **时钟页**：Bebas Neue 176px 白字，深色底，宽度铺满 320px（冒号按数字视觉中心对齐），仅在分钟变化时刷新
- **深色主题**：LVGL 官方 dark 调色板（屏幕 `#15171A`、卡片 `#282B30`、文字 `#F5F5F5`）
- **状态栏**：右上角 WiFi 状态图标 + 分段电池图标 + 电量百分比（10s 刷新，充电时显示充电图标与 `+`）
- **下拉面板**：以下拉手势展开（跟手拖动、位移/甩动双判定、上滑或点状态栏或底部返回键收回），内为 3×2 六个圆形按钮：
  - **WiFi 开关**：图标随连接状态变化；关闭后抑制自动重连，重新打开会重连并恢复 SNTP 对时
  - **电源键**：弹出中文二次确认框（`确定要关机吗？` / `关机` / `取消`），确认后经 AXP2101 **真断电**
  - 亮度 / 音量 / 主题 / 重启：**占位**（灰暗、不可点，避免"点了没反应像坏了"）
- **底部三个电容虚拟键**：左 / 中 / 右（右键作为"返回"，可收起面板）；每次按下有 60ms 震动反馈
- **时间**：BM8563 RTC + SNTP（联网自动对时并回写 RTC），固定 UTC+8；无网络时继续走 RTC，未对时显示 `--:--`
- **串口服务终端**：`status` / `time`（含 `time set YYYY-MM-DD HH:MM:SS` 手工对时）/ `heap` / `power` / `touch` / `log level <tag|*> <level>` / `reboot`

---

## 架构

只有一个组件直接接触硬件（`bsp`），其余组件通过**总线**交换强类型消息；
状态类信息单向广播，输入类事件直连（LVGL indev / 按键事件），命令类也走总线。

```
main                 启动编排（bsp → bus → time_service → net → power_service → button_service → gui → app_console）
├── bsp              板级：显示/背光/RTC/触摸/虚拟键/震动/关机（封装 M5Unified，唯一碰硬件的组件）
├── bus              强类型发布订阅总线；保留每个 topic 的最新值（晚订阅者立即拿到当前状态）
├── time_service     BM8563 + SNTP        → BUS_TOPIC_TIME_UPDATED / TIME_INVALID
├── net              WiFi 连接与开关      → 发布 BUS_TOPIC_NET_STATE，订阅 BUS_TOPIC_WIFI_SET
├── power_service    PMU 电量/充电        → BUS_TOPIC_POWER_STATE
├── button_service   屏下三虚拟键         → BUS_TOPIC_BUTTON{LEFT|CENTER|RIGHT, PRESS|RELEASE|LONG}
├── gui              LVGL 移植 + 主题 + 状态栏 + 下拉面板 + 时钟页 + 开机动画（订阅上述 topic）
└── app_console      串口服务终端
```

总线 topic 一览：

| topic | 方向 | payload |
|---|---|---|
| `TIME_UPDATED` / `TIME_INVALID` | time_service → 任意 | `bus_time_t {hour, minute, valid}` |
| `NET_STATE` | net → 任意 | `bus_net_state_t {state, enabled, ipv4}` |
| `POWER_STATE` | power_service → 任意 | `bus_power_state_t {level, charging, valid, millivolt}` |
| `BUTTON` | button_service → 任意 | `bus_button_t {id, action}` |
| `WIFI_SET` | gui → net（命令） | `bus_wifi_set_t {enabled}` |

---

## 目录结构

```
CMakeLists.txt            项目入口（EXTRA_COMPONENT_DIRS 指向 external/）
partitions.csv            分区表：nvs / phy_init / 4MB factory（无 OTA）
sdkconfig.defaults        可复现配置（16MB Flash、PSRAM、LVGL 刷新周期、IRAM 优化、WiFi 凭据留空）
dependencies.lock         组件管理器锁文件（仅 idf）
components/               项目组件（见上方架构）
  bsp/ bus/ time_service/ net/ power_service/ button_service/
  gui/                    LVGL 移植、shell（状态栏+面板）、主题、开机动画
    fonts/                生成好的位图字体（已入库，构建不需要字体源文件）
    boot_frames/          生成好的开机动画笔迹路径（已入库）
  app_console/            串口终端
main/                     启动编排
tools/                    生成脚本与依赖还原脚本
  gen_boot_anim.js        从字体轮廓导出开机动画笔迹路径
  restore_external.ps1    还原 external/ 第三方库（见下）
external/                 ✗ 不入库：LVGL / M5GFX / M5Unified
reference/                ✗ 不入库：字体、图标包、LVGL 转换工具（仅生成资产时用）
```

---

## 构建与烧录

### 0. 前置

- **ESP-IDF v5.5.5**（`idf.py --version` 能跑通；本机为 Windows + EIM 安装）
- M5Stack Core2 v1.1 与 USB-C 线；Windows 需安装 **CH9102** 驱动

### 1. 克隆与还原依赖

`external/` 不入库，克隆后先还原（脚本按 **commit** 固定版本，可复现）：

```powershell
git clone https://github.com/0nion9ght/core2v1.1_minipad.git
cd core2v1.1_minipad
pwsh -File tools/restore_external.ps1
```

手动还原（不使用脚本时）——下载后解压到 `external/`，目录名必须一致：

| 目录 | 版本 | 上游 | commit |
|---|---|---|---|
| `external/lvgl` | 9.6.0 | <https://github.com/lvgl/lvgl> | `80ca777e37a2b176770726a02e07a6fb79ef0b39` |
| `external/M5GFX` | 0.2.28 | <https://github.com/m5stack/M5GFX> | `d91077b9a607b59404e4e4a49f775c792bfae382` |
| `external/m5unified` | 0.2.21 | <https://github.com/m5stack/M5Unified> | `3eaaf828adfd0923c71ccc2e233a0199d9958faa` |

> ⚠️ 若要**删除/重命名**：这三个库的 `idf_component.yml` 会声明注册表依赖（lvgl 的可选编解码器、M5Unified 的
> `m5stack/m5gfx`），组件管理器会再拉一份到 `managed_components/`，导致重复组件。还原脚本会删掉这些 manifest，
> 手动还原时请同样处理（或不要使用组件管理器安装同名库）。

### 2. 生成 sdkconfig 并构建

```powershell
idf.py set-target esp32     # 依据 sdkconfig.defaults 生成 sdkconfig
idf.py build
```

### 3. 烧录与监视

```powershell
idf.py -p COM5 flash        # 端口按设备管理器里的实际值替换
idf.py -p COM5 monitor      # 退出：Ctrl+]
```

串口参数 **115200**，控制台（`core2>`）与日志共用该串口；输入 `help` 查看命令。

---

## WiFi 凭据（重要）

- 凭据只写在本地 **`sdkconfig`** 的 `CONFIG_APP_WIFI_SSID` / `CONFIG_APP_WIFI_PASSWORD`；
  **`sdkconfig` 已被 `.gitignore` 忽略，不会进入仓库**（`sdkconfig.defaults` 里这两项为空占位）
- 新克隆后填写方式（任选其一）：
  1. `idf.py menuconfig` → `Application` → 填入 SSID / 密码，保存后重新构建
  2. 直接编辑 `sdkconfig`（或把自己的密码写进**本地**的 `sdkconfig.defaults`，但注意别提交）
- 未配置时固件照常运行：时间仅依赖 BM8563，屏幕显示 `--:--`，可用串口 `time set` 手工对时

---

## 资源占用（实测）

| 项 | 数值 |
|---|---|
| app 分区 | 1.48MB / 4MB（余 65%） |
| IRAM | 93.8%（余 8.1KB） |
| DRAM（内部） | 约 70KB 空闲 |
| PSRAM | 约 4.1MB 空闲（渲染缓冲与开机动画蒙版在此） |

> IRAM 偏紧：后续加入较重功能（TLS/HTTP 等）若再溢出，可关闭 `CONFIG_ESP_WIFI_IRAM_OPT`
> （再省 >10KB，代价是 WiFi 吞吐下降，对本项目无感）。

---

## 已知限制

- M5GFX 启动时会打印 `Core2 touch version read failed (...)`：那是**面板版本识别**（ILI9342C vs E）的结果，
  触摸本身工作正常，可忽略
- ESP32 经典款只能映射 8MB PSRAM 中的 4MB；PSRAM 运行在 40MHz
- 下拉面板中「亮度 / 音量 / 主题 / 重启」四个按钮为占位，未绑定功能
- 触摸坐标覆盖屏下虚拟键区域（raw y ≥ 240），由 M5Unified 映射为 BtnA/BtnC，不会与屏幕内容冲突
- 下拉面板展开时覆盖状态栏以下全部区域；面板按钮若被拖动超过 10px 会被判定为"拖面板"而非点击

---

## 字体、图标与生成物

构建**不需要**字体源文件——生成结果已入库：

| 生成物 | 来源 | 许可 |
|---|---|---|
| `components/gui/fonts/bebas_neue_176.c`、`bebas_neue_20.c` | Bebas Neue Regular | SIL OFL 1.1 |
| `components/gui/fonts/cjk_20.c` | ChillDINGothic Regular（`确定要关机吗？取消` 共 9 字） | SIL OFL 1.1 |
| `components/gui/fonts/icons_20.c`、`icons_32.c` | Material Symbols Outlined | Apache-2.0 |
| `components/gui/boot_frames/boot_path.{c,h}` | `tools/gen_boot_anim.js` 生成 | 本项目代码 |

重新生成的方式（把字体/工具放回 `reference/`）：

- **位图字体**（文字与图标）：`lv_font_conv`
  ```bash
  node lv_font_conv.js --font reference/bebas_neue/BebasNeue-Regular.ttf \
      --symbols '0123456789:-' --size 176 --bpp 4 --format lvgl --no-compress \
      --lv-font-name bebas_neue_176 -o components/gui/fonts/bebas_neue_176.c
  ```
  图标字体同理（用 Material Symbols Outlined 的码点，如 `wifi` U+E63E、`power_settings_new` U+E8AC）
- **开机动画路径**：`node tools/gen_boot_anim.js`（依赖 `reference/` 中的 ChillDINGothic OTF 与 `opentype.js`）
- **图片资源**：`LVGLImage.py`（需 `pip install pypng`）

---

## 第三方与许可

本项目代码以 **MIT** 许可发布（见仓库 [LICENSE](LICENSE)）。

| 组件 | 许可 |
|---|---|
| [LVGL](https://github.com/lvgl/lvgl) | MIT |
| [M5GFX](https://github.com/m5stack/M5GFX) / [M5Unified](https://github.com/m5stack/M5Unified) | MIT |
| [Bebas Neue](https://github.com/dharmatype/Bebas-Neue) | SIL Open Font License 1.1 |
| ChillDINGothic | SIL Open Font License 1.1 |
| [Material Symbols](https://github.com/google/material-design-icons) | Apache License 2.0 |
| [lv_font_conv](https://github.com/lvgl/lv_font_conv) | MIT |

字体仅以**转换后的位图字体 C 源文件**形式随固件分发（各字体的完整许可文本未随仓库提供，
需要时请从上游获取）。

---

## 开发说明

- 开发与调试过程中借助了 `.agents/skills/` 中的工程技能（该目录属个人工具链，未入库）
- 构建脚本示例（Windows + EIM 环境）：`tools/` 下的还原脚本；构建/烧录沿用 `idf.py`
