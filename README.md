# TailPanda — ESP32-S3 小熊猫动作控制器

基于 ESP32-S3 的 Wi-Fi 遥控舵机控制器，内嵌 Web 控制面板，用于控制小熊猫玩偶的**头部转动**、**尾巴 360° 运动**、**呼吸动画**以及**音频播放**。

> 最新动作系统请见 [小熊猫“活体感”动作系统 V2](docs/red_panda_living_motion_v2.md) 和 [完整动作表与新旧对比](docs/actions.md)。该版本使用一次性行为片段、洗牌防重复、平衡左右镜像和实时 PCM 音频包络。

## 项目概览

| 项目         | 说明                                    |
| ------------ | --------------------------------------- |
| **芯片**     | ESP32-S3                                |
| **舵机**     | 3 路 180° 舵机 (PWM 50Hz)               |
| **音频**     | ES8311 DAC，Opus 解码，SPI Flash 存储    |
| **Wi-Fi**    | STA 模式，连接路由器后从服务器同步音频文件 |
| **框架**     | ESP-IDF v5.5 (FreeRTOS)                 |
| **语言**     | C++ (C++17)                             |
| **编译产物** | `action_cat.bin`                        |

## 硬件引脚

### 电源管理

| 引脚    | 功能                   | 说明                                         |
| ------- | ---------------------- | -------------------------------------------- |
| **IO7** | `POWER_CTRL`           | 电源锁存 — HIGH 保持供电，LOW 断电           |
| **IO6** | `POWER_OUT` (ADC1_CH5) | 电源按钮 — 长按 1.5s 关机                   |
| **IO3** | 电池电压 (ADC1_CH2)    | 分压比 R_upper=2k / R_lower=4.7k，分压系数 ×2 |

### 舵机 (3 × 180°)

| 引脚     | 名称               | 默认角 | 范围    | 说明                                     |
| -------- | ------------------ | ------ | ------- | ---------------------------------------- |
| **IO4**  | `SERVO_POWER`      | —      | —       | 舵机电源使能                             |
| **IO18** | Head 头部           | 90°    | 0°~180° | 0°=右转, 90°=正中, 180°=左转             |
| **IO15** | Tail LR 尾巴左右    | 90°    | 0°~180° | 0°=最左, 90°=正中, 180°=最右             |
| **IO16** | Tail UD 尾巴上下    | 180°   | 0°~180° | 0°=最上, 90°=中间, 180°=最下             |

> 默认姿态：头正中 (90°)，尾巴在中间最下面 (LR=90°, UD=180°)。尾巴通过舵盘拉线实现 360° 运动。

### 音频 (ES8311)

| 引脚   | 功能            |
| ------ | --------------- |
| IO2    | I2C SDA         |
| IO38   | I2C SCL         |
| IO13   | I2S WS           |
| IO48   | I2S BCLK         |
| IO46   | I2S DOUT         |
| IO14   | I2S DIN          |

### SPI Flash (W25Q512)

| 引脚   | 功能  |
| ------ | ----- |
| IO10   | CS    |
| IO9    | CLK   |
| IO47   | MOSI  |
| IO21   | MISO  |

## 启动流程

```
app_main()
  ├─ nvs_flash_init()           NVS 初始化
  ├─ InitPower()                锁存 IO7 HIGH，启动电源监控任务
  ├─ InitAudio()                初始化 ES8311 + I2S + Opus 解码器
  ├─ InitServos()               3 路舵机归默认角度 (90, 90, 180)
  ├─ InitWiFi()                 连接路由器
  ├─ flash_audio_init()         挂载 SPI Flash 音频 TOC
  ├─ StartHttpServer()          启动 HTTP 服务
  ├─ sync_audio_files()         从服务器同步音频文件到 Flash
  └─ InitAutoRun()              启动自运行动画任务 (呼吸 + 随机叫声音效)
```

## 编译宏

| 宏                         | 默认值  | 作用                                   |
| -------------------------- | ------- | -------------------------------------- |
| `ENABLE_AUTO_RUN`          | `1`     | 编译自运行动画功能                      |
| `AUTO_RUN_DEFAULT_ON`      | `1`     | 上电后动画默认开启                      |
| `AUTO_RUN_DEFAULT_HARD`    | `0`     | 默认使用 sin² 柔滑曲线 (0) 或硬摆 (1)   |
| `HARD_SWING_SPEED_X`       | `4.0f`  | 硬摆模式速度倍率                        |
| `AUDIO_OUTPUT_VOLUME`      | `80`    | 音量 0-100                              |
| `AUDIO_SILENT_INTERVAL_MIN_S` | `20` | 两次随机叫声音效最小间隔 (秒)            |
| `AUDIO_SILENT_INTERVAL_MAX_S` | `40` | 两次随机叫声音效最大间隔 (秒)            |

## FreeRTOS 任务

### `power_mon` — 电源监控

- 每 20ms 轮询 IO6 ADC，对称迟滞消抖 (50ms)
- 长按 1.5 秒 → IO7 拉 LOW，系统断电
- 每 5 秒采样电池电压 32 次过采样 + 一阶低通滤波
- 电量百分比: `(Vbat - 3.2V) / (4.2V - 3.2V) × 100%`

### `auto_run` — 自运行动画

上电自动启动，控制频率 50Hz（每 20ms 一帧）。V2 使用 27 个一次性关键帧片段，不再让头尾长期重复固定正弦：

- 日常、好奇、进食、幼崽、威风、撒娇、玩耍、长环境声共 8 个动作家族
- 尾巴覆盖低、中、高位置，包含大范围扫尾、抬尾、画圆和 8 字
- 分家族洗牌防重复、随机节奏、受约束左右镜像
- 音频按文件名语义和类别自动选动作，新增音频不依赖固定 TOC 编号
- PCM 实时提取 `level` 与 `attack`：头先注意，尾巴延迟回应
- 每个完整片段结束时头部缓慢回到 90°正前

完整动作表、音频映射、保护范围及新旧对比见 [docs/actions.md](docs/actions.md)。

#### Hard Swing (硬摆)

通过网页开关或宏启用后，动作整体节奏加快约 22%。关键帧仍保持平滑起停，不采用高频机械抖动。

## PWM 舵机控制

- **频率**：50Hz (周期 20ms)
- **脉冲范围**：500μs (0°) ~ 2500μs (180°)
- **分辨率**：13-bit
- **映射公式**：`pulse_us = 500 + angle × 2000 / 180`

## HTTP API

| 方法       | 路径             | 说明                                             |
| ---------- | ---------------- | ------------------------------------------------ |
| `GET`      | `/`              | Web 控制面板                                     |
| `POST`     | `/api/servo`     | 设置舵机 `{"angles":[head, tail_lr, tail_ud]}`    |
| `GET`      | `/api/battery`   | 电池状态 `{"voltage_mv":xxx,"level":xx}`          |
| `GET/POST` | `/api/autoplay`  | 查询/切换自运行动画 + hard_swing                  |
| `GET/POST` | `/api/flash/*`   | SPI Flash 音频文件管理 (上传/列表/删除)             |
| `GET`      | `/api/files`     | 列出 Flash 中音频文件                              |

## Web 控制面板

内嵌固件中的单页 Web 应用：

| 模块           | 功能                                                      |
| -------------- | --------------------------------------------------------- |
| **头部控制**   | 滑块 0°~180°，快捷按钮 (右转/正中/左转)                    |
| **尾巴控制**   | 双轴滑块 (LR 左右 + UD 上下) + Canvas 触摸摇杆            |
| **动作预设**   | 一键执行：呼吸 / 摇尾巴 / 画圈 / 环顾                      |
| **自运行动画** | 开关按钮 + sin²/硬摆 曲线切换                              |
| **电池显示**   | 电量百分比 + 电压，5s 刷新                                 |

## 音频系统

- **编解码**：Opus 48kHz 单声道，流式解码 (60ms 帧)
- **存储**：W25Q512 SPI Flash (64MB)，TOC 索引管理
- **同步**：WiFi 连接后自动从服务器 (`SYNC_SERVER_IP:5000`) 拉取音频文件
- **播放**：非阻塞队列播放，一次只播放一个音效
- **文件管理**：Web 端可通过 Flash Upload 端点管理音频文件

## 开发

### 构建

```bash
source ~/.espressif/v5.5.4/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
```

### 烧录

```bash
idf.py -p /dev/ttyUSB0 flash
```

### 音频文件准备

将 `.opus` 文件放入 `opus_audio/` 目录，运行构建脚本生成 `opus_data.bin` 并烧录到 SPI Flash，或通过 WiFi 从服务器同步。

```bash
python3 scripts/build_opus_bin.py
python3 scripts/flash_opus.py
```

## 文件结构

```
tailPanda_1.0_wifi/
├── CMakeLists.txt
├── sdkconfig / sdkconfig.defaults
├── README.md
├── main/
│   ├── CMakeLists.txt
│   ├── config.h              # 引脚、参数、编译宏
│   ├── main.cc               # 入口 app_main
│   ├── power.h / power.cc    # 电源管理
│   ├── servo.h / servo.cc    # 舵机 PWM
│   ├── wifi.h / wifi.cc      # WiFi STA
│   ├── audio.h / audio.cc    # ES8311 + Opus 音频播放
│   ├── flash_audio.h / flash_audio.cc  # SPI Flash 音频存储
│   ├── ogg_demuxer.h / ogg_demuxer.cc  # OGG 容器解析
│   ├── w25q512.h / w25q512.cc          # W25Q512 Flash 驱动
│   ├── auto_run.h / auto_run.cc        # 自运行动画系统
│   ├── http_server.h / http_server.cc  # HTTP 服务 + Web UI
│   ├── sync_audio.h / sync_audio.cc    # 音频文件网络同步
│   ├── flash_upload_server.h / flash_upload_server.cc  # Flash 上传端点
│   └── panda_samples.h / panda_samples.cc  # 音效类型枚举
├── scripts/
│   ├── build_opus_bin.py     # 构建 Opus 数据镜像
│   └── flash_opus.py         # 烧录 Opus 数据到 Flash
├── docs/
│   ├── audio.md
│   ├── opus_flash_audio.md
│   └── system.md
└── .vscode/ / .devcontainer/
```

## 设计要点

1. **长按关机**：硬件锁存电路，短按开机、长按 1.5s IO7 拉 LOW 断电
2. **尾巴 360° 运动**：通过两个舵机 (LR + UD) 组合控制，实现画圈、8 字、摇尾等多种轨迹
3. **呼吸动画**：默认持续运行，头部 ±8° + 尾巴底部微胀，模拟生命感
4. **音效联动**：播放叫声音效时自动匹配相应动作，增强表现力
5. **PWM 精度**：13-bit @ 50Hz，角度精度约 0.22°
6. **电池监测**：ADC 曲线拟合校准 + 32×过采样 + 指数移动平均
7. **Flash 存储**：64MB W25Q512，TOC 索引支持 32 个 Opus 音频文件
8. **手动优先**：网页手动控制舵机时自动暂停自运行动画

## Audio Hub 设备绑定

联网模式下，`main/device_registry.cc` 会使用 ESP32 出厂 MAC 生成唯一设备 ID。首次启动时向 Audio Hub 注册，并在串口输出六位激活码；管理员在服务端后台绑定后，设备把正式令牌保存到 NVS，并每 60 秒上报一次心跳。

启用前将 `main/config.h` 的 `OFFLINE_DEMO` 改为 `0`，正确填写 Wi-Fi、`SYNC_SERVER_IP` 和 `SYNC_SERVER_PORT`。完整协议见服务端仓库的 `docs/DEVICE_ONBOARDING.md`。
