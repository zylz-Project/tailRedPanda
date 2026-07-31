# TailPanda 舵机运动系统详解

## 1. 硬件概述

### 1.1 舵机引脚

| 舵机 | GPIO | 功能 | 角度范围 |
|---|---|---|---|
| HEAD | IO18 | 头部左右转动 | 0°=右转, 90°=正中, 180°=左转 |
| TAIL_LR | IO15 | 尾部左右摆动 | 0°=最左, 90°=正中, 180°=最右 |
| TAIL_UD | IO16 | 尾部上下摆动 | 0°=最上, 90°=中间, 180°=最下 |
| POWER | IO4 | 舵机电源控制 | HIGH=通电, LOW=断电 |

### 1.2 PWM 参数

- 频率：**50Hz**（周期 20ms）
- 分辨率：**13 bit**（0~8191）
- 脉冲映射：`pulse_us = 500 + angle × 2000 / 180`
  - 0° → 500μs
  - 90° → 1500μs
  - 180° → 2500μs

### 1.3 上电初始化流程 (`servo.cc:InitServos()`)

```
1. 将所有舵机 GPIO 设为 OUTPUT LOW（防止浮空导致乱跳）
2. 将电源 GPIO 拉低（舵机断电）
3. 配置 LEDC PWM 定时器（50Hz, 13bit）
4. 为每个舵机配置 LEDC 通道，预设默认角度 {90, 90, 180}
5. 等待 500ms 让 PWM 稳定
6. 电源 GPIO 拉高（舵机通电）
7. 等待 500ms 让舵机到达预设位置
8. 再次 SetServoAngle 确保角度正确
9. 等待 300ms 稳定
```

---

## 2. 运动计算核心

主循环在 `auto_run_task` 中，以 **20ms（50FPS）** 为周期运行。

### 2.1 头部角度计算 (`calc_head_angle`)

```
angle(t) = center + amplitude × sin(2π × t / period) × amp_eff
```

- `center`：摆动中心角度
- `amplitude`：摆动幅度（半幅）
- `period`：完整周期（秒）
- `amp_eff`：有效幅度系数（ = head_amp × decay ）

**实际效果**：头部在 `[center - amplitude×amp_eff, center + amplitude×amp_eff]` 之间正弦摆动。

### 2.2 尾部角度计算 (`calc_tail_angles`)

尾部有两轴，各自独立计算：

```
LR(t) = lr_center + lr_amplitude × sin(2π × t / period) × amp_eff
UD(t) = ud_center + ud_amplitude × sin(2π × t / period + phase_offs × 2π) × amp_eff
```

- `phase_offs`：UD 相对 LR 的相位偏移
  - `0.0`：LR 和 UD 完全同步（同向摆动）
  - `0.25`：UD 滞后 90°（画圆）
  - `0.5`：UD 滞后 180°（反向摆动 = 画8字）

**hard swing 模式**：当 `is_hard = true` 时，正弦波经过 smoothstep 锐化：
```
harden(v) = v³ × (3 - 2 × |v|)   （保持符号，推向极值）
```
速度还会乘以 `HARD_SWING_SPEED_X`（默认 4×）。

### 2.3 Crossfade 交叉渐变

当切换动作模式时，不是瞬间跳变，而是通过 **3 秒 smoothstep** 从旧角度平滑过渡到新角度：

```
mix(t) = t² × (3 - 2t)    // smoothstep, t ∈ [0, 1]
angle = old_angle × (1 - mix) + new_angle × mix
```

- 两个角度各自独立计算（旧模式的 sine + 新模式的 sine）
- XFade 结构体保存旧模式的模式类型、幅度，确保渐变基于正确的"旧角度"

### 2.4 幅度衰减

每次切换动作后，幅度会从初始值**线性衰减**：

```
decay = 1.0 - (elapsed / 5s) × 0.25    // 5秒内从100%衰减到75%
amp_eff = amp × decay
```

这模拟了"动作刚切换时较大，随后逐渐收敛"的自然感。

---

## 3. 动作模式

### 3.1 头部模式（12 种）

| 模式 | center | amplitude | period | 效果描述 |
|---|---|---|---|---|
| HEAD_BREATHE | 90 | 12 | 7.0s | 极慢呼吸式微晃 |
| HEAD_IDLE | 90 | 12 | 4.0s | 慢速待机小晃 |
| HEAD_NOD | 90 | 20 | 3.5s | 缓慢点头 |
| HEAD_TILT | 80 | 18 | 4.0s | 偏右倾斜 |
| HEAD_SHAKE | 90 | 22 | 3.0s | 摇头 |
| HEAD_SWEEP | 90 | 30 | 5.0s | 大范围慢扫 |
| HEAD_LOOK_LEFT | 115 | 8 | 7.0s | 偏左凝视 |
| HEAD_LOOK_RIGHT | 65 | 8 | 7.0s | 偏右凝视 |
| HEAD_SLEEP | 95 | 5 | 8.0s | 微偏+极微呼吸（睡觉） |
| HEAD_PECK | 90 | 14 | 2.0s | 啄食 |
| HEAD_SNUGGLE | 90 | 12 | 3.0s | 蹭蹭 |
| HEAD_ALERT | 90 | 22 | 2.5s | 警觉抬头 |

### 3.2 尾部模式（19 种）

| 模式 | lr_c | lr_a | ud_c | ud_a | period | phase | 效果描述 |
|---|---|---|---|---|---|---|---|
| TAIL_BREATHE | 90 | 25 | 120 | 22 | 5.0s | 0.25 | 呼吸式慢摆（画椭圆） |
| TAIL_RELAX | 90 | 22 | 130 | 18 | 3.5s | 0.0 | 放松微晃 |
| TAIL_WAG | 90 | 32 | 180 | 0 | 1.2s | 0.0 | 快速左右摇尾巴（UD不动） |
| TAIL_SWING_WIDE | 90 | 32 | 120 | 25 | 3.0s | 0.0 | 大幅度左右摇摆 |
| TAIL_CIRCLE | 90 | 32 | 90 | 32 | 3.0s | 0.25 | 画圆 |
| TAIL_FIGURE8 | 90 | 28 | 90 | 32 | 3.5s | 0.25 | 画8字 |
| TAIL_RAISE_SWAY | 90 | 22 | 70 | 32 | 3.5s | 0.5 | 尾巴翘起+反向摆动 |
| TAIL_DROOP | 90 | 5 | 178 | 3 | 8.0s | 0.0 | 尾巴下垂，极微晃动 |
| TAIL_WAVE | 90 | 32 | 120 | 22 | 4.0s | 0.0 | 波浪式摆动 |
| TAIL_FLICK | 90 | 32 | 130 | 12 | 2.0s | 0.0 | 弹一下 |
| TAIL_TWITCH | 90 | 18 | 150 | 12 | 1.8s | 0.0 | 抽动 |
| TAIL_BOUNCE | 90 | 25 | 120 | 22 | 2.5s | 0.0 | 弹跳 |
| TAIL_RAISE_HOLD | 90 | 15 | 50 | 12 | 6.0s | 0.25 | 尾巴翘起保持+微晃 |
| TAIL_SLOW_RAISE | 90 | 22 | 90 | 38 | 12.0s | 0.3 | 极慢抬尾巴 |
| TAIL_RAISE_WAG | 90 | 32 | 60 | 18 | 1.8s | 0.0 | 翘尾巴+快摇 |
| TAIL_RAISE_CIRCLE | 90 | 28 | 80 | 25 | 2.0s | 0.25 | 翘尾巴+画圆 |
| TAIL_RAISE_FIGURE8 | 90 | 25 | 80 | 22 | 3.0s | 0.25 | 翘尾巴+画8字 |
| TAIL_ALERT | 90 | 6 | 30 | 5 | 5.0s | 0.0 | 警觉（尾巴直立+微晃） |
| TAIL_HAPPY | 90 | 25 | 60 | 18 | 1.2s | 0.0 | 开心快摇（呼吸变体） |

---

## 4. 模式选择池

不同状态下会从不同池中随机挑选动作：

### 4.1 舒缓模式（Relax — 自然白噪音播放时）

**头部池 `kRelaxH`**：BREATHE, IDLE, TILT, SLEEP, SNUGGLE
**尾部池 `kRelaxT`**：BREATHE, RELAX, DROOP, RAISE_HOLD, SLOW_RAISE, ALERT, WAVE

> 特点：全是慢速（≥3.5s周期）、小幅度（≤25°）的动作

### 4.2 待机模式（Idle — 无音频时）

**头部池 `kIdleH`**：BREATHE, IDLE, TILT, NOD, SNUGGLE, SLEEP, LOOK_LEFT, LOOK_RIGHT
**尾部池 `kIdleT`**：BREATHE, BREATHE2, RELAX, DROOP, WAVE, CIRCLE, FIGURE8, RAISE_SWAY, RAISE_HOLD, SLOW_RAISE, RAISE_WAG, RAISE_CIRCLE, RAISE_FIGURE8, ALERT, HAPPY

> 特点：比 Relax 多一些变化，但去掉了最快的 WAG/FLICK/TWITCH

### 4.3 暂停姿态池（Still）

**头部池 `kStillH`**：SLEEP, IDLE, LOOK_LEFT, LOOK_RIGHT
**尾部池 `kStillT`**：DROOP(×3权重), ALERT, RAISE_HOLD

---

## 5. 状态机总览

```
┌──────────────────────────────────────────────────────┐
│                    上电启动                           │
│  初始姿态: HEAD_BREATHE + TAIL_RELAX, amp≈0.55       │
└──────────────────────┬───────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────┐
│                   IDLE 状态                          │
│  · 每 10~20s 随机换一个 idle 动作                    │
│  · 每 15~35s 有 20% 概率触发自发微动作                │
│  · 每 8s 尝试触发短叫声音效                           │
│  · 持续约 120s 后进入 Relax                          │
└──────────┬───────────────────────────────────────────┘
           │                    │
           │ 短音效触发         │ 120s 定时器到期
           ▼                    ▼
┌──────────────────┐  ┌──────────────────────────────┐
│  SOUND REACTION  │  │       RELAX 状态              │
│  · sound_to_     │  │  · 播放自然白噪音 (index 0)   │
│    action() 映射 │  │  · 从 kRelaxH/T 随机选动作    │
│  · 持续 4~7s     │  │  · 每 3~6s 换一次动作         │
│  · 结束后回 Idle │  │  · 叠加 nature_wobble 微晃    │
└──────────────────┘  │  · 音频结束后回 Idle          │
                       └──────────────────────────────┘
```

### 5.1 音效→动作映射 (`sound_to_action`)

短叫声音效触发时，根据音效类型映射到特定的头尾动作组合：

| 音效类型 | 可能的动作组合 |
|---|---|
| 1-2 (熊猫叫声) | LOOK_LEFT+WAG / LOOK_RIGHT+RAISE_HOLD / TILT+FLICK / SWEEP+RAISE_WAG |
| 3 (吃竹子) | PECK+RELAX / NOD+TWITCH / SNUGGLE+BOUNCE |
| 4 (宝宝嘤嘤) | SHAKE+WAG / NOD+BOUNCE / ALERT+RAISE_HOLD |
| 5 (成年叫声) | SWEEP+SWING_WIDE / ALERT+RAISE_HOLD / LOOK_LEFT+RAISE_WAG |
| 6 (撒娇) | TILT+CIRCLE / SNUGGLE+FIGURE8 / NOD+RAISE_FIGURE8 |
| 7 (类似猫叫) | TILT+CIRCLE / SNUGGLE+RAISE_HOLD |
| 8 (类似熊猫声) | SHAKE+FIGURE8 / ALERT+BOUNCE / PECK+TWITCH |

每种音效内部还有随机分支（`r < 30/50/70` 等），增加多样性。

### 5.2 自然音频晃动 (`apply_nature_wobble`)

仅在 Relax 模式下生效，叠加多频段正弦微晃：

```
wob_lr = 16×sin(0.18Hz×t) + 5×sin(0.37Hz×t+1.0)   // 左右晃动
wob_ud =  8×cos(0.22Hz×t) + 3×sin(0.47Hz×t+0.7)   // 上下晃动

final_lr = lr×0.40 + (90 + wob_lr)×0.60
final_ud = ud×0.40 + (150 + wob_ud)×0.60
```

- 60% 权重来自 wobble（有机晃动），40% 来自原动作
- 频率特意避开整数比（0.18/0.37/0.22/0.47 Hz），避免机械重复感
- 效果：在原有动作基础上叠加"随风摇曳"的自然感

---

## 6. 关键时间参数汇总

| 参数 | 值 | 说明 |
|---|---|---|
| 主循环周期 | 20ms (50FPS) | 每帧计算一次角度 |
| Crossfade 时长 | 3.0s | 动作间平滑过渡 |
| 幅度衰减 | 5s → 75% | 线性衰减 |
| Idle 动作轮换 | 10~20s | 随机间隔 |
| 自发微动作 | 15~35s | 20% 触发概率 |
| 短音效间隔 | 20~40s | 播放后静默间隔 |
| 首次短音效 | 上电后 8s | |
| 首次 Relax | 上电后 120s | |
| Relax 动作轮换 | 3~6s | 比 Idle 更频繁 |
| Relax 重复间隔 | 120~240s | Relax 结束后再次进入的间隔 |
| 音效反应持续 | 4~7s | 音效结束后保持反应姿态 |

---

## 7. 振幅链条（最终有效幅度计算）

以头部为例，最终输出的角度由以下因素逐级相乘：

```
最终角度 = center + amplitude × head_amp × decay × crossfade_mix
           ───────   ─────────   ────────────   ────   ──────────────
           模式参数    模式幅度    状态幅度系数   衰减    渐变插值(过渡期)
```

| 层级 | 变量 | 典型范围 | 说明 |
|---|---|---|---|
| 模式参数 | `kHeadP[mode].amplitude` | 5~30 | 模式固有的摆动幅度 |
| 状态幅度 | `head_amp` | 0.55~0.85 | 由状态机设定 |
| 衰减 | `decay` | 0.75~1.00 | 5秒线性衰减 |
| 渐变 | `mix` | 0~1 | 3秒 smoothstep |

举例：Relax 模式下 HEAD_BREATHE 的**最大**有效幅度 = 12 × 0.85 × 1.0 = **10.2°**（中心90°，范围 79.8~100.2°），几乎察觉不到的微晃。
