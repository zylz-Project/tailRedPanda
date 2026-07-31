# 音频播放系统

## 概览

本项目的音频系统基于 **ES8311 编解码芯片** + **I2S 协议**，通过 FreeRTOS 任务实现后台播放。自动运行调度实现了 **音频 ⇄ 动作联动**：播放时动作增强，静音时动作缓和，音效间隔由宏控制。

## 硬件架构

```
ESP32-S3
  ├─ I2C (IO2=SDA, IO38=SCL) ─── ES8311 Codec (地址 0x18)
  └─ I2S (WS=IO13, BCLK=IO48, DOUT=IO46) ─── ES8311 → PA → 扬声器
```

| 信号 | GPIO | 说明 |
|------|------|------|
| I2C SDA | IO2 | ES8311 控制指令 |
| I2C SCL | IO38 | ES8311 控制时钟 |
| I2S WS | IO13 | 左右通道时钟 |
| I2S BCLK | IO48 | 位时钟 |
| I2S DOUT | IO46 | PCM 数据输出 |
| MCLK | NC | 未使用（ES8311 内部 PLL 从 BCLK 恢复时钟） |

### I2C 地址注意事项

ES8311 的 **7-bit I2C 地址是 `0x18`**。宏 `ES8311_CODEC_DEFAULT_ADDR` 定义为 `0x30` 是 **8-bit 格式**（`0x18 << 1`），仅用于传递给 `audio_codec_new_i2c_ctrl()`（函数内部会自动右移）。对 `i2c_master_probe()` 必须使用 7-bit 地址 `0x18`。

## 音频参数

所有音频参数集中定义在 [config.h](main/config.h#L40-L55)：

```c
#define AUDIO_SAMPLE_RATE 24000            // 采样率 24kHz
#define AUDIO_OUTPUT_VOLUME 80             // 音量 0-100
#define AUDIO_SILENT_INTERVAL_MIN_S 20     // 音效间最短静音间隔
#define AUDIO_SILENT_INTERVAL_MAX_S 40     // 音效间最长静音间隔
```

PCM 格式：**16-bit 有符号，单声道，24kHz**。

## 代码架构

```
panda_samples.h/cc    →  音频样本数据（PCM 数组 + 时长）
audio.h/cc            →  播放引擎（初始化、队列、播放任务）
auto_run.cc           →  自动运行调度 + 音频-动作联动
config.h              →  所有可调参数
```

## 初始化流程

`InitAudio()` 按以下顺序初始化：

```
1. I2C 总线初始化 (IO2=SDA, IO38=SCL)
   ├─ 全总线扫描 (0x01-0x7E)，打印所有设备
   └─ 单独 probe ES8311 (7-bit addr 0x18)
        └─ 失败则 return（音频降级，其他功能正常）

2. I2S TX 通道初始化
   ├─ 标准模式 (I2S_STD)
   ├─ 24kHz, 16-bit, 单声道
   └─ I2S_NUM_0, Master 模式

3. Codec 接口创建
   ├─ audio_codec_new_i2s_data()    — 数据通道
   ├─ audio_codec_new_i2c_ctrl()   — I2C 控制 (addr=0x30)
   └─ audio_codec_new_gpio()       — GPIO 控制

4. ES8311 Codec 配置
   ├─ DAC 模式（仅输出，无麦克风）
   ├─ hw_gain.pa_voltage = 5.0V
   ├─ hw_gain.codec_dac_voltage = 3.3V
   └─ no_dac_ref = true

5. Codec Device 创建并打开
   ├─ esp_codec_dev_new()
   ├─ esp_codec_dev_open()        — 锁定采样率/位深配置
   └─ esp_codec_dev_set_out_vol(AUDIO_OUTPUT_VOLUME)

6. 启动播放任务
   ├─ xQueueCreate(8)             — 8 槽位播放队列
   └─ xTaskCreate(AudioPlayTask)  — 优先级 3, 栈 4KB
```

## 播放模型

### 队列机制

```
PlayPandaSound(type)            AudioPlayTask (后台任务)
      │                               │
      ├─ 队列非空? → 静默丢弃          │
      └─ 入队 ──────────────────→ xQueueReceive (阻塞等待)
                                       │
                                       ├─ is_playing_ = true
                                       ├─ 分块写入 codec (每块 240 samples = 10ms)
                                       ├─ 每块间隔 5ms yield
                                       ├─ is_playing_ = false
                                       └─ 回到阻塞等待
```

### 关键设计决策

1. **非阻塞触发** — `PlayPandaSound()` 仅将音效类型入队，立即返回。实际播放由独立任务完成。
2. **最多 1 个待播** — 队列中最多保留 1 个待播音效，超出则静默丢弃，防止长音频播放期间积压。
3. **is_playing_ 标志** — 跟踪当前是否正在播放，供 `IsAudioPlaying()` 查询，也用于动作联动。

### 分块写入

每个 chunk 为 **240 samples (10ms @ 24kHz)**，配合 `vTaskDelay(5ms)` 释放 CPU 给其他任务（舵机控制、WiFi 等）：

```c
while (offset < samples) {
    esp_codec_dev_write(dev_, data + offset, chunk * sizeof(int16_t));
    offset += chunk;
    vTaskDelay(pdMS_TO_TICKS(5));
}
```

## 音效调度 & 动作联动

位于 [auto_run.cc](main/auto_run.cc#L191-L299)，核心逻辑：**播完 → 等间隔 → 再播下一个**，同时**音频播放时动作增强，静音时动作缓和**。

### 音频状态机

```
      计时到 (idle)              播放结束
  ┌─────────────────┐       ┌─────────────┐
  │  静音 + 缓和动作  │ ───→ │ 播放 + 激烈动作 │
  │  head: 慢/摇/歪   │ ←─── │  head: 快/狂摇   │
  │  crawl: 0.5~0.8  │       │  crawl: 0.85~1.0│
  └─────────────────┘       └─────────────┘
         ↑                         │
         │    next_sound_tick      │
         │    = tick + 20~40s      │
         └─────────────────────────┘
```

### 代码逻辑

```c
// === 状态迁移检测 ===
bool now_playing = IsAudioPlaying();

if (now_playing && !was_playing) {
    // 音效开始 → 保存当前动作模式，切换到激烈模式
    saved_head_mode = head_mode;
    head_mode = (随机) HEAD_NOD_FAST 或 HEAD_SHAKE;
    crawl_amp = 0.85~1.0;
}
if (!now_playing && was_playing) {
    // 音效结束 → 恢复缓和模式
    head_mode = saved_head_mode;
    crawl_amp = 0.5~0.8;
}
was_playing = now_playing;

// === 静音期间：正常轮换动作模式 ===
if (!in_relax && !now_playing) {
    // 定时切换 head mode 和 crawl pattern
    // 定时触发下一个音效
}

// === 播放期间：不断推迟计时器，间隔从播放结束后开始算 ===
if (now_playing) {
    next_sound_tick = tick + (20~40s);
}
```

### 时序示意

```
音效 A (2s)           静音 25s                音效 B (1s)       静音 33s
├────────┤←─────────────────────────────→├───────┤←─────────────────────→
│ FAST   │                               │ SHAKE │
│ 0.95   │  head 慢/摇/歪, crawl 0.6     │ 0.9   │  head 慢/摇/歪, crawl 0.55
└────────┘                               └───────┘
  is_playing = true                       is_playing = true
  next_sound_tick 持续推后                 next_sound_tick 持续推后
```

### 动作联动规则

| 状态 | Head 模式 | Crawl 幅度 | 模式轮换 |
|------|----------|------------|----------|
| 音效播放中 | FAST 或 SHAKE（随机） | 0.85 ~ 1.0 | 暂停轮换 |
| 静音期间 | 慢点头 / 歪头 / 摇头（定时轮换） | 0.5 ~ 0.8 | 正常轮换 |
| Relax 模式 | RELAX (±12°, 4s) | RELAX (±15°, 6s) | 固定不变 |

## Relax 模式

每约 90 秒进入一次 20 秒的 Relax 模式：

```
进入 Relax:
  FlushAudioQueue()          → 清空队列
  PlayPandaSound(0)          → 播放自然白噪音
  in_relax = true            → 暂停随机音效触发 + 动作联动

Relax 期间:
  无随机音效触发
  Head 固定 RELAX 模式 (±12°, 4s 周期)
  Crawl 固定 RELAX 模式 (±15°, 6s 周期)

退出 Relax:
  in_relax = false           → 恢复音效触发 + 动作联动
  自然白噪音自然播放完毕（不打断）

自然白噪音播放期间的联动:
  is_playing_ = true → 暂停静音时模式轮换
  但不进入 FAST/SHAKE（Relax 期间联动被 in_relax 屏蔽）
```

## API 参考

| 函数 | 说明 |
|------|------|
| `InitAudio()` | 初始化 I2C/I2S/ES8311，启动播放任务 |
| `PlayPandaSound(int type)` | 非阻塞触发音效，队列忙时静默丢弃 |
| `IsAudioPlaying()` | 查询当前是否有音频正在播放 |
| `FlushAudioQueue()` | 清空所有待播音效（不打断当前播放） |

## 可调参数 (config.h)

| 宏 | 默认值 | 说明 |
|------|------|------|
| `AUDIO_SAMPLE_RATE` | 24000 | 采样率 Hz |
| `AUDIO_OUTPUT_VOLUME` | 80 | 音量 0-100 |
| `AUDIO_SILENT_INTERVAL_MIN_S` | 20 | 音效间最短间隔（秒） |
| `AUDIO_SILENT_INTERVAL_MAX_S` | 40 | 音效间最长间隔（秒） |

## 音效列表

| 索引 | 名称 | 用途 |
|------|------|------|
| 0 | 虫鸣鸟叫下雨流水声（大自然的声音） | Relax 模式专用 |
| 1 | 国宝熊猫叫声音效 | 随机触发 |
| 2 | 熊猫叫声 | 随机触发 |
| 3 | 熊猫吃竹子的声音 | 随机触发 |
| 4 | 熊猫宝宝嘤嘤叫声音效 | 随机触发 |
| 5 | 熊猫成年叫声 | 随机触发 |
| 6 | 熊猫撒娇声音 | 随机触发 |
| 7 | 类似熊猫叫声（猫叫） | 随机触发 |
| 8 | 类似熊猫声音 | 随机触发 |
