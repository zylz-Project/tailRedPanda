#pragma once

#include <cstdint>

struct AudioMotionData {
  bool playing;
  float level;       // 0..1, smoothed programme loudness
  float attack;      // 0..1, short onset energy
  uint32_t elapsed_ms;
  uint32_t duration_ms;
  int sound_index;
};

void InitAudio();
bool PlayPandaSound(int type);  // non-blocking: returns immediately, plays in background
bool IsAudioPlaying();
void FlushAudioQueue();         // clear all pending sounds from queue
AudioMotionData GetAudioMotionData();
