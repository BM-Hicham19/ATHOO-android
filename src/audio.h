/* Audio engine: a small General-MIDI-ish synthesizer + WAV sample player + mixer.
 * audio_render() is called from the frontend's audio thread; everything else from the VM thread.
 * The two sides talk through a lock-free command queue, so nothing here blocks. */
#ifndef AUDIO_H
#define AUDIO_H
#include <stdint.h>
#include <stddef.h>

#define AUDIO_RATE 48000
typedef struct Asset Asset;

Asset   *audio_load_midi(const uint8_t *data, size_t n);     /* NULL if not a valid MIDI file */
Asset   *audio_load_wav(const uint8_t *data, size_t n);      /* 16-bit PCM only, NULL otherwise */

/* a Player is the VM-side handle of one MediaPlayer */
typedef struct { int ch; uint32_t gen; } AudioHandle;
void audio_handle_init(AudioHandle *h);
void audio_play(AudioHandle *h, Asset *a, int loop, float vol, uint32_t start_ms);
void audio_stop(AudioHandle *h);
void audio_set_volume(AudioHandle *h, float vol);
int  audio_is_playing(const AudioHandle *h);

void audio_render(int16_t *stereo_out, int frames);          /* frames <= 4096 */
#endif
