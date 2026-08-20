# 熊猫玩具系统运行逻辑文档

> 注意：本文含有早期“头部＋双手”产品的残留内容，其 IO15/IO16/IO17 映射不适用于尾巴小熊猫。本产品的确认映射和当前动作系统请以 [小熊猫“活体感”动作系统 V2](red_panda_living_motion_v2.md) 为准。

## 1. 系统总览

```
┌─────────────────────────────────────────────────────────────────┐
│                        ESP32-S3 主控                            │
├─────────────────────────────────────────────────────────────────┤
│  app_main()                                                     │
│   ├─ InitPower()     电源管理 (IO7 latch, IO6 ADC)               │
│   ├─ InitAudio()     ES8311 音频初始化                           │
│   ├─ InitServos()    3路舵机 PWM                                 │
│   ├─ InitAutoRun()   自动运行动画任务 (FreeRTOS Task)             │
│   ├─ InitWiFi()      WiFi AP 模式                                │
│   └─ StartHttpServer() HTTP 控制面板                             │
└─────────────────────────────────────────────────────────────────┘
```

### 源文件结构

| 文件 | 职责 |
|------|------|
| [config.h](main/config.h#L1) | 所有可调参数宏定义 |
| [main.cc](main/main.cc#L1) | 入口，初始化顺序 |
| [auto_run.cc](main/auto_run.cc#L1) | 自动运行状态机 (FreeRTOS Task) |
| [audio.cc](main/audio.cc#L1) | ES8311 音频播放引擎 |
| [panda_samples.h](main/panda_samples.h#L1) | 9 段 PCM 音频数据声明 |
| [servo.cc](main/servo.cc#L1) | LEDC PWM 舵机驱动 |
| [power.cc](main/power.cc#L1) | 电源按键 + 电池检测 |
| [wifi.cc](main/wifi.cc#L1) | WiFi AP |
| [http_server.cc](main/http_server.cc#L1) | HTTP 控制接口 |

---

## 2. 硬件引脚分配

```
ESP32-S3
├─ 电源
│   ├─ IO7  (OUT)   → 电源锁存 (HIGH=保持供电)
│   └─ IO6  (ADC)   → 电源按键检测 (<1V=按下)
├─ 电池
│   └─ IO3  (ADC)   → 电池电压 (分压比 4.7:2k)
├─ 舵机 (LEDC PWM 50Hz)
│   ├─ IO4  (OUT)   → 舵机电源控制
│   ├─ IO15 (PWM)   → 头部舵机 (index 0)
│   ├─ IO16 (PWM)   → 左手舵机 (index 1)
│   └─ IO17 (PWM)   → 右手舵机 (index 2)
├─ 音频 ES8311
│   ├─ IO2  (I2C)   → SDA (控制指令)
│   ├─ IO38 (I2C)   → SCL (控制时钟)
│   ├─ IO13 (I2S)   → WS  (字时钟)
│   ├─ IO48 (I2S)   → BCLK (位时钟)
│   └─ IO46 (I2S)   → DOUT (PCM数据输出)
└─ WiFi
    └─ 内置天线     → AP 模式, SSID: paxing_hx
```

---

## 3. 初始化顺序 (app_main)

```
app_main()
│
├─ 1. nvs_flash_init()           NVS 存储初始化
│
├─ 2. InitPower()                电源管理
│     ├─ IO7 HIGH → 锁存供电
│     └─ 启动 IO6 ADC 监控 (长按1.5s关机)
│
├─ 3. InitAudio()                ES8311 音频系统
│     ├─ I2C0 总线初始化 (扫描全部设备)
│     ├─ Probe ES8311 (7-bit addr 0x18)
│     │   └─ 失败 → 打印警告, 后续功能正常运行 (无声音)
│     ├─ I2S0 通道 (24kHz, 16-bit, 单声道)
│     ├─ Codec 接口 + Device 创建
│     └─ 启动 AudioPlayTask (优先级3, 栈4KB)
│
├─ 4. InitServos()               舵机初始化
│     ├─ IO4 供电 ON
│     ├─ LEDC Timer 50Hz / 13bit
│     └─ 3 通道初始角度 90°
│
├─ 5. InitAutoRun()             自动运行
│     └─ 创建 AutoRunTask (优先级2, 栈4KB, tick=20ms)
│
├─ 6. InitWiFi()                 WiFi AP
│     └─ SSID: paxing_hx, 开放网络
│
└─ 7. StartHttpServer()         HTTP 服务器
      └─ 监听 192.168.4.1:80
```

---

## 4. 音频引擎 (audio.cc)

### 4.1 初始化

```
InitAudio()
  │
  ├─ I2C0 总线: SDA=IO2, SCL=IO38, 内部上拉
  ├─ 全总线扫描: 打印所有 I2C 设备
  ├─ Probe ES8311: 7-bit addr 0x18
  │   └─ 注意: ES8311_CODEC_DEFAULT_ADDR=0x30 是 8-bit 格式,
  │      传给 i2c_master_probe() 必须右移一位 → 0x18
  │
  ├─ I2S0 TX: Master, 24kHz, 16-bit 单声道
  │
  ├─ Codec 配置: DAC 模式, 无 MCLK, pa_voltage=5.0V, dac_voltage=3.3V
  │
  ├─ Device: esp_codec_dev_open() → 锁定参数
  ├─ 音量: esp_codec_dev_set_out_vol(AUDIO_OUTPUT_VOLUME)
  │
  └─ 播放队列: xQueueCreate(8, sizeof(int))
     └─ AudioPlayTask: 阻塞等待 → 播放 → 阻塞等待 → ...
```

### 4.2 播放任务 (AudioPlayTask)

```
loop:
  xQueueReceive(portMAX_DELAY)  ← 阻塞等待音效请求
    ↓
  is_playing_ = true
    ↓
  while (offset < total_samples):
    esp_codec_dev_write(chunk=240 samples)  ← 10ms 数据
    vTaskDelay(5ms)                         ← 释放 CPU
    ↓
  is_playing_ = false
    ↓
  回到阻塞等待
```

### 4.3 接口

| 函数 | 行为 |
|------|------|
| `PlayPandaSound(type)` | 非阻塞入队, 队列有内容则静默丢弃 |
| `IsAudioPlaying()` | 返回 `is_playing_` 标志 |
| `FlushAudioQueue()` | 清空队列 (不打断当前播放) |

### 4.4 音效列表

| idx | 名称 | 用途 | 时长 |
|-----|------|------|------|
| 0 | 虫鸣鸟叫下雨流水声 | Relax 专用 | ~158s |
| 1-8 | 各种熊猫叫声 | 随机触发 | 1-5s |

---

## 5. 自动运行状态机 (auto_run.cc)

### 5.1 核心循环 (每 20ms 一次 tick)

```
┌─────────────────────────────────────────────────────────────┐
│                    AutoRunTask 主循环                        │
│                                                             │
│  1. Relax 定时器检查                                         │
│  2. 音频状态机 (now_playing vs was_playing 边沿检测)          │
│  3. Nature Sound 变速器 (每 8~17s 切换动作)                   │
│  4. 计算 Head 角度 → SetServoAngle(0)                        │
│  5. 计算 Crawl 角度 → SetServoAngle(1,2)                     │
│  6. 静音期间: 计时到 → 触发随机短音效                         │
│  7. 播放期间: 持续推后 next_sound_tick                        │
└─────────────────────────────────────────────────────────────┘
```

### 5.2 三大状态

```
 ┌──────────┐    计时到            ┌──────────────┐
 │  IDLE    │ ─────────────────→  │  REACTING    │
 │  静音    │                     │  短音效播放   │
 │  微动    │ ←────── 音效结束 ─── │  动作增强    │
 └──────────┘                     └──────────────┘
      │                                  │
      │  ~90s 周期                       │
      ↓                                  ↓
 ┌──────────────────────────────────────────┐
 │              NATURE SOUND                │
 │  自然白噪音播放中 (最长达 158s)             │
 │  每 8~17s 切换一种舒缓模式                │
 │  - Head: IDLE / NOD / TILT / RELAX       │
 │  - Crawl: RELAX / ALT 交替               │
 │  - Amp: 0.35~0.55                        │
 └──────────────────────────────────────────┘
```

### 5.3 各状态详细参数

| 状态 | Head 模式 | Crawl 模式 | Crawl Amp | 说明 |
|------|----------|-----------|-----------|------|
| **IDLE** | IDLE (±4°, 6s) | RELAX (±15°) | 0.06~0.10 | 几乎静止, 微弱呼吸 |
| **REACTING** | 随机: NOD / TILT / SHAKE (±15~20°) | 随机: ALT / BOTH / NARROW | 0.50~0.70 | 声音响起, 动作跟随 |
| **NATURE** | 随机: IDLE / NOD / TILT / RELAX (±4~18°) | 随机: RELAX / ALT | 0.35~0.55 | 舒缓持续, 每8~17s 切换 |

### 5.4 Head 运动参数表

| 模式 | 中心 (°) | 振幅 (±°) | 周期 (s) | 效果 |
|------|---------|----------|---------|------|
| HEAD_IDLE | 90 | 4 | 6.0 | 几乎不可见的呼吸 |
| HEAD_NOD_SLOW | 90 | 18 | 1.5 | 轻柔点头 |
| HEAD_TILT_SLOW | 80 | 15 | 3.0 | 向左偏头, 缓慢晃动 |
| HEAD_SHAKE_SLOW | 90 | 20 | 2.0 | 轻柔左右摇头 |
| HEAD_RELAX | 90 | 10 | 5.0 | 极慢微动 |

### 5.5 Crawl 运动参数表

| 模式 | 左手范围 | 右手范围 | 周期 | 模式 |
|------|---------|---------|------|------|
| CRAWL_ALT | 90°↔0° | 90°↔180° | 0.8s | 交替爬行 |
| CRAWL_BOTH | 90°↔0° | 90°↔180° | 1.0s | 同步爬行 |
| CRAWL_NARROW | 90°↔40° | 90°↔140° | 0.6s | 窄幅爬行 |
| CRAWL_RELAX | 90°↔75° | 90°↔105° | 6.0s | ±15° 极慢摇摆 |

实际摆动 = (极限值 - 90°) × crawl_amp

### 5.6 爬行算法

使用 sin² 平滑曲线 (非线性的 `sin²(π·phase)`) 让动作在端点处减速, 模拟生物运动:

```
v = sin²(π·phase)      ← 0→1→0 平滑过渡
angle = start + (end - start) × v × amp

交替模式: 右手相位 = 左手相位 + 0.5 (偏移半个周期)
同步模式: 左右手同相
```

### 5.7 动作切换 Crossfade

当切换 crawl 模式时, 使用 1 秒的 smoothstep 过渡:

```
mix = t² × (3 - 2t)     ← Hermite 平滑
final = old × (1-mix) + new × mix
```

### 5.8 音频调度时序

```
              sound A 播放 (2s)              静音 25s              sound B
...──────┬──────────────────┬─────────────────────────────────┬───────────→
         ↑                  ↑                                 ↑
     tick=X              tick=X+100                         tick=X+1350
     TriggerRandomSound()  is_playing_=false               计时到, 触发 B
     next_sound_tick = X   next_sound_tick 在播放期间
                          被推到 X+100 + 20~40s
```

关键代码:
```cpp
// 播放期间: 每 tick 把计时器推到 "当前 + 20~40s"
if (now_playing)
    next_sound_tick = tick + (20~40s);

// 静音且计时到: 触发
if (!in_relax && !now_playing && tick >= next_sound_tick)
    TriggerRandomSound();
```

### 5.9 Relax 与 Nature Sound 的生命周期

```
Timeline (s):  0        20       90      110     158      180
              ├────────┤        ├───────┤       ├────────┤
              │ relax1 │        │relax2 │       │ relax3 │
              └────────┘        └───────┘       └────────┘
              ├───────────────────────────────────────────┤
              │         nature sound #1 (158s)             │
              └───────────────────────────────────────────┘

relax1 (t=0):  nature_playing=false → Flush + Play → nature_playing=true
relax2 (t=90): nature_playing=true → 跳过 Flush+Play, 直接续用
relax3 (t=180): nature_playing=false → 重新播放新的 nature sound
```

`nature_playing` 标志独立于 `in_relax`, 确保:
- Variation cycling 在整段 158s 内持续
- 不会重复 queue 同一个超长音效
- 音效结束后自然过渡到 IDLE

---

## 6. 舵机控制 (servo.cc)

- **PWM**: LEDC 50Hz, 13-bit 分辨率
- **角度映射**: `pulse = 500μs + angle × 2000μs/180°`
- **范围**: 0~180° (对应 500~2500μs)
- **3 通道**: Head(IO15), LH(IO16), RH(IO17)
- **上电**: IO4 拉高供电

---

## 7. HTTP API

| 端点 | 方法 | 参数 | 说明 |
|------|------|------|------|
| `/` | GET | - | 返回控制面板 HTML |
| `/autoplay` | POST | `{"enable":1,"hard_swing":0}` | 开关自动运行 / 切换硬摆动 |
| `/sound/x` | GET | x=0~8 | 手动触发指定音效 |
| `/servo?idx=0&angle=90` | GET | idx, angle | 手动设置舵机角度 |

---

## 8. 可调参数 (config.h)

| 宏 | 默认 | 说明 |
|------|------|------|
| `AUDIO_OUTPUT_VOLUME` | 80 | 音量 0-100% |
| `AUDIO_SILENT_INTERVAL_MIN_S` | 20 | 短音效间最小间隔 (秒) |
| `AUDIO_SILENT_INTERVAL_MAX_S` | 40 | 短音效间最大间隔 (秒) |
| `AUDIO_SAMPLE_RATE` | 24000 | 音频采样率 Hz |
| `ENABLE_AUTO_RUN` | 1 | 编译自动运行 |
| `AUTO_RUN_DEFAULT_ON` | 1 | 开机自动启动 |
| `AUTO_RUN_DEFAULT_HARD` | 0 | 默认硬摆动 (0=sin²平滑) |
| `HARD_SWING_SPEED_X` | 4.0 | 硬摆动速度倍率 |

---

## 9. FreeRTOS 任务一览

| 任务名 | 优先级 | 栈 | 职责 |
|------|--------|------|------|
| `panda_play` | 3 | 4KB | 音频播放: 阻塞等队列, 分块写 codec |
| `auto_run` | 2 | 4KB | 自动运行: 20ms tick, 状态机 + 舵机更新 |
| `main_task` | 1 | - | ESP-IDF 主任务 |
| HTTP server | 1 | - | 处理 Web 请求 |
