# Opus 音频 + SPI Flash 存储系统

## 概览

音频系统从 **嵌入式 PCM** 升级为 **外部 SPI Flash 存储 Opus 压缩音频**。Opus 文件在播放时实时解码，无需事先解压为 PCM。

| 对比项 | 旧方案 (PCM) | 新方案 (Opus + SPI Flash) |
|--------|-------------|--------------------------|
| 存储介质 | 固件 Flash (内嵌) | 外部 W25Q512 SPI NOR Flash (64MB) |
| 音频格式 | PCM 16-bit 24kHz 单声道 | Opus 48kHz 单声道 (OGG 容器) |
| 音频大小 | ~10MB | ~1.1MB |
| 解码方式 | 无需解码，直接写 I2S | 实时 OGG 解封装 → Opus 解码 → PCM |
| 固件体积 | 包含音频数据 (~12MB) | 仅代码 (~2MB) |
| 更新音频 | 重新编译固件 | WiFi 上传 / 串口烧录 |

## 系统架构

```
┌──────────────────────────────────────────────────────────────────┐
│                         ESP32-S3                                  │
│                                                                   │
│  ┌──────────────┐    ┌──────────────┐    ┌───────────────────┐   │
│  │ auto_run.cc  │───→│  audio.cc    │───→│ ES8311 Codec      │   │
│  │ 自动运行调度  │    │  播放引擎     │    │ I2S → 扬声器       │   │
│  └──────────────┘    └──────┬───────┘    └───────────────────┘   │
│                             │                                     │
│              ┌──────────────┼──────────────┐                     │
│              │              │              │                      │
│              ▼              ▼              ▼                      │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐             │
│  │ flash_audio  │ │ ogg_demuxer  │ │ esp_opus_dec │             │
│  │ Flash 读写    │ │ OGG 解封装   │ │ Opus 解码器  │             │
│  └──────┬───────┘ └──────────────┘ └──────────────┘             │
│         │                                                         │
│  ┌──────▼───────┐                                                │
│  │ w25q512      │  SPI Flash 驱动                                 │
│  │ IO9=CLK      │                                                │
│  │ IO47=MOSI    │                                                │
│  │ IO21=MISO    │                                                │
│  │ IO10=CS      │                                                │
│  └──────────────┘                                                │
└──────────────────────────────────────────────────────────────────┘
```

### 音频播放流水线

```
PlayPandaSound(type)
       │
       ▼
  文件名查表 (panda_samples.cc)
       │
       ▼
  SPI Flash 读取 (4KB/块)
       │
       ▼
  OGG 解封装 (ogg_demuxer) ──→ 提取 Opus 数据包
       │
       ▼
  Opus 解码 (esp_opus_dec) ──→ PCM 16-bit 48kHz
       │
       ▼
  I2S 写入 (240 samples/块, 5ms)
       │
       ▼
  ES8311 Codec → 扬声器
```

## 硬件引脚

### SPI Flash (W25Q512JVEIQ)

| 引脚 | 信号 | 说明 |
|------|------|------|
| **IO9** | CLK | SPI 时钟 (40MHz) |
| **IO47** | MOSI | 主机输出 / Flash 输入 (DI) |
| **IO21** | MISO | Flash 输出 / 主机输入 (DO) |
| **IO10** | CS | 片选 (手动控制) |

### 音频 (ES8311)

| 引脚 | 信号 | 说明 |
|------|------|------|
| **IO2** | I2C SDA | ES8311 控制数据 |
| **IO38** | I2C SCL | ES8311 控制时钟 |
| **IO13** | I2S WS | 字时钟 |
| **IO48** | I2S BCLK | 位时钟 |
| **IO46** | I2S DOUT | PCM 数据输出 |

## 文件结构

```
main/
├── audio.cc / audio.h               # 音频播放引擎 (Opus 解码流水线)
├── flash_audio.cc / flash_audio.h   # SPI Flash 文件管理 (TOC + 读写)
├── w25q512.cc / w25q512.h           # W25Q512 SPI Flash 底层驱动
├── ogg_demuxer.cc / ogg_demuxer.h   # OGG 容器解析器
├── flash_upload_server.cc/h         # WiFi HTTP 上传 API
├── panda_samples.cc / panda_samples.h  # 音效名称/文件名映射
├── config.h                         # 所有引脚和参数定义
├── auto_run.cc / auto_run.h         # 自动运行调度 (使用 panda_sound_type_t)
├── http_server.cc / http_server.h   # HTTP 服务器 (注册 flash 端点)
├── servo.cc / servo.h               # 舵机 PWM 控制
├── power.cc / power.h               # 电源管理
├── wifi.cc / wifi.h                 # WiFi AP
├── main.cc                          # 入口 (初始化顺序)
├── idf_component.yml                # 组件依赖
└── CMakeLists.txt                   # 构建配置

opus_audio/                          # 音频源文件 (待烧录到 SPI Flash)
├── 冯梦舟 - 虫鸣 鸟叫 下雨 流水声 (大自然的声音).opus
├── 国宝熊猫叫声音效_爱给网_aigei_com.opus
├── 熊猫叫声_爱给网_aigei_com.opus
├── 熊猫吃竹子的声音.opus
├── 熊猫宝宝嘤嘤叫声音效_爱给网_aigei_com.opus
├── 熊猫成年.opus
├── 熊猫撒娇声音.opus
├── 类似熊猫叫声- Cat Meows from a Close Perspective.opus
└── 类似熊猫声音.opus

scripts/
└── flash_opus.py                    # Python 工具: WiFi 上传 Opus 文件到 Flash
```

## SPI Flash 布局

```
┌────────────────────────────────────────────────────────────────┐
│ Sector 0 (0x000000, 4KB): TOC 目录表                            │
├────────────────────────────────────────────────────────────────┤
│ Offset │ Size  │ Field                                          │
│ 0x0000 │ 4     │ Magic: "PNDA" (0x41444E50)                    │
│ 0x0004 │ 4     │ Version: uint32_t (1)                          │
│ 0x0008 │ 4     │ File count: uint32_t (N)                       │
│ 0x000C │ N×80  │ File entries:                                  │
│        │       │   [0..63]   文件名 (UTF-8, null-padded, 64B)   │
│        │       │   [64..67]  数据区偏移量 (uint32_t)             │
│        │       │   [68..71]  文件大小 (uint32_t)                 │
│        │       │   [72..75]  采样率 (uint32_t)                   │
│        │       │   [76..79]  保留                               │
├────────────────────────────────────────────────────────────────┤
│ Sector 1+ (0x001000+): Opus 文件数据                            │
│                                                                 │
│ File 0: 0x001000 ~ 0x001000+size_0                              │
│ File 1: (4KB 对齐) ~ +size_1                                    │
│ File 2: (4KB 对齐) ~ +size_2                                    │
│ ...                                                             │
│ Total capacity: 64MB                                            │
└────────────────────────────────────────────────────────────────┘
```

## 音效文件列表

| 索引 | 文件名 | 时长 | 大小 |
|------|--------|------|------|
| 0 | 冯梦舟 - 虫鸣 鸟叫 下雨 流水声 (大自然的声音).opus | ~158s | 866KB |
| 1 | 国宝熊猫叫声音效_爱给网_aigei_com.opus | ~4.8s | 27KB |
| 2 | 熊猫叫声_爱给网_aigei_com.opus | ~1.9s | 13KB |
| 3 | 熊猫吃竹子的声音.opus | ~5.7s | 32KB |
| 4 | 熊猫宝宝嘤嘤叫声音效_爱给网_aigei_com.opus | ~26.5s | 148KB |
| 5 | 熊猫成年.opus | ~1.1s | 8KB |
| 6 | 熊猫撒娇声音.opus | ~1.5s | 8KB |
| 7 | 类似熊猫叫声- Cat Meows from a Close Perspective.opus | ~5.2s | 30KB |
| 8 | 类似熊猫声音.opus | ~3.7s | 20KB |

总大小约 **1.1MB**，仅占 Flash 容量的 **1.7%**。

## 初始化流程

`app_main()` 中的初始化顺序：

```
1. nvs_flash_init()        NVS 存储初始化
2. InitPower()             电源管理 (IO7 锁存, IO6 ADC)
3. InitAudio()             ES8311 + I2S (48kHz)
                             └─ 创建 AudioPlayTask (优先级3, 栈8KB)
4. flash_audio_init()      W25Q512 初始化 + 读取 TOC
                             ├─ 未检测到 Flash → 打印警告, 静默降级
                             └─ 无有效 TOC → 空文件列表 (等待上传)
5. InitServos()            3 路舵机归中
6. InitAutoRun()           自动运行动画任务
7. InitWiFi()              WiFi AP 模式
8. StartHttpServer()       HTTP 服务器 + Flash 管理 API
```

## API 参考

### 音频播放 (audio.h)

| 函数 | 说明 |
|------|------|
| `InitAudio()` | 初始化 ES8311 + I2S (48kHz)，启动播放任务 |
| `PlayPandaSound(int type)` | 非阻塞触发音效，从 SPI Flash 读取并解码 Opus |
| `IsAudioPlaying()` | 查询是否正在播放 |
| `FlushAudioQueue()` | 清空待播放队列 |

### Flash 存储 (flash_audio.h)

| 函数 | 说明 |
|------|------|
| `flash_audio_init()` | 初始化 W25Q512，读取或创建 TOC |
| `flash_audio_get_file_count()` | 获取已存储文件数量 |
| `flash_audio_get_file_info(index, &info)` | 获取文件元数据 |
| `flash_audio_find_file(filename)` | 按文件名查找索引 |
| `flash_audio_read_file(index, offset, buf, len)` | 读取文件数据块 |
| `flash_audio_write_file(filename, data, len, rate)` | 写入文件并更新 TOC |
| `flash_audio_erase_all()` | 擦除所有音频数据 |

### HTTP API (新增)

| 端点 | 方法 | 说明 |
|------|------|------|
| `/flash` | GET | Flash 管理 Web 界面 |
| `/api/flash/status` | GET | JSON 文件列表 |
| `/api/flash/upload` | POST | 上传 .opus 文件 (multipart/form-data) |
| `/api/flash/erase` | POST | 擦除所有文件 |

## 将 Opus 文件烧录到 Flash

音频文件已随项目存放于 `opus_audio/` 目录（共 9 个 .opus 文件，约 1.2MB）。

### 方式 1: WiFi 上传 (推荐)

1. 给 ESP32 上电，等待 WiFi AP 启动
2. 电脑连接 WiFi `paxing_hx`
3. 运行脚本（默认读取 `opus_audio/`）：
   ```bash
   python3 scripts/flash_opus.py
   ```
   或指定目录：
   ```bash
   python3 scripts/flash_opus.py opus_audio/
   ```
4. 或打开浏览器访问 `http://192.168.4.1/flash`，通过 Web 界面上传

### 方式 2: 首次烧录 (文件嵌入固件)

如果需要在首次烧录固件时同时写入音频文件：

```bash
# 1. 准备 TOC + 文件数据的 raw binary
python3 scripts/flash_opus.py --erase /path/to/opus_compressed/
# 2. 正常烧录固件
idf.py flash
```

## 可调参数 (config.h)

### 音频

| 宏 | 默认值 | 说明 |
|------|--------|------|
| `AUDIO_SAMPLE_RATE` | 48000 | I2S 输出采样率 Hz (匹配 Opus 48kHz) |
| `AUDIO_OUTPUT_VOLUME` | 80 | 音量 0-100% |

### SPI Flash

| 宏 | 默认值 | 说明 |
|------|--------|------|
| `SPI_FLASH_CS_PIN` | IO10 | 片选 |
| `SPI_FLASH_CLK_PIN` | IO9 | 时钟 |
| `SPI_FLASH_MOSI_PIN` | IO47 | 数据输出 |
| `SPI_FLASH_MISO_PIN` | IO21 | 数据输入 |

### 播放参数 (audio.cc 内部常量)

| 常量 | 值 | 说明 |
|------|------|------|
| `OPUS_FRAME_DURATION` | 60ms | Opus 解码帧时长 |
| `FLASH_READ_CHUNK` | 4096 | 每次从 Flash 读取的块大小 |
| `I2S_CHUNK_SAMPLES` | 240 | 每次写入 I2S 的采样数 (5ms @ 48kHz) |

## 组件依赖

```yaml
# idf_component.yml
dependencies:
  espressif/esp_codec_dev: ~1.5.4     # ES8311 编解码器驱动
  espressif/esp_audio_codec: ~2.4.1   # Opus 解码器 (libopus)
```

## FreeRTOS 任务一览

| 任务名 | 优先级 | 栈 | 职责 |
|--------|--------|------|------|
| `panda_play` | 3 | 8KB | 音频播放：Flash 读取 → OGG 解封装 → Opus 解码 → I2S 输出 |
| `auto_run` | 2 | 4KB | 自动运行：20ms tick，状态机 + 舵机更新 |
| `power_mon` | 1 | 2KB | 电源按键监控 + 长按关机 |
| HTTP server | 1 | - | Web 请求 + Flash 上传 |

## 调试

### 检查 SPI Flash 是否检测到

串口日志应显示：
```
I (1234) w25q512: JEDEC ID: MF=0xEF  Type=0x60  Cap=0x20 → 512 Mbit (64 MB)
I (1235) w25q512: ✅ W25Q512 初始化成功
I (1236) flash_audio: Flash audio initialized: 9 files, 64 MB flash
```

### 检查文件列表

访问 `http://192.168.4.1/api/flash/status`：
```json
{
  "count": 9,
  "files": [
    {"name": "熊猫成年.opus", "size": 8236, "sample_rate": 48000},
    ...
  ],
  "total_size": 1152000
}
```

### 常见问题

**Flash 未检测到：**
- 检查 IO9/IO47/IO21/IO10 接线
- 确认 Flash 供电 (2.7~3.6V)
- 检查 CS 上拉电阻

**播放无声音：**
- 确认 ES8311 在 I2C 总线上 (地址 0x18)
- 确认 Opus 文件已上传到 Flash (检查 `/api/flash/status`)
- 检查音量设置 (`AUDIO_OUTPUT_VOLUME`)

**音质问题：**
- Opus 解码为 48kHz，确保 I2S 输出配置为 48kHz
- ES8311 的 `no_dac_ref = true` (当前配置)
