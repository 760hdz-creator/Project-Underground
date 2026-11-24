/* DSOUND (XDK 5659) HLE at the C-API level (ported from Project Gauntlet's XDK 4242 version).
 *
 * The game calls DSOUND only through the C wrappers listed in recomp/hle.txt (IDirectSound_*,
 * IDirectSoundBuffer_*, DirectSoundCreate*, DirectSoundDoWork) and through the vtable of the
 * IDirectSoundStream objects it creates (the WMV player). Every other DSOUND function is lifted as
 * an HLE stub that fails loudly (recomp/lift.py), so the library never runs on our fake objects.
 *
 * Objects are small guest allocations; the guest only passes their addresses back to us, except the
 * stream, whose vtable points at HLE sentinels (HSENTINEL_BASE + slot*4, dispatched from
 * x86_call_indirect). Refcounts are cosmetic and objects are never freed.
 *
 * A host mixer thread owns the play cursors: buffers advance only as audio is mixed, so status and
 * cursor queries move in real time. Output is waveOut 48 kHz stereo s16; without a device (or with
 * UG_NOAUDIO=1) the mixer still runs, paced by Sleep, and discards its output. UG_WAVDUMP=<file>
 * writes the mixed output as raw s16 stereo 48 kHz. UG_DSLOG=1 traces IDirectSoundBuffer calls.
 */
#include <windows.h>
#include <mmsystem.h>
#include <math.h>
#include "rt.h"

#define OUT_RATE    48000
#define OUT_FRAMES  960          /* 20 ms per waveOut buffer */
#define OUT_BUFS    4

#define MAX_BUFFERS 512
#define DSBSTATUS_PLAYING 0x1
#define DSBSTATUS_LOOPING 0x4
#define DSBPLAY_LOOPING   0x1
#define DSBPLAY_FROMSTART 0x2
#define WAVE_FORMAT_PCM_       1
#define WAVE_FORMAT_XBOX_ADPCM 0x69
#define XMEDIAPACKET_STATUS_SUCCESS 0
#define XMEDIAPACKET_STATUS_PENDING 0x8000000Au   /* E_PENDING: THUG's streamer polls packet status for it */
#define XMEDIAPACKET_STATUS_FLUSHED 0x80004004u   /* E_ABORT */

typedef struct {
    uint16_t tag, channels, block_align, bits;
    uint32_t rate;
} Fmt;

typedef struct {
    uint32_t guest;              /* object address the guest holds */
    Fmt fmt;
    uint32_t data, bytes;        /* guest audio data */
    uint32_t owned;              /* buffer we allocated at create (freed on Release); 0 if none */
    uint32_t freq;               /* 0 = fmt.rate */
    int32_t volume;              /* hundredths of a dB, <= 0 */
    int32_t bin_vol[2];          /* mix bins 0/1 (front left/right), hundredths of a dB; 0 until set */
    int playing, looping;
    double pos;                  /* in frames */
    int32_t adpcm_block;         /* decoded block cache */
    int16_t adpcm[2 * 65];
} Voice;

static CRITICAL_SECTION g_lock;
static Voice g_voices[MAX_BUFFERS];
static int g_nvoices;
static uint32_t g_ds_obj;        /* the IDirectSound the game holds */

/* ---- streams (music, cutscene speech; Xbox ADPCM is decoded to PCM as packets arrive). A packet completes when
 * the mixer has played past its end, not when it is submitted: the game queues dwMaxAttachedPackets packets and
 * refills each one as it completes (completed at once, it streamed whole tracks into the FIFO in seconds). ---- */
#define FIFO_BYTES (4u << 20)
#define STREAM_QUEUE 64
typedef struct { uint32_t end, size, pdone, pstat, evh, ctx; } Packet;
typedef struct {
    uint32_t guest;
    int adpcm;                   /* source is Xbox ADPCM (fmt below is the decoded PCM) */
    uint32_t maxpk;
    Packet q[STREAM_QUEUE];      /* attached packets, oldest first */
    int qh, qn;
    Fmt fmt;
    int paused;
    int32_t volume;              /* hundredths of a dB */
    uint32_t callback, context;  /* DSSTREAMDESC lpfnCallback / lpvContext: run on packet completion */
    uint8_t *fifo;               /* raw packet bytes in fmt */
    uint32_t rd, wr;             /* byte counters, wrap at FIFO_BYTES */
    double frac;                 /* fractional source frame */
} Stream;
#define MAX_STREAMS 16
static Stream g_streams[MAX_STREAMS];
static int g_nstreams;
static uint32_t g_stream_vtbl;

#define STREAM_SLOTS 32

/* ------------------------------------------------------------------ helpers */

static void ret_std(X86Context *c, int nargs, uint32_t eax) {
    c->eax = eax;
    c->esp += 4 + 4 * nargs;
}

static void read_fmt(uint32_t wfx, Fmt *f) {
    if (!wfx) { f->tag = WAVE_FORMAT_PCM_; f->channels = 2; f->rate = 48000; f->bits = 16; f->block_align = 4; return; }
    f->tag = RD16(wfx);
    f->channels = RD16(wfx + 2);
    f->rate = RD32(wfx + 4);
    f->block_align = RD16(wfx + 12);
    f->bits = RD16(wfx + 14);
    if (f->channels < 1) f->channels = 1;
    if (f->channels > 2) f->channels = 2;   /* 6-channel would need mix bins: not used by the game so far */
    if (!f->block_align) f->block_align = (uint16_t)(f->channels * f->bits / 8);
}

static uint32_t fmt_frames(const Fmt *f, uint32_t bytes) {
    if (f->tag == WAVE_FORMAT_XBOX_ADPCM) return bytes / (36u * f->channels) * 64u;
    return f->block_align ? bytes / f->block_align : 0;
}

static uint32_t frames_to_bytes(const Fmt *f, uint32_t frames) {
    if (f->tag == WAVE_FORMAT_XBOX_ADPCM) return frames / 64u * 36u * f->channels;
    return frames * f->block_align;
}

static Voice *voice_of(uint32_t guest) {
    for (int i = 0; i < g_nvoices; i++)
        if (g_voices[i].guest == guest) return &g_voices[i];
    rt_fatal("DSOUND: unknown buffer object %08X", guest);
}

/* Xbox ADPCM = IMA ADPCM, 36-byte blocks per channel: 4-byte header (sample, step index) then
 * 64 nibble samples interleaved in 4-byte words per channel. The header sample is not output
 * (64 frames per block), matching DSOUND's own frame accounting. */
static const int ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const int ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767 };

static void adpcm_decode_block(uint32_t src, int ch, int16_t *out /* [64*ch] interleaved */) {
    for (int c = 0; c < ch; c++) {
        int pred = (int16_t)RD16(src + 4 * c);
        int idx = RD8(src + 4 * c + 2);
        if (idx > 88) idx = 88;
        int n = 0;
        for (int word = 0; word < 8; word++) {
            uint32_t w = RD32(src + 4 * ch + (word * ch + c) * 4);
            for (int k = 0; k < 8; k++, w >>= 4) {
                int nib = w & 15, step = ima_step[idx];
                int diff = step >> 3;
                if (nib & 1) diff += step >> 2;
                if (nib & 2) diff += step >> 1;
                if (nib & 4) diff += step;
                pred += (nib & 8) ? -diff : diff;
                if (pred > 32767) pred = 32767;
                if (pred < -32768) pred = -32768;
                idx += ima_index[nib];
                if (idx < 0) idx = 0;
                if (idx > 88) idx = 88;
                out[n++ * ch + c] = (int16_t)pred;
            }
        }
    }
}

/* One source frame as floats in [-1, 1]. */
static void voice_frame(Voice *v, uint32_t frame, float *l, float *r) {
    const Fmt *f = &v->fmt;
    float a, b;
    if (f->tag == WAVE_FORMAT_XBOX_ADPCM) {
        int32_t blk = (int32_t)(frame / 64);
        if (blk != v->adpcm_block) {
            adpcm_decode_block(v->data + (uint32_t)blk * 36u * f->channels, f->channels, v->adpcm);
            v->adpcm_block = blk;
        }
        int k = (int)(frame % 64) * f->channels;
        a = v->adpcm[k] / 32768.0f;
        b = f->channels == 2 ? v->adpcm[k + 1] / 32768.0f : a;
    } else {
        uint32_t p = v->data + frame * f->block_align;
        if (f->bits == 8) {
            a = ((int)RD8(p) - 128) / 128.0f;
            b = f->channels == 2 ? ((int)RD8(p + 1) - 128) / 128.0f : a;
        } else {
            a = (int16_t)RD16(p) / 32768.0f;
            b = f->channels == 2 ? (int16_t)RD16(p + 2) / 32768.0f : a;
        }
    }
    *l = a; *r = b;
}

static float db_gain(int32_t vol) {
    if (vol >= 0) return 1.0f;
    if (vol <= -10000) return 0.0f;
    return powf(10.0f, vol / 2000.0f);
}

/* ------------------------------------------------------------------ mixer */

static void mix_voice(Voice *v, float *out, int n) {
    uint32_t frames = fmt_frames(&v->fmt, v->bytes);
    if (!v->data || !frames) { v->playing = 0; return; }
    uint32_t rate = v->freq ? v->freq : v->fmt.rate;
    double step = (double)rate / OUT_RATE;
    float gl = db_gain(v->volume + v->bin_vol[0]), gr = db_gain(v->volume + v->bin_vol[1]);
    for (int i = 0; i < n; i++) {
        if (v->pos >= frames) {
            if (!v->looping) { v->playing = 0; v->pos = 0; return; }
            v->pos = fmod(v->pos, frames);
        }
        float l, r, l1, r1;   /* linear interpolation between frame k and k+1 (wraps when looping) */
        uint32_t k = (uint32_t)v->pos, k1 = k + 1 < frames ? k + 1 : v->looping ? 0 : k;
        float t = (float)(v->pos - k);
        voice_frame(v, k1, &l1, &r1);
        voice_frame(v, k, &l, &r);
        l += (l1 - l) * t; r += (r1 - r) * t;
        out[2 * i] += l * gl;
        out[2 * i + 1] += r * gr;
        v->pos += step;
    }
}

static void mix_stream(Stream *s, float *out, int n) {
    if (!s->guest || s->paused || !s->fifo) return;
    float g = db_gain(s->volume);
    const Fmt *f = &s->fmt;
    if (f->tag != WAVE_FORMAT_PCM_ || !f->block_align) return;
    double step = (double)f->rate / OUT_RATE;
    for (int i = 0; i < n; i++) {
        if (s->wr - s->rd < f->block_align) return;   /* underrun */
        const uint8_t *p = s->fifo + (s->rd % FIFO_BYTES);
        float l, r;
        if (f->bits == 8) { l = (p[0] - 128) / 128.0f; r = f->channels == 2 ? (p[1] - 128) / 128.0f : l; }
        else {
            l = *(const int16_t *)p / 32768.0f;
            r = f->channels == 2 ? *(const int16_t *)(p + 2) / 32768.0f : l;
        }
        if (s->wr - s->rd >= 2u * f->block_align) {   /* linear interpolation with the next frame */
            const uint8_t *q = s->fifo + ((s->rd + f->block_align) % FIFO_BYTES);
            float l1, r1, t = (float)s->frac;
            if (f->bits == 8) { l1 = (q[0] - 128) / 128.0f; r1 = f->channels == 2 ? (q[1] - 128) / 128.0f : l1; }
            else {
                l1 = *(const int16_t *)q / 32768.0f;
                r1 = f->channels == 2 ? *(const int16_t *)(q + 2) / 32768.0f : l1;
            }
            l += (l1 - l) * t; r += (r1 - r) * t;
        }
        out[2 * i] += l * g;
        out[2 * i + 1] += r * g;
        s->frac += step;
        while (s->frac >= 1.0) { s->frac -= 1.0; s->rd += f->block_align; }
    }
}

static void mix(int16_t *dst, int n) {
    static float acc[2 * OUT_FRAMES];
    memset(acc, 0, sizeof(float) * 2 * n);
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_nvoices; i++)
        if (g_voices[i].playing) mix_voice(&g_voices[i], acc, n);
    for (int i = 0; i < g_nstreams; i++) mix_stream(&g_streams[i], acc, n);
    LeaveCriticalSection(&g_lock);
    for (int i = 0; i < 2 * n; i++) {
        float x = acc[i] * 32767.0f;
        dst[i] = (int16_t)(x > 32767.0f ? 32767 : x < -32768.0f ? -32768 : x);
    }
    /* UG_WAVDUMP=<file>: raw s16 stereo 48 kHz copy of the mixer output, for checking audio offline */
    static FILE *dump;
    static int dump_init;
    if (!dump_init++) { const char *p = getenv("UG_WAVDUMP"); if (p && *p) dump = fopen(p, "wb"); }
    if (dump) fwrite(dst, 4, (size_t)n, dump);
}

#ifndef _WIN32
/* Linux: SDL3 audio stream, kept 3 buffers (60 ms) ahead like the waveOut queue. */
#include <SDL3/SDL.h>
static DWORD WINAPI mixer_thread(void *arg) {
    (void)arg;
    static int16_t pcm[2 * OUT_FRAMES];
    SDL_AudioStream *st = NULL;
    const char *no = getenv("UG_NOAUDIO");
    if (!(no && *no == '1') && SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_AudioSpec spec = {SDL_AUDIO_S16LE, 2, OUT_RATE};
        st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (st) SDL_ResumeAudioStreamDevice(st);
    }
    LOG("[dsound] mixer: %s\n", st ? "SDL audio 48 kHz" : "no output device (silent, paced)");
    if (!st) {
        for (;;) { mix(pcm, OUT_FRAMES); Sleep(OUT_FRAMES * 1000 / OUT_RATE); }
    }
    for (;;) {
        while (SDL_GetAudioStreamQueued(st) < OUT_BUFS * (int)sizeof pcm) {
            mix(pcm, OUT_FRAMES);
            SDL_PutAudioStreamData(st, pcm, sizeof pcm);
        }
        Sleep(5);
    }
}
#else
static DWORD WINAPI mixer_thread(void *arg) {
    (void)arg;
    static int16_t pcm[OUT_BUFS][2 * OUT_FRAMES];
    HWAVEOUT wo = NULL;
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    const char *no = getenv("UG_NOAUDIO");
    if (!(no && *no == '1')) {
        WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 2, OUT_RATE, OUT_RATE * 4, 4, 16, 0 };
        if (waveOutOpen(&wo, WAVE_MAPPER, &wf, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) wo = NULL;
    }
    LOG("[dsound] mixer: %s\n", wo ? "waveOut 48 kHz" : "no output device (silent, paced)");
    if (!wo) {
        for (;;) { mix(pcm[0], OUT_FRAMES); Sleep(OUT_FRAMES * 1000 / OUT_RATE); }
    }
    WAVEHDR hdr[OUT_BUFS];
    memset(hdr, 0, sizeof hdr);
    for (int i = 0; i < OUT_BUFS; i++) {
        hdr[i].lpData = (LPSTR)pcm[i];
        hdr[i].dwBufferLength = sizeof pcm[i];
        waveOutPrepareHeader(wo, &hdr[i], sizeof hdr[i]);
        mix(pcm[i], OUT_FRAMES);
        waveOutWrite(wo, &hdr[i], sizeof hdr[i]);
    }
    for (;;) {
        WaitForSingleObject(ev, 100);
        for (int i = 0; i < OUT_BUFS; i++) {
            if (!(hdr[i].dwFlags & WHDR_DONE)) continue;
            mix(pcm[i], OUT_FRAMES);
            waveOutWrite(wo, &hdr[i], sizeof hdr[i]);
        }
    }
}
#endif


static void dsound_init(void) {
    static int done;
    if (done++) return;
    InitializeCriticalSection(&g_lock);
    /* stream vtable: one sentinel per slot */
    g_stream_vtbl = karea_alloc(4 * STREAM_SLOTS);
    for (int i = 0; i < STREAM_SLOTS; i++) WR32(g_stream_vtbl + 4 * i, HSENTINEL_BASE + 4 * i);
    CreateThread(NULL, 0, mixer_thread, NULL, 0, NULL);
}

static Stream *stream_of(uint32_t guest) {
    for (int i = 0; i < g_nstreams; i++)
        if (guest && g_streams[i].guest == guest) return &g_streams[i];
    return NULL;
}

/* ------------------------------------------------------------------ stream vtable (XMediaObject) */

/* Completes the oldest attached packet with `status`: size done, status word, event, callback (guest thread only). */
static void stream_complete_head(X86Context *c, Stream *s, uint32_t status) {
    EnterCriticalSection(&g_lock);
    if (!s->qn) { LeaveCriticalSection(&g_lock); return; }
    Packet p = s->q[s->qh];
    s->qh = (s->qh + 1) % STREAM_QUEUE;
    s->qn--;
    uint32_t cb = s->callback, cctx = s->context;
    LeaveCriticalSection(&g_lock);
    if (p.pdone) WR32(p.pdone, status == XMEDIAPACKET_STATUS_SUCCESS ? p.size : 0);
    if (p.pstat) WR32(p.pstat, status);
    if (p.evh) { uint32_t ev = handle_object(p.evh); if (ev) WR32(ev + 4, 1); }
    if (cb) {   /* VOID CALLBACK (LPVOID pStreamContext, LPVOID pPacketContext, DWORD dwStatus) */
        uint32_t args[3] = {cctx, p.ctx, status};
        X86Context sub = *c;
        sub.esp = c->esp - 64;
        guest_call(&sub, cb, 3, args);
    }
}
/* every packet the mixer has played past */
static void stream_complete(X86Context *c, Stream *s) {
    for (;;) {
        EnterCriticalSection(&g_lock);
        int done = s->qn && (int32_t)(s->rd - s->q[s->qh].end) >= 0;
        LeaveCriticalSection(&g_lock);
        if (!done) return;
        stream_complete_head(c, s, XMEDIAPACKET_STATUS_SUCCESS);
    }
}
static void stream_flush(X86Context *c, Stream *s) {
    EnterCriticalSection(&g_lock);
    s->rd = s->wr;
    LeaveCriticalSection(&g_lock);
    while (s->qn) stream_complete_head(c, s, XMEDIAPACKET_STATUS_FLUSHED);
}
static void streams_poll(X86Context *c) {
    for (int i = 0; i < g_nstreams; i++) if (g_streams[i].guest) stream_complete(c, &g_streams[i]);
}

int dsound_vcall(X86Context *c, uint32_t va) {
    if (va < HSENTINEL_BASE || va >= HSENTINEL_BASE + 4 * STREAM_SLOTS || (va & 3)) return 0;
    int slot = (int)(va - HSENTINEL_BASE) / 4;
    sched_maybe_yield();
    Stream *s = stream_of(ARG(0));
    if (!s) rt_fatal("DSOUND stream method %d on unknown object %08X", slot, ARG(0));
    switch (slot) {
    case 0: /* ULONG AddRef(this) */
        ret_std(c, 1, 1);
        return 1;
    case 1: /* ULONG Release(this): pending packets complete as flushed, the slot is reused (one stream per track) */
        stream_flush(c, s);
        EnterCriticalSection(&g_lock);
        free(s->fifo);
        s->fifo = NULL;
        s->guest = 0;
        LeaveCriticalSection(&g_lock);
        ret_std(c, 1, 0);
        return 1;
    case 2: { /* HRESULT GetInfo(this, XMEDIAINFO *): dwFlags, dwInputSize, dwOutputSize, dwMaxLookahead */
        uint32_t p = ARG(1);
        if (p) { WR32(p, 0); WR32(p + 4, s->fmt.block_align); WR32(p + 8, 0); WR32(p + 12, 0); }
        ret_std(c, 2, 0);
        return 1;
    }
    case 3: /* HRESULT GetStatus(this, DWORD *): XMO_STATUSF_ACCEPT_INPUT_DATA while a packet slot is free */
        stream_complete(c, s);
        if (ARG(1)) WR32(ARG(1), s->qn < (int)(s->maxpk ? s->maxpk : 8) && s->qn < STREAM_QUEUE ? 1 : 0);
        ret_std(c, 2, 0);
        return 1;
    case 4: { /* HRESULT Process(this, XMEDIAPACKET *in, XMEDIAPACKET *out): queued, completes once played */
        uint32_t pk = ARG(1);
        stream_complete(c, s);
        if (pk) {
            uint32_t buf = RD32(pk), size = RD32(pk + 4), pdone = RD32(pk + 8), pstat = RD32(pk + 12);
            uint32_t evh = RD32(pk + 16), pctx = RD32(pk + 24);
            if (s->qn == STREAM_QUEUE) stream_complete_head(c, s, XMEDIAPACKET_STATUS_SUCCESS);   /* cannot happen */
            EnterCriticalSection(&g_lock);
            if (s->adpcm) {   /* 36-byte blocks per channel -> 64 s16 frames */
                uint32_t bb = 36u * s->fmt.channels, nb = size / bb;
                int16_t pcm[64 * 2];
                for (uint32_t b = 0; b < nb; b++) {
                    adpcm_decode_block(buf + b * bb, s->fmt.channels, pcm);
                    uint32_t n = 64u * s->fmt.channels * 2;
                    if (s->wr - s->rd + n > FIFO_BYTES) s->rd = s->wr + n - FIFO_BYTES;   /* overflow: drop oldest */
                    for (uint32_t i = 0; i < n; i++) s->fifo[(s->wr + i) % FIFO_BYTES] = ((uint8_t *)pcm)[i];
                    s->wr += n;
                }
            } else {
                if (s->wr - s->rd + size > FIFO_BYTES) s->rd = s->wr + size - FIFO_BYTES;
                for (uint32_t i = 0; i < size; i++) s->fifo[(s->wr + i) % FIFO_BYTES] = RD8(buf + i);
                s->wr += size;
            }
            Packet *p = &s->q[(s->qh + s->qn) % STREAM_QUEUE];
            p->end = s->wr; p->size = size; p->pdone = pdone; p->pstat = pstat; p->evh = evh; p->ctx = pctx;
            s->qn++;
            LeaveCriticalSection(&g_lock);
            if (pdone) WR32(pdone, 0);
            if (pstat) WR32(pstat, XMEDIAPACKET_STATUS_PENDING);
        }
        ret_std(c, 3, 0);
        return 1;
    }
    case 5: /* HRESULT Discontinuity(this) */
        ret_std(c, 1, 0);
        return 1;
    case 6: /* HRESULT Flush(this) */
        stream_flush(c, s);
        ret_std(c, 1, 0);
        return 1;
    default:
        rt_fatal("DSOUND stream vtable slot %d (+0x%X) not implemented (return %08X)", slot, slot * 4, RD32(c->esp));
    }
}

/* ------------------------------------------------------------------ C API (addresses: tools/oovpa_scan.py) */

static uint32_t create_buffer(uint32_t d) {
    /* DSBUFFERDESC: dwSize, dwFlags, dwBufferBytes, lpwfxFormat, lpMixBins, dwInputMixBin */
    uint32_t flags = RD32(d + 4), bytes = RD32(d + 8), wfx = RD32(d + 12);
    EnterCriticalSection(&g_lock);
    int slot = 0;
    while (slot < g_nvoices && g_voices[slot].guest) slot++;   /* reuse a released slot */
    if (slot >= MAX_BUFFERS) rt_fatal("DSOUND: more than %d live buffers", MAX_BUFFERS);
    Voice *v = &g_voices[slot];
    memset(v, 0, sizeof *v);
    v->guest = mem_alloc(0x40, 16);
    read_fmt(wfx, &v->fmt);
    v->adpcm_block = -1;
    if (bytes) { v->data = v->owned = mem_alloc(bytes, 16); v->bytes = bytes; }
    if (slot == g_nvoices) g_nvoices++;
    LeaveCriticalSection(&g_lock);
    if (g_log_kernel || g_nvoices <= 4)
        LOG("[dsound] CreateSoundBuffer flags %X bytes %X fmt %X ch %u rate %u bits %u -> %08X\n", flags, bytes,
            v->fmt.tag, v->fmt.channels, v->fmt.rate, v->fmt.bits, v->guest);
    return v->guest;
}

static uint32_t create_stream(uint32_t d) {
    /* DSSTREAMDESC: dwFlags, dwMaxAttachedPackets, lpwfxFormat, lpfnCallback, lpvContext, lpMixBins */
    uint32_t flags = RD32(d), maxpk = RD32(d + 4), wfx = RD32(d + 8);
    EnterCriticalSection(&g_lock);
    int slot = 0;
    while (slot < g_nstreams && g_streams[slot].guest) slot++;   /* released streams are reused (one per track) */
    if (slot >= MAX_STREAMS) rt_fatal("DSOUND: more than %d live streams", MAX_STREAMS);
    Stream *s = &g_streams[slot];
    memset(s, 0, sizeof *s);
    s->guest = mem_alloc(0x40, 16);
    WR32(s->guest, g_stream_vtbl);
    s->fifo = malloc(FIFO_BYTES);
    s->callback = RD32(d + 12);
    s->context = RD32(d + 16);
    read_fmt(wfx, &s->fmt);
    s->maxpk = maxpk;
    if (s->fmt.tag == WAVE_FORMAT_XBOX_ADPCM) {   /* decoded to s16 PCM in Process */
        s->adpcm = 1;
        s->fmt.tag = WAVE_FORMAT_PCM_; s->fmt.bits = 16; s->fmt.block_align = (uint16_t)(2 * s->fmt.channels);
    }
    if (slot == g_nstreams) g_nstreams++;
    LeaveCriticalSection(&g_lock);
    LOG("[dsound] CreateStream flags %X packets %u fmt %X ch %u rate %u bits %u cb %08X -> %08X\n", flags, maxpk,
        s->fmt.tag, s->fmt.channels, s->fmt.rate, s->fmt.bits, s->callback, s->guest);
    return s->guest;
}

int dsound_hle(X86Context *c, uint32_t va) {
    switch (va) {
    case 0x1C087D: /* HRESULT DirectSoundCreate(LPGUID, LPDIRECTSOUND *, LPUNKNOWN) */
        dsound_init();
        if (!g_ds_obj) g_ds_obj = mem_alloc(16, 16);
        WR32(ARG(1), g_ds_obj);
        LOG("[dsound] DirectSoundCreate -> %08X\n", g_ds_obj);
        ret_std(c, 3, 0);
        return 1;
    case 0x1BE541: /* ULONG IDirectSound_Release(this) */
        ret_std(c, 1, 1);
        return 1;
    case 0x1BE55C: { /* ULONG IDirectSoundBuffer_Release(this): the game never calls AddRef, so this frees */
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < g_nvoices; i++) {
            Voice *v = &g_voices[i];
            if (v->guest != ARG(0)) continue;
            mem_free(v->guest);
            if (v->owned) mem_free(v->owned);
            memset(v, 0, sizeof *v);
            break;
        }
        LeaveCriticalSection(&g_lock);
        ret_std(c, 1, 0);
        return 1;
    }
    case 0x1BF5E8: /* VOID DirectSoundDoWork(void): stream packets the mixer has played complete here */
        streams_poll(c);
        c->esp += 4;
        return 1;
    case 0x1BF482: { /* HRESULT IDirectSound_DownloadEffectsImage(this, LPCVOID image, DWORD size,
                      *   LPCDSEFFECTIMAGELOC, LPDSEFFECTIMAGEDESC *): no DSP; an empty image description */
        static uint32_t desc;
        if (!desc) desc = mem_alloc(64, 16);
        if (ARG(4)) WR32(ARG(4), desc);
        ret_std(c, 5, 0);
        return 1;
    }
    case 0x1C06B7: /* HRESULT IDirectSound_SetI3DL2Listener(this, LPCDSI3DL2LISTENER, DWORD apply): no reverb */
        ret_std(c, 3, 0);
        return 1;
    case 0x1C0693: /* HRESULT IDirectSound_CreateSoundBuffer(this, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER *, LPUNKNOWN) */
        dsound_init();
        WR32(ARG(2), create_buffer(ARG(1)));
        ret_std(c, 4, 0);
        return 1;
    case 0x1C08C4: /* HRESULT DirectSoundCreateBuffer(LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER *) */
        dsound_init();
        WR32(ARG(1), create_buffer(ARG(0)));
        ret_std(c, 2, 0);
        return 1;
    case 0x1C091B: /* HRESULT DirectSoundCreateStream(LPCDSSTREAMDESC, LPDIRECTSOUNDSTREAM *) */
        dsound_init();
        WR32(ARG(1), create_stream(ARG(0)));
        ret_std(c, 2, 0);
        return 1;
    case 0x1BF5C1: case 0x1BF5C6: case 0x1BF5CB: case 0x1BF5D0: {
        Stream *s = stream_of(ARG(0));
        if (!s) rt_fatal("DSOUND: stream call %X on unknown object %08X", va, ARG(0));
        EnterCriticalSection(&g_lock);
        int nargs = 2;
        if (va == 0x1BF5C1) s->volume = (int32_t)ARG(1);          /* IDirectSoundStream_SetVolume(this, LONG) */
        else if (va == 0x1BF5CB) s->paused = (ARG(1) & 1) != 0;   /* IDirectSoundStream_Pause(this, DWORD) */
        else if (va == 0x1BF5D0) nargs = 4;                       /* FlushEx(this, REFERENCE_TIME, DWORD flags) */
        /* 0x1BF5C6 IDirectSoundStream_SetMixBins(this, LPCDSMIXBINS): ignored */
        LeaveCriticalSection(&g_lock);
        if (va == 0x1BF5D0) stream_flush(c, s);
        ret_std(c, nargs, 0);
        return 1;
    }
    }

    /* IDirectSoundBuffer_* */
    int nargs;
    switch (va) {
    case 0x1BF4A9: case 0x1BF4C5: case 0x1BF4E1: case 0x1BF9F5: case 0x1BFA11: case 0x1BF539: case 0x1BF575:
        nargs = 2; break;
    case 0x1BF555: case 0x1BFF68: nargs = 3; break;
    case 0x1BF4FD: nargs = 4; break;
    case 0x1BF521: nargs = 1; break;
    case 0x1BE557: nargs = 5; break;
    case 0x1BF591: nargs = 8; break;
    default: return 0;
    }
    static int dslog = -1;
    if (dslog < 0) { const char *e = getenv("UG_DSLOG"); dslog = e && *e == '1'; }
    if (dslog) LOG("[dsound] %06X(%08X, %08X, %08X, %08X)\n", va, ARG(0), ARG(1), ARG(2), ARG(3));
    EnterCriticalSection(&g_lock);
    Voice *v = voice_of(ARG(0));
    uint32_t eax = 0;
    switch (va) {
    case 0x1BF4A9: /* SetVolume(this, LONG) */
        v->volume = (int32_t)ARG(1);
        break;
    case 0x1BF4C5: /* SetHeadroom(this, DWORD) */
        break;
    case 0x1BF4E1: { /* SetMixBinVolumes(this, LPCDSMIXBINS {count, {bin, LONG volume}*}): front pair only */
        uint32_t mb = ARG(1);
        uint32_t n = mb ? RD32(mb) : 0, pairs = mb ? RD32(mb + 4) : 0;
        for (uint32_t i = 0; i < n && i < 8 && pairs; i++) {
            uint32_t bin = RD32(pairs + i * 8);
            if (bin < 2) v->bin_vol[bin] = (int32_t)RD32(pairs + i * 8 + 4);
        }
        break;
    }
    case 0x1BFA11: /* SetMixBins(this, LPCDSMIXBINS) */
        break;
    case 0x1BF9F5: /* SetFrequency(this, DWORD): 0 = format rate */
        v->freq = ARG(1);
        break;
    case 0x1BF4FD: /* Play(this, reserved, reserved, DWORD flags) */
        v->looping = (ARG(3) & DSBPLAY_LOOPING) != 0;
        if (ARG(3) & DSBPLAY_FROMSTART) v->pos = 0;
        v->playing = 1;
        break;
    case 0x1BF521: /* Stop(this) */
        v->playing = 0;
        break;
    case 0x1BF539: { /* GetStatus(this, LPDWORD) */
        uint32_t st = v->playing ? DSBSTATUS_PLAYING | (v->looping ? DSBSTATUS_LOOPING : 0) : 0;
        if (ARG(1)) WR32(ARG(1), st);
        break;
    }
    case 0x1BF555: { /* GetCurrentPosition(this, LPDWORD play, LPDWORD write) */
        uint32_t play = frames_to_bytes(&v->fmt, (uint32_t)v->pos);
        uint32_t lead = frames_to_bytes(&v->fmt, (v->freq ? v->freq : v->fmt.rate) / 100 + 64);   /* ~10 ms */
        uint32_t write = v->bytes ? (play + lead) % v->bytes : 0;
        if (ARG(1)) WR32(ARG(1), play);
        if (ARG(2)) WR32(ARG(2), write);
        break;
    }
    case 0x1BF575: /* SetCurrentPosition(this, DWORD bytes) */
        v->pos = fmt_frames(&v->fmt, ARG(1));
        break;
    case 0x1BFF68: /* SetBufferData(this, LPVOID, DWORD bytes) */
        v->data = ARG(1);
        v->bytes = ARG(2);
        v->pos = 0;
        v->adpcm_block = -1;
        break;
    case 0x1BF591: { /* Lock(this, DWORD offset, DWORD bytes, LPVOID *p1, LPDWORD n1, LPVOID *p2, LPDWORD n2, flags) */
        uint32_t off = ARG(1), n = ARG(2), flags = ARG(7);
        if (flags & 2) { off = 0; n = v->bytes; }                                       /* DSBLOCK_ENTIREBUFFER */
        if (flags & 1) off = frames_to_bytes(&v->fmt, (uint32_t)v->pos) % (v->bytes ? v->bytes : 1);   /* FROMWRITECURSOR */
        if (n > v->bytes) n = v->bytes;
        if (off >= v->bytes) off = 0;
        uint32_t n1 = off + n <= v->bytes ? n : v->bytes - off;
        if (ARG(3)) WR32(ARG(3), v->data + off);
        if (ARG(4)) WR32(ARG(4), n1);
        if (ARG(5)) WR32(ARG(5), n1 < n ? v->data : 0);
        if (ARG(6)) WR32(ARG(6), n - n1);
        break;
    }
    case 0x1BE557: /* Unlock(this, p1, n1, p2, n2): the data is already in place */
        break;
    }
    LeaveCriticalSection(&g_lock);
    ret_std(c, nargs, eax);
    return 1;
}
