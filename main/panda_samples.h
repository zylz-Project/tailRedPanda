#pragma once

// Sound type enum — order matches alphabetical sort of .opus files in opus_audio/.
// TOC index = enum value.  Display names, durations, and all file metadata are read
// from TOC on external SPI Flash at runtime — no hardcoded filenames in firmware.
typedef enum {
    PANDA_SOUND_冯梦舟__虫鸣_鸟叫_下雨_流水声_大自然的声音 = 0,
    PANDA_SOUND_国宝熊猫叫声音效_爱给网_AIGEI_COM = 1,
    PANDA_SOUND_熊猫叫声_爱给网_AIGEI_COM = 2,
    PANDA_SOUND_熊猫吃竹子的声音 = 3,
    PANDA_SOUND_熊猫宝宝嘤嘤叫声音效_爱给网_AIGEI_COM = 4,
    PANDA_SOUND_熊猫成年 = 5,
    PANDA_SOUND_熊猫撒娇声音 = 6,
    PANDA_SOUND_类似熊猫叫声_CAT_MEOWS_FROM_A_CLOSE_PERSPECTIVE = 7,
    PANDA_SOUND_类似熊猫声音 = 8,
    PANDA_SOUND_COUNT
} panda_sound_type_t;

// Read from SPI Flash TOC at runtime.
// Returns "???" if TOC not loaded or index out of range.
const char *panda_sound_name(int type);

// Read from SPI Flash TOC at runtime (estimated from file size).
// Returns 0 if TOC not loaded or index out of range.
int panda_sound_duration_ms(int type);
