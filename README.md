# Travel Frog (旅行青蛙) — ESP32-S3 圆形屏游戏

目标硬件：Waveshare ESP32-S3-Touch-LCD-1.85B（360×360 圆形屏，ST77916 QSPI + CST816 触摸 + ES8311 音频 + PCF85063A RTC）

## 目录结构

```
shared/          平台无关代码（两个目标共用）
  core/          游戏逻辑：状态机、经济、奖励 + 存档帧（tf_persist）
  ui/            LVGL 9 圆形 UI（同心环布局）、素材表接口 tf_assets.h
sim/             PC 模拟器（SDL2 + LVGL，MSVC/CMake；音频走 SDL_QueueAudio）
firmware/        ESP-IDF v5.5 固件（Waveshare BSP）
assets/          美术资源（AI 生成像素风，洋红底待抠图）
  audio/         音效（tools/gen_sfx.py 合成，22050Hz/mono/16bit，烧进 storage 分区）
```

## PC 模拟器

```bash
# 需要 Visual Studio 2022 + 任意 cmake（ESP-IDF 自带的可用）
cmake -G "Visual Studio 17 2022" -A x64 -S sim -B sim/build
cmake --build sim/build --config Debug -j 8
sim/build/Debug/travel_frog_sim.exe
```

- 存档：运行目录 `travel_frog_save.bin` + `travel_frog_save.bak`（两个轮换槽位）
- 「速度」按钮循环时间流速 ×1/×30/×300，用于快速验证旅行/三叶草计时
- 首次构建会联网拉取 LVGL v9.3.0 与 SDL2 预编译包
- 截图验证（无需人工点击，配合 PrintWindow 抓窗口）：

```powershell
# 只截图
powershell -File tools/sim_shot.ps1 -Out shot.png
# 先点模拟器客户区坐标 (x,y) 再截图
powershell -File tools/sim_shot.ps1 -X 84 -Y 272 -Out shot.png
```

## 固件

**中文渲染必须单线程（`CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=1`）**，这是实机「文字不停变乱码」的真正根因：
LVGL 9.3 解压压缩字形用的 RLE 游标是**一整个进程只有一份的全局状态**
（`lv_font_fmt_txt.c:22` → `#define font_rle LV_GLOBAL_DEFAULT()->font_fmt_rle`），而且**没有加锁**。
`DRAW_UNIT_CNT=2` 会起两个渲染线程并行解码不同汉字，游标互相覆盖，解出来的点阵就是随机垃圾。
表现完全对得上：只有文字乱（图片不走这条路）、只有被重画的区域乱（label 不失效就还是上一帧的正确像素，
所以底部 dock 平时正常、切「出发/召回/哄它」时偶尔乱）、且每帧乱的样子都在变。
模拟器复现不了，因为 `sim/lv_conf.h` 没设这一项、取 LVGL 默认值 1。
板厂例程 `02_lvgl_demo` 里是 2，但它只跑 Montserrat，扛不住 CJK 的解码压力——**这一项不要跟着例程改**。

内存分配同样要走系统堆（`CONFIG_LV_USE_CLIB_MALLOC=y`），内置池只有
`CONFIG_LV_MEM_SIZE_KILOBYTES=64` KB，中文字形缓存 + 卡片/弹窗阴影 layer 会把它打满；
`CONFIG_SPIRAM_USE_MALLOC=y` 让 `malloc` 能用 8MB PSRAM，所以 CLIB 分配是安全的。
不过实测换成 CLIB malloc 之后乱码并没有消失，真正的元凶是上面那条并发解码。

这三项与板厂例程 `E:\HardWork\ESP32-S3-Touch-LCD-1.85B\Examples\ESP-IDF-V5.5.3\02_lvgl_demo\sdkconfig.defaults`
保持一致，调 LVGL/PSRAM 相关配置时以该文件为准：
`CONFIG_LV_USE_CLIB_MALLOC / _STRING / _SPRINTF`。

```powershell
# Git Bash 不支持 IDF export，需 PowerShell（或 ESP-IDF 命令行快捷方式）
cd firmware
. C:\Users\dadachu\esp\v5.5\esp-idf\Export.ps1
idf.py set-target esp32s3   # 首次
idf.py build flash monitor
```

或直接：

```powershell
powershell -File firmware/build_fw.ps1              # 仅 build
powershell -File firmware/flash_fw.ps1 -Port COM3   # build + flash
```

抓启动日志（Git Bash 下最省事的方式）——注意**这块板 USB 串口的 DTR 接到 IO0**，
用 pyserial 直接 `Serial('COM3', ...)` 打开时会把 DTR 拉起来，配合随后的 RTS 复位脉冲
会把芯片带进 `DOWNLOAD(USB/UART0)`（一直打印 `waiting for download`），看起来像程序没跑：

```bash
# 先让 esptool 用 RTS 硬复位回正常启动，再用「不碰 DTR/RTS」的方式读
C:/Users/dadachu/.espressif/python_env/idf5.5_py3.11_env/Scripts/python.exe -m esptool \
  --chip esp32s3 -p COM3 run
```

```bash
C:/Users/dadachu/.espressif/python_env/idf5.5_py3.11_env/Scripts/python.exe -c "
import serial, time
s = serial.Serial()
s.dtr = False; s.rts = False      # 必须在 open 之前设，否则 IO0 被拉低
s.port = 'COM3'; s.baudrate = 115200; s.timeout = 0.2
s.open()
t0 = time.time()
while time.time() - t0 < 8: print(s.read(4096).decode('utf-8','replace'), end='')
"
```

真掉进下载模式了就用上面的 `esptool run` 拉回来；手动进下载模式则是**按住 BOOT 再上电/复位**。

## 存档与断电

**按键操作**（官方文档 <https://docs.waveshare.net/ESP32-S3-Touch-LCD-1.85B>）：
PWR 键**短按 1 秒开机、长按 2 秒关机**，插 USB 时自动通电开机；进下载模式是
**按住 BOOT 再上电/复位**。所以「长按开不了机」不是故障——长按是关机手势，开机要短按一下。

**硬件电源键不参与固件**：Key1 → U6 ECJ23001（KEY 脚）→ U6 OUT = PWR_EN →
Q4/Q5 锁存 → DC-DC 通断，整条链路没有 ESP32 的 GPIO。也就是说长按关机是**瞬间掉电**，
程序拿不到任何预警，也无法软件关机。所以存档必须按「随时会被拔电」来设计：

- `shared/core/tf_persist.c`：一帧 = 魔数 + 版本 + 长度 + 递增 seq + saved_at_ms + CRC32 + `tf_save_t`。
- **两个 NVS 槽位轮换写**（`game/save0`、`game/save1`），加载时取 CRC 通过且 seq 最大的那个。
  写一半掉电只会毁掉新帧，旧帧仍在。
- 任何状态变化都落盘：派出/召回、收割、购物、行程结算、生气结束。
- 时间基准用 **PCF85063A RTC** 而不是 `esp_timer`：开机读 RTC 作为时钟起点，
  关机期间行程照常推进。RTC 失效（掉电且无后备、或年份 <2020）时退回 2020-01-01，
  并把 RTC 写到该时间，下次开机继续。
- `tf_port_clock_floor()`：读档后如果时钟比 `saved_at_ms` 还小，就把时钟整体前推并回写 RTC，
  避免「重启把进行中的行程推回未来」或时间倒流。

模拟器同样走这套帧格式（`travel_frog_save.bin` / `.bak` 两个文件），只是时钟来自 SDL ticks。

## 玩法（M1 骨架范围）

- 状态机：在家 → 派出（需便当）→ 外出计时 → 自动回家带明信片/特产
- 召回：外出中点「出发/召回」，空手回家 + 生气 30s，无奖励
- 三叶草：庭院 4 个点位（带呼吸光晕便于在草地上识别），收割后 20s（游戏时间）重生
- 商店：便当 10 / 护身符 25（远途，明信片池升级）/ 帐篷 20（行程×2）
- 图鉴：12 张明信片格子，未收集显示占位，底部给出收集进度

底部 dock 四个按钮：商店 / 出发（外出时变「召回」，生气时变「哄它」）/ 图鉴 / 速度。

## 字体（中文）

LVGL 自带的 CJK 字体只有约 1338 字，缺「叶/草/发/图/蛙」等本作用字，不可直接用。
改为按实际文案裁剪子集：

```bash
python tools/gen_font.py     # 扫描 shared/**/*.c 的字符串字面量 -> 生成子集字体
```

产出 `shared/ui/tf_font_cn_16.c`、`tf_font_cn_20.c` 与 `tf_fonts.h`
（`TF_FONT_BODY` / `TF_FONT_TITLE`，正体/粗体 Noto Sans SC，OFL 可商用）。
**改完中文文案必须重跑一次**，否则新字会画不出来。构建侧要求：

- 字体是 `lv_font_conv` 默认压缩输出 → 必须开 `LV_USE_FONT_COMPRESSED`
  （模拟器 `sim/lv_conf.h`，固件 `CONFIG_LV_USE_FONT_COMPRESSED=y`），否则启动即段错误。
- MSVC 默认按本地代码页解析源码，中文会乱码 → `sim/CMakeLists.txt` 里加了 `/utf-8`。
- 生成文件默认 `#include "lvgl/lvgl.h"`，IDF 组件只有 `"lvgl.h"` → 固件定义
  `LV_LVGL_H_INCLUDE_SIMPLE`。

## 后续路线

M1 双构建骨架 + 逻辑（已完成）
M2a 模拟器像素素材接入（已完成：庭院背景 / 青蛙在家与生气 / 三叶草 / dock 图标）
M2b 中文与 UI 美化（已完成：字体子集、圆屏安全区、气泡/弹窗/动画）
M2c 固件素材打包（已完成：预解码 C 数组，实机烧录验证）
M3 RTC 离线行程结算（已完成：RTC 时钟基准 + 断电安全存档；剩余：休眠省电）
M4 明信片图片奖励
M5 音效（已完成：ES8311 直推 PCM + SPIFFS 原创音效；剩余：背景音乐需要混音）
M6 WiFi（NTP/云存档）与 AI 语音（esp-sr 唤醒词）

## 音效

**格式必须是 22050 Hz / 单声道 / 16-bit**：BSP 把 I2S 写死成
`BSP_I2S_DUPLEX_MONO_CFG(22050)`（16bit、MONO 槽位），所以采样率/声道/位深都不用转换，
`esp_codec_dev_write` 直接把 PCM 推进 I2S 就行。全部素材由
`python tools/gen_sfx.py` 用振子合成，5 个音一共 56KB，不涉及任何第三方音频版权。

```bash
python tools/gen_sfx.py           # -> assets/audio/{click,harvest,croak,sad,reward}.wav
```

调用点只有一个：`tf_port_sfx(TF_SFX_xxx)`（`shared/core/tf_port.h`），枚举到文件名的表在
`shared/core/tf_sfx.c`，两个后端各实现一次：

- 模拟器 `sim/tf_snd_sdl.c`：`SDL_OpenAudioDevice` + `SDL_LoadWAV` + `SDL_QueueAudio`，
  所以**烧录前就能在 PC 上试听**；音量 `SDL_SetAudioDeviceGain`。
- 固件 `firmware/main/tf_snd_esp.c`：`bsp_spiffs_mount()` 挂 `/spiffs`，独立任务
  （优先级 5）解析 RIFF 分块找 `data`，每次读 2KB 进**内部 DMA RAM** 再写 codec——
  读 SPIFFS 本身就是一次 flash 操作、期间 cache 是关的，缓冲放 PSRAM 会出事。
  `tf_port_sfx` 只往队列里塞 id，绝不阻塞 LVGL 任务；SPIFFS 挂载失败只打日志、不拦住开机。

音效文件通过 `spiffs_create_partition_image(storage ../../assets/audio FLASH_IN_PROJECT)`
打进 `storage` 分区（0x620000，2MB），`idf.py flash` 一起写。

没走 `bsp_extra` 的 `Audio_Play_Music()`：那条是 GMF/`esp_audio_simple_player`，按路径播 MP3，
把 codec 开成 16kHz/双声道/32bit，还要 SD 卡。以后如果要做背景音乐（MP3 循环）再引入它，
并加一个能同时混两路的方案。

## 素材说明

旅行青蛙原版素材受版权保护，不可提取使用。`assets/generated/` 为 AI 生成的
原创像素风素材（洋红背景便于抠图），风格致敬但内容原创；`assets/ui/` 为处理后的成品。

生成/更新流程：

```bash
python tools/gen_assets.py       # 读 assets/generated/*.png -> 写 assets/ui/*.png
python tools/gen_fw_assets.py    # 读 assets/ui/*.png -> 写 firmware/main/tf_assets_gen.c
```

- AI 出的"洋红"实际约 RGB(205,45,133) 且带渐变，因此抠图取**边框中位色**做键色，
  用从边缘连通的洪泛填充去底（内部粉色不会被误删），半透明边缘再做去色溢。
- 背景按内容外接框裁圆后放大 3% 再套 360×360 圆罩，避免圆边露出键色缝隙。
- UI 只认 `shared/ui/tf_assets.h` 里的 `tf_asset_src(TF_ASSET_xxx)`，两个目标各自实现：
  模拟器返回 `"A:..."` 路径（`LV_USE_LODEPNG` + `LV_USE_FS_STDIO`，运行时解 PNG），
  固件返回预解码的 `lv_image_dsc_t`（`TF_HAS_ASSETS` 两边都定义，不再有矢量回退分支）。
- 固件侧格式：背景 **RGB565**（360×360 = 259,200 字节），精灵 **ARGB8888**，合计 379KB
  常量数据，`CONFIG_SPIRAM_RODATA` 会把它搬进 PSRAM。像素数组 `aligned(16)`
  以满足 `CONFIG_LV_DRAW_BUF_ALIGN=4`；`CONFIG_LV_DRAW_BUF_STRIDE_ALIGN=1` 所以 stride 无填充。
- **通道顺序是坑，且模拟器抓不到**（模拟器自己解 PNG，根本不走这些数组）：
  `lv_color16_t` 是 `blue:5, green:6, red:5`（蓝在低位），所以打包要写
  `blue | green<<5 | red<<11`；`lv_color32_t` 是 `{blue, green, red, alpha}`，
  所以 ARGB8888 在内存里是 **B,G,R,A**（`lv_lodepng` 解完 PNG 后专门做了一次 R/B 交换）。
  写反的现场表现是图标发蓝、背景色怪。校验方法：取源 PNG 若干像素，按上面的公式
  反解生成的 C 数组，比对回原色（量化误差 ±8 以内）。
- 注意：360×360 ARGB 实时解码需 ~518KB，`sim/lv_conf.h` 的 `LV_MEM_SIZE` 已提到 8MB；
  **固件不要照搬**，固件走上面的预解码路径。
- `assets/generated/dock_icons.png` 是 2×2 图标整图，`gen_assets.py` 按象限切成
  `icon_shop/go/card/spd.png`；改图后两个脚本依次重跑，再重新构建固件。

## 圆屏安全区

360×360 圆屏可用区是以 (180,180) 为圆心、r=180 的圆，控件外接矩形角必须落在圆内：

- 顶部状态胶囊 176×30 @ y=25（角点距圆心 ≈177）
- 底部 dock 4 个 56px 圆钮，圆心 (84,272)(146,300)(214,300)(276,272)（角点 ≈173）
- 弹窗面板 252 宽、内边距 10，图鉴 2×6 格子（角点 ≈178）
- 气泡最大宽 248 + 内边距，超出自动换行，避免长文案顶出圆边
