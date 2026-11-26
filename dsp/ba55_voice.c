/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (BA-55) */
/* BA-55 – die Stimme der DB-01: VCO, SUB, NOISE, DETUNE, das Filter, die zwei Hüllkurven, OVERDRIVE, der LFO.
 * Teil von ba55_core.c */

static float bipn(int v) { return ba_clamp(((float)v - 64.0f) / 63.0f, -1.0f, 1.0f); }

/* ---- Oszillator: Dreieck, Säge, Rechteck (PolyBLEP) ---- */
static inline float blep(float t, float dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

static inline float osc_step(ba_osc *o, float inc)
{
    o->phase += inc;
    if (o->phase >= 1.0f)
        o->phase -= (float)(int)o->phase;
    return o->phase;
}

static inline float square(float ph, float dt)
{
    float y = ph < 0.5f ? 1.0f : -1.0f, t2 = ph - 0.5f;
    if (t2 < 0.0f)
        t2 += 1.0f;
    return y + blep(ph, dt) - blep(t2, dt);
}

static inline float vco(float ph, float dt, int w)
{
    switch (w) {
    case 0: return 1.0f - 4.0f * ba_abs(ph - 0.5f);   /* Dreieck (bei doppelter Samplerate sauber genug) */
    case 1: return 2.0f * ph - 1.0f - blep(ph, dt);   /* Säge */
    default: return square(ph, dt);                    /* Rechteck */
    }
}

/* ---- das Filter: ein State-Variable-Filter (12 dB) im Geist des Polivoks / der Acidbox. Die beiden Integratoren
 *      sättigen wie die übersteuerten Verstärker im Original: bei viel Resonanz kippt es in ein rauhes, stabiles
 *      Pfeifen, ganz rechts schwingt es selbst. Tiefpass oder Bandpass. ---- */
static inline float svf_run(ba_svf *f, float x, float g, float r, int bp)
{
    float hp = (x - (2.0f * r + g) * f->s1 - f->s2) / (1.0f + 2.0f * r * g + g * g);
    float b = g * hp + f->s1, l;
    f->s1 = ba_soft(g * hp + b);
    l = g * b + f->s2;
    f->s2 = ba_soft(0.85f * (g * b + l)) * 1.1765f;
    return bp ? b * 1.6f : l;
}

/* tan(pi * x) für x bis ~0.24 */
static inline float svf_g(float x)
{
    float w = BA_PI * x, w2 = w * w;
    return w * (1.0f + w2 * (0.3333333f + w2 * (0.1333333f + w2 * 0.0539683f)));
}

/* der Halbband-Dezimierer (2x Oversampling zurück) */
#define HB_N 23
static const float HB_H[6] = {0.003001f, -0.011798f, 0.033245f, -0.084613f, 0.310479f, 0.499918f};
static inline float hb_push(ba_engine *v, float a, float b)
{
    float *h;
    int p = v->hb_pos;
    v->hb[p] = v->hb[p + HB_N] = a;
    p = p + 1 >= HB_N ? 0 : p + 1;
    v->hb[p] = v->hb[p + HB_N] = b;
    p = p + 1 >= HB_N ? 0 : p + 1;
    v->hb_pos = p;
    h = v->hb + p;
    return HB_H[5] * h[11] + HB_H[4] * (h[10] + h[12]) + HB_H[3] * (h[8] + h[14]) + HB_H[2] * (h[6] + h[16])
           + HB_H[1] * (h[4] + h[18]) + HB_H[0] * (h[2] + h[20]) - 0.000272f * (h[0] + h[22]);
}

static inline float rnd_white(ba_engine *e)
{
    e->rs ^= e->rs << 13;
    e->rs ^= e->rs >> 17;
    e->rs ^= e->rs << 5;
    return (float)(int32_t)e->rs * (1.0f / 2147483648.0f);
}

/* ---- umrechnen, wenn sich der Klang geändert hat ---- */
static void voice_update(ba_engine *e)
{
    const uint8_t *p = e->pt->p;
    float vr = ba_vrel_ms(p[P_VREL]);
    e->wave = ba_wave_index(p[P_WAVE]);
    e->u_sub = nrm(p[P_SUB]);
    e->u_noise = nrm(p[P_NOISE]);
    e->u_det = nrm(p[P_DETUNE]);
    e->u_drive = nrm(p[P_DRIVE]);
    e->u_fm = nrm(p[P_FMLVL]) * nrm(p[P_FMLVL]) * 36.0f;   /* bis ±36 Halbtöne: weit neben der Tonhöhe */
    e->u_cut = nrm(p[P_CUTOFF]) * 10.0f;                    /* Oktaven über 20 Hz */
    e->u_res = nrm(p[P_RESON]);
    e->u_fdep = nrm(p[P_FDEPTH]) * 7.0f;
    e->bp = p[P_FMODE] >= 64;
    e->u_lfolvl = nrm(p[P_LFOLVL]) * 5.0f;
    e->u_glide = ba_glide_ms(p[P_GLIDE]);
    e->fdec_coef = ba_expf(-6.9f / (ba_fdecay_ms(p[P_FDECAY]) * 0.001f * e->sr));
    e->u_vrel = vr;
    e->vrel_coef = vr < 0.0f ? 1.0f : ba_expf(-6.9f / (vr * 0.001f * e->sr));
    e->dirty = 0;
}

/* ---- Noten an die Stimme. tie: ohne neuen Anschlag (Gate 100 %); slide: gleitet dabei (SLIDE) ---- */
#define SLIDE_MS 60.0f                   /* die Slide-Zeit ist fest, wie am Gerät */

static void voice_on(ba_engine *e, int note, int tie, int slide, const ba_step *st)
{
    float ms = slide ? SLIDE_MS : (!tie && e->u_glide > 0.0f && !st) ? e->u_glide : 0.0f;
    e->pitch_to = (float)note;
    if (ms <= 0.0f || e->note < 0)
        e->pitch = e->pitch_to;
    e->glide_coef = ms > 0.0f ? ba_expf(-8.0f / (ms * 0.001f * e->sr * 0.3f)) : 0.0f;
    e->note = note;
    if (!(tie && e->gate)) {
        e->fenv = 1.0f;                  /* die Filterhüllkurve beginnt von vorn */
        e->acc = st && (st->fl & BA_ACC) ? 1.0f : 0.0f;
        if (st && BA_PE_DEPTH(st->pe)) { /* PITCH ENV dieses Steps */
            e->pe = 1.0f;
            e->pe_amt = (float)BA_PE_DEPTH(st->pe) * 2.0f;   /* 2 .. 18 Halbtöne über der Note, fällt zu ihr */
            e->pe_coef = ba_expf(-6.9f * 8.0f / (ba_pe_decay_ms(BA_PE_DECAY(st->pe)) * 0.001f * e->sr));
        } else {
            e->pe = 0.0f;
        }
        if (e->pt->p[P_LRESET] >= 64)
            e->lfo_ph = 0.0f;
    }
    e->mod_cut = st ? bipn(st->mod) * 4.0f : 0.0f;   /* MOD: bis ±4 Oktaven zum Cutoff */
    e->gate = 1;
}

static void voice_off(ba_engine *e) { e->gate = 0; }

/* ---- der LFO: einmal pro Stück zu 8 Samples ---- */
static float lfo_step(ba_engine *e, int c)
{
    const uint8_t *p = e->pt->p;
    int sh = ba_list_index(P_LSHAPE, p[P_LSHAPE]);
    float hz, ph, v, sub;
    if (p[P_LSYNC] >= 64) {              /* im Tempo: RATE wählt die Länge eines Zyklus */
        int i = (p[P_LRATE] * (BA_NSYNC - 1) + 63) / 127;
        hz = ba_bpm(e) / 60.0f / BA_SYNC_BEATS[i];
    } else {
        hz = ba_lfo_hz(p[P_LRATE]);
    }
    e->lfo_ph += hz * (float)c / e->sr;
    if (e->lfo_ph >= 1.0f) {
        e->lfo_ph -= (float)(int)e->lfo_ph;
        e->lfo_sh = rnd_white(e);        /* S&H: ein neuer Wert pro Zyklus */
    }
    ph = e->lfo_ph;
    switch (sh) {
    case LS_SIN: v = 1.0f - 4.0f * ba_abs(ph - 0.5f); v = v * (1.5f - 0.5f * v * v); break;
    case LS_TRI: v = 1.0f - 4.0f * ba_abs(ph - 0.5f); break;
    case LS_SAW: v = 1.0f - 2.0f * ph; break;
    case LS_RAMP: v = 2.0f * ph - 1.0f; break;
    case LS_SQR: v = ph < 0.5f ? 1.0f : -1.0f; break;
    case LS_3DEC:                        /* drei fallende Kurven pro Zyklus */
        sub = ph * 3.0f;
        sub -= (float)(int)sub;
        v = (1.0f - sub) * (1.0f - sub) * (1.0f - sub) * 2.0f - 1.0f;
        break;
    case LS_SAWTRI:                      /* erst eine Säge, dann ein Dreieck */
        v = ph < 0.5f ? 1.0f - 4.0f * ph : 1.0f - 4.0f * ba_abs(2.0f * ph - 1.5f);
        break;
    case LS_3RAMP: {                     /* drei steigende Rampen, jede höher */
        int k;
        sub = ph * 3.0f;
        k = (int)sub;
        sub -= (float)k;
        v = sub * (float)(k + 1) / 1.5f - 1.0f;
        break;
    }
    case LS_SH: v = e->lfo_sh; break;
    default:                             /* NOISE: geglättetes Rauschen, so schnell wie RATE */
        e->lfo_tgt += (rnd_white(e) - e->lfo_tgt) * ba_clamp(hz * (float)c / e->sr * 6.0f, 0.001f, 1.0f);
        v = ba_clamp(e->lfo_tgt * 2.5f, -1.0f, 1.0f);
        break;
    }
    e->lfo_out = v;
    return v;
}

/* ---- die Stimme: n Samples, in Stücken zu 8 ---- */
static void voice_render(ba_engine *e, float *out, int n)
{
    int i, os = e->eco ? 1 : 2;
    float inv = 1.0f / ((float)os * e->sr), fmax = ba_clamp(0.22f * (float)os * e->sr, 1000.0f, 18000.0f);
    float srs = e->sr * (float)os;
    if (e->dirty)
        voice_update(e);
    for (i = 0; i < n; i += 8) {
        int j, c = n - i < 8 ? n - i : 8;
        float lfo = lfo_step(e, c), pitch, f1, inc1, inc2, cut, res, r, g = 0.0f, comp = 1.0f, mixg;
        float det = e->u_det, dmix, drate, ddep, dbase, drv = e->u_drive, drv_g, amp_base;
        /* Glide / Slide und PITCH ENV, einmal pro Stück */
        e->pitch = e->pitch_to + (e->pitch - e->pitch_to) * e->glide_coef;
        e->pe *= e->pe_coef;
        e->drift += 2e-4f * rnd_white(e) - 2e-3f * e->drift;
        pitch = e->pitch + e->bend + lfo * e->u_fm + e->pe * e->pe_amt;
        f1 = e->tune_hz * ba_exp2((pitch - 69.0f) * (1.0f / 12.0f) + e->drift);
        inc1 = ba_clamp(f1 * inv, 0.0f, 0.45f);
        inc2 = inc1 * 0.5f;              /* SUB: eine Oktave tiefer */
        /* DETUNE: weiter rechts mehr Mischung und ein schnellerer innerer LFO; ganz rechts wird es FM */
        dmix = ba_clamp(det * 2.2f, 0.0f, 1.0f) * 0.85f;
        drate = 0.3f * ba_exp2(det * 7.0f);
        ddep = (0.0015f + det * 0.002f) * srs;
        dbase = 0.005f * srs;
        mixg = 0.52f / (1.0f + 0.55f * e->u_sub + 0.35f * e->u_noise + 0.5f * dmix);
        /* das Filter: Cutoff, Hüllkurve, ACCENT, MOD-Spur, LFO */
        cut = e->u_cut + e->mod_cut + lfo * e->u_lfolvl + e->acc * 0.35f;
        res = ba_clamp(e->u_res + e->acc * 0.04f, 0.0f, 1.0f);
        r = 1.0f - 0.99f * res * (2.0f - res);
        amp_base = 1.0f + e->acc * 0.4f;     /* ACCENT: der VCA öffnet etwas weiter */
        drv_g = 1.0f + drv * drv * 14.0f;
        for (j = 0; j < c; j++) {
            float y = 0.0f, ya = 0.0f, nz = rnd_white(e), amp;
            int sub;
            /* die Hüllkurven: VCF nur Decay; VCA Sustain-Release nach dem Gate (ganz rechts: offen) */
            e->fenv *= e->fdec_coef;
            if (e->fenv < 1e-6f)
                e->fenv = 0.0f;
            if (e->u_vrel < 0.0f)
                e->venv = 1.0f;
            else if (e->gate)
                e->venv += (1.0f - e->venv) * 0.03f;   /* ~1 ms Anstieg, gegen Knacksen */
            else {
                e->venv *= e->vrel_coef;
                if (e->venv < 1e-6f)
                    e->venv = 0.0f;
            }
            if (!(j & 1)) {
                float fc = 20.0f * ba_exp2(cut + e->fenv * (e->u_fdep + e->acc * 1.2f));
                fc = ba_clamp(fc, 15.0f, fmax);
                g = svf_g(fc * inv);
                comp = 1.0f + (1.0f - r) * (e->bp ? 0.6f : 0.3f);
            }
            for (sub = 0; sub < os; sub++) {
                float ph1 = osc_step(&e->o1, inc1), ph2 = osc_step(&e->o2, inc2), v, d1 = ba_clamp(inc1, 1e-6f, 0.5f);
                v = vco(ph1, d1, e->wave);
                if (dmix > 0.001f) {     /* die Eimerkette: eine Kopie, deren Verzögerung schwingt (Tonhöhe schiebt sich) */
                    float rd, fr, a, b, wet;
                    int ri;
                    e->bbd[e->bbd_w] = v;
                    e->bbd_ph += drate / srs;
                    if (e->bbd_ph >= 1.0f)
                        e->bbd_ph -= 1.0f;
                    rd = dbase + ddep * (1.0f - 4.0f * ba_abs(e->bbd_ph - 0.5f)) * 0.5f;
                    ri = (int)rd;
                    fr = rd - (float)ri;
                    a = e->bbd[(e->bbd_w - ri + BA_BBD_N) & (BA_BBD_N - 1)];
                    b = e->bbd[(e->bbd_w - ri - 1 + BA_BBD_N) & (BA_BBD_N - 1)];
                    wet = a + (b - a) * fr;
                    e->bbd_lp += (wet - e->bbd_lp) * 0.35f;   /* eine BBD klingt dunkler */
                    v += e->bbd_lp * dmix;
                    e->bbd_w = (e->bbd_w + 1) & (BA_BBD_N - 1);
                }
                v += square(ph2, ba_clamp(inc2, 1e-6f, 0.5f)) * e->u_sub * 0.95f + nz * e->u_noise;
                y = svf_run(&e->vcf, v * mixg * comp * 1.4f + 1e-9f, g, r, e->bp);
                if (!sub)
                    ya = y;
            }
            if (os == 2)
                y = hb_push(e, ya, y);
            if (drv > 0.002f) {          /* OVERDRIVE: vom trockenen Klang zur Übersteuerung überblenden */
                float od = ba_soft(y * drv_g * 2.2f + 0.15f) - ba_soft(0.15f);
                y = y * (1.0f - drv) + od * drv * 0.62f;
            }
            amp = e->venv * amp_base;
            y *= amp;
            {   /* DC-Blocker */
                float d = y - e->dc_x + 0.995f * e->dc_y;
                e->dc_x = y;
                e->dc_y = ba_abs(d) < 1e-15f ? 0.0f : d;
                out[i + j] = d;
            }
        }
        e->level_now = e->venv;
        e->cut_hz = 20.0f * ba_exp2(ba_clamp(cut + e->fenv * e->u_fdep, 0.0f, 10.0f));
    }
}
