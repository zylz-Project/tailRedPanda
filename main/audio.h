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
void AudioStopCurrent();        // interrupt the currently playing sound (if any)
void AudioPlayChime(bool ascending);  // 合成叮咚提示音: true=上扬, false=下扬
AudioMotionData GetAudioMotionData();

// === LLM chat audio (48kHz mono duplex over the ES8311 codec) ===
// Reads `samples` mono int16 samples captured from the codec mic @ 48kHz.
// Returns bytes actually read (0 on failure).
int  AudioReadMic48k(int16_t *buf, int samples);
// Writes `samples` mono int16 samples @ 48kHz to the speaker (mutex-protected).
void AudioWritePcm48k(const int16_t *pcm, int samples);
// Plays the short two-tone prompt used when realtime chat is actually ready.
void AudioPlayChatReadyTone();
