#pragma once

/*
 * Tiny audio subsystem: a single key-click sample played on every soft
 * keyboard tap.  Initialization may fail silently (e.g. dsp firmware not
 * present) — audio_play_click() then becomes a safe no-op.
 */

void audio_init(void);
void audio_exit(void);
void audio_play_click(void);
