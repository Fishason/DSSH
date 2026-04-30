#include "audio.h"
#include <3ds.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/*
 * Synthesizes a short key-click sample at startup using a sine burst
 * with an exponential-decay envelope.  Plays it through ndsp channel 0
 * on demand.
 *
 * Sample format: 22050 Hz mono PCM16, ~512 samples = ~23 ms.
 *
 * If ndsp init fails (dsp firmware missing on a stripped-down CFW
 * setup, etc.), audio_play_click() is a no-op and the rest of the app
 * works fine.
 */

#define CLICK_RATE 22050
#define CLICK_LEN  512        /* ~23 ms */

static int16_t   *click_buf = NULL;
static ndspWaveBuf click_wave;
static int audio_ok = 0;

void audio_init(void) {
    if (R_FAILED(ndspInit())) return;

    click_buf = (int16_t *)linearAlloc(sizeof(int16_t) * CLICK_LEN);
    if (!click_buf) {
        ndspExit();
        return;
    }

    /* Synthesize a "thock" click:
     *   - first 6 samples = sharp noise transient (the impact)
     *   - rest = 2.7 kHz sine with exponential decay (the resonance)
     * Both shaped by an env that goes 1.0 → 0 over the buffer. */
    uint32_t rng = 0x9E3779B9;
    for (int i = 0; i < CLICK_LEN; i++) {
        float t = (float)i / (float)CLICK_RATE;
        float env = expf(-t * 90.0f);          /* exponential decay */
        float wave;
        if (i < 6) {
            rng = rng * 1664525 + 1013904223;
            float noise = (float)((int)(rng & 0xff) - 128) / 128.0f;
            wave = noise;
        } else {
            wave = sinf(2.0f * 3.14159265f * 2700.0f * t);
        }
        float v = env * wave * 0.45f;          /* 0.45 = comfortable loudness */
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        click_buf[i] = (int16_t)(v * 32000);
    }
    DSP_FlushDataCache(click_buf, sizeof(int16_t) * CLICK_LEN);

    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, (float)CLICK_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);

    /* Modest volume on the front (left+right) channels so the click is
     * subtle, not jarring. */
    float mix[12] = { 0.6f, 0.6f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    ndspChnSetMix(0, mix);

    memset(&click_wave, 0, sizeof(click_wave));
    click_wave.data_pcm16 = click_buf;
    click_wave.nsamples   = CLICK_LEN;
    click_wave.looping    = false;
    click_wave.status     = NDSP_WBUF_DONE;

    audio_ok = 1;
}

void audio_exit(void) {
    if (!audio_ok) return;
    ndspExit();
    if (click_buf) {
        linearFree(click_buf);
        click_buf = NULL;
    }
    audio_ok = 0;
}

void audio_play_click(void) {
    if (!audio_ok || !click_buf) return;
    /* If the previous click is still playing, skip — at typing speed the
     * 23 ms buffer finishes well before the next tap, and overlap would
     * sound clipped. */
    if (click_wave.status == NDSP_WBUF_PLAYING ||
        click_wave.status == NDSP_WBUF_QUEUED) return;
    /* Reset and queue. */
    click_wave.status = NDSP_WBUF_DONE;
    DSP_FlushDataCache(click_buf, sizeof(int16_t) * CLICK_LEN);
    ndspChnWaveBufAdd(0, &click_wave);
}
