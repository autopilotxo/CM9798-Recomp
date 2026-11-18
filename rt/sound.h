/* sound.h - CM2's match sounds (sound.c) */
#pragma once
#include <stdint.h>
extern int g_sound;                         /* cm9798.cfg "sound = on | off" (F1, Gameplay: Match sounds On / Off) */
extern int g_sound_volume;                  /* cm9798.cfg "sound_volume" 0-100 (F1: the Volume slider) */
int  sound_available(void);                 /* CM2's six .PCM files found? */
void sound_mix(int16_t *out, int frames, int rate);   /* sdl.c's audio callback: stereo, interleaved */
