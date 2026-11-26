/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (BA-55) */
/* BA-55 – Noten, der 64-Step-Sequencer mit ACCENT und SLIDE, der Zufalls-Generator, der Takt (intern oder MIDI-Clock) und die Schnittstelle.
 * Teil von ba55_core.c */

void ba_init(ba_engine *e, float sr, ba_patch *pt)
{
    int i;
    char *z = (char *)e;
    for (i = 0; i < (int)sizeof *e; i++)
        z[i] = 0;
    e->sr = sr;
    e->pt = pt;
    e->note = -1;
    e->seq_note = -1;
    e->cur = -1;
    e->rs = 0x6C8E9CF5u;
    e->bend_range = 2.0f;
    e->pitch = e->pitch_to = 48.0f;
    for (i = 0; i < 3; i++)
        e->mod[i].rs = 0x2545F491u * (uint32_t)(i + 1) + 12345u;
    e->dirty = 1;
}

void ba_changed(ba_engine *e) { e->dirty = 1; }

/* ---- die Ereignisse: aus der Hauptschleife in den Ring, im Audio-Teil ausgeführt ---- */
static void ev_push(ba_engine *e, int type, int a, int b)
{
    uint32_t w = e->ev_w;
    if (w - e->ev_r >= BA_NEV)
        return;                          /* voll: verworfen */
    e->ev[w % BA_NEV].type = (uint8_t)type;
    e->ev[w % BA_NEV].a = (uint8_t)a;
    e->ev[w % BA_NEV].b = (uint8_t)b;
    e->ev_w = w + 1;
}
void ba_note_on(ba_engine *e, int note, int vel) { ev_push(e, MEV_ON, note & 127, vel < 1 ? 1 : vel & 127); }
void ba_note_on_play(ba_engine *e, int note, int vel) { ev_push(e, MEV_ONF, note & 127, vel < 1 ? 1 : vel & 127); }
void ba_note_off(ba_engine *e, int note) { ev_push(e, MEV_OFF, note & 127, 0); }
void ba_all_off(ba_engine *e) { ev_push(e, MEV_ALLOFF, 0, 0); }
void ba_play(ba_engine *e, int on) { ev_push(e, on ? MEV_PLAY : MEV_STOP, 0, 0); }
void ba_continue(ba_engine *e) { ev_push(e, MEV_CONT, 0, 0); }
void ba_midi_clock(ba_engine *e) { ev_push(e, MEV_CLOCK, 0, 0); }
void ba_bend(ba_engine *e, int v14) { ev_push(e, MEV_BEND, v14 & 127, (v14 >> 7) & 127); }
void ba_hold(ba_engine *e, int on) { ev_push(e, MEV_HOLD, on != 0, 0); }

/* ---- Takt ---- */
float ba_bpm(const ba_engine *e)
{
    if (e->ext && e->ext_bpm > 0.0f)
        return e->ext_bpm;
    return (float)(e->pt->bpm < 30 ? 30 : e->pt->bpm > 300 ? 300 : e->pt->bpm);
}
int ba_dvs(const ba_engine *e) { return BA_DVS[ba_list_index(P_SDVS, e->pt->p[P_SDVS])]; }
int ba_seq_len(const ba_engine *e) { return ba_list_index(P_SLEN, e->pt->p[P_SLEN]) + 1; }
int ba_step_now(const ba_engine *e) { return e->running ? e->cur : -1; }
static float step_len(const ba_engine *e) { return e->sr * 60.0f / ba_bpm(e) * 4.0f / (float)ba_dvs(e); }

/* ---- die Tasten: ein Stapel, die letzte gedrückte klingt (wie bei MONSUN) ---- */
static void stack_remove(ba_engine *e, int note)
{
    int i, j = 0;
    for (i = 0; i < e->nheld; i++)
        if (e->held[i] != note)
            e->held[j++] = e->held[i];
    e->nheld = j;
}
static void stack_push(ba_engine *e, int note)
{
    int i;
    stack_remove(e, note);
    if (e->nheld >= 12) {                /* voll: die älteste fällt raus */
        for (i = 1; i < 12; i++)
            e->held[i - 1] = e->held[i];
        e->nheld = 11;
    }
    e->held[e->nheld++] = (uint8_t)note;
}

static void key_on(ba_engine *e, int note, int vel, int force)
{
    if (e->running && !force) {          /* der Sequencer läuft: die Taste transponiert ihn (C4 = unverändert) */
        e->transpose = note - 60;
        return;
    }
    stack_push(e, note);
    voice_on(e, note, (float)vel * (1.0f / 127.0f), 0, 0, 0);
    mods_step(e, 1);                     /* die Step-Modulatoren beginnen mit jeder Note von vorn */
    e->step_clock = step_len(e);
}

static void key_off(ba_engine *e, int note)
{
    int was = e->nheld;
    stack_remove(e, note);
    if (was == e->nheld)
        return;                          /* war keine gespielte (eine Transpose-Taste) */
    if (e->nheld > 0) {
        int top = e->held[e->nheld - 1];
        if (top != e->note)
            voice_on(e, top, e->vel, 0, 0, 0);  /* zurück zur vorigen Taste */
        return;
    }
    if (e->hold || e->seq_note >= 0)
        return;                          /* HOLD: sie klingt weiter */
    voice_off(e);
}

/* ---- der Sequencer ---- */
static void seq_release(ba_engine *e)
{
    if (e->seq_note >= 0 && e->nheld == 0 && !e->hold)
        voice_off(e);
    e->seq_note = -1;
}

static void advance(ba_engine *e)
{
    const ba_step *st;
    int len = ba_seq_len(e), note, play;
    float sl = step_len(e);
    e->cur = (e->cur + 1) % len;
    st = &e->pt->seq[e->cur];
    play = st->gate != BA_REST;
    if (play && e->pt->p[P_SPROB] < 127) {   /* PROB: die Chance, dass der Step spielt */
        e->rs ^= e->rs << 13;
        e->rs ^= e->rs >> 17;
        e->rs ^= e->rs << 5;
        play = (float)(e->rs & 1023u) < (float)e->pt->p[P_SPROB] / 127.0f * 1024.0f;
    }
    if (!play) {
        if (e->tie_hold)                 /* nach einem TIE / SLIDE endet die Note hier */
            seq_release(e);
        e->tie_hold = e->slide_next = 0;
        e->gate_left = 0.0f;
        return;
    }
    note = st->note + e->transpose;
    note = note < 21 ? 21 : note > 108 ? 108 : note;
    if (e->slide_next && e->seq_note >= 0)
        voice_on(e, note, e->vel, 1, 1, 0);   /* SLIDE: gleitet ohne neuen Anschlag in die Note (wie die 303) */
    else if (e->tie_hold && e->seq_note >= 0)
        voice_on(e, note, e->vel, 1, 0, 0);   /* TIE: gebunden, ohne neuen Anschlag */
    else
        voice_on(e, note, 0.8f, 0, 0, st->fl & BA_ACC);
    e->seq_note = note;
    e->slide_next = (st->fl & BA_SLD) != 0;
    e->tie_hold = st->gate == BA_TIE || e->slide_next;
    e->gate_left = e->tie_hold ? 0.0f : ba_clamp((float)st->gate / 127.0f * sl, 0.004f * e->sr, sl * 1.2f);
}

static void seq_start(ba_engine *e)
{
    e->running = 1;
    e->cur = -1;
    e->to_next = 0.0f;
    e->transpose = 0;
    e->ext_ticks = 0;
    e->tie_hold = e->slide_next = 0;
    e->step_clock = step_len(e);
    mods_step(e, 1);
}

static void seq_stop(ba_engine *e)
{
    e->running = 0;
    e->cur = -1;
    e->tie_hold = e->slide_next = 0;
    e->transpose = 0;
    seq_release(e);
}

/* 24 Pulse pro Viertel: das Tempo gemessen, und im Takt der Pulse die Steps */
static void clock_tick(ba_engine *e)
{
    if (e->last_clock) {
        uint32_t d = e->counter - e->last_clock;
        if (d > 0u && (float)d < e->sr) {
            float b = e->sr * 60.0f / ((float)d * 24.0f);
            if (b >= 20.0f && b <= 400.0f)
                e->ext_bpm = e->ext_bpm <= 0.0f ? b : e->ext_bpm * 0.9f + b * 0.1f;
        }
    }
    e->last_clock = e->counter ? e->counter : 1u;
    e->ext = 1;
    if (e->running) {
        int tps = 96 / ba_dvs(e);
        if (e->ext_ticks % tps == 0)
            advance(e);
        e->ext_ticks++;
    }
}

static void events(ba_engine *e)
{
    while (e->ev_r != e->ev_w) {
        const ba_ev *v = &e->ev[e->ev_r % BA_NEV];
        switch (v->type) {
        case MEV_ON: key_on(e, v->a, v->b, 0); break;
        case MEV_ONF: key_on(e, v->a, v->b, 1); break;
        case MEV_OFF: key_off(e, v->a); break;
        case MEV_ALLOFF:
            e->nheld = 0;
            e->seq_note = -1;
            voice_off(e);
            break;
        case MEV_PLAY: seq_start(e); break;
        case MEV_STOP: seq_stop(e); break;
        case MEV_CONT: e->running = 1; break;
        case MEV_CLOCK: clock_tick(e); break;
        case MEV_BEND: e->bend = (float)(((int)v->b << 7 | v->a) - 8192) / 8192.0f * e->bend_range; break;
        case MEV_HOLD:
            e->hold = v->a;
            if (!e->hold && e->nheld == 0 && e->seq_note < 0)
                voice_off(e);
            break;
        default: break;
        }
        e->ev_r++;
    }
}

void ba_render(ba_engine *e, float *l, float *r, int n)
{
    float *out = e->mono;
    int done = 0;
    if (n > 256)
        n = 256;
    events(e);
    if (e->dirty)
        voice_update(e);
    if (e->ext && (float)(e->counter - e->last_clock) > e->sr * 0.5f)
        e->ext = 0;                      /* eine halbe Sekunde ohne Clock: wieder das eigene Tempo */
    while (done < n) {
        int chunk = n - done;
        float base = step_len(e);
        if (e->running && !e->ext) {
            int until;
            if (e->to_next <= 0.0f) {
                float sw = nrm(e->pt->p[P_SSWING]) * 0.5f;   /* SWING: 50 % .. 75 % */
                advance(e);
                e->to_next += base * ((e->cur & 1) ? 1.0f - sw : 1.0f + sw);
            }
            until = (int)e->to_next;
            if ((float)until < e->to_next)
                until++;
            if (until < chunk)
                chunk = until < 1 ? 1 : until;
        }
        if (e->gate_left > 0.0f) {       /* das Gate des Steps endet mitten im Block */
            int g = (int)e->gate_left;
            if ((float)g < e->gate_left)
                g++;
            if (g < chunk)
                chunk = g < 1 ? 1 : g;
        }
        voice_render(e, out + done, chunk);
        if (e->running && !e->ext)
            e->to_next -= (float)chunk;
        if (e->gate_left > 0.0f) {
            e->gate_left -= (float)chunk;
            if (e->gate_left <= 0.0f) {
                e->gate_left = 0.0f;
                if (!e->tie_hold)
                    seq_release(e);
            }
        }
        /* die Step-Modulatoren laufen immer im Takt der Teilung */
        e->step_clock -= (float)chunk;
        if (e->step_clock <= 0.0f) {
            mods_step(e, 0);
            e->step_clock += base;
            if (e->step_clock <= 0.0f)
                e->step_clock = base;
        }
        done += chunk;
    }
    fx_render(e, out, l, r, n);
    e->counter += (uint32_t)n;
}

/* ---- der Zufalls-Generator: ein Acid-Pattern über die ersten LENG Steps ---- */
static uint32_t rnd_next(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

void ba_randomize(ba_patch *pt, int root, uint32_t seed)
{
    static const int8_t PENTA[8] = {0, 0, 3, 5, 7, 10, 12, 0};   /* Moll-Pentatonik, der Grundton doppelt so oft */
    uint32_t s = seed ? seed : 0x9E3779B9u;
    int len = ba_list_index(P_SLEN, pt->p[P_SLEN]) + 1, i;
    root = root < 24 ? 24 : root > 72 ? 72 : root;
    for (i = 0; i < 4; i++)
        rnd_next(&s);
    for (i = 0; i < len && i < BA_STEPS; i++) {
        ba_step *st = &pt->seq[i];
        uint32_t r = rnd_next(&s);
        int n = PENTA[r % 8u], oct = (int)((r >> 8) % 100u);
        n += oct < 14 ? 12 : oct < 20 ? -12 : 0;   /* hin und wieder eine Oktave hoch oder runter */
        st->note = (uint8_t)(root + n);
        r = rnd_next(&s);
        if ((r % 100u) < (i % 4 == 0 ? 6u : 22u))   /* auf der Eins seltener eine Pause */
            st->gate = BA_REST;
        else
            st->gate = (uint8_t)(40 + (r >> 8) % 50u);
        r = rnd_next(&s);
        st->fl = 0;
        if (st->gate != BA_REST) {
            if ((r % 100u) < (i % 4 == 0 ? 45u : 22u))
                st->fl |= BA_ACC;
            if (((r >> 8) % 100u) < 18u)
                st->fl |= BA_SLD;
        }
    }
}
