# TailPanda 动作系统设计文档

## 硬件映射

| 舵机 | 引脚 | 含义 | 0° | 90° | 180° | 默认角 |
|------|------|------|-----|------|------|--------|
| Head 头部 | IO18 | 左右转头 | 右转 | 正中 | 左转 | 90° |
| Tail LR 尾左右 | IO15 | 尾巴左右摆 | 最左 | 正中 | 最右 | 90° |
| Tail UD 尾上下 | IO16 | 尾巴上下 | 最上 | 中间 | 最下 | 180° |

默认姿态：头正中 + 尾巴正中最下（90°, 90°, 180°）。尾巴通过两个舵机组合实现 360° 轨迹。

---

## 头部模式（12 种）

所有模式基于正弦波 `angle = center + amplitude × sin(2π × t / period)`。

| 模式 | 中心 | 幅度 | 周期 | 效果 |
|------|------|------|------|------|
| BREATHE | 90° | ±8° | 6.0s | 慢呼吸 |
| IDLE | 90° | ±15° | 4.0s | 微摆 |
| NOD | 90° | ±30° | 1.8s | 明显点头 |
| TILT | 80° | ±25° | 3.5s | 歪头偏左 |
| SHAKE | 90° | ±40° | 1.2s | 快速摇头 |
| SWEEP | 90° | ±55° | 5.0s | 宽幅环顾 |
| LOOK_LEFT | 155° | ±8° | 5.0s | 左转凝望 |
| LOOK_RIGHT | 25° | ±8° | 5.0s | 右转凝望 |
| SLEEP | 95° | ±5° | 8.0s | 瞌睡微动 |
| PECK | 90° | ±20° | 0.6s | 啄食 |
| SNUGGLE | 90° | ±15° | 2.5s | 蹭蹭 |
| ALERT | 90° | ±45° | 2.0s | 警觉 |

---

## 尾部模式（19 种）

每个模式定义了两个自由度（LR 左右 + UD 上下）的正弦参数，通过 `phase_offs` 控制两轴相位差实现不同轨迹。

### 底部姿态（尾巴在下垂位置）

| 模式 | LR 中心/幅度 | UD 中心/幅度 | 周期 | 说明 |
|------|-------------|-------------|------|------|
| BREATHE | 90°/±15° | 155°/±22° | 4.5s | 呼吸型身体膨胀 |
| RELAX | 90°/±20° | 168°/±12° | 3.0s | 底部钟摆 |
| WAG | 90°/±40° | 180°/0 | 0.4s | 快速左右摇尾 |
| SWING_WIDE | 90°/±60° | 175°/±15° | 1.5s | 大幅摇摆 |
| DROOP | 90°/±5° | 178°/±3° | 7.0s | 完全垂尾 |
| WAVE | 90°/±45° | 170°/±15° | 3.5s | 波浪 |
| FLICK | 90°/±55° | 175°/±10° | 0.7s | 快速甩尾 |
| TWITCH | 90°/±20° | 172°/±10° | 0.55s | 抽抽 |
| BOUNCE | 90°/±30° | 162°/±18° | 0.9s | 弹跳 |

### 中部姿态

| 模式 | LR 中心/幅度 | UD 中心/幅度 | 周期 | 说明 |
|------|-------------|-------------|------|------|
| CIRCLE | 90°/±40° | 140°/±40° | 1.2s | 画圈（LR/UD 相位差 90°） |
| FIGURE8 | 90°/±35° | 150°/±30° | 2.0s | 8 字轨迹 |

### 上翘姿态（尾巴翘起）

| 模式 | LR 中心/幅度 | UD 中心/幅度 | 周期 | 说明 |
|------|-------------|-------------|------|------|
| RAISE_SWAY | 90°/±25° | 80°/±20° | 3.0s | 翘起轻摆 |
| RAISE_HOLD | 90°/±8° | 40°/±5° | 5.0s | 翘高微颤 |
| SLOW_RAISE | 90°/±20° | 100°/±40° | 10.0s | 缓慢升降（如深呼吸） |
| RAISE_WAG | 90°/±35° | 50°/±10° | 0.5s | 翘起快速摇 |
| RAISE_CIRCLE | 90°/±30° | 60°/±15° | 1.5s | 翘起画圈 |
| RAISE_FIGURE8 | 90°/±25° | 55°/±12° | 2.5s | 翘起走 8 字 |
| ALERT | 90°/±5° | 20°/±3° | 4.0s | 高高翘起，几不动（警觉） |
| HAPPY | 90°/±25° | 45°/±15° | 1.0s | 翘起欢快弹跳 |

---

## 状态机

```
                  ┌──────────┐
                  │  开机     │
                  └────┬─────┘
                       │
                  ┌────▼─────┐
          ┌───────│  BREATHE  │◄──────────────┐
          │       └────┬─────┘                │
          │            │                      │
          │     ┌──────▼──────┐               │
          │     │    IDLE     │ 每10~20s换一次  │
          │     │ (6头×7尾)   │               │
          │     └──┬───┬───┬─┘               │
          │        │   │   │                  │
          │   ┌────▼┐  │   └─────────┐        │
          │   │触发  │  │ 15~35s间隔  │        │
          │   │叫声音效│  │ 自发微动作  │        │
          │   └──┬───┘  └─────┬─────┘        │
          │      │            │               │
          │  ┌───▼────┐  ┌───▼────┐          │
          │  │音效联动 │  │FLICK/  │          │
          │  │3~4种随机│  │TWITCH  │          │
          │  │动作映射 │  │0.8s    │          │
          │  └───┬────┘  └───┬────┘          │
          │      │           │               │
          │  ┌───▼───────────▼───┐           │
          │  │ 保持反应 2~5s     │           │
          │  │ 幅度3s衰减至60%   │           │
          │  └────────┬─────────┘           │
          │           │                      │
          │           └──────────────────────┘
          │
          │  每约2小时
     ┌────▼──────────────┐
     │    RELAX 舒缓模式   │
     │  播放自然白噪音      │
     │  每4~8s换动作       │
     │  25%概率1.5s甩尾    │
     │  多频有机晃动叠加    │
     │  音频播完自动退出    │
     └───────────────────┘
```

### 状态详细说明

**IDLE 态**（无声播放时的默认状态）：
- 每 10~20 秒从 idle 池随机选一组头+尾模式切换，避免一成不变
- 每 15~35 秒有 20% 概率触发 0.8s 自发甩尾/抽抽（无声音触发）
- 每 20~40 秒随机触发一个短叫声音效

**REACTION 态**（短叫声音效播放中）：
- 音效开始时根据声音类型随机选择 3~4 种可能动作之一
- 动作持续到音效结束后 2~5 秒
- 幅度从初始值经过 3 秒线性衰减到 60%（模拟动作自然消退）

**RELAX 态**（自然白噪音播放中）：
- 每约 2 小时自动触发（随机 ±2 分钟）
- 循环播放"虫鸣鸟叫下雨流水声"
- 每 4~8 秒换一组头尾动作（以舒缓翘尾姿态为主）
- 25% 概率在换动作时插入 1.5 秒突发甩尾
- 叠加多频有机晃动：0.13Hz 慢漂 + 0.3Hz 中摆
- 音频播多久就持续多久（无固定时限）

---

## 音效→动作映射

系统有 9 个音频文件（TOC 索引 0~8），其中索引 0 为自然白噪音（用于 RELAX），索引 1~8 为短叫声音效。

每个短叫声音效触发时，`SoundToReaction()` 按概率分布选一组头+尾+幅度+硬摆：

| 音效 | 概率 | 头部 | 尾部 | 幅度 | 硬摆 |
|------|------|------|------|------|------|
| **吃竹子** (40%) | | PECK 啄食 | RELAX 放松 | h95% t60% | |
| (30%) | | NOD 点头 | TWITCH 抽抽 | h80% t50% | |
| (30%) | | SNUGGLE 蹭蹭 | BOUNCE 弹弹 | h75% t70% | |
| **宝宝嘤嘤** (50%) | | SHAKE 摇头 | WAG 摇尾 | h90% t100% | 20%硬 |
| (30%) | | NOD 点头 | BOUNCE 弹弹 | h85% t90% | |
| (20%) | | ALERT 警觉 | RAISE_HOLD 翘停 | h80% t85% | 硬摆 |
| **成年叫声** (35%) | | SWEEP 环顾 | SWING_WIDE 大摇 | h90% t85% | 15%硬 |
| (30%) | | ALERT 警觉 | RAISE_HOLD 翘停 | h95% t90% | 硬摆 |
| (35%) | | LOOK_LEFT 左转 | RAISE_WAG 翘摇 | h85% t80% | |
| **撒娇** (35%) | | TILT 歪头 | CIRCLE 画圈 | h80% t75% | |
| (25%) | | SNUGGLE 蹭蹭 | FIGURE8 8字 | h75% t70% | |
| (40%) | | NOD 点头 | RAISE_FIGURE8 翘8字 | h70% t65% | |
| **熊猫叫** (30%) | | LOOK_LEFT 左转 | WAG 摇尾 | h80% t85% | |
| (25%) | | LOOK_RIGHT 右转 | RAISE_HOLD 翘停 | h80% t80% | |
| (20%) | | TILT 歪头 | FLICK 甩尾 | h75% t90% | 硬摆 |
| (25%) | | SWEEP 环顾 | RAISE_WAG 翘摇 | h85% t80% | |
| **猫叫声** (50%) | | TILT 歪头 | CIRCLE 画圈 | h85% t90% | |
| (50%) | | SNUGGLE 蹭蹭 | RAISE_HOLD 翘停 | h75% t80% | |
| **其他叫** (40%) | | SHAKE 摇头 | FIGURE8 8字 | h90% t85% | 15%硬 |
| (30%) | | ALERT 警觉 | BOUNCE 弹弹 | h85% t80% | 硬摆 |
| (30%) | | PECK 啄食 | TWITCH 抽抽 | h70% t65% | |

---

## 动态参数

### 幅度衰减
每次切换动作时，幅度从初始值开始，经过 3 秒线性衰减至 60%。模拟动物动作的自然消退——动作开始时最有力，逐渐回归到柔和的基础态。公式：

```
amp_decay = 1.0 - (t / 3.0) × 0.4    // t=0→1.0, t=3s→0.6
```

### 动作切换平滑过渡（Crossfade）
切换动作时使用 **smoothstep 缓动** 在 1 秒内从旧角度平滑过渡到新角度：

```
mix(t) = t² × (3 - 2t)     // smoothstep, t ∈ [0, 1]
angle   = old × (1-mix) + new × mix
```

避免直接跳变造成的机械感。

### Hard Swing（硬摆）
通过网页 `/api/autoplay` 或特定音效触发。启用时：
- **全局硬摆**：尾运动周期除以 `HARD_SWING_SPEED_X`（默认 4x），速度加快
- **动作级硬摆**：对正弦值施加 smoothstep 锐化 `s = |v|; s = s²(3-2s)`，使极值点停留更久、过渡更陡

---

## 自然音频特殊处理

当播放"虫鸣鸟叫下雨流水声"时，在基础尾动作之上叠加**多频有机晃动**：

**LR 左右通道：**
```
w1 = sin(t × 0.25Hz) × 10°    // 0.125Hz 慢漂
w2 = sin(t × 0.55Hz) × 8°     // 0.275Hz 中摆
wob_lr = w1 + w2 × 0.5
```

**UD 上下通道：**
```
w1u = cos(t × 0.30Hz) × 7°
w2u = sin(t × 0.65Hz) × 6°
wob_ud = w1u + w2u × 0.5
```

**混合公式**（40% wobble + 60% 基础动作）：
```
tail_lr = tail_lr × 0.60 + (90 + wob_lr) × 0.40
tail_ud = tail_ud × 0.60 + (150 + wob_ud) × 0.40
```

效果：尾巴在做基础舒缓动作的同时，叠加了一个不规则有机晃动，模拟真实动物身体在环境音中的自然微调，助眠风格。

---

## 舵机上电时序

`InitServos()` 的时序经过优化，防止上电瞬间舵机跳顶：

1. 配置 LEDC PWM（目标角度对应的占空比）
2. 等待 200ms（PWM 稳定）
3. 给舵机供电（IO4→HIGH）
4. 等待 500ms（舵机物理转到目标位置）
5. 显式调用 `SetServoAngle()` 锁定角度

这样 `InitServos()` 返回时尾巴已在默认底部位置（180°），不再等待 auto_run 第一帧。

---

## Web 控制 API

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/` | 控制面板网页 |
| POST | `/api/servo` | 手动设舵机 `{"angles":[head, lr, ud]}` |
| GET | `/api/battery` | 电池 `{"voltage_mv", "level"}` |
| GET/POST | `/api/autoplay` | 开关自运行动画 / 切换硬摆 |

---

## 动作计算流水线（每帧 20ms）

### 1. 基础正弦公式

头部和尾巴的角度计算核心都是：

```
angle = center + amplitude × sin(phase × 2π) × amp × decay
```

**相位计算**：
```
t     = tick × 20ms / 1000          // 累计秒数
phase = fmodf(t / period, 1.0)       // [0,1) 循环相位
```

以 BREATHE (head: center=90, amplitude=8, period=6s) 为例：
```
t=0s   → phase=0,    sin=0  → 90°
t=1.5s → phase=0.25, sin=1  → 98° (最右)
t=3s   → phase=0.5,  sin=0  → 90°
t=4.5s → phase=0.75, sin=-1 → 82° (最左)
```

### 2. 尾巴双轴同步

尾巴 LR 和 UD 各自独立跑正弦，但共享同一个计时器。UD 通过 `phase_offs` 偏移相位实现不同轨迹：

```
phase_lr = fmodf(t / period, 1.0)
phase_ud = fmodf(t / period + phase_offs, 1.0)

tail_lr = lr_center + lr_amplitude × sin(phase_lr × 2π) × amp × decay
tail_ud = ud_center + ud_amplitude × sin(phase_ud × 2π) × amp × decay
```

**相位差效果**：
| phase_offs | 轨迹 | 例子 |
|-----------|------|------|
| 0.0 | 同步摆动（对角线） | RELAX、WAG |
| 0.25 | 圆形（正交 90°） | CIRCLE、BREATHE |
| 0.5 | 反向摆动 | RAISE_SWAY |

### 3. 动作切换平滑过渡（Crossfade）

`applyAction()` 调用时记录旧/新参数到 `blend` 结构体。后续每帧在 1 秒内用 **smoothstep** 缓动混合：

```
bt = (tick - blend.start_tick) × 20ms / 1000    // [0, 1]
mx = bt² × (3 - 2×bt)                            // smoothstep

old = old_center + old_amplitude × sin(old_phase × 2π) × old_amp
new = new_center + new_amplitude × sin(new_phase × 2π) × new_amp × decay

angle = old + (new - old) × mx
```

smoothstep 的特性：起停缓、中间快，过渡自然不突兀。超过 1 秒后 `blend.active = false`，直接走新参数。

### 4. Hard Swing 波形锐化

`sharpen_sin()` 对正弦值施加 smoothstep，推向 ±1 极值：

```cpp
float sharpen_sin(float v) {
    float s = fabsf(v);
    s = s * s * (3.0f - 2.0f * s);   // smoothstep on |v|
    return (v > 0 ? s : -s);           // restore sign
}
```

**效果**：正弦波的中间过渡段被压窄，极值处停留更久。视觉上就是"突然甩过去 → 停住 → 突然甩回来"。

触发条件：
- Web API 设置全局硬摆 → 所有尾巴动作周期缩短 4x
- 特定音效（成年叫声、警觉）动作级硬摆 → 仅该动作波形锐化

### 5. 自然音频多频有机晃动

在自然白噪音播放时，基础尾巴动作之上额外叠加一个双层 wobbler：

**LR 左右通道**：
```
w1  = sin(t × 0.25) × 10°       // 0.125Hz 极慢漂移
w2  = sin(t × 0.55 + 1.0) × 8°  // 0.275Hz 中速摆动
wob = w1 + w2 × 0.5              // 加权混合
```

**UD 上下通道**（频率略不同，避免和 LR 同频共振）：
```
w1u = cos(t × 0.30) × 7°
w2u = sin(t × 0.65 + 0.7) × 6°
wob = w1u + w2u × 0.5
```

**混合**（40% wobble + 60% 保持基础动作）：
```
tail_lr = tail_lr × 0.60 + (90 + wob_lr) × 0.40
tail_ud = tail_ud × 0.60 + (150 + wob_ud) × 0.40
```

### 6. 幅度衰减

每次 `applyAction()` 重置 `amp_start_tick`。后续每帧：

```
dt = tick - amp_start_tick
if (dt < 3s / 20ms)            // 前 3 秒 (150 ticks)
    decay = 1.0 - (dt/150) × 0.4   // 线性 1.0 → 0.6
else
    decay = 0.6                     // 保持 60%
```

最终传给舵机的有效幅度：
```
head_eff = head_amp × decay     // 头部有效幅度
tail_eff = tail_amp × decay     // 尾部有效幅度
```

### 7. 完整流水线示意图

```
tick × 20ms → 累计时间 t
    │
    ├─ 头部 ──────────────────────────────────────
    │   phase = t / head_period
    │   sv = sin(phase × 2π)
    │   angle = center + amplitude × sv × head_amp × decay
    │   if crossfade: smoothstep(old_head, new_head)
    │   → SetServoAngle(HEAD, angle)
    │
    └─ 尾巴 ──────────────────────────────────────
        phase_lr = t / tail_period
        phase_ud = t / tail_period + phase_offs
        slr = sin(phase_lr × 2π)
        sud = sin(phase_ud × 2π)
        if 硬摆: slr=sharpen_sin(slr), sud=sharpen_sin(sud)
        lr = lr_c + lr_a × slr × tail_amp × decay
        ud = ud_c + ud_a × sud × tail_amp × decay
        if crossfade: smoothstep(old_tail, new_tail)
        if 自然音频: lr=0.60×lr + 0.40×(90+wob_lr)
                     ud=0.60×ud + 0.40×(150+wob_ud)
        → SetServoAngle(TAIL_LR, lr)
        → SetServoAngle(TAIL_UD, ud)
```

### 8. 关键函数一览

| 函数 | 位置 | 作用 |
|------|------|------|
| `applyAction(h, t, ha, ta, hd)` | auto_run.cc | 切换动作：记录旧参数到 blend，设置新参数，重置衰减计时 |
| `SoundToReaction(st, h, t, ha, ta, hd)` | auto_run.cc | 音效→动作映射：按概率随机选头尾组合 |
| `sharpen_sin(v)` | auto_run.cc | 硬摆锐化：smoothstep 推极值 |
| `enterRelax()` | auto_run.cc | 进入舒缓模式：播放自然白噪音，5s 后首次变化 |
| `exitRelax()` | auto_run.cc | 退出舒缓模式：标记 in_relax=false |
| `pickReaction()` | auto_run.cc | 音效触发时选动作，设定 hold 时长 |
| `pickRelaxVariation()` | auto_run.cc | 舒缓模式换动作，每 4~8s 触发，25% 概率 1.5s 甩尾 |
| `goIdle()` | auto_run.cc | 回到 idle 态，随机选一组头+尾 |
| `SetServoAngle(idx, angle)` | servo.cc | 将角度转为 PWM 占空比写入 LEDC |
| `AngleToDuty(angle)` | servo.cc | 角度→脉宽→占空比：`500µs + angle/180×2000µs` |
