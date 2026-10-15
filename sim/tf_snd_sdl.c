#include <SDL.h>
#include <stdio.h>
#include "tf_port.h"

#define AUDIO_DIR  TF_AUDIO_DIR

static SDL_AudioDeviceID s_dev;
static SDL_AudioSpec   s_spec[TF_SFX_COUNT];
static Uint8          *s_buf[TF_SFX_COUNT];
static Uint32          s_len[TF_SFX_COUNT];

void tf_port_audio_init(void)
{
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[snd] SDL audio unavailable: %s\n", SDL_GetError());
        return;
    }

    SDL_AudioSpec want = {0};
    want.freq    = 22050;
    want.format  = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    s_dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (!s_dev) {
        fprintf(stderr, "[snd] cannot open audio device: %s\n", SDL_GetError());
        return;
    }

    char path[256];
    for (int i = 0; i < TF_SFX_COUNT; i++) {
        snprintf(path, sizeof path, "%s%s.wav", AUDIO_DIR, tf_sfx_file(i));
        if (!SDL_LoadWAV(path, &s_spec[i], &s_buf[i], &s_len[i])) {
            fprintf(stderr, "[snd] %s: %s\n", path, SDL_GetError());
            s_buf[i] = NULL;
            continue;
        }
        /* Loaded data must already be in the device format — no conversion here. */
        if (s_spec[i].freq != want.freq || s_spec[i].format != want.format
            || s_spec[i].channels != want.channels) {
            fprintf(stderr, "[snd] %s is %uHz/%uch/fmt %#x, expected 22050Hz/1ch — rerun tools/gen_sfx.py\n",
                    tf_sfx_file(i), s_spec[i].freq, s_spec[i].channels, s_spec[i].format);
            SDL_FreeWAV(s_buf[i]);
            s_buf[i] = NULL;
        }
    }

    SDL_PauseAudioDevice(s_dev, 0);
}

void tf_port_sfx(int id)
{
    if (!s_dev || id < 0 || id >= TF_SFX_COUNT || !s_buf[id]) return;
    SDL_QueueAudio(s_dev, s_buf[id], s_len[id]);
}
