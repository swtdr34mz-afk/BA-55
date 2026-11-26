/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (BA-55) */
/* BA-55 – die Stimme: zwei VCOs mit dem WAVE-Regler, der Ladder, die Hüllkurven, DETUNE. Teil von ba55_core.c */

/* ---- Oszillator: Dreieck, Säge, Rechteck, schmaler Puls (PolyBLEP); die Form stufenlos 0..3 ---- */
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
    else if (o->phase < 0.0f) {
        o->phase += 1.0f + (float)(int)(-o->phase);
        if (o->phase >= 1.0f)
            o->phase -= 1.0f;
    }
    return o->phase;
}

static inline float shape_one(float ph, float dt, int s)
{
    float y, t2, pw;
    switch (s) {
    case 0: return 1.0f - 4.0f * ba_abs(ph - 0.5f);   /* Dreieck (am 2x Oversampling sauber genug) */
    case 1: return 2.0f * ph - 1.0f - blep(ph, dt);   /* Säge */
    default:
        pw = s == 2 ? 0.5f : 0.16f;                  /* Rechteck, schmaler Puls */
        y = ph < pw ? 1.0f : -1.0f;
        y += blep(ph, dt);
        t2 = ph - pw;
        if (t2 < 0.0f)
            t2 += 1.0f;
        y -= blep(t2, dt);
        return y - (2.0f * pw - 1.0f);               /* ohne Gleichanteil */
    }
}

/* ---- das Filter: ein State-Variable-Filter (12 dB) im Geist des Polivoks / der Erica Acidbox: die beiden
 *      Integratoren sättigen (wie die übersteuerten Operationsverstärker im Original), dadurch kippt es bei hoher
 *      Resonanz in ein rauhes, stabiles Pfeifen statt zu explodieren. Tiefpass oder Bandpass. ---- */
static inline float svf_run(ba_svf *f, float x, float g, float r, int bp)
{
    float hp = (x - (2.0f * r + g) * f->s1 - f->s2) / (1.0f + 2.0f * r * g + g * g);
    float b = g * hp + f->s1, l;
    f->s1 = ba_soft(g * hp + b);
    l = g * b + f->s2;
    f->s2 = ba_soft(0.85f * (g * b + l)) * 1.1765f;
    return bp ? b : l;
}

/* tan(pi * x) für x bis ~0.24 (die Filterfrequenz bei doppelter Samplerate) */
static inline float svf_g(float x)
{
    float w = BA_PI * x, w2 = w * w;
    return w * (1.0f + w2 * (0.3333333f + w2 * (0.1333333f + w2 * 0.0539683f)));
}

/* der Halbband-Dezimierer (2x Oversampling zurück), aus RAND */
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

/* ---- ADSR: linearer Anstieg, exponentielles Abfallen; Neustart vom aktuellen Pegel (kein Knacksen) ---- */
static void adsr_set(ba_adsr *a, int va, int vd, int vs, int vr, float scale, float sr)
{
    float ta = ba_env_ms(va, 1) * 0.001f * scale, td = ba_env_ms(vd, 0) * 0.001f * scale;
    float tr = ba_env_ms(vr, 0) * 0.001f * scale;
    a->a_inc = 1.0f / (ba_clamp(ta, 0.0005f, 60.0f) * sr);
    a->d_coef = ba_expf(-6.9f / (ba_clamp(td, 0.001f, 90.0f) * sr));
    a->r_coef = ba_expf(-6.9f / (ba_clamp(tr, 0.001f, 90.0f) * sr));
    a->sus = (float)vs / 127.0f;
}

static inline float adsr_run(ba_adsr *a)
{
    switch (a->stage) {
    case 1:
        a->level += a->a_inc;
        if (a->level >= 1.0f) {
            a->level = 1.0f;
            a->stage = 2;
        }
        break;
    case 2:
        a->level = a->sus + (a->level - a->sus) * a->d_coef;
        if (a->level < 1e-6f)            /* (keine Denormals, wenn S = 0) */
            a->level = 0.0f;
        break;
    case 3:
        a->level *= a->r_coef;
        if (a->level < 1e-5f) {
            a->level = 0.0f;
            a->stage = 0;
        }
        break;
    default: break;
    }
    return a->level;
}

/* ---- umrechnen, wenn sich der Klang geändert hat ---- */
static float nrm(int v) { return (float)v * (1.0f / 127.0f); }
static float bipn(int v) { return ba_clamp(((float)v - 64.0f) / 63.0f, -1.0f, 1.0f); }

static void voice_update(ba_engine *e)
{
    const uint8_t *p = e->pt->p;
    int m, d;
    e->wave = ba_wave_index(p[P_WAVE]);
    e->u_sub = nrm(p[P_SUB]);
    e->u_noise = nrm(p[P_NOISE]);
    e->u_cut = nrm(p[P_CUTOFF]) * 10.0f;               /* Oktaven über 20 Hz */
    e->u_res = nrm(p[P_RESON]);
    e->u_egl = bipn(p[P_EGLVL]);
    e->bp = p[P_FMODE] >= 64;
    e->u_acc = nrm(p[P_ACCENT]);
    e->u_track = nrm(p[P_TRACK]);
    e->u_vca = nrm(p[P_VCALVL]);
    e->u_velcut = nrm(p[P_VELCUT]) * 3.0f;
    e->u_velvca = nrm(p[P_VELVCA]);
    e->u_tune = ba_tune_semis(p[P_TUNE]);
    e->u_penv = bipn(p[P_PENV]);
    e->u_penv *= ba_abs(e->u_penv) * 36.0f;            /* bis ±36 Halbtöne, fein um 0 */
    e->pe_coef = ba_expf(-6.9f / (ba_clamp(ba_env_ms(p[P_PDEC], 0) * 0.001f, 0.002f, 20.0f) * e->sr / 8.0f));
    e->u_glide = ba_glide_ms(p[P_GLIDE]);
    e->u_slide = 8.0f + nrm(p[P_SLIDE]) * nrm(p[P_SLIDE]) * 400.0f;   /* 8 .. 408 ms; die 303 hat ~60 */
    e->legato = p[P_LEGATO] >= 64;
    e->auto_glide = ba_list_index(P_GMODE, p[P_GMODE]) == 1;
    e->u_det = nrm(p[P_DETUNE]);
    e->u_drive = nrm(p[P_DRIVE]);
    e->u_hp = p[P_HPCUT] ? 0.01f + nrm(p[P_HPCUT]) * 10.0f : 0.0f;    /* 0: aus; sonst 20 Hz * 2^u (bis 7 kHz) */
    e->u_hpq = nrm(p[P_HPRES]);
    adsr_set(&e->fe, p[P_FA], p[P_FD], p[P_FS], p[P_FR], 1.0f, e->sr);
    adsr_set(&e->ae, p[P_AA], p[P_AD], p[P_AS], p[P_AR], 1.0f, e->sr);
    for (m = 0; m < 3; m++)
        for (d = 0; d < BA_NDEST; d++) {
            float l = bipn(e->pt->lvl[m][d]);
            if (d == D_CV)                   /* Tonhöhe: fein um 0 (Vibrato), bis ±24 Halbtöne am Rand */
                l = l * ba_abs(l);
            e->lvl[m][d] = l;
        }
    e->dirty = 0;
}

/* ---- Noten an die Stimme. slide: von der vorigen Note gleiten (SLIDE-Step); tie: ohne neuen Anschlag ---- */
static void mods_key(ba_engine *e);

static void voice_on(ba_engine *e, int note, float vel, int tie, int slide, int accent)
{
    int legato_now = e->gate && (e->legato || tie || slide);
    int glide = e->u_glide > 0.0f && (!e->auto_glide || e->gate);
    float ms = slide && e->gate ? e->u_slide : glide ? e->u_glide : 0.0f;
    e->pitch_to = (float)note;
    if (ms <= 0.0f || e->note < 0)
        e->pitch = e->pitch_to;
    e->glide_coef = ms > 0.0f ? ba_expf(-1.0f / (ms * 0.001f * e->sr / 8.0f * 0.25f + 1.0f)) : 0.0f;
    e->note = note;
    if (!legato_now) {
        e->vel = vel;
        e->acc = accent ? e->u_acc : 0.0f;
        if (accent)
            e->acc_env = 1.0f;
        e->fe.stage = e->ae.stage = 1;
        e->pe = 1.0f;
        mods_key(e);
    }
    e->gate = 1;
}

static void voice_off(ba_engine *e)
{
    e->gate = 0;
    if (e->fe.stage)
        e->fe.stage = 3;
    if (e->ae.stage)
        e->ae.stage = 3;
}

/* ---- die Stimme: n Samples, in Stücken zu 8 (die Modulatoren rechnen pro Stück) ---- */
static void mods_run(ba_engine *e, int n);

static inline float rnd_white(ba_engine *e)
{
    e->rs ^= e->rs << 13;
    e->rs ^= e->rs >> 17;
    e->rs ^= e->rs << 5;
    return (float)(int32_t)e->rs * (1.0f / 2147483648.0f);
}

static void voice_render(ba_engine *e, float *out, int n)
{
    int i, os = e->eco ? 1 : 2;
    float inv = 1.0f / ((float)os * e->sr), fmax = ba_clamp(0.22f * (float)os * e->sr, 1000.0f, 18000.0f);
    if (e->dirty)
        voice_update(e);
    for (i = 0; i < n; i += 8) {
        int j, c = n - i < 8 ? n - i : 8;
        const float *md = e->md;
        float pitch, f1, inc1, inc2, inc3, det, cut, res, r, amp_base, velg, egl, g = 0.0f, comp = 1.0f;
        float subl, noil, pw, dt, mixg, drv, drv_g, drv_c, accg;
        mods_run(e, c);
        /* Glide / Slide, die Tonhöhen-Hüllkurve: einmal pro Stück */
        e->pitch = e->pitch_to + (e->pitch - e->pitch_to) * e->glide_coef;
        e->pe *= e->pe_coef;
        e->acc_env *= 0.9965f;           /* der Accent-Stoß im Filter: ~200 ms, wie die 303 ihn über den Kondensator lädt */
        pitch = e->pitch + e->bend + e->u_tune + md[D_CV] * 24.0f + e->u_penv * e->pe;
        e->drift1 += 2e-4f * rnd_white(e) - 2e-3f * e->drift1;
        e->drift2 += 2e-4f * rnd_white(e) - 2e-3f * e->drift2;
        f1 = 440.0f * ba_exp2((pitch - 69.0f) * (1.0f / 12.0f) + e->drift1);
        inc1 = ba_clamp(f1 * inv, 0.0f, 0.45f);
        inc2 = inc1 * 0.5f;              /* der Sub: eine Oktave tiefer */
        /* DETUNE: eine Kopie, die wie durch eine Eimerkette (BBD) langsam schwimmt, bis ~40 Cent daneben */
        dt = ba_clamp(e->u_det + md[D_DET], 0.0f, 1.0f);
        e->bbd_ph += 0.8f * 8.0f / e->sr;
        if (e->bbd_ph >= 1.0f)
            e->bbd_ph -= 1.0f;
        e->bbd_wob += (rnd_white(e) - e->bbd_wob) * 0.004f;
        det = ba_exp2(dt * (0.022f + 0.012f * (1.0f - 4.0f * ba_abs(e->bbd_ph - 0.5f))) + dt * 0.006f * e->bbd_wob + e->drift2);
        inc3 = ba_clamp(inc1 * det, 0.0f, 0.45f);
        subl = ba_clamp(e->u_sub + md[D_SUB], 0.0f, 1.0f);
        noil = ba_clamp(e->u_noise + md[D_NOISE], 0.0f, 1.0f);
        pw = ba_clamp(0.5f - md[D_PW] * 0.45f, 0.05f, 0.95f);
        mixg = 0.55f / (1.0f + 0.5f * subl + 0.4f * noil + 0.6f * dt);
        /* das Filter: Cutoff, Key-Track, Hüllkurve, ACCENT, Velocity, Modulatoren */
        accg = e->acc;
        egl = ba_clamp(e->u_egl + md[D_EGL], -1.0f, 1.0f) * 7.0f;
        cut = e->u_cut + e->u_track * (pitch - 48.0f) * (1.0f / 12.0f) + e->u_velcut * (e->vel - 0.6f)
              + md[D_CUT] * 5.0f + accg * e->acc_env * 2.6f;
        res = ba_clamp(e->u_res + md[D_RES] + accg * 0.08f, 0.0f, 1.0f);
        r = 1.0f - 0.985f * res * (2.0f - res);   /* Dämpfung: 1 ganz weich .. 0.015 pfeift */
        velg = 1.0f - e->u_velvca + e->u_velvca * e->vel;
        amp_base = e->u_vca * (1.0f + accg * 0.9f);
        /* DRIVE: ein Knopf, vom warmen Andicken bis zum Zerreißen (asymmetrisch, wie eine übersteuerte Stufe) */
        drv = ba_clamp(e->u_drive + md[D_DRV], 0.0f, 1.0f);
        drv_g = ba_exp2(drv * 6.0f);
        drv_c = 1.0f / (1.0f + drv * 2.2f);
        for (j = 0; j < c; j++) {
            float fe = adsr_run(&e->fe), ae = adsr_run(&e->ae), amp, y = 0.0f, ya = 0.0f, nz = rnd_white(e);
            int sub;
            if (!(j & 1)) {                  /* das Filter alle zwei Samples neu (22 kHz Steuerrate) */
                float fc = 20.0f * ba_exp2(cut + egl * fe);
                fc = ba_clamp(fc, 15.0f, fmax);
                g = svf_g(fc * inv);
                comp = 1.0f + (1.0f - r) * (e->bp ? 0.9f : 0.35f);
            }
            for (sub = 0; sub < os; sub++) {
                float ph1 = osc_step(&e->o1, inc1), ph2 = osc_step(&e->o2, inc2), v, sq;
                float d1 = ba_clamp(inc1, 1e-6f, 0.5f);
                if (e->wave == 2) {          /* Rechteck mit PW */
                    float t2 = ph1 - pw;
                    v = ph1 < pw ? 1.0f : -1.0f;
                    v += blep(ph1, d1);
                    if (t2 < 0.0f)
                        t2 += 1.0f;
                    v -= blep(t2, d1);
                    v -= 2.0f * pw - 1.0f;
                } else
                    v = shape_one(ph1, d1, e->wave);
                if (dt > 0.005f) {
                    float ph3 = osc_step(&e->o3, inc3);
                    v += dt * 0.8f * shape_one(ph3, ba_clamp(inc3, 1e-6f, 0.5f), e->wave == 2 ? 1 : e->wave);
                }
                /* der Sub: ein Rechteck eine Oktave tiefer (Transistor-Teiler) */
                sq = (ph2 < 0.5f ? 1.0f : -1.0f) + blep(ph2, ba_clamp(inc2, 1e-6f, 0.5f));
                {
                    float t2 = ph2 - 0.5f;
                    if (t2 < 0.0f)
                        t2 += 1.0f;
                    sq -= blep(t2, ba_clamp(inc2, 1e-6f, 0.5f));
                }
                v += sq * subl * 0.9f + nz * noil;
                y = svf_run(&e->vcf, v * mixg * comp * 1.4f + 1e-9f, g, r, e->bp);   /* (+1e-9: keine Denormals) */
                if (!sub)
                    ya = y;
            }
            if (os == 2)
                y = hb_push(e, ya, y);
            if (drv > 0.002f) {
                float x = y * drv_g;
                y = (ba_soft(x + 0.18f * drv) - ba_soft(0.18f * drv)) * drv_c * 1.4f;
            }
            amp = ba_clamp(ae * velg + md[D_VCA], 0.0f, 1.5f) * amp_base;
            y *= amp;
            {   /* DC-Blocker */
                float d = y - e->dc_x + 0.995f * e->dc_y;
                e->dc_x = y;
                e->dc_y = ba_abs(d) < 1e-15f ? 0.0f : d;   /* (keine Denormals) */
                out[i + j] = d;
            }
        }
        e->level_now = e->ae.level;
        e->cut_hz = 20.0f * ba_exp2(ba_clamp(cut + egl * e->fe.level, 0.0f, 10.0f));
    }
}
