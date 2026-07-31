#include "panda_samples.h"
#include "flash_audio.h"

const char *panda_sound_name(int type) {
    return flash_audio_get_name(type);
}

int panda_sound_duration_ms(int type) {
    return flash_audio_get_duration_ms(type);
}
