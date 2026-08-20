#include "auto_run.h"
#include "audio.h"
#include "config.h"
#include "flash_audio.h"
#include "servo.h"

#if ENABLE_AUTO_RUN

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <esp_log.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "tailpanda_life";
static volatile bool g_running = AUTO_RUN_DEFAULT_ON;
static volatile bool g_hard_swing = AUTO_RUN_DEFAULT_HARD;

bool IsAutoRunRunning() { return g_running; }
void SetAutoRunRunning(bool value) { g_running = value; }
bool IsAutoRunHardSwing() { return g_hard_swing; }
void SetAutoRunHardSwing(bool value) { g_hard_swing = value; }

static int irnd(int count) { return count > 0 ? (int)(esp_random() % (uint32_t)count) : 0; }
static int irand(int low, int high) { return low + irnd(high - low + 1); }
static float frand(float low, float high) {
  return low + ((float)esp_random() / (float)UINT32_MAX) * (high - low);
}
static float smoothstep(float value) {
  value = std::clamp(value, 0.0f, 1.0f);
  return value * value * value * (value * (value * 6.0f - 15.0f) + 10.0f);
}

// README/docs confirmed: head 0=right; tail LR 0=left; tail UD 0=up.
struct Pose { float head, tail_lr, tail_ud; };
struct Keyframe { uint16_t duration_ms; int16_t head, tail_lr, tail_ud; };
struct Clip { const char *name; const Keyframe *frames; uint8_t count; bool mirrorable; };
#define COUNT_OF(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

// One-shot performances: anticipation -> emotional peak -> soft return.
static constexpr Keyframe kIdleGlance[] = {
  {650,132,82,156},{420,145,82,156},{820,112,112,142},{950,90,90,150}};
static constexpr Keyframe kIdleLowSweep[] = {
  {850,78,32,170},{1150,108,154,154},{950,96,55,174},{900,90,90,152}};
static constexpr Keyframe kIdlePerk[] = {
  {700,118,95,148},{850,150,142,58},{520,136,156,44},{1100,96,104,126},{700,90,90,150}};
static constexpr Keyframe kIdleCircle[] = {
  {650,62,35,126},{650,75,76,58},{650,105,150,78},{650,125,158,144},
  {650,102,78,170},{850,90,90,148}};
static constexpr Keyframe kCuriousPeek[] = {
  {380,142,90,158},{520,154,90,158},{700,148,138,92},{420,62,146,74},
  {680,128,45,120},{950,90,90,148}};
static constexpr Keyframe kCuriousTrack[] = {
  {500,38,104,158},{820,54,150,132},{350,145,150,132},{760,132,45,72},{950,90,90,146}};
static constexpr Keyframe kCuriousCircle[] = {
  {450,150,92,158},{600,138,34,106},{600,92,70,42},{600,48,148,68},
  {600,72,158,146},{900,90,90,150}};
static constexpr Keyframe kEatSearch[] = {
  {700,42,70,168},{520,128,38,174},{420,55,118,172},{420,138,150,165},
  {600,72,62,174},{850,90,90,154}};
static constexpr Keyframe kEatChew[] = {
  {620,128,74,168},{360,56,76,168},{470,122,108,172},{330,68,110,172},
  {620,145,148,158},{900,90,90,152}};
static constexpr Keyframe kEatDelight[] = {
  {650,45,42,174},{500,132,148,166},{650,55,154,138},{620,138,36,102},{850,90,90,150}};
static constexpr Keyframe kBabySeek[] = {
  {420,35,92,160},{360,145,92,160},{650,128,42,128},{520,52,148,86},
  {750,135,122,52},{950,90,90,146}};
static constexpr Keyframe kBabyWiggle[] = {
  {500,138,48,145},{440,58,144,116},{500,126,32,82},{460,65,155,58},
  {750,120,72,126},{900,90,90,150}};
static constexpr Keyframe kBabyComfort[] = {
  {800,138,128,142},{900,150,154,94},{1000,126,122,52},{1000,104,62,96},{950,90,90,148}};
static constexpr Keyframe kProudDisplay[] = {
  {650,45,88,158},{750,142,40,92},{900,155,84,28},{650,62,156,48},
  {850,132,138,108},{1000,90,90,150}};
static constexpr Keyframe kProudPatrol[] = {
  {900,32,148,130},{900,148,34,98},{750,122,70,44},{750,52,152,66},{1000,90,90,148}};
static constexpr Keyframe kProudCall[] = {
  {500,118,90,154},{780,155,46,92},{900,148,90,24},{700,48,158,54},
  {750,128,34,118},{1050,90,90,150}};
static constexpr Keyframe kNuzzle[] = {
  {800,142,118,152},{900,158,150,118},{850,146,128,70},{900,120,48,102},{1050,90,90,150}};
static constexpr Keyframe kShyApproach[] = {
  {650,38,76,164},{900,55,42,140},{620,142,138,96},{850,152,154,58},{1100,90,90,150}};
static constexpr Keyframe kSoftCurl[] = {
  {1000,128,36,154},{1100,145,58,92},{1200,132,122,46},{1000,108,156,108},{1050,90,90,150}};
static constexpr Keyframe kPlayWag[] = {
  {480,138,28,132},{480,55,154,112},{520,128,36,78},{520,62,158,54},
  {620,145,42,98},{850,90,90,148}};
static constexpr Keyframe kPlayChase[] = {
  {550,42,150,154},{550,138,42,126},{600,52,30,76},{600,145,148,42},
  {650,118,158,126},{900,90,90,150}};
static constexpr Keyframe kPlayEight[] = {
  {520,145,35,112},{520,72,78,48},{520,42,150,106},{520,128,105,164},
  {520,152,38,104},{520,58,104,52},{520,36,154,116},{850,90,90,150}};
static constexpr Keyframe kAmbientListen[] = {
  {1100,42,90,166},{1500,55,132,138},{1200,145,142,84},{1800,132,62,56},{1600,90,90,146}};
static constexpr Keyframe kAmbientRain[] = {
  {1000,132,42,168},{800,48,148,154},{1300,120,154,106},{1400,62,84,48},
  {1500,104,32,112},{1500,90,90,152}};
static constexpr Keyframe kAmbientScent[] = {
  {1300,35,54,170},{1500,62,148,158},{1300,142,128,116},{1500,118,38,72},{1600,90,90,150}};
static constexpr Keyframe kAmbientStretch[] = {
  {1500,122,90,174},{1800,145,42,116},{1800,132,82,34},{1700,55,154,68},{1800,90,90,150}};
static constexpr Keyframe kSettle[] = {
  {700,90,90,170},{1200,102,108,146},{850,90,90,150}};

enum ClipId {
  C_IDLE_GLANCE,C_IDLE_LOW_SWEEP,C_IDLE_PERK,C_IDLE_CIRCLE,
  C_CURIOUS_PEEK,C_CURIOUS_TRACK,C_CURIOUS_CIRCLE,
  C_EAT_SEARCH,C_EAT_CHEW,C_EAT_DELIGHT,
  C_BABY_SEEK,C_BABY_WIGGLE,C_BABY_COMFORT,
  C_PROUD_DISPLAY,C_PROUD_PATROL,C_PROUD_CALL,
  C_NUZZLE,C_SHY_APPROACH,C_SOFT_CURL,
  C_PLAY_WAG,C_PLAY_CHASE,C_PLAY_EIGHT,
  C_AMBIENT_LISTEN,C_AMBIENT_RAIN,C_AMBIENT_SCENT,C_AMBIENT_STRETCH,
  C_SETTLE,C_COUNT
};
static constexpr Clip kClips[C_COUNT] = {
  {"idle_glance",kIdleGlance,COUNT_OF(kIdleGlance),true},
  {"idle_low_sweep",kIdleLowSweep,COUNT_OF(kIdleLowSweep),true},
  {"idle_perk",kIdlePerk,COUNT_OF(kIdlePerk),true},
  {"idle_circle",kIdleCircle,COUNT_OF(kIdleCircle),true},
  {"curious_peek",kCuriousPeek,COUNT_OF(kCuriousPeek),true},
  {"curious_track",kCuriousTrack,COUNT_OF(kCuriousTrack),true},
  {"curious_circle",kCuriousCircle,COUNT_OF(kCuriousCircle),true},
  {"eat_search",kEatSearch,COUNT_OF(kEatSearch),true},
  {"eat_chew",kEatChew,COUNT_OF(kEatChew),true},
  {"eat_look_up",kEatDelight,COUNT_OF(kEatDelight),true},
  {"baby_seek",kBabySeek,COUNT_OF(kBabySeek),true},
  {"baby_wiggle",kBabyWiggle,COUNT_OF(kBabyWiggle),true},
  {"baby_comfort",kBabyComfort,COUNT_OF(kBabyComfort),true},
  {"proud_display",kProudDisplay,COUNT_OF(kProudDisplay),true},
  {"proud_patrol",kProudPatrol,COUNT_OF(kProudPatrol),true},
  {"proud_call",kProudCall,COUNT_OF(kProudCall),true},
  {"affection_nuzzle",kNuzzle,COUNT_OF(kNuzzle),true},
  {"affection_shy",kShyApproach,COUNT_OF(kShyApproach),true},
  {"affection_curl",kSoftCurl,COUNT_OF(kSoftCurl),true},
  {"playful_wag",kPlayWag,COUNT_OF(kPlayWag),true},
  {"playful_chase",kPlayChase,COUNT_OF(kPlayChase),true},
  {"playful_figure8",kPlayEight,COUNT_OF(kPlayEight),true},
  {"ambient_listen",kAmbientListen,COUNT_OF(kAmbientListen),true},
  {"ambient_rain",kAmbientRain,COUNT_OF(kAmbientRain),true},
  {"ambient_scent",kAmbientScent,COUNT_OF(kAmbientScent),true},
  {"ambient_stretch",kAmbientStretch,COUNT_OF(kAmbientStretch),true},
  {"settle",kSettle,COUNT_OF(kSettle),false},
};

enum Behaviour { B_IDLE,B_CURIOUS,B_EAT,B_BABY,B_PROUD,B_AFFECTION,B_PLAYFUL,B_AMBIENT,B_SETTLE,B_COUNT };
static constexpr int8_t kFamily[B_COUNT][4] = {
  {C_IDLE_GLANCE,C_IDLE_LOW_SWEEP,C_IDLE_PERK,C_IDLE_CIRCLE},
  {C_CURIOUS_PEEK,C_CURIOUS_TRACK,C_CURIOUS_CIRCLE,-1},
  {C_EAT_SEARCH,C_EAT_CHEW,C_EAT_DELIGHT,-1},
  {C_BABY_SEEK,C_BABY_WIGGLE,C_BABY_COMFORT,-1},
  {C_PROUD_DISPLAY,C_PROUD_PATROL,C_PROUD_CALL,-1},
  {C_NUZZLE,C_SHY_APPROACH,C_SOFT_CURL,-1},
  {C_PLAY_WAG,C_PLAY_CHASE,C_PLAY_EIGHT,-1},
  {C_AMBIENT_LISTEN,C_AMBIENT_RAIN,C_AMBIENT_SCENT,C_AMBIENT_STRETCH},
  {C_SETTLE,-1,-1,-1},
};

struct ShuffleBag { int8_t items[4] = {-1,-1,-1,-1}; uint8_t count=0, position=0; };
static ShuffleBag g_bags[B_COUNT];
static int g_last_clip = -1;
static int choose_clip(Behaviour behaviour) {
  ShuffleBag &bag = g_bags[behaviour];
  if (bag.position >= bag.count) {
    bag.count = 0;
    for (int i=0; i<4 && kFamily[behaviour][i]>=0; ++i) bag.items[bag.count++] = kFamily[behaviour][i];
    for (int i=bag.count-1; i>0; --i) std::swap(bag.items[i], bag.items[irnd(i+1)]);
    if (bag.count>1 && bag.items[0]==g_last_clip) std::swap(bag.items[0],bag.items[1]);
    bag.position = 0;
  }
  int result = bag.items[bag.position++];
  g_last_clip = result;
  return result;
}

struct Player {
  Pose pose={90,90,150};
  const Clip *clip=nullptr;
  uint8_t frame=0;
  uint32_t frame_started_ms=0, frame_duration_ms=1;
  float tempo=1;
  bool mirror=false, active=false;
};
static Player g_player;
static int g_mirror_balance = 0;

static Pose lerp_pose(Pose a, Pose b, float t) {
  Pose r;
  r.head=a.head+(b.head-a.head)*t;
  r.tail_lr=a.tail_lr+(b.tail_lr-a.tail_lr)*t;
  r.tail_ud=a.tail_ud+(b.tail_ud-a.tail_ud)*t;
  return r;
}
static Pose target_pose(const Clip &clip, uint8_t frame, bool mirror) {
  const Keyframe &key=clip.frames[frame];
  Pose pose={(float)key.head,(float)key.tail_lr,(float)key.tail_ud};
  if (mirror && clip.mirrorable) { pose.head=180-pose.head; pose.tail_lr=180-pose.tail_lr; }
  return pose;
}
// 连续目标流: 帧进度的最后 30% 提前滑向下一帧 → 动作连续流动、永不停顿
static Pose flow_target(const Clip &clip, uint8_t frame, float progress, bool mirror) {
  uint8_t next=std::min<uint8_t>(frame+1,clip.count-1);
  Pose cur=target_pose(clip,frame,mirror);
  Pose nxt=target_pose(clip,next,mirror);
  float lead=std::clamp((progress-0.70f)/0.30f,0.0f,1.0f);
  return lerp_pose(cur,nxt,smoothstep(lead));
}
static void start_clip(int clip_id, uint32_t now_ms) {
  const Clip &clip=kClips[clip_id];
  g_player.clip=&clip; g_player.frame=0;
  g_player.frame_started_ms=now_ms;
  g_player.tempo=g_hard_swing ? frand(0.76f,0.90f) : frand(0.88f,1.14f);
  g_player.frame_duration_ms=std::max<uint32_t>(180,(uint32_t)(clip.frames[0].duration_ms*g_player.tempo));
  if (!clip.mirrorable) {
    g_player.mirror=false;
  } else {
    // Do not allow random mirroring to create a visible long-term side bias.
    if (g_mirror_balance>=2) g_player.mirror=false;
    else if (g_mirror_balance<=-2) g_player.mirror=true;
    else g_player.mirror=irnd(2);
    g_mirror_balance += g_player.mirror ? 1 : -1;
  }
  g_player.active=true;
  ESP_LOGI(TAG,"clip=%s mirror=%d tempo=%.2f",clip.name,g_player.mirror,(double)g_player.tempo);
}
// 指数追踪: 姿态持续逼近目标流,自然缓入缓出,软着陆无硬停
static void chase_pose(Pose target, uint32_t tau_ms) {
  float k=1.0f-expf(-20.0f/(float)tau_ms);
  g_player.pose.head+=(target.head-g_player.pose.head)*k;
  g_player.pose.tail_lr+=(target.tail_lr-g_player.pose.tail_lr)*k;
  g_player.pose.tail_ud+=(target.tail_ud-g_player.pose.tail_ud)*k;
}
static Pose update_player(uint32_t now_ms) {
  if (!g_player.clip) return g_player.pose;
  while (g_player.active && now_ms-g_player.frame_started_ms>=g_player.frame_duration_ms) {
    g_player.frame_started_ms+=g_player.frame_duration_ms;
    if (++g_player.frame>=g_player.clip->count) { g_player.active=false; break; }
    g_player.frame_duration_ms=std::max<uint32_t>(180,(uint32_t)(g_player.clip->frames[g_player.frame].duration_ms*g_player.tempo));
  }
  uint8_t frame=std::min<uint8_t>(g_player.frame,g_player.clip->count-1);
  float progress=std::clamp((float)(now_ms-g_player.frame_started_ms)/(float)g_player.frame_duration_ms,0.0f,1.0f);
  Pose target=flow_target(*g_player.clip,frame,progress,g_player.mirror);
  // tau 与帧时长联动: 快速动作跟得紧,慢速动作更慵懒
  uint32_t tau=std::max<uint32_t>(70,g_player.frame_duration_ms/3);
  chase_pose(target,tau);
  return g_player.pose;
}

// ===== 常驻微动层: 让动物时刻"活着" =====
static float g_breath_phase=0; static uint32_t g_breath_period=3400;
static void add_breath(Pose &pose, float amount) {
  g_breath_phase+=20.0f;
  if (g_breath_phase>g_breath_period) { g_breath_phase=0; g_breath_period=(uint32_t)irand(2800,4300); }
  float b=sinf(6.2831853f*g_breath_phase/(float)g_breath_period);
  pose.head+=b*0.7f*amount;          // 呼吸带动头部微幅起伏
  pose.tail_ud-=b*1.4f*amount;       // 尾巴随之轻轻浮沉
}
static float g_gaze_pos=0,g_gaze_target=0; static uint32_t g_gaze_next_ms=0;
static void add_gaze(Pose &pose,uint32_t now_ms,float amount) {
  if (now_ms>=g_gaze_next_ms) {
    g_gaze_target=frand(-9.0f,9.0f);
    g_gaze_next_ms=now_ms+(uint32_t)irand(1200,3800);
  }
  g_gaze_pos+=(g_gaze_target-g_gaze_pos)*0.03f;
  pose.head+=g_gaze_pos*amount;      // 头部漫无目的地漂移凝视
}
static void add_tail_sway(Pose &pose,uint32_t now_ms,float amount) {
  float t=(float)now_ms*0.001f;
  pose.tail_lr+=(sinf(t*1.7f)+sinf(t*0.9f+1.3f)*0.7f)*1.1f*amount;
  pose.tail_ud+=sinf(t*2.1f+0.7f)*0.5f*amount;
}
static float g_tremor_phase=0;
static void add_tremor(Pose &pose,float amount) {
  g_tremor_phase+=0.9f;
  pose.head+=(sinf(g_tremor_phase*3.1f)+sinf(g_tremor_phase*5.7f+2.0f))*0.15f*amount;
  pose.tail_lr+=(sinf(g_tremor_phase*4.3f+1.1f)+sinf(g_tremor_phase*2.3f))*0.12f*amount;
  pose.tail_ud+=sinf(g_tremor_phase*3.7f+0.4f)*0.12f*amount;
}

static bool contains(const char *text,const char *needle) { return text && needle && strstr(text,needle); }
static Behaviour classify_sound(int index) {
  const char *name=flash_audio_get_name(index); int duration=flash_audio_get_duration_ms(index);
  if (contains(name,"吃竹")||contains(name,"咀嚼")||contains(name,"eat")||contains(name,"chew")) return B_EAT;
  if (contains(name,"宝宝")||contains(name,"嘤嘤")||contains(name,"baby")) return B_BABY;
  if (contains(name,"撒娇")||contains(name,"亲近")||contains(name,"Cat")||contains(name,"cat")) return B_AFFECTION;
  if (contains(name,"成年")||contains(name,"威风")||contains(name,"adult")) return B_PROUD;
  if (duration>6500) return B_PROUD;
  if (duration>0 && duration<1700) return B_CURIOUS;
  return irnd(100)<55 ? B_CURIOUS : B_PLAYFUL;
}
static bool queue_random_sound(const char *category) {
  int index=flash_audio_get_random_in_category(category);
  if (index<0 || !PlayPandaSound(index)) return false;
  ESP_LOGI(TAG,"queued #%d %s [%s]",index,flash_audio_get_name(index),category); return true;
}

static Pose apply_audio_motion(Pose pose,const AudioMotionData &audio,uint32_t now_ms) {
  static uint32_t last_ms=now_ms,last_onset_ms=0;
  static uint32_t tail_due_ms=0;
  static float onset_offset=0,tail_onset_offset=0,pending_tail_offset=0,tail_phase=0;
  static int onset_side=1;
  uint32_t elapsed=now_ms-last_ms; last_ms=now_ms;
  if (!audio.playing) {
    onset_offset*=0.88f; tail_onset_offset*=0.94f; tail_due_ms=0;
    return pose;
  }
  if (audio.attack>0.16f && now_ms-last_onset_ms>320) {
    onset_side=-onset_side;
    onset_offset=onset_side*(5+10*audio.attack);
    pending_tail_offset=-onset_side*(5+8*audio.attack);
    tail_due_ms=now_ms+(uint32_t)irand(160,320);
    last_onset_ms=now_ms;
  }
  if (tail_due_ms && now_ms>=tail_due_ms) {
    tail_onset_offset=pending_tail_offset;
    tail_due_ms=0;
  }
  onset_offset*=0.90f; tail_onset_offset*=0.955f;
  tail_phase+=(float)elapsed*(0.00055f+0.00125f*audio.level);
  pose.head+=onset_offset;
  pose.tail_lr+=tail_onset_offset+sinf(tail_phase)*std::min(14.0f,audio.level*24.0f);
  pose.tail_ud-=std::min(11.0f,audio.level*18.0f);
  return pose;
}
static void output_pose(Pose pose) {
  int head=(int)std::lround(std::clamp(pose.head,8.0f,172.0f));
  int lr=(int)std::lround(std::clamp(pose.tail_lr,6.0f,174.0f));
  int ud=(int)std::lround(std::clamp(pose.tail_ud,10.0f,178.0f));
  SetServoAngle(SERVO_HEAD,head); SetServoAngle(SERVO_TAIL_LR,lr); SetServoAngle(SERVO_TAIL_UD,ud);
}

static void auto_run_task(void *) {
  constexpr uint32_t FRAME_MS=20;
  int animal_count=flash_audio_get_count_by_category("animal");
  int ambient_count=flash_audio_get_count_by_category("ambient");
  ESP_LOGI(TAG,"living motion v3: animal=%d ambient=%d",animal_count,ambient_count);
  uint32_t now_ms=esp_log_timestamp(),next_sound_ms=now_ms+8000;
  uint32_t next_ambient_ms=now_ms+300000,next_idle_ms=now_ms+1800;
  uint32_t clip_finished_ms=0;
  bool was_playing=false; Behaviour sound_behaviour=B_CURIOUS;
  start_clip(C_SETTLE,now_ms);
  while (true) {
    now_ms=esp_log_timestamp();
    if (!g_running) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
    AudioMotionData audio=GetAudioMotionData(); bool playing=audio.playing;
    bool clip_was_active=g_player.active;
    if (playing && !was_playing) {
      const char *category=flash_audio_get_category(audio.sound_index);
      sound_behaviour=strcmp(category,"ambient")==0 ? B_AMBIENT : classify_sound(audio.sound_index);
      start_clip(choose_clip(sound_behaviour),now_ms);
      ESP_LOGI(TAG,"audio start #%d %s -> behaviour %d",audio.sound_index,flash_audio_get_name(audio.sound_index),sound_behaviour);
    }
    if (!playing && was_playing) {
      start_clip(choose_clip(B_SETTLE),now_ms);
      next_sound_ms=now_ms+(uint32_t)irand(AUDIO_SILENT_INTERVAL_MIN_S*1000,AUDIO_SILENT_INTERVAL_MAX_S*1000);
      next_idle_ms=now_ms+(uint32_t)irand(1800,4200);
    }
    was_playing=playing;
    if (!g_player.active && clip_was_active) clip_finished_ms=now_ms;
    if (!g_player.active) {
      if (playing) start_clip(choose_clip(sound_behaviour),now_ms);
      else if (now_ms>=next_idle_ms) {
        start_clip(choose_clip(B_IDLE),now_ms); next_idle_ms=now_ms+(uint32_t)irand(4200,9000);
      } else if (clip_finished_ms && now_ms-clip_finished_ms>1400) {
        // 表演结束后短暂停留,微动层接管,再轻轻收尾
        start_clip(choose_clip(B_SETTLE),now_ms); clip_finished_ms=0;
        next_idle_ms=now_ms+(uint32_t)irand(1500,3200);
      }
    }
    if (!playing && now_ms>=next_ambient_ms && ambient_count>0) {
      if (queue_random_sound("ambient")) { next_ambient_ms=now_ms+(uint32_t)irand(480000,780000); next_sound_ms=now_ms+10000; }
      else next_ambient_ms=now_ms+30000;
    } else if (!playing && now_ms>=next_sound_ms && animal_count>0) {
      next_sound_ms=now_ms+(queue_random_sound("animal") ? 10000 : 1000);
    }
    Pose pose=update_player(now_ms);
    // 常驻微动层: 呼吸 + 头部凝视漂移 + 尾巴轻摆 + 肌肉微颤
    float life=playing ? 0.55f : 1.0f;
    add_breath(pose,life);
    add_gaze(pose,now_ms,life);
    add_tail_sway(pose,now_ms,life);
    add_tremor(pose,life);
    pose=apply_audio_motion(pose,audio,now_ms);
    output_pose(pose); vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
  }
}

esp_err_t HandleAutoPlay(httpd_req_t *req) {
  if (req->method==HTTP_POST) {
    char buffer[64]={}; httpd_req_recv(req,buffer,sizeof(buffer)-1);
    const char *value=strstr(buffer,"\"enable\":");
    if (value) { value+=9; g_running=atoi(value)!=0; }
    else if (!strstr(buffer,"hard_swing")) g_running=!g_running;
    value=strstr(buffer,"\"hard_swing\":");
    if (value) { value+=13; g_hard_swing=strncmp(value,"true",4)==0 || atoi(value)==1; }
  }
  char response[96];
  snprintf(response,sizeof(response),"{\"autoplay\":%s,\"hard_swing\":%s}",g_running?"true":"false",g_hard_swing?"true":"false");
  httpd_resp_set_type(req,"application/json"); httpd_resp_sendstr(req,response); return ESP_OK;
}
void InitAutoRun() {
  xTaskCreate(auto_run_task,"auto_run",8192,nullptr,2,nullptr);
  ESP_LOGI(TAG,"Living-motion auto-run task created");
}

#endif
