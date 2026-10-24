#include "audio.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define NCH 16            /* simultaneous sounds */
#define NVOICE 32         /* synth voices per MIDI player */
#define RING 64
#define MAXFRAMES 4096
#define TWO_PI 6.28318530718f

/* ------------------------------------------------------------------ assets -- */
typedef struct { double t; uint8_t st, a, b; } Ev;            /* t in output samples */
struct Asset {
    int type;                                                /* 1 = PCM, 2 = MIDI */
    int16_t *pcm; int frames, rate, channels;
    Ev *ev; int nev; double length;                          /* MIDI */
};

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t be16(const uint8_t *p) { return p[0] << 8 | p[1]; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }

Asset *audio_load_wav(const uint8_t *d, size_t n) {
    if (n < 44 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return NULL;
    size_t p = 12; int ch = 0, rate = 0, bits = 0, fmt = 0; const uint8_t *data = NULL; size_t dl = 0;
    while (p + 8 <= n) {
        uint32_t sz = le32(d + p + 4);
        if (!memcmp(d + p, "fmt ", 4) && p + 8 + 16 <= n) { fmt = le16(d + p + 8); ch = le16(d + p + 10); rate = le32(d + p + 12); bits = le16(d + p + 22); }
        else if (!memcmp(d + p, "data", 4)) { data = d + p + 8; dl = sz; if (p + 8 + dl > n) dl = n - p - 8; break; }
        p += 8 + sz + (sz & 1);
    }
    if (!data || fmt != 1 || bits != 16 || ch < 1 || ch > 2 || rate < 4000) return NULL;
    Asset *a = calloc(1, sizeof *a); a->type = 1; a->channels = ch; a->rate = rate; a->frames = (int)(dl / (2 * ch));
    a->pcm = malloc((size_t)a->frames * ch * 2 + 2);
    for (int i = 0; i < a->frames * ch; i++) a->pcm[i] = (int16_t)le16(data + 2 * i);
    return a;
}

typedef struct { uint32_t tick, order; uint8_t st, a, b; uint32_t tempo; } RawEv;
static int cmp_raw(const void *x, const void *y) {
    const RawEv *a = x, *b = y;
    if (a->tick != b->tick) return a->tick < b->tick ? -1 : 1;
    return a->order < b->order ? -1 : a->order > b->order;
}
static uint32_t vlq(const uint8_t *d, size_t n, size_t *p) {
    uint32_t v = 0; for (int i = 0; i < 4 && *p < n; i++) { uint8_t b = d[(*p)++]; v = v << 7 | (b & 0x7f); if (!(b & 0x80)) break; } return v;
}
Asset *audio_load_midi(const uint8_t *d, size_t n) {
    if (n < 14 || memcmp(d, "MThd", 4)) return NULL;
    int ntr = be16(d + 10); uint32_t div = be16(d + 12); if (!div || (div & 0x8000)) div = 480;
    size_t p = 8 + be32(d + 4); RawEv *raw = NULL; size_t nraw = 0, cap = 0; uint32_t order = 0;
    for (int t = 0; t < ntr && p + 8 <= n; t++) {
        if (memcmp(d + p, "MTrk", 4)) break;
        size_t len = be32(d + p + 4), q = p + 8, end = q + len; if (end > n) end = n;
        uint32_t tick = 0; uint8_t run = 0;
        while (q < end) {
            tick += vlq(d, end, &q); if (q >= end) break;
            uint8_t s = d[q];
            RawEv e; memset(&e, 0, sizeof e); e.tick = tick; e.order = order++;
            if (s == 0xff) {
                if (q + 1 >= end) break;
                uint8_t ty = d[q + 1]; q += 2; uint32_t l = vlq(d, end, &q);
                if (ty == 0x51 && l == 3 && q + 3 <= end) { e.st = 0xff; e.tempo = d[q] << 16 | d[q + 1] << 8 | d[q + 2]; }
                else e.st = 0;
                q += l;
            } else if (s == 0xf0 || s == 0xf7) { q++; q += vlq(d, end, &q); e.st = 0; }
            else {
                if (s & 0x80) { run = s; q++; }
                uint8_t k = run >> 4; e.st = run;
                if (k == 0xc || k == 0xd) { if (q < end) e.a = d[q++]; }
                else { if (q + 1 < end) { e.a = d[q]; e.b = d[q + 1]; } q += 2; }
            }
            if (e.st) { if (nraw == cap) { cap = cap ? cap * 2 : 1024; raw = realloc(raw, cap * sizeof *raw); } raw[nraw++] = e; }
        }
        p += 8 + len;
    }
    if (!nraw) { free(raw); return NULL; }
    qsort(raw, nraw, sizeof *raw, cmp_raw);
    Asset *a = calloc(1, sizeof *a); a->type = 2; a->ev = malloc(nraw * sizeof(Ev));
    double tempo = 500000, us = 0; uint32_t last = 0; int k = 0;
    for (size_t i = 0; i < nraw; i++) {
        us += (double)(raw[i].tick - last) * tempo / div; last = raw[i].tick;
        if (raw[i].st == 0xff) { tempo = raw[i].tempo; continue; }
        a->ev[k].t = us * AUDIO_RATE / 1e6; a->ev[k].st = raw[i].st; a->ev[k].a = raw[i].a; a->ev[k].b = raw[i].b; k++;
    }
    a->nev = k; a->length = k ? a->ev[k - 1].t + 0.4 * AUDIO_RATE : 0; free(raw);
    return a;
}

/* ---------------------------------------------------------------- synthesis -- */
enum { W_SINE, W_TRI, W_SAW, W_PULSE, W_NOISE };
typedef struct { int wave; float duty, att, dec, sus, rel, gain; } Patch;
static Patch patch_for(int prog) {
    int g = prog >> 3; Patch p = {W_PULSE, 0.5f, 0.01f, 0.1f, 0.8f, 0.1f, 0.4f};
    switch (g) {
        case 0:  p = (Patch){W_TRI, 0, 0.003f, 0.6f, 0.12f, 0.15f, 0.85f}; break;
        case 1:  p = (Patch){W_SINE, 0, 0.001f, 0.35f, 0.0f, 0.2f, 0.8f}; break;
        case 2:  p = (Patch){W_PULSE, 0.5f, 0.01f, 0.05f, 0.8f, 0.08f, 0.4f}; break;
        case 3:  p = (Patch){W_SAW, 0, 0.002f, 0.4f, 0.1f, 0.1f, 0.5f}; break;
        case 4:  p = (Patch){W_TRI, 0, 0.003f, 0.3f, 0.6f, 0.08f, 1.0f}; break;
        case 5:  p = (prog == 45 || prog == 46) ? (Patch){W_TRI, 0, 0.002f, 0.3f, 0.0f, 0.15f, 0.8f} : (Patch){W_SAW, 0, 0.08f, 0.2f, 0.7f, 0.25f, 0.35f}; break;
        case 6:  p = (Patch){W_SAW, 0, 0.1f, 0.2f, 0.7f, 0.3f, 0.35f}; break;
        case 7:  p = (Patch){W_SAW, 0, 0.03f, 0.1f, 0.7f, 0.1f, 0.45f}; break;
        case 8:  p = (Patch){W_PULSE, 0.35f, 0.03f, 0.1f, 0.8f, 0.1f, 0.45f}; break;
        case 9:  p = (Patch){W_SINE, 0, 0.04f, 0.1f, 0.8f, 0.1f, 0.7f}; break;
        case 10: p = (Patch){W_PULSE, 0.25f, 0.005f, 0.1f, 0.8f, 0.08f, 0.42f}; break;
        case 11: p = (Patch){W_TRI, 0, 0.15f, 0.2f, 0.8f, 0.4f, 0.5f}; break;
        case 12: p = (Patch){W_SINE, 0, 0.1f, 0.2f, 0.6f, 0.4f, 0.4f}; break;
        case 13: p = (Patch){W_TRI, 0, 0.002f, 0.3f, 0.0f, 0.15f, 0.7f}; break;
        case 14: p = (Patch){W_NOISE, 0, 0.5f, 0.3f, 0.0f, 0.2f, 0.3f}; break;
        default: p = (Patch){W_NOISE, 0, 0.002f, 0.12f, 0.0f, 0.1f, 0.5f}; break;
    }
    return p;
}
typedef struct {
    int active, ch, note, released, drum, wave, stage; float duty, phase, freq, vel, env, att, dec_k, sus, rel_k, gain;
    float t, tau_k, len; uint32_t age;
} SV;
typedef struct {
    Asset *a; int idx; double pos; int loop; uint32_t clock;
    SV v[NVOICE]; uint8_t prog[16], vol[16], expr[16]; float rng_hp; uint32_t rng;
} MidiPlayer;

static float sinetab[2049]; static int synth_ready;
static void synth_init(void) { if (synth_ready) return; for (int i = 0; i <= 2048; i++) sinetab[i] = sinf(TWO_PI * i / 2048.0f); synth_ready = 1; }
static float fsin(float ph) { float x = ph * 2048.0f; int i = (int)x; float f = x - i; return sinetab[i & 2047] * (1 - f) + sinetab[(i + 1) & 2047] * f; }
static float kcoef(float seconds) { return seconds <= 0 ? 0.0f : expf(-1.0f / (seconds * AUDIO_RATE)); }

static void mp_reset(MidiPlayer *p) {
    memset(p->v, 0, sizeof p->v); p->idx = 0; p->pos = 0;
    for (int c = 0; c < 16; c++) { p->prog[c] = 0; p->vol[c] = 100; p->expr[c] = 127; }
}
static SV *alloc_voice(MidiPlayer *p) {
    SV *best = NULL; for (int i = 0; i < NVOICE; i++) if (!p->v[i].active) return &p->v[i];
    for (int i = 0; i < NVOICE; i++) { SV *v = &p->v[i]; if (!best || (v->released && !best->released) || (v->released == best->released && v->age < best->age)) best = v; }
    return best;
}
static void note_on(MidiPlayer *p, int ch, int note, int vel) {
    SV *v = alloc_voice(p); memset(v, 0, sizeof *v);
    v->active = 1; v->ch = ch; v->note = note; v->vel = vel / 127.0f; v->age = p->clock++;
    if (ch == 9) {                                           /* percussion */
        v->drum = 1; v->wave = W_SINE; v->t = 0;
        float tau, len, f0 = 100, f1 = 100; v->gain = 0.9f;
        if (note == 35 || note == 36) { v->drum = 1; f0 = 170; f1 = 48; tau = 0.14f; len = 0.35f; }
        else if (note == 38 || note == 40 || note == 37 || note == 39) { v->drum = 2; tau = 0.09f; len = 0.25f; f0 = f1 = 190; }
        else if (note == 42 || note == 44) { v->drum = 3; tau = 0.025f; len = 0.1f; v->gain = 0.5f; }
        else if (note == 46) { v->drum = 3; tau = 0.12f; len = 0.35f; v->gain = 0.5f; }
        else if (note >= 41 && note <= 50 && note != 42 && note != 44 && note != 46 && note != 49) { v->drum = 4; f1 = 80 + (note - 41) * 18; f0 = f1 * 1.5f; tau = 0.14f; len = 0.4f; }
        else if (note == 49 || note == 51 || note == 52 || note == 53 || note == 55 || note == 57 || note == 59) { v->drum = 3; tau = 0.5f; len = 1.2f; v->gain = 0.45f; }
        else { v->drum = 3; tau = 0.05f; len = 0.15f; v->gain = 0.5f; }
        v->tau_k = kcoef(tau); v->len = len * AUDIO_RATE; v->freq = f0; v->sus = f1; v->env = 1; v->dec_k = kcoef(0.04f);
        return;
    }
    Patch pt = patch_for(p->prog[ch]);
    v->wave = pt.wave; v->duty = pt.duty; v->gain = pt.gain; v->sus = pt.sus;
    v->freq = 440.0f * powf(2.0f, (note - 69) / 12.0f);
    v->att = pt.att > 0 ? 1.0f / (pt.att * AUDIO_RATE) : 1.0f; v->dec_k = kcoef(pt.dec / 3.0f); v->rel_k = kcoef(pt.rel / 4.0f);
    v->env = 0; v->stage = 0;
    if (p->prog[ch] == 119) { v->att = 1.0f / (0.45f * AUDIO_RATE); }
}
static void note_off(MidiPlayer *p, int ch, int note) {
    for (int i = 0; i < NVOICE; i++) { SV *v = &p->v[i]; if (v->active && !v->drum && v->ch == ch && v->note == note && !v->released) { v->released = 1; v->stage = 2; } }
}
static void all_off(MidiPlayer *p, int ch) { for (int i = 0; i < NVOICE; i++) { SV *v = &p->v[i]; if (v->active && v->ch == ch && !v->drum && !v->released) { v->released = 1; v->stage = 2; } } }
static void mp_event(MidiPlayer *p, const Ev *e) {
    int ch = e->st & 15, k = e->st >> 4;
    switch (k) {
        case 9: if (e->b) note_on(p, ch, e->a, e->b); else note_off(p, ch, e->a); break;
        case 8: note_off(p, ch, e->a); break;
        case 0xb:
            if (e->a == 7) p->vol[ch] = e->b; else if (e->a == 11) p->expr[ch] = e->b;
            else if (e->a == 123 || e->a == 120) all_off(p, ch);
            break;
        case 0xc: p->prog[ch] = e->a; break;
    }
}
static inline float noise(MidiPlayer *p) { p->rng ^= p->rng << 13; p->rng ^= p->rng >> 17; p->rng ^= p->rng << 5; return ((int32_t)p->rng) * (1.0f / 2147483648.0f); }
static float voice_sample(MidiPlayer *p, SV *v) {
    float s;
    if (v->drum) {
        v->t += 1;
        float amp = v->env; v->env *= v->tau_k;
        if (v->t >= v->len || amp < 0.0005f) { v->active = 0; return 0; }
        switch (v->drum) {
            case 1: case 4: { v->freq = v->sus + (v->freq - v->sus) * v->dec_k; v->phase += v->freq / AUDIO_RATE; if (v->phase >= 1) v->phase -= 1; s = fsin(v->phase); break; }
            case 2: { v->phase += 190.0f / AUDIO_RATE; if (v->phase >= 1) v->phase -= 1; s = 0.6f * noise(p) + 0.4f * fsin(v->phase); break; }
            default: { float nz = noise(p); s = nz - p->rng_hp; p->rng_hp = nz * 0.9f + p->rng_hp * 0.1f; break; }
        }
        return s * amp * v->gain * v->vel;
    }
    switch (v->stage) {
        case 0: v->env += v->att; if (v->env >= 1) { v->env = 1; v->stage = 1; } break;
        case 1: v->env = v->sus + (v->env - v->sus) * v->dec_k; break;
        default: v->env *= v->rel_k; if (v->env < 0.0006f) { v->active = 0; return 0; }
    }
    v->phase += v->freq / AUDIO_RATE; if (v->phase >= 1) v->phase -= 1;
    switch (v->wave) {
        case W_SINE: s = fsin(v->phase); break;
        case W_TRI: s = 4 * fabsf(v->phase - 0.5f) - 1; break;
        case W_SAW: s = 2 * v->phase - 1; break;
        case W_PULSE: s = v->phase < v->duty ? 1.0f : -1.0f; break;
        default: s = noise(p); break;
    }
    float cv = (p->vol[v->ch] / 127.0f) * (p->expr[v->ch] / 127.0f);
    return s * v->env * v->gain * v->vel * cv;
}

/* ------------------------------------------------------------------ mixer --- */
typedef struct { Asset *a; int active; uint32_t cur_gen; int loop; float vol; double pos; MidiPlayer *mp; } Chan;
typedef struct { volatile uint32_t req_gen, done_gen; } ChShared;
typedef struct { int type, ch; uint32_t gen; Asset *a; int loop; float vol; uint32_t start_ms; } Cmd;

static Chan chans[NCH]; static MidiPlayer mps[NCH];
static ChShared shared[NCH];
static Cmd ring[RING]; static volatile uint32_t ring_wr, ring_rd;
static uint32_t use_counter; static uint32_t last_use[NCH];

static void post(Cmd c) {
    uint32_t w = ring_wr; if (w - ring_rd >= RING) return;           /* full: drop */
    ring[w % RING] = c; __sync_synchronize(); ring_wr = w + 1;
}
void audio_handle_init(AudioHandle *h) { h->ch = -1; h->gen = 0; }
static int owns(const AudioHandle *h) { return h->ch >= 0 && h->gen && shared[h->ch].req_gen == h->gen; }
void audio_play(AudioHandle *h, Asset *a, int loop, float vol, uint32_t start_ms) {
    if (!a) return;
    int ch = -1;
    if (owns(h)) ch = h->ch;
    else {
        for (int i = 0; i < NCH; i++) if (shared[i].done_gen == shared[i].req_gen && (ch < 0 || last_use[i] < last_use[ch])) ch = i;
        if (ch < 0) for (int i = 0; i < NCH; i++) if (ch < 0 || last_use[i] < last_use[ch]) ch = i;      /* steal oldest */
    }
    uint32_t gen = shared[ch].req_gen + 1; if (!gen) gen = 1;
    shared[ch].req_gen = gen; last_use[ch] = ++use_counter; h->ch = ch; h->gen = gen;
    Cmd c = {1, ch, gen, a, loop, vol, start_ms}; post(c);
}
void audio_stop(AudioHandle *h) { if (!owns(h)) return; Cmd c = {2, h->ch, h->gen, NULL, 0, 0, 0}; post(c); }
void audio_set_volume(AudioHandle *h, float vol) { if (!owns(h)) return; Cmd c = {3, h->ch, h->gen, NULL, 0, vol, 0}; post(c); }
int audio_is_playing(const AudioHandle *h) { return owns(h) && shared[h->ch].done_gen != h->gen; }

static void apply(const Cmd *c) {
    Chan *ch = &chans[c->ch];
    if (c->type == 1) {
        ch->a = c->a; ch->cur_gen = c->gen; ch->loop = c->loop; ch->vol = c->vol; ch->active = 1;
        if (c->a->type == 1) ch->pos = (double)c->start_ms * c->a->rate / 1000.0;
        else {
            MidiPlayer *mp = &mps[c->ch]; synth_init(); mp->a = c->a; mp_reset(mp); mp->rng = 2463534242u; ch->mp = mp; ch->pos = 0;
            double target = (double)c->start_ms * AUDIO_RATE / 1000.0;           /* seek: skip events before target */
            while (mp->idx < c->a->nev && c->a->ev[mp->idx].t < target) { const Ev *e = &c->a->ev[mp->idx]; if ((e->st >> 4) != 9 && (e->st >> 4) != 8) mp_event(mp, e); mp->idx++; }
            ch->pos = target;
        }
    } else if (ch->active && ch->cur_gen == c->gen) {
        if (c->type == 2) { ch->active = 0; shared[c->ch].done_gen = c->gen; }
        else if (c->type == 3) ch->vol = c->vol;
    }
}
static float acc[MAXFRAMES * 2];
void audio_render(int16_t *out, int frames) {
    if (frames > MAXFRAMES) frames = MAXFRAMES;
    uint32_t r = ring_rd; __sync_synchronize();
    while (r != ring_wr) { apply(&ring[r % RING]); r++; }
    ring_rd = r;
    memset(acc, 0, sizeof(float) * 2 * frames);
    for (int c = 0; c < NCH; c++) {
        Chan *ch = &chans[c]; if (!ch->active) continue;
        float vol = ch->vol; if (vol > 1) vol = 1; if (vol < 0) vol = 0;
        Asset *a = ch->a;
        if (a->type == 1) {
            double step = (double)a->rate / AUDIO_RATE; float g = vol * 0.9f;
            for (int i = 0; i < frames; i++) {
                if (ch->pos >= a->frames) { if (ch->loop && a->frames) ch->pos = fmod(ch->pos, a->frames); else { ch->active = 0; shared[c].done_gen = ch->cur_gen; break; } }
                int i0 = (int)ch->pos, i1 = i0 + 1 < a->frames ? i0 + 1 : i0; float f = (float)(ch->pos - i0), l, rr;
                if (a->channels == 1) { l = rr = (a->pcm[i0] * (1 - f) + a->pcm[i1] * f) * (1.0f / 32768); }
                else { l = (a->pcm[2 * i0] * (1 - f) + a->pcm[2 * i1] * f) * (1.0f / 32768); rr = (a->pcm[2 * i0 + 1] * (1 - f) + a->pcm[2 * i1 + 1] * f) * (1.0f / 32768); }
                acc[2 * i] += l * g; acc[2 * i + 1] += rr * g; ch->pos += step;
            }
        } else {
            MidiPlayer *mp = ch->mp; float g = vol * 0.45f;
            for (int i = 0; i < frames; i++) {
                while (mp->idx < a->nev && a->ev[mp->idx].t <= ch->pos) mp_event(mp, &a->ev[mp->idx++]);
                float s = 0; for (int k = 0; k < NVOICE; k++) if (mp->v[k].active) s += voice_sample(mp, &mp->v[k]);
                acc[2 * i] += s * g; acc[2 * i + 1] += s * g; ch->pos += 1;
                if (ch->pos >= a->length) {
                    if (ch->loop) { mp_reset(mp); ch->pos = 0; }
                    else { ch->active = 0; shared[c].done_gen = ch->cur_gen; break; }
                }
            }
        }
    }
    for (int i = 0; i < frames * 2; i++) { float x = acc[i]; if (x > 0.98f) x = 0.98f; if (x < -0.98f) x = -0.98f; out[i] = (int16_t)(x * 32767.0f); }
}
