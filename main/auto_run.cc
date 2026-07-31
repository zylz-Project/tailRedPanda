#include "auto_run.h"
#include "audio.h"
#include "config.h"
#include "flash_audio.h"
#include "panda_samples.h"
#include "servo.h"

#if ENABLE_AUTO_RUN

#include <esp_log.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char *TAG = "tailpanda";

// =========================================================================
// 全局开关
// =========================================================================
static volatile bool g_running    = AUTO_RUN_DEFAULT_ON;
static volatile bool g_hard_swing = AUTO_RUN_DEFAULT_HARD;

bool IsAutoRunRunning()          { return g_running; }
void SetAutoRunRunning(bool v)   { g_running = v; }
bool IsAutoRunHardSwing()        { return g_hard_swing; }
void SetAutoRunHardSwing(bool v) { g_hard_swing = v; }

// =========================================================================
// 工具函数
// =========================================================================
inline int irnd(int n) { return (int)(esp_random() % (uint32_t)(n)); }       // [0, n)
inline int irand(int lo, int hi) { return lo + irnd(hi - lo + 1); }           // [lo, hi]
inline float frnd() { return (float)esp_random() / (float)UINT32_MAX; }      // [0, 1)

// 硬摆波形锐化：smoothstep 推极值
inline float harden(float v) {
    float s = fabsf(v);
    s = s * s * (3.0f - 2.0f * s);
    return (v > 0.0f ? s : -s);
}

// 相位扭曲：让正弦波在极值处"停顿"更久、过零点快速穿过
// 模拟真实肌肉的"快收缩、慢放松"特征，打破匀速机械感
inline float warp_phase(float linear) {
    // smoothstep 扭曲：极值处平缓、过零处陡峭
    float s = linear * linear * (3.0f - 2.0f * linear);
    return linear * 0.25f + s * 0.75f;  // 25%保留线性 + 75%smoothstep扭曲
}

// 有机正弦 v2：相位扭曲 + 谐波 = 不对称的"肌肉感"
// 效果：动作在两端有微停顿（像呼吸），穿过中点时快速（像弹跳）
inline float organic_sin(float x) {
    float phase = fmodf(x / (2.0f * M_PI), 1.0f);
    if (phase < 0.0f) phase += 1.0f;
    float wx = warp_phase(phase) * 2.0f * M_PI;
    return sinf(wx)
         + 0.12f * sinf(2.0f * wx + 0.5f)
         + 0.05f * sinf(3.0f * wx + 1.2f);
}

// =========================================================================
// 运行时音效索引 —— 根据 Flash 中的 category 字段动态选择
// =========================================================================
static int g_last_sound = 0;

static void init_sound_indices() {
    int total = flash_audio_get_file_count();
    int animal = flash_audio_get_count_by_category("animal");
    int ambient = flash_audio_get_count_by_category("ambient");
    ESP_LOGI(TAG, "Audio: %d total (🐾%d animal  🌿%d ambient)", total, animal, ambient);
}

static bool trigger_sound() {
    int idx = flash_audio_get_random_in_category("animal");
    if (idx < 0) return false;
    g_last_sound = idx;
    PlayPandaSound(idx);
    return true;
}

// =========================================================================
// 头部模式（12 种）—— center + amplitude × sin(2π·t/period)
// =========================================================================
enum HeadMode {
    HEAD_BREATHE, HEAD_IDLE, HEAD_NOD, HEAD_TILT, HEAD_SHAKE,
    HEAD_SWEEP, HEAD_LOOK_LEFT, HEAD_LOOK_RIGHT, HEAD_SLEEP,
    HEAD_PECK, HEAD_SNUGGLE, HEAD_ALERT,
    HEAD_MODE_COUNT
};
struct HParam { int center; int amplitude; float period_s; };
// 大幅度 + 长周期 = 舒展舒缓，不追求小幅度
static constexpr HParam kHeadP[HEAD_MODE_COUNT] = {
    {90,25,7.0f}, {90,22,5.0f}, {90,40,3.5f}, {80,35,4.5f},  // BREATHE, IDLE, NOD, TILT
    {90,40,3.0f}, {90,60,5.0f}, {130,12,6.0f}, {50,12,6.0f}, // SHAKE, SWEEP, LOOK_L, LOOK_R
    {95, 8,8.0f}, {90,30,2.0f}, {90,22,3.5f}, {90,45,2.5f},  // SLEEP, PECK, SNUGGLE, ALERT
};

// =========================================================================
// 尾部模式（19 种）—— IO15=LR (0左180右), IO16=UD (0上180下)
// phase_offs: 0=同步, 0.25=画圆, 0.5=反向
// =========================================================================
enum TailMode {
    TAIL_BREATHE, TAIL_RELAX, TAIL_WAG, TAIL_SWING_WIDE,
    TAIL_CIRCLE, TAIL_FIGURE8, TAIL_RAISE_SWAY, TAIL_DROOP,
    TAIL_WAVE, TAIL_FLICK, TAIL_TWITCH, TAIL_BOUNCE,
    TAIL_RAISE_HOLD, TAIL_SLOW_RAISE,
    TAIL_RAISE_WAG, TAIL_RAISE_CIRCLE, TAIL_RAISE_FIGURE8,
    TAIL_ALERT, TAIL_HAPPY,
    TAIL_BREATHE2,  // 呼吸变体
    TAIL_MODE_COUNT
};
struct TParam { int lr_c, lr_a, ud_c, ud_a; float period_s, phase_offs; };
// phase>0 → 椭圆轨迹；一轴大另一轴小的模式 = "定住一个方向轻轻晃"
static constexpr TParam kTailP[TAIL_MODE_COUNT] = {
    {90,55,120,45, 5.0f,0.25f}, {90,45,130,40, 3.5f,0.22f}, // BREATHE, RELAX
    {90,70,180, 0, 1.0f,0.0f},  {90,65,120,45, 2.5f,0.18f}, // WAG(纯LR), SWING_WIDE
    {90,60, 90,60, 2.5f,0.25f}, {90,55, 90,55, 3.0f,0.28f}, // CIRCLE, FIGURE8
    {90,35, 55, 6, 4.0f,0.0f},  {90, 6,172, 8, 7.0f,0.0f}, // RAISE_SWAY(翘起+LR微晃), DROOP(下垂+UD微晃)
    {90,60,120,45, 4.0f,0.20f}, {90,65,130,25, 1.8f,0.25f}, // WAVE, FLICK
    {90,35,150,25, 1.5f,0.30f}, {90,50,120,40, 2.2f,0.20f}, // TWITCH, BOUNCE
    {90, 5, 55,25, 6.0f,0.0f},  {90, 6, 90,55,12.0f,0.0f}, // RAISE_HOLD(LR定住+UD微晃), SLOW_RAISE(LR定住+UD慢抬)
    {90,60, 60,35, 1.5f,0.20f}, {90,50, 80,45, 1.8f,0.25f}, // RAISE_WAG, RAISE_CIRCLE
    {90,45, 80,40, 2.5f,0.25f}, {90, 4, 35, 5, 5.5f,0.0f}, // RAISE_FIGURE8, ALERT(直立定住+极微晃)
    {90,45, 60,35, 1.0f,0.22f},                               // HAPPY
};

// =========================================================================
// 音效 → 动作映射
// =========================================================================
static void sound_to_action(int sound, HeadMode &h, TailMode &t,
                             float &ha, float &ta, bool &hd) {
    int r = irnd(100);
    switch (sound) {
    case 1: case 2: // 熊猫叫声
        if (r < 30)      { h = HEAD_LOOK_LEFT;  t = TAIL_WAG;        ha = 0.80f; ta = 0.85f; hd = false; }
        else if (r < 55) { h = HEAD_LOOK_RIGHT; t = TAIL_RAISE_HOLD; ha = 0.80f; ta = 0.80f; hd = false; }
        else if (r < 75) { h = HEAD_TILT;       t = TAIL_FLICK;      ha = 0.75f; ta = 0.90f; hd = true; }
        else             { h = HEAD_SWEEP;       t = TAIL_RAISE_WAG;  ha = 0.85f; ta = 0.80f; hd = false; }
        break;
    case 3: // 吃竹子
        if (r < 40)      { h = HEAD_PECK;    t = TAIL_RELAX;  ha = 0.95f; ta = 0.60f; hd = false; }
        else if (r < 70) { h = HEAD_NOD;     t = TAIL_TWITCH; ha = 0.80f; ta = 0.50f; hd = false; }
        else             { h = HEAD_SNUGGLE;  t = TAIL_BOUNCE; ha = 0.75f; ta = 0.70f; hd = false; }
        break;
    case 4: // 宝宝嘤嘤
        if (r < 50)      { h = HEAD_SHAKE;  t = TAIL_WAG;        ha = 0.90f; ta = 1.00f; hd = (r < 20); }
        else if (r < 80) { h = HEAD_NOD;    t = TAIL_BOUNCE;     ha = 0.85f; ta = 0.90f; hd = false; }
        else             { h = HEAD_ALERT;  t = TAIL_RAISE_HOLD; ha = 0.80f; ta = 0.85f; hd = true; }
        break;
    case 5: // 成年叫声
        if (r < 35)      { h = HEAD_SWEEP;      t = TAIL_SWING_WIDE; ha = 0.90f; ta = 0.85f; hd = (r < 15); }
        else if (r < 65) { h = HEAD_ALERT;      t = TAIL_RAISE_HOLD; ha = 0.95f; ta = 0.90f; hd = true; }
        else             { h = HEAD_LOOK_LEFT;  t = TAIL_RAISE_WAG;  ha = 0.85f; ta = 0.80f; hd = false; }
        break;
    case 6: // 撒娇
        if (r < 35)      { h = HEAD_TILT;    t = TAIL_CIRCLE;        ha = 0.80f; ta = 0.75f; hd = false; }
        else if (r < 60) { h = HEAD_SNUGGLE; t = TAIL_FIGURE8;       ha = 0.75f; ta = 0.70f; hd = false; }
        else             { h = HEAD_NOD;     t = TAIL_RAISE_FIGURE8; ha = 0.70f; ta = 0.65f; hd = false; }
        break;
    case 7: // 类似猫叫
        if (r < 50)      { h = HEAD_TILT;    t = TAIL_CIRCLE;      ha = 0.85f; ta = 0.90f; hd = false; }
        else             { h = HEAD_SNUGGLE;  t = TAIL_RAISE_HOLD; ha = 0.75f; ta = 0.80f; hd = false; }
        break;
    case 8: // 类似熊猫声
        if (r < 40)      { h = HEAD_SHAKE;  t = TAIL_FIGURE8; ha = 0.90f; ta = 0.85f; hd = (r < 15); }
        else if (r < 70) { h = HEAD_ALERT;  t = TAIL_BOUNCE;  ha = 0.85f; ta = 0.80f; hd = true; }
        else             { h = HEAD_PECK;   t = TAIL_TWITCH;  ha = 0.70f; ta = 0.65f; hd = false; }
        break;
    default:
        h = HEAD_IDLE; t = TAIL_RELAX; ha = 0.80f; ta = 0.80f; hd = false; break;
    }
}

// =========================================================================
// 模式选择池
// =========================================================================
// 舒缓模式头部：只保留最温柔的动作，NOD 在自然音下也显得突兀
static constexpr HeadMode kRelaxH[] = {
    HEAD_BREATHE, HEAD_IDLE, HEAD_TILT, HEAD_SLEEP, HEAD_SNUGGLE
};
static constexpr int kRelaxHN = sizeof(kRelaxH) / sizeof(kRelaxH[0]);

// 舒缓模式尾部：只保留最慢最柔的动作，<=1.5s 周期的全部去掉
static constexpr TailMode kRelaxT[] = {
    TAIL_BREATHE, TAIL_RELAX, TAIL_DROOP,
    TAIL_RAISE_HOLD, TAIL_SLOW_RAISE, TAIL_ALERT,
    TAIL_WAVE
};
static constexpr int kRelaxTN = sizeof(kRelaxT) / sizeof(kRelaxT[0]);

// Idle 头部池：去掉 SHAKE/PECK，整体偏温柔
static constexpr HeadMode kIdleH[] = {
    HEAD_BREATHE, HEAD_IDLE, HEAD_TILT, HEAD_NOD, HEAD_SNUGGLE,
    HEAD_SLEEP, HEAD_LOOK_LEFT, HEAD_LOOK_RIGHT
};
static constexpr int kIdleHN = sizeof(kIdleH) / sizeof(kIdleH[0]);

// Idle 尾部池：去掉 WAG/SWING_WIDE/FLICK/TWITCH/BOUNCE，保留温和动作
static constexpr TailMode kIdleT[] = {
    TAIL_BREATHE, TAIL_BREATHE2, TAIL_RELAX, TAIL_DROOP,
    TAIL_WAVE, TAIL_CIRCLE, TAIL_FIGURE8,
    TAIL_RAISE_SWAY, TAIL_RAISE_HOLD, TAIL_SLOW_RAISE,
    TAIL_RAISE_WAG, TAIL_RAISE_CIRCLE, TAIL_RAISE_FIGURE8,
    TAIL_ALERT, TAIL_HAPPY
};
static constexpr int kIdleTN = sizeof(kIdleT) / sizeof(kIdleT[0]);

// 暂停姿态池（动-停交替中的"停"阶段）
static constexpr HeadMode kStillH[] = {
    HEAD_SLEEP, HEAD_IDLE, HEAD_LOOK_LEFT, HEAD_LOOK_RIGHT
};
static constexpr int kStillHN = sizeof(kStillH) / sizeof(kStillH[0]);

static constexpr TailMode kStillT[] = {
    TAIL_DROOP, TAIL_DROOP, TAIL_DROOP,   // DROOP 权重 3x
    TAIL_ALERT, TAIL_RAISE_HOLD
};
static constexpr int kStillTN = sizeof(kStillT) / sizeof(kStillT[0]);


// =========================================================================
// 动作渐变交叉结构
// =========================================================================
struct XFade {
    bool     active     = false;
    uint32_t start_tick = 0;
    HeadMode from_head;
    TailMode from_tail;
    float    from_head_amp, from_tail_amp;
};

// =========================================================================
// 头部角度计算（含交叉渐变），纯函数，无副作用
// =========================================================================
static int calc_head_angle(uint32_t tick, int tick_ms,
                            HeadMode mode, float amp_eff,
                            const XFade *xfade) {
    auto &hp = kHeadP[mode];
    float t_s = tick * tick_ms / 1000.0f;
    float ph  = fmodf(t_s / hp.period_s, 1.0f);
    float sv  = organic_sin(ph * 2.0f * M_PI);

    if (!xfade || !xfade->active)
        return hp.center + (int)(hp.amplitude * sv * amp_eff);

    float bt = (tick - xfade->start_tick) * tick_ms / 1000.0f;
    if (bt >= 3.0f) return hp.center + (int)(hp.amplitude * sv * amp_eff);

    float mx  = bt * bt * (3.0f - 2.0f * bt);
    auto &ohp = kHeadP[xfade->from_head];
    float op  = fmodf(t_s / ohp.period_s, 1.0f);
    int old_a = ohp.center + (int)(ohp.amplitude * organic_sin(op * 2.0f * M_PI) * xfade->from_head_amp);
    int new_a = hp.center  + (int)(hp.amplitude * sv * amp_eff);
    return old_a + (int)((new_a - old_a) * mx);
}

// =========================================================================
// 尾部角度计算（含交叉渐变），纯函数
// =========================================================================
static void calc_tail_angles(uint32_t tick, int tick_ms,
                              TailMode mode, float amp_eff,
                              const XFade *xfade, bool hard,
                              int &lr, int &ud) {
    auto &tp = kTailP[mode];
    float speed = g_hard_swing ? HARD_SWING_SPEED_X : 1.0f;
    float t_s   = tick * tick_ms / 1000.0f / speed;
    float plr   = fmodf(t_s / tp.period_s, 1.0f);
    float pud   = fmodf(t_s / tp.period_s + tp.phase_offs, 1.0f);
    float slr   = organic_sin(plr * 2.0f * M_PI);
    float sud   = organic_sin(pud * 2.0f * M_PI);
    if (hard) { slr = harden(slr); sud = harden(sud); }

    // 中心点缓慢漂移：模拟动物重心微调，避免永远回到同一点
    float drift_lr = 3.0f * sinf(t_s * 0.12f + 0.8f);    // ~52s 周期
    float drift_ud = 2.5f * sinf(t_s * 0.09f + 2.1f);    // ~70s 周期

    if (!xfade || !xfade->active) {
        lr = tp.lr_c + (int)((tp.lr_a * slr + drift_lr) * amp_eff);
        ud = tp.ud_c + (int)((tp.ud_a * sud + drift_ud) * amp_eff);
        return;
    }

    float bt = (tick - xfade->start_tick) * tick_ms / 1000.0f;
    if (bt >= 3.0f) {
        lr = tp.lr_c + (int)((tp.lr_a * slr + drift_lr) * amp_eff);
        ud = tp.ud_c + (int)((tp.ud_a * sud + drift_ud) * amp_eff);
        return;
    }

    float mx   = bt * bt * (3.0f - 2.0f * bt);
    auto &otp  = kTailP[xfade->from_tail];
    float oplr = fmodf(t_s / otp.period_s, 1.0f);
    float opud = fmodf(t_s / otp.period_s + otp.phase_offs, 1.0f);
    float oslr = organic_sin(oplr * 2.0f * M_PI), osud = organic_sin(opud * 2.0f * M_PI);
    if (hard) { oslr = harden(oslr); osud = harden(osud); }

    int olr = otp.lr_c + (int)(otp.lr_a * oslr * xfade->from_tail_amp);
    int oud = otp.ud_c + (int)(otp.ud_a * osud * xfade->from_tail_amp);
    int nlr = tp.lr_c  + (int)((tp.lr_a * slr + drift_lr) * amp_eff);
    int nud = tp.ud_c  + (int)((tp.ud_a * sud + drift_ud) * amp_eff);
    lr = olr + (int)((nlr - olr) * mx);
    ud = oud + (int)((nud - oud) * mx);
}

// =========================================================================
// 自然音频多频有机晃动（助眠风格）
// =========================================================================
static void apply_nature_wobble(uint32_t tick, int tick_ms,
                                 int &lr, int &ud) {
    float nt  = tick * tick_ms / 1000.0f;
    // 自然音效：多频段有机晃动，大幅度但慢节奏
    float w1  = sinf(nt * 0.17f)       * 24.0f;
    float w2  = sinf(nt * 0.35f + 1.0f) * 16.0f;
    float w1u = cosf(nt * 0.21f)       * 10.0f;
    float w2u = sinf(nt * 0.43f + 0.7f) *  8.0f;
    float wob_lr = w1 + w2 * 0.5f;
    float wob_ud = w1u + w2u * 0.5f;
    lr = (int)((float)lr * 0.35f + (90.0f  + wob_lr) * 0.65f);
    ud = (int)((float)ud * 0.35f + (150.0f + wob_ud) * 0.65f);
}

// =========================================================================
// AutoRun 任务主函数
// =========================================================================
static void auto_run_task(void *arg) {
    constexpr int MS = 20;                       // 帧间隔 ms
    constexpr int TICK_DECAY = 5000 / MS;        // 幅度衰减 5s (tick数) — 更慢更丝滑
    constexpr int TICK_DEBOUNCE = 500 / MS;      // 开关防抖 500ms

    init_sound_indices();                        // 初始化音效索引

    uint32_t tick = 0;

    // ---- 当前动作状态 ----
    HeadMode head_mode = HEAD_BREATHE;
    TailMode tail_mode = TAIL_RELAX;      // 中性起始姿态，避免上电尾巴上翘
    float    head_amp = 0.60f, tail_amp = 0.55f;  // 初始幅度温和
    bool     is_hard  = false;
    uint32_t amp_start = 0;

    // ---- 舒缓模式 ----
    bool     in_relax = false, nature_on = false;
    uint32_t next_relax = (uint32_t)(300 * 1000 / MS);  // first relax after ~5min
    uint32_t next_var   = 0;

    // ---- 音效触发 ----
    uint32_t next_sound   = (uint32_t)(8 * 1000 / MS);
    uint32_t react_end    = 0;
    bool     was_playing  = false;

    // ---- 其他定时器 ----
    uint32_t next_spont = (uint32_t)(irand(15, 35) * 1000 / MS);
    uint32_t next_idle  = (uint32_t)(irand(8, 15) * 1000 / MS);

    // ---- 防抖 ----
    uint32_t last_toggle = 0;
    bool     prev_running = g_running;

    // ---- 渐变结构 ----
    XFade xfade;

    // ======================== 主循环 ========================
    while (true) {
        if (!g_running) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }

        // ----- 舒缓模式定时 -----
        if (!in_relax && tick >= next_relax) {
            int nature_idx = flash_audio_get_random_in_category("ambient");
            if (nature_idx >= 0) {
                in_relax  = true;
                nature_on = true;
                FlushAudioQueue();
                PlayPandaSound(nature_idx);
                // 初始也用舒缓池随机挑，避免每次都是同一个动作
                head_mode = kRelaxH[irnd(kRelaxHN)];
                tail_mode = kRelaxT[irnd(kRelaxTN)];
                head_amp  = 0.75f + frnd() * 0.25f;
                tail_amp  = 0.70f + frnd() * 0.30f;
                is_hard   = false;
                amp_start = tick;
                xfade.active = false;
                next_var  = tick + (uint32_t)(irand(1500, 2500) / MS);  // 更快开始第一轮切换
                ESP_LOGI(TAG, "Relax ON");
            } else {
                next_relax = tick + (uint32_t)(300 * 1000 / MS); // 无声时5分钟后再试
            }
        }

        // ----- 音频状态机 -----
        bool now_playing = IsAudioPlaying();

        // 看门狗：播放超180s告警
        {
            static uint32_t ps = 0;
            if (now_playing && !was_playing) ps = tick;
            if (now_playing && (tick - ps) > (uint32_t)(180000 / MS))
                ESP_LOGE(TAG, "Audio stuck >180s");
        }

        if (now_playing && !was_playing) {
            if (nature_on) {
                // 舒缓模式：换动作（先保存旧状态，再设新模式，crossfade 才能正确插值）
                HeadMode old_h = head_mode; TailMode old_t = tail_mode;
                float old_ha = head_amp, old_ta = tail_amp;
                head_mode = kRelaxH[irnd(kRelaxHN)];
                tail_mode = kRelaxT[irnd(kRelaxTN)];
                head_amp  = 0.75f + frnd() * 0.25f;  // 自然音效：大幅度 + 长周期 = 舒展
                tail_amp  = 0.70f + frnd() * 0.30f;
                is_hard   = false;
                amp_start = tick;
                xfade.active = true; xfade.start_tick = tick;
                xfade.from_head = old_h; xfade.from_tail = old_t;
                xfade.from_head_amp = old_ha; xfade.from_tail_amp = old_ta;
                next_var  = tick + (uint32_t)(irand(3, 6) * 1000 / MS);  // 换动作更快，增加多样性
                if (irnd(5) == 0) next_var = tick + (uint32_t)(2000 / MS);
            } else {
                // 叫声音效联动（此处顺序正确：先捕获旧值再覆盖）
                HeadMode h; TailMode t; float ha, ta; bool hd;
                sound_to_action(g_last_sound, h, t, ha, ta, hd);
                xfade.from_head = head_mode; xfade.from_tail = tail_mode;
                xfade.from_head_amp = head_amp; xfade.from_tail_amp = tail_amp;
                xfade.active = true; xfade.start_tick = tick;
                head_mode = h; tail_mode = t;
                head_amp = ha; tail_amp = ta; is_hard = hd;
                amp_start = tick;
                react_end = tick + (uint32_t)((4000 + irnd(3000)) / MS);
            }
        }
        if (!now_playing && was_playing && nature_on) {
            nature_on = false; in_relax = false;
            next_relax = tick + (uint32_t)(irand(300, 600) * 1000 / MS);  // 5~10min
            ESP_LOGI(TAG, "Relax OFF");
            // 回idle：先保存旧状态再设新模式
            HeadMode old_h = head_mode; TailMode old_t = tail_mode;
            float old_ha = head_amp, old_ta = tail_amp;
            head_mode = kIdleH[irnd(kIdleHN)];
            tail_mode = kIdleT[irnd(kIdleTN)];
            head_amp  = 0.65f + frnd() * 0.25f;
            tail_amp  = 0.60f + frnd() * 0.25f;
            is_hard   = false;
            amp_start = tick;
            xfade.active = true; xfade.start_tick = tick;
            xfade.from_head = old_h; xfade.from_tail = old_t;
            xfade.from_head_amp = old_ha; xfade.from_tail_amp = old_ta;
        }

        was_playing = now_playing;

        // ----- 联动结束回idle -----
        if (!nature_on && !now_playing && react_end > 0 && tick >= react_end) {
            HeadMode old_h = head_mode; TailMode old_t = tail_mode;
            float old_ha = head_amp, old_ta = tail_amp;
            head_mode = kIdleH[irnd(kIdleHN)];
            tail_mode = kIdleT[irnd(kIdleTN)];
            head_amp  = 0.65f + frnd() * 0.25f;
            tail_amp  = 0.60f + frnd() * 0.25f;
            is_hard   = false;
            amp_start = tick;
            xfade.active = true; xfade.start_tick = tick;
            xfade.from_head = old_h; xfade.from_tail = old_t;
            xfade.from_head_amp = old_ha; xfade.from_tail_amp = old_ta;
            react_end = 0;
        }

        // ----- 舒缓模式动作轮换 -----
        if (nature_on && now_playing && tick >= next_var) {
            HeadMode old_h = head_mode; TailMode old_t = tail_mode;
            float old_ha = head_amp, old_ta = tail_amp;
            head_mode = kRelaxH[irnd(kRelaxHN)];
            tail_mode = kRelaxT[irnd(kRelaxTN)];
            head_amp  = 0.75f + frnd() * 0.25f;
            tail_amp  = 0.70f + frnd() * 0.30f;
            is_hard   = false;
            amp_start = tick;
            xfade.active = true; xfade.start_tick = tick;
            xfade.from_head = old_h; xfade.from_tail = old_t;
            xfade.from_head_amp = old_ha; xfade.from_tail_amp = old_ta;
            next_var  = tick + (uint32_t)(irand(3, 6) * 1000 / MS);
            if (irnd(5) == 0) next_var = tick + (uint32_t)(2000 / MS);
        }

        // ----- 自发微动作 -----
        if (!in_relax && !now_playing && react_end == 0 && tick >= next_spont) {
            if (irnd(5) == 0) {
                HeadMode old_h = head_mode; TailMode old_t = tail_mode;
                float old_ha = head_amp, old_ta = tail_amp;
                tail_mode = irnd(2) ? TAIL_WAVE : TAIL_BREATHE2;  // 用温柔动作替代 FLICK/TWITCH
                head_amp  = 0.60f; tail_amp = 0.50f;
                is_hard   = false;
                amp_start = tick;
                xfade.active = true; xfade.start_tick = tick;
                xfade.from_head = old_h; xfade.from_tail = old_t;
                xfade.from_head_amp = old_ha; xfade.from_tail_amp = old_ta;
                react_end = tick + (uint32_t)(1200 / MS);
            }
            next_spont = tick + (uint32_t)(irand(15, 35) * 1000 / MS);
        }

        // ----- 周期性idle轮换 -----
        if (!in_relax && !now_playing && react_end == 0 && tick >= next_idle) {
            HeadMode old_h = head_mode; TailMode old_t = tail_mode;
            float old_ha = head_amp, old_ta = tail_amp;
            head_mode = kIdleH[irnd(kIdleHN)];
            tail_mode = kIdleT[irnd(kIdleTN)];
            head_amp  = 0.65f + frnd() * 0.25f;
            tail_amp  = 0.60f + frnd() * 0.25f;
            is_hard   = false;
            amp_start = tick;
            xfade.active = true; xfade.start_tick = tick;
            xfade.from_head = old_h; xfade.from_tail = old_t;
            xfade.from_head_amp = old_ha; xfade.from_tail_amp = old_ta;
            next_idle = tick + (uint32_t)(irand(10, 20) * 1000 / MS);
        }

        // ----- 音效触发 -----
        if (!in_relax && !now_playing && tick >= next_sound) {
            trigger_sound();
            next_sound = tick + (uint32_t)(1000 / MS); // 失败也1s后重试
        }
        if (now_playing) {
            next_sound  = tick + (uint32_t)(irand(AUDIO_SILENT_INTERVAL_MIN_S,
                                                   AUDIO_SILENT_INTERVAL_MAX_S) * 1000 / MS);
            next_spont  = tick + (uint32_t)(irand(15, 35) * 1000 / MS);
            next_idle   = tick + (uint32_t)(irand(8, 15) * 1000 / MS);
        }

        // ----- 防抖 -----
        if (g_running != prev_running) {
            if (tick - last_toggle < TICK_DEBOUNCE)
                g_running = prev_running;
            else {
                last_toggle = tick;
                prev_running = g_running;
            }
        }

        // ----- 幅度衰减：5秒线性至75%（比之前的60%更温和）-----
        float decay = 1.0f;
        {
            uint32_t dt = tick - amp_start;
            if (dt < TICK_DECAY)
                decay = 1.0f - (float)dt / (float)TICK_DECAY * 0.25f;
            else
                decay = 0.75f;
        }
        float ha = head_amp * decay;
        float ta = tail_amp * decay;

        // ----- 检查渐变是否过期 -----
        if (xfade.active && (tick - xfade.start_tick) * MS / 1000 >= 3.0f)
            xfade.active = false;

        // ----- 计算角度并输出 -----
        int h_ang = calc_head_angle(tick, MS, head_mode, ha,
                                     xfade.active ? &xfade : nullptr);
        SetServoAngle(SERVO_HEAD, h_ang);

        int t_lr, t_ud;
        calc_tail_angles(tick, MS, tail_mode, ta,
                          xfade.active ? &xfade : nullptr, is_hard, t_lr, t_ud);

        if (nature_on && now_playing)
            apply_nature_wobble(tick, MS, t_lr, t_ud);

        SetServoAngle(SERVO_TAIL_LR, t_lr);
        SetServoAngle(SERVO_TAIL_UD, t_ud);

        tick++;
        vTaskDelay(pdMS_TO_TICKS(MS));
    }
}

// =========================================================================
// HTTP
// =========================================================================
esp_err_t HandleAutoPlay(httpd_req_t *req) {
    if (req->method == HTTP_POST) {
        char buf[64] = {};
        httpd_req_recv(req, buf, sizeof(buf) - 1);
        const char *p = strstr(buf, "\"enable\":");
        if (p) { p += 9; g_running = (atoi(p) != 0); }
        else if (!strstr(buf, "hard_swing"))
            g_running = !g_running;
        p = strstr(buf, "\"hard_swing\":");
        if (p) { p += 13; g_hard_swing = (strncmp(p, "true", 4) == 0 || atoi(p) == 1); }
    }
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"autoplay\":%s,\"hard_swing\":%s}",
             g_running ? "true" : "false", g_hard_swing ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

void InitAutoRun() {
    xTaskCreate(auto_run_task, "auto_run", 4096, nullptr, 2, nullptr);
    ESP_LOGI(TAG, "Auto-run task created");
}

#endif
