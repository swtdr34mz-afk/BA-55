/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (BA-55) */
/* BA-55 – Tasten, der 64-Step-Sequencer der DB-01 (Play-Modi, Wahrscheinlichkeit, Accent, Slide, Mod-Spur, Pitch
 * Env), der Arpeggiator, der Takt (intern oder MIDI-Clock), RND PATT und die Schnittstelle. Teil von ba55_core.c */

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
    e->pp_dir = 1;
    e->rs = 0x6C8E9CF5u;
    e->bend_range = 2.0f;
    e->tune_hz = 440.0f;
    e->pitch = e->pitch_to = 36.0f;
    e->dr.rs = 0x1B873593u;
    e->dr.sc_t = 1e9f;
    e->g_step = -1;
    e->sc_now = 1.0f;
    e->dirty = 1;
}

void ba_changed(ba_engine *e) { e->dirty = 1; }

/* ---- die Ereignisse: aus der Hauptschleife in den Ring, im Audio-Teil ausgeführt ---- */
static void ev_push(ba_engine *e, int type, int a, int b)
{
    uint32_t w = e->ev_w;
    if (w - e->ev_r >= BA_NEV)
        return;
    e->ev[w % BA_NEV].type = (uint8_t)type;
    e->ev[w % BA_NEV].a = (uint8_t)a;
    e->ev[w % BA_NEV].b = (uint8_t)b;
    e->ev_w = w + 1;
}
void ba_note_on(ba_engine *e, int note, int vel) { ev_push(e, MEV_ON, note & 127, vel & 127); }
void ba_note_on_play(ba_engine *e, int note, int vel) { ev_push(e, MEV_ONF, note & 127, vel & 127); }
void ba_note_off(ba_engine *e, int note) { ev_push(e, MEV_OFF, note & 127, 0); }
void ba_all_off(ba_engine *e) { ev_push(e, MEV_ALLOFF, 0, 0); }
void ba_play(ba_engine *e, int on) { ev_push(e, on ? MEV_PLAY : MEV_STOP, 0, 0); }
void ba_continue(ba_engine *e) { ev_push(e, MEV_CONT, 0, 0); }
void ba_midi_clock(ba_engine *e) { ev_push(e, MEV_CLOCK, 0, 0); }
void ba_bend(ba_engine *e, int v14) { ev_push(e, MEV_BEND, v14 & 127, (v14 >> 7) & 127); }
void ba_arp(ba_engine *e, int on) { ev_push(e, MEV_ARP, on != 0, 0); }

/* ---- Takt: der Sequencer läuft in Sechzehnteln, wie am Gerät ---- */
float ba_bpm(const ba_engine *e)
{
    if (e->ext && e->ext_bpm > 0.0f)
        return e->ext_bpm;
    return (float)(e->pt->bpm < 30 ? 30 : e->pt->bpm > 300 ? 300 : e->pt->bpm);
}
int ba_seq_len(const ba_engine *e) { return ba_list_index(P_LAST, e->pt->p[P_LAST]) + 1; }
int ba_step_now(const ba_engine *e) { return e->running ? e->cur : -1; }
float ba_lfo_out(const ba_engine *e) { return e->lfo_out; }
static float step_len(const ba_engine *e) { return e->sr * 60.0f / ba_bpm(e) / 4.0f; }

static uint32_t rnd_u(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

/* ---- die Tasten: ein Stapel, die letzte gedrückte klingt ---- */
static void stack_remove(uint8_t *st, int *n, int note)
{
    int i, j = 0;
    for (i = 0; i < *n; i++)
        if (st[i] != note)
            st[j++] = st[i];
    *n = j;
}
static void stack_push(uint8_t *st, int *n, int note)
{
    int i;
    stack_remove(st, n, note);
    if (*n >= 12) {
        for (i = 1; i < 12; i++)
            st[i - 1] = st[i];
        *n = 11;
    }
    st[(*n)++] = (uint8_t)note;
}

static void key_on(ba_engine *e, int note, int force)
{
    if (e->running && !force) {          /* der Sequencer läuft: die Taste transponiert ihn (C3 = unverändert) */
        e->transpose = note - 60;
        return;
    }
    if (e->arp && !e->running) {         /* der Arpeggiator: die Tasten sammeln Noten */
        if (e->pt->p[P_ALATCH] >= 64 && e->nheld == 0)
            e->nlatched = 0;             /* LATCH: eine neue Hand ersetzt die gehaltenen Noten */
        stack_push(e->held, &e->nheld, note);
        stack_push(e->latched, &e->nlatched, note);
        if (e->nheld == 1 && e->nlatched == 1) {
            e->arp_i = -1;
            e->arp_next = 0.0f;
        }
        return;
    }
    stack_push(e->held, &e->nheld, note);
    voice_on(e, note, 0, 0, 0);
}

static void key_off(ba_engine *e, int note)
{
    int was = e->nheld;
    stack_remove(e->held, &e->nheld, note);
    if (was == e->nheld)
        return;
    if (e->arp && !e->running) {
        if (e->pt->p[P_ALATCH] < 64)
            stack_remove(e->latched, &e->nlatched, note);
        if (e->nlatched == 0)
            voice_off(e);
        return;
    }
    if (e->nheld > 0) {
        int top = e->held[e->nheld - 1];
        if (top != e->note)
            voice_on(e, top, 1, 0, 0);   /* zurück zur vorigen Taste, gebunden */
        return;
    }
    if (e->seq_note >= 0)
        return;
    voice_off(e);
}

/* ---- der Sequencer ---- */
static void seq_release(ba_engine *e)
{
    if (e->seq_note >= 0 && e->nheld == 0)
        voice_off(e);
    e->seq_note = -1;
}

/* der nächste Step nach dem PLAY MODE */
static int next_step(ba_engine *e, int len)
{
    int pm = ba_list_index(P_PMODE, e->pt->p[P_PMODE]), c = e->cur, n;
    if (len <= 1) {
        if (c >= 0)
            e->loops++;
        return 0;
    }
    switch (pm) {
    case PM_BWD:
        n = c < 0 ? len - 1 : c - 1;
        if (n < 0 || n >= len) {
            n = len - 1;
            e->loops++;
        }
        return n;
    case PM_RND:
        if (c >= 0 && ++e->pp_dir > len) {   /* (pp_dir zählt hier die Steps einer Runde) */
            e->pp_dir = 1;
            e->loops++;
        }
        return (int)(rnd_u(&e->rs) % (uint32_t)len);
    case PM_PP:
    case PM_PP2:
        if (c < 0) {
            e->pp_dir = 1;
            return 0;
        }
        if (c >= len)
            c = len - 1;
        if (e->pp_dir > 0) {
            if (c + 1 >= len) {
                e->pp_dir = -1;
                return pm == PM_PP2 ? len - 1 : len - 2;   /* PP2: das Ende zweimal */
            }
            return c + 1;
        }
        if (c - 1 < 0) {
            e->pp_dir = 1;
            e->loops++;
            return pm == PM_PP2 ? 0 : 1;
        }
        return c - 1;
    default:
        n = c + 1;
        if (n >= len) {
            n = 0;
            e->loops++;
        }
        return n;
    }
}

static int step_plays(ba_engine *e, const ba_step *st)
{
    static const uint16_t PCT[4] = {1024, 768, 512, 256};
    if (!st->gate)
        return 0;
    if (st->prob < 4)
        return st->prob == 0 || (rnd_u(&e->rs) & 1023u) < PCT[st->prob];
    return e->loops % (st->prob - 2) == 0;   /* 1/2, 1/3, 1/4: nur in jeder 2., 3., 4. Runde */
}

static void advance(ba_engine *e)
{
    const ba_step *st;
    int len = ba_seq_len(e), note;
    float sl = step_len(e);
    e->cur = next_step(e, len);
    st = &e->pt->seq[e->cur];
    if (!step_plays(e, st)) {
        if (e->tie_hold)
            seq_release(e);
        e->tie_hold = e->slide_next = 0;
        e->gate_left = 0.0f;
        return;
    }
    note = st->note + e->transpose;
    note = note < 12 ? 12 : note > 108 ? 108 : note;
    if (e->slide_next && e->seq_note >= 0)
        voice_on(e, note, 1, 1, st);     /* SLIDE: gleitet ohne neuen Anschlag in die Note */
    else if (e->tie_hold && e->seq_note >= 0)
        voice_on(e, note, 1, 0, st);     /* 100 %: verbunden, ohne neuen Anschlag */
    else
        voice_on(e, note, 0, 0, st);
    e->seq_note = note;
    e->slide_next = (st->fl & BA_SLD) != 0;
    e->tie_hold = st->gate >= 10 || e->slide_next;
    e->gate_left = e->tie_hold ? 0.0f : ba_clamp((float)st->gate * 0.1f * sl, 0.003f * e->sr, sl);
}

static void seq_start(ba_engine *e)
{
    e->running = 1;
    e->cur = -1;
    e->pp_dir = 1;
    e->loops = 0;
    e->to_next = 0.0f;
    e->transpose = 0;
    e->ext_ticks = 0;
    e->tie_hold = e->slide_next = 0;
    e->g_step = -1;                      /* der Groove beginnt mit dem Pattern */
    e->g_to_next = 0.0f;
}

static void seq_stop(ba_engine *e)
{
    e->running = 0;
    e->cur = -1;
    e->tie_hold = e->slide_next = 0;
    e->transpose = 0;
    seq_release(e);
}

/* ---- der Arpeggiator: die gehaltenen Noten, sortiert, über OCT Oktaven, in DIR ---- */
static void arp_tick(ba_engine *e)
{
    uint8_t nt[36];
    int n = 0, i, j, oct = ba_list_index(P_AOCT, e->pt->p[P_AOCT]) + 1, dir = ba_list_index(P_ADIR, e->pt->p[P_ADIR]);
    int gate = ba_list_index(P_AGATE, e->pt->p[P_AGATE]) + 1, cnt, k;
    for (i = 0; i < e->nlatched; i++) {  /* sortieren */
        int v = e->latched[i];
        for (j = n; j > 0 && nt[j - 1] > v; j--)
            nt[j] = nt[j - 1];
        nt[j] = (uint8_t)v;
        n++;
    }
    if (!n)
        return;
    for (k = 1; k < oct; k++)
        for (i = 0; i < e->nlatched; i++)
            nt[k * e->nlatched + i] = (uint8_t)(nt[i] + 12 * k);
    cnt = n * oct;
    switch (dir) {
    case AD_DOWN: e->arp_i = e->arp_i <= 0 ? cnt - 1 : e->arp_i - 1; k = e->arp_i; break;
    case AD_UPDN: {                      /* hin und zurück, ohne die Enden doppelt */
        int per = cnt > 1 ? 2 * cnt - 2 : 1;
        e->arp_i = (e->arp_i + 1) % per;
        k = e->arp_i < cnt ? e->arp_i : per - e->arp_i;
        break;
    }
    case AD_RND: k = (int)(rnd_u(&e->rs) % (uint32_t)cnt); break;
    default: e->arp_i = (e->arp_i + 1) % cnt; k = e->arp_i; break;
    }
    e->arp_note = nt[k < 0 ? 0 : k >= cnt ? cnt - 1 : k];
    voice_on(e, e->arp_note, gate >= 10 && e->gate, 0, 0);
    e->gate_left = gate >= 10 ? 0.0f : (float)gate * 0.1f * step_len(e);
}

/* 24 Pulse pro Viertel: das Tempo gemessen, und alle 6 Pulse ein Sechzehntel */
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
    if (e->ext_ticks % 6 == 0) {
        if (e->running && e->gv && e->gv->on)   /* der Groove: eine Sechzehntel je 6 Pulse */
            groove_step(e, 0);
        if (e->running)
            advance(e);
        else if (e->arp && e->nlatched)
            arp_tick(e);
    }
    e->ext_ticks++;
}

static void events(ba_engine *e)
{
    while (e->ev_r != e->ev_w) {
        const ba_ev *v = &e->ev[e->ev_r % BA_NEV];
        switch (v->type) {
        case MEV_ON: key_on(e, v->a, 0); break;
        case MEV_ONF: key_on(e, v->a, 1); break;
        case MEV_OFF: key_off(e, v->a); break;
        case MEV_ALLOFF:
            e->nheld = e->nlatched = 0;
            e->seq_note = -1;
            voice_off(e);
            break;
        case MEV_PLAY: seq_start(e); break;
        case MEV_STOP: seq_stop(e); break;
        case MEV_CONT: e->running = 1; break;
        case MEV_CLOCK: clock_tick(e); break;
        case MEV_BEND: e->bend = (float)(((int)v->b << 7 | v->a) - 8192) / 8192.0f * e->bend_range; break;
        case MEV_ARP:
            e->arp = v->a;
            e->nlatched = 0;
            e->arp_i = -1;
            if (!e->running && e->nheld == 0)
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
    int done = 0, i;
    if (n > 256)
        n = 256;
    events(e);
    if (e->dirty)
        voice_update(e);
    if (e->ext && (float)(e->counter - e->last_clock) > e->sr * 0.5f)
        e->ext = 0;                      /* eine halbe Sekunde ohne Clock: wieder das eigene Tempo */
    while (done < n) {
        int chunk = n - done, arp_on = e->arp && !e->running && e->nlatched > 0;
        float base = step_len(e);
        if (!e->ext && (e->running || arp_on)) {
            int until;
            if (e->to_next <= 0.0f) {
                float sw = nrm(e->pt->p[P_SHUFFLE]) * 0.5f;   /* SHUFFLE: 50 % .. 75 % */
                if (e->running)
                    advance(e);
                else
                    arp_tick(e);
                e->sw_odd = !e->sw_odd;
                e->to_next += base * (e->sw_odd ? 1.0f + sw : 1.0f - sw);
            }
            until = (int)e->to_next;
            if ((float)until < e->to_next)
                until++;
            if (until < chunk)
                chunk = until < 1 ? 1 : until;
        } else if (!e->running && !arp_on) {
            e->to_next = 0.0f;
            e->sw_odd = 0;
        }
        if (e->running && !e->ext && e->gv && e->gv->on) {   /* der Groove: seine Sechzehntel, mit seinem Swing */
            int until;
            if (e->g_to_next <= 0.0f) {
                float g16 = e->sr * 15.0f / ba_bpm(e), sw = STYLES[grv_style(e)].swing;
                groove_step(e, done);
                e->g_to_next += g16 * ((e->g_step & 1) ? 1.0f - sw : 1.0f + sw);
            }
            until = (int)e->g_to_next;
            if ((float)until < e->g_to_next)
                until++;
            if (until < chunk)
                chunk = until < 1 ? 1 : until;
        }
        if (e->gate_left > 0.0f) {
            int g = (int)e->gate_left;
            if ((float)g < e->gate_left)
                g++;
            if (g < chunk)
                chunk = g < 1 ? 1 : g;
        }
        voice_render(e, out + done, chunk);
        if (!e->ext && (e->running || arp_on))
            e->to_next -= (float)chunk;
        if (e->running && !e->ext && e->gv && e->gv->on)
            e->g_to_next -= (float)chunk;
        if (e->gate_left > 0.0f) {
            e->gate_left -= (float)chunk;
            if (e->gate_left <= 0.0f) {
                e->gate_left = 0.0f;
                if (e->running) {
                    if (!e->tie_hold)
                        seq_release(e);
                } else if (e->arp) {
                    voice_off(e);
                }
            }
        }
        done += chunk;
    }
    for (i = 0; i < n; i++) {            /* mono, wie das Gerät; ein weicher Begrenzer am Ende */
        float y = ba_soft(out[i] * 0.9f);
        l[i] = r[i] = y;
    }
    if (e->gv && (e->gv->on || e->gqn))  /* GROOVE: die Pumpe und die Drums (ab hier stereo: die Hats leicht verteilt) */
        groove_render(e, l, r, n);
    else
        e->sc_now = 1.0f;
    e->counter += (uint32_t)n;
}

/* ---- RND PATT: neue Werte für die Steps 0..LAST-1, nach Ziel, Menge (AMOUNT) und Grenzen (LOW, HIGH) ---- */
static int rnd_range(uint32_t *s, int lo, int hi)
{
    if (hi < lo) {
        int t = lo;
        lo = hi;
        hi = t;
    }
    return lo + (int)(rnd_u(s) % (uint32_t)(hi - lo + 1));
}

void ba_randomize(ba_patch *pt, uint32_t seed)
{
    uint32_t s = seed ? seed : 0x9E3779B9u;
    int len = ba_list_index(P_LAST, pt->p[P_LAST]) + 1, tgt = ba_list_index(P_RTGT, pt->p[P_RTGT]), i;
    int amt = pt->p[P_RAMT] * 100 / 127, lo = pt->p[P_RLO], hi = pt->p[P_RHI];
    int root = 36 + ba_list_index(P_ROOT, pt->p[P_ROOT]), all = tgt == RT_PATTERN;
    for (i = 0; i < 4; i++)
        rnd_u(&s);
    for (i = 0; i < len && i < BA_STEPS; i++) {
        ba_step *st = &pt->seq[i];
        int pc = (int)(rnd_u(&s) % 100u);
        if (all || tgt == RT_NOTES) {    /* Noten aus der Skala, LOW .. HIGH: 0 .. 24 Halbtöne über dem Grundton */
            int a = lo * 24 / 127, b = hi * 24 / 127;
            st->note = (uint8_t)ba_scale_snap(pt, root + rnd_range(&s, a, b));
        }
        if (all || tgt == RT_GATES)      /* LOW .. HIGH: 10 % .. 100 % */
            st->gate = st->gate || !all ? (uint8_t)rnd_range(&s, 1 + (all ? 30 : lo) * 9 / 127, 1 + (all ? 90 : hi) * 9 / 127) : 0;
        if (all || tgt == RT_ON) {       /* AMOUNT: die Chance, dass ein Step an ist */
            int on = (int)(rnd_u(&s) % 100u) < (all ? 75 + amt / 5 : amt) || (all && i % 4 == 0);
            if (!on)
                st->gate = 0;
            else if (!st->gate)
                st->gate = 5;
        }
        if (all || tgt == RT_ACC)
            st->fl = (uint8_t)((st->fl & ~BA_ACC) | (pc < amt ? BA_ACC : 0));
        if (all || tgt == RT_SLIDE)
            st->fl = (uint8_t)((st->fl & ~BA_SLD) | ((int)(rnd_u(&s) % 100u) < (all ? amt / 2 : amt) ? BA_SLD : 0));
        if (tgt == RT_MOD)               /* LOW .. HIGH direkt */
            st->mod = (uint8_t)rnd_range(&s, lo, hi);
        if (all)
            st->mod = 64;
        if (tgt == RT_PENV || all) {     /* AMOUNT: wie viele Steps; LOW .. HIGH: die Tiefe 1..9 */
            if ((int)(rnd_u(&s) % 100u) < (all ? amt / 6 : amt))
                st->pe = (uint8_t)(rnd_range(&s, 1 + (all ? 20 : lo) * 8 / 127, 1 + (all ? 70 : hi) * 8 / 127) << 4
                                   | rnd_range(&s, 1, 9));
            else
                st->pe = (uint8_t)(BA_PE_DECAY(st->pe) ? BA_PE_DECAY(st->pe) : 5);
        }
        if (tgt == RT_PROB)              /* AMOUNT: wie viele Steps eine Wahrscheinlichkeit bekommen */
            st->prob = (uint8_t)((int)(rnd_u(&s) % 100u) < amt ? rnd_range(&s, 1, BA_NPROB - 1) : 0);
        if (all)
            st->prob = 0;
    }
}
