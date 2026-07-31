# 崩溃分析与修复记录

## 问题现象

ESP32-S3 播放音频时反复崩溃：

```
I (5606) panda_audio: Playing: 熊猫撒娇声音.opus (73 audio pkts, sr=24000)
Guru Meditation Error: Core 0 panic'ed (LoadProhibited). Exception was unhandled.

Core 0 register dump:
PC      : 0x4037f672  PS      : 0x00060033  A0      : 0x803800e9  A1      : 0x3fcbe2e0
A2      : 0xa5a5a5a5  A3      : 0xa5a5a5a5
```

## 根因分析

### 崩溃 1：栈溢出 → TCB 损坏

**直接原因**：`AudioPlayTask` 栈空间不足。

`OggDemuxer` 在栈上创建，其内部 `context_t` 包含一个 `uint8_t packet_buf[8192]`（8KB），加上其他缓冲区共约 **8.5KB**。任务栈仅 **16KB**。

解码和 I2S 驱动调用链进一步消耗栈空间，剩余 ~7KB 不够用，发生栈溢出。溢出的数据破坏了相邻内存中的 FreeRTOS TCB（任务控制块）。

Core 0 调度器读取被破坏的 TCB 时，遇到 `0xa5a5a5a5`（ESP-IDF 堆内存释放后的填充模式），解引用触发 `LoadProhibited`。

**修复**：
1. `OggDemuxer` 从栈分配改为堆分配（`std::make_unique`），释放 ~8.5KB 栈空间
2. 任务栈从 16KB 扩大到 32KB
3. 重构为流式解码（见下文）

### 崩溃 2：200KB 文件大小限制

**直接原因**：`AudioPlayTask` 中硬编码 `if (info.size > 200000) { skip; }`。

自然白噪音文件（2分40秒）为 866KB，被静默跳过。

**修复**：重构为流式解码，移除文件大小限制。

### Bug 3：自然白噪音文件名不匹配

**现象**：
```
W panda_audio: Not found: 冯梦舟 - 虫鸣 鸟叫 下雨 流水声 (大自然的声音)国宝熊猫叫声...
```

**原因**：
1. TOC 文件名 63 字节（超过 64 字节字段限制，`.opus` 后缀被截断）
2. 编译器将 `kSoundFilenames` 数组中相邻的字符串字面量合并，丢失 null 终止符

**修复**：
1. `panda_samples.cc` 中去掉 `.opus` 后缀以匹配 TOC 实际存储的文件名
2. 将内联字符串字面量改为独立 `const char[]` 变量，强制编译器正确 null 终止

### Bug 4：`nature_playing` 标志位卡死

**现象**：自然白噪音找不到后，放松模式日志显示 `nature already playing`，但实际并未播放。

**原因**：文件找不到时 `is_playing_` 从未设为 `true`，声音结束清理代码永不触发，`nature_playing` 永远卡在 `true`。下次进入放松模式直接跳过播放。

**修复**：`enterRelax` 条件从 `!nature_playing` 改为 `!nature_playing || !IsAudioPlaying()`。

## 架构：流式音频解码

### 旧方案（全量加载）

```
malloc(整个文件) → OGG 解封装 → 存储所有 Opus 包 → 逐个解码 → I2S 输出
内存: 866KB(文件) + ~800KB(包数据) ≈ 1.6MB
```

### 新方案（流式解码）

```
while 文件未读完:
  读 4KB chunk → OGG 解封装 → 回调中解码一个包 → 立即 I2S 输出
  峰值内存: 4KB(chunk) + 5.7KB(PCM buffer) + 8.5KB(OGG context, 堆) ≈ 18KB
```

### 数据流

```
SPI Flash (W25Q512)
    │  w25q512_read(addr, chunk, 4096)
    ▼
OggDemuxer::Process(chunk)
    │  状态机: FIND_PAGE → PARSE_HEADER → PARSE_SEGMENTS → PARSE_DATA
    │  过滤 OpusHead / OpusTags，仅回调音频数据包
    ▼
OnDemuxerFinished 回调
    │  lazy-init: 首次调用时打开 Opus 解码器
    │  esp_opus_dec_decode() → PCM (int16, 48kHz mono, 60ms/帧)
    ▼
esp_codec_dev_write() → I2S → ES8311 DAC → 喇叭
```

## 外接 Flash 存储布局

```
W25Q512 (64MB)
┌──────────────────────────────────────────────┐
│ Sector 0 (0x000000, 4KB): TOC (文件目录)      │
│  [0..3]   Magic: "PNDA" (0x41444E50)          │
│  [4..7]   Version: 1                           │
│  [8..11]  File count: 9                        │
│  [12..91] Entry 0: name(64B) + off + size + sr │
│  [92..171] Entry 1: ...                        │
│  ...                                           │
├──────────────────────────────────────────────┤
│ Sector 1+ (0x001000+): Opus 文件数据           │
│  Entry 0: 866,281 bytes (自然白噪音)           │
│  Entry 1:  27,277 bytes (国宝熊猫)             │
│  ...                                           │
└──────────────────────────────────────────────┘
```

TOC 条目结构（80 字节）：
```
[0..63]   filename (UTF-8, null-padded, 64 bytes)
[64..67]  offset   (uint32, 从 FLASH_AUDIO_DATA_START 起)
[68..71]  size     (uint32, bytes)
[72..75]  sample_rate (uint32, e.g. 48000)
[76..79]  reserved
```

### 查找流程

```
sound_type → panda_sound_filename() → "xxx.opus"
    → flash_audio_find_file() → 线性搜索 TOC, strcmp 匹配
    → flash_audio_get_file_info() → offset, size
    → flash_audio_read_file(idx, offset, buf, len) → w25q512_read(addr, buf, len)
```

## 修改文件清单

| 文件 | 修改内容 |
|------|----------|
| `main/audio.cc` | 流式解码重构；栈 16K→32K；OggDemuxer 堆分配；TOC dump 诊断日志 |
| `main/panda_samples.cc` | 文件名去 .opus 匹配 TOC；独立 const char[] 变量防止编译器优化 |
| `main/auto_run.cc` | enterRelax 条件增加 IsAudioPlaying() 检查 |
| `main/flash_upload_server.cc` | /play 响应添加 Content-Length 头（浏览器可确定 OGG 时长和 seek） |

## 构建与烧录

```bash
cd action_breathe_xu
idf.py build flash monitor
```

若串口被占用：
```bash
lsof /dev/ttyACM0          # 查找占用进程
kill <PID>                 # 关闭占用
idf.py flash monitor       # 重新烧录
```
