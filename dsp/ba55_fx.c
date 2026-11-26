/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (BA-55) */
/* BA-55 – die FX nach der Stimme: eine Filterbank nach der Erica Synths Resonant Filterbank (zehn Bandpässe auf
 * festen Frequenzen von 29 Hz bis 11 kHz, jeder mit eigenem Pegel, eine bipolare Resonanz als Rückkopplung in jedes
 * Band, eine Eingangsstufe bis +24 dB, Stereo-Spreizung, Modulation: LFO, Takt oder Hüllkurvenfolger), danach ein
 * Reverb (sechs Kämme und vier Allpässe je Seite, ein Pre-Delay, das mit der Größe wächst). Nur der Bass geht
 * hindurch; die GROOVE-Drums kommen danach. Teil von ba55_core.c */

static const int RV_COMB[6] = {1116, 1188, 1277, 1356, 1422, 1491}, RV_AP[4] = {556, 441, 341, 225};
#define RV_SPREAD 23
#define RV_PRE_MAX 1800

void ba_fx_memory(ba_engine *e, float *rv)
{
    int i, at = 0;
    e->rv = 0;
    if (!rv)
        return;
    for (i = 0; i < 12; i++) {           /* Kämme 0..5 links, 6..11 rechts (etwas länger: Stereo) */
        e->rv_clen[i] = RV_COMB[i % 6] + (i >= 6 ? RV_SPREAD : 0);
        e->rv_comb[i] = rv + at;
        at += e->rv_clen[i];
    }
    for (i = 0; i < 8; i++) {
        e->rv_alen[i] = RV_AP[i & 3] + (i >= 4 ? RV_SPREAD : 0);
        e->rv_ap[i] = rv + at;
        at += e->rv_alen[i];
    }
    e->rv_pre = rv + at;
    at += RV_PRE_MAX;
    e->rv_ch[0] = rv + at;
    e->rv_ch[1] = rv + at + BA_RV_CH;
    at += 2 * BA_RV_CH;
    if (at > BA_RV_FLOATS)
        return;
    for (i = 0; i < at; i++)
        rv[i] = 0.0f;
    e->rv = rv;
}

float ba_band_level(const ba_engine *e, int b) { return b >= 0 && b < BA_NBANDS ? ba_clamp(e->fb_lvl[b], 0.0f, 1.0f) : 0.0f; }
float ba_band_gain(const ba_engine *e, int b) { return b >= 0 && b < BA_NBANDS ? e->fb_g[b] : 0.0f; }
float ba_reverb_level(const ba_engine *e) { return ba_clamp(e->rv_lvl, 0.0f, 1.0f); }
float ba_reverb_duck(const ba_engine *e) { return ba_clamp(e->rv_duck, 0.0f, 1.0f); }

/* das gewählte Effekt-Preset, und was AMOUNT (wie stark) und MOTION (wie bewegt) daraus machen */
static const ba_fxpreset *fx_pre(const ba_engine *e) { return &BA_FXPRESET[ba_list_index(P_FX, e->pt->p[P_FX])]; }
static float fx_amount(const ba_engine *e) { return nrm(e->pt->p[P_FXAMT]); }
static float fx_motion(const ba_engine *e) { return nrm(e->pt->p[P_FXMOT]); }

/* die Pegel der Bänder für dieses Stück, mit der Bewegung */
static void fb_gains(ba_engine *e, int n, float in_env)
{
    const ba_fxpreset *fp = fx_pre(e);
    int mode = fp->mod, b;
    float m = fx_motion(e), depth = m * (float)fp->depth * 0.01f, pos = 0.0f;
    if (m < 0.01f)
        mode = FM_OFF;                   /* MOTION ganz links: der Effekt steht still */
    if (mode == FM_LFO) {                /* eine Welle läuft durch die Bänder: mit MOTION von 0.1 bis 10 Hz */
        e->fb_ph += 0.1f * ba_exp2(m * 6.6f) * (float)n / e->sr;
        e->fb_ph -= (float)(int)e->fb_ph;
    } else if (mode == FM_CLOCK) {       /* ein Lichtpunkt springt im Takt: mit MOTION von 1 Takt bis 1/32 pro Band */
        static const float DIV[6] = {4.0f, 2.0f, 1.0f, 0.5f, 0.25f, 0.125f};
        float beats = DIV[(int)(m * 5.0f + 0.5f)];
        e->fb_ph += ba_bpm(e) / 60.0f / beats * (float)n / e->sr;
        if (e->fb_ph >= (float)BA_NBANDS)
            e->fb_ph -= (float)BA_NBANDS;
    } else if (mode == FM_ENV) {         /* der Hüllkurvenfolger: laut öffnet die Bänder; MOTION: schneller zurück */
        float rel = ba_expf(-(float)n / ((0.03f + 0.5f * (1.0f - m)) * e->sr));
        e->fb_env = in_env > e->fb_env ? e->fb_env + (in_env - e->fb_env) * 0.5f : e->fb_env * rel + in_env * (1.0f - rel);
        pos = ba_clamp(e->fb_env * 3.0f, 0.0f, 1.0f) * (float)BA_NBANDS;
    }
    for (b = 0; b < BA_NBANDS; b++) {
        float v = (float)fp->band[b] / 127.0f, g = v * v * 2.0f, k = 1.0f;
        switch (mode) {
        case FM_LFO: {
            float w = e->fb_ph + (float)b * 0.1f, tri;
            w -= (float)(int)w;
            tri = 1.0f - 4.0f * ba_abs(w - 0.5f);
            tri = tri * (1.5f - 0.5f * tri * tri);
            k = 1.0f - depth * (0.5f - 0.5f * tri);
            break;
        }
        case FM_CLOCK: k = b == (int)e->fb_ph ? 1.0f + depth * 0.6f : 1.0f - depth * 0.9f; break;
        case FM_ENV: k = 1.0f - depth * ba_clamp((float)b + 1.0f - pos, 0.0f, 1.0f); break;
        default: break;
        }
        e->fb_g[b] = g * k;
    }
}

/* ---- die zweite FX-Seite: was mit dem Hall passiert, bevor er zum Bass kommt ----
 * DRIVE zerrt ihn (wenig: warm, viel: eine kaputte Fahne), FILTER (bipolar: links dunkel, rechts dünn, Mitte aus,
 * 12 dB), MOD lässt ihn leiern wie ein Band (eine Verzögerung, deren Länge schwingt; weit auf ein zweiter Abgriff
 * gegenläufig: Chorus, Flanger), DUCK drückt ihn weg: mit GROOVE bei jeder Kick, sonst solange der Bass spielt, und
 * lässt ihn in den Pausen aufblühen */
typedef struct {
    float drv, dnorm, lpg, hpg, mdep, mrate, m2, duck, datk, drel, kick_t;
    int filt, kick;
    const float *in_l, *in_r;
} rv_pp;

static float onepole_g(float hz, float sr) { float w = BA_PI * ba_clamp(hz / sr, 0.0f, 0.45f); return w / (1.0f + w); }

static void rv_post_setup(ba_engine *e, rv_pp *pp, const float *l, const float *r, int n)
{
    float fv = (float)e->pt->p[P_RVFILT] - 64.0f, dv = nrm(e->pt->p[P_RVDRV]), mv = nrm(e->pt->p[P_RVMOD]);
    float uv = nrm(e->pt->p[P_RVDUCK]);
    (void)n;
    pp->filt = fv < -1.5f ? -1 : fv > 1.5f ? 1 : 0;
    pp->lpg = onepole_g(fv < 0.0f ? 18000.0f * ba_exp2(fv / 63.0f * 6.0f) : 20000.0f, e->sr);       /* bis ~280 Hz */
    pp->hpg = onepole_g(fv > 0.0f ? 20.0f * ba_exp2(fv / 63.0f * 7.3f) : 20.0f, e->sr);             /* bis ~3,2 kHz */
    pp->drv = 1.0f + dv * dv * 30.0f;
    pp->dnorm = 1.0f / (1.0f + dv * 2.5f);
    pp->mdep = mv * mv * 380.0f;         /* Samples: bis ~9 ms Hub */
    pp->mrate = (0.25f + 1.6f * mv) / e->sr;
    pp->m2 = mv > 0.6f ? (mv - 0.6f) * 2.5f : 0.0f;   /* weit auf: der zweite Abgriff */
    pp->duck = uv;
    pp->datk = 1.0f - ba_expf(-1.0f / (0.004f * e->sr));
    pp->drel = 1.0f - ba_expf(-1.0f / ((0.05f + 0.22f * uv) * e->sr));
    pp->kick = e->gv && e->gv->on && e->running;
    pp->kick_t = e->dr.sc_t;
    pp->in_l = l;
    pp->in_r = r;
}

static float rv_tap(const float *b, int w, float d)   /* d Samples zurück, linear dazwischen */
{
    int i0 = (int)d;
    float fr = d - (float)i0, a = b[(w - i0 + 2 * BA_RV_CH) % BA_RV_CH], c = b[(w - i0 - 1 + 2 * BA_RV_CH) % BA_RV_CH];
    return a + (c - a) * fr;
}

static void rv_post(ba_engine *e, float *sl, float *sr, rv_pp *pp, int i)
{
    int k;
    float target = 0.0f;
    for (k = 0; k < 2; k++) {
        float *s = k ? sr : sl, x = *s;
        if (pp->drv > 1.01f)             /* DRIVE */
            x = ba_soft(x * pp->drv) * pp->dnorm * (1.0f + 0.15f * (pp->drv - 1.0f) / 30.0f);
        if (pp->filt < 0) {              /* FILTER: dunkel */
            e->rv_lp[k] += pp->lpg * (x - e->rv_lp[k]);
            e->rv_lp2[k] += pp->lpg * (e->rv_lp[k] - e->rv_lp2[k]);
            x = e->rv_lp2[k];
        } else if (pp->filt > 0) {       /* FILTER: dünn */
            e->rv_hp[k] += pp->hpg * (x - e->rv_hp[k]);
            x -= e->rv_hp[k];
            e->rv_hp2[k] += pp->hpg * (x - e->rv_hp2[k]);
            x -= e->rv_hp2[k];
        }
        if (pp->mdep > 0.5f) {           /* MOD: die Bandleier, links und rechts gegenläufig */
            float ph = e->rv_mph + (k ? 0.5f : 0.0f), t, d;
            ph -= (float)(int)ph;
            t = ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph;
            d = 8.0f + pp->mdep * (1.0f + t);
            e->rv_ch[k][e->rv_cw] = x;
            x = rv_tap(e->rv_ch[k], e->rv_cw, d) * (1.0f - 0.4f * pp->m2)
                + rv_tap(e->rv_ch[k], e->rv_cw, 8.0f + pp->mdep * (1.0f - t)) * 0.6f * pp->m2;
        }
        *s = x;
    }
    if (pp->mdep > 0.5f) {
        if (++e->rv_cw >= BA_RV_CH)
            e->rv_cw = 0;
        e->rv_mph += pp->mrate;
        if (e->rv_mph >= 1.0f)
            e->rv_mph -= 1.0f;
    }
    if (pp->duck > 0.005f) {             /* DUCK: mit GROOVE die Kick (gleich nach ihr ganz weg), sonst der Bass */
        if (pp->kick)
            target = pp->kick_t + (float)i < 0.03f * e->sr ? 1.0f : 0.0f;
        else {
            float a = ba_abs(pp->in_l[i]) + ba_abs(pp->in_r[i]);
            target = ba_clamp(a * 4.0f, 0.0f, 1.0f);
        }
        e->rv_duck += (target > e->rv_duck ? pp->datk : pp->drel) * (target - e->rv_duck);
        {
            float g = 1.0f - 0.97f * pp->duck * e->rv_duck;
            *sl *= g;
            *sr *= g;
        }
    }
}

static void fx_render(ba_engine *e, const float *in, float *l, float *r, int n)
{
    const ba_fxpreset *fp = fx_pre(e);
    float amt = fx_amount(e), fmix = ba_clamp(amt * 1.6f, 0.0f, 1.0f) * (float)fp->mix * 0.01f;
    float rmix = nrm(e->pt->p[P_RVMIX]);
    int i;
    for (i = 0; i < n; i++)
        l[i] = r[i] = in[i];
    /* ---- die Filterbank ---- */
    if (fmix > 0.002f) {
        float drv = amt * (float)fp->drive * 0.01f, gin = 1.0f + drv * drv * 15.0f;   /* bis +24 dB */
        float k = (float)fp->reso * 0.01f * (0.3f + 0.7f * amt), spr = (float)fp->spread * 0.009f;
        float gg[BA_NBANDS], rr[BA_NBANDS], pan[BA_NBANDS];
        float env = 0.0f, lv[BA_NBANDS];
        int b;
        k = k * ba_abs(k) * 0.985f;      /* fein um die Mitte, ganz außen fast endlos */
        for (i = 0; i < n; i++)
            env = ba_abs(in[i]) > env ? ba_abs(in[i]) : env;
        fb_gains(e, n, env);
        for (b = 0; b < BA_NBANDS; b++) {
            gg[b] = svf_g(ba_clamp(BA_BAND_HZ[b] / e->sr, 0.0f, 0.24f));
            rr[b] = 0.36f;               /* etwa eine Oktave breit, wie die festen Filter des Originals */
            pan[b] = (b & 1 ? 1.0f : -1.0f) * spr * (0.4f + 0.06f * (float)b);
            lv[b] = 0.0f;
        }
        for (i = 0; i < n; i++) {
            float x = ba_soft(in[i] * gin) * (1.0f / (1.0f + drv * 1.6f)), wl = 0.0f, wr = 0.0f;
            for (b = 0; b < BA_NBANDS; b++) {
                float g = gg[b], rd = rr[b], u, hp, bp, y;
                u = x + k * ba_soft(e->fb_y[b] * 1.4f) * (1.0f / 1.4f);   /* die Rückkopplung ins Band */
                hp = (u - (2.0f * rd + g) * e->fb_s1[b] - e->fb_s2[b]) / (1.0f + 2.0f * rd * g + g * g);
                bp = g * hp + e->fb_s1[b];
                e->fb_s1[b] = g * hp + bp;
                e->fb_s2[b] = g * bp + e->fb_s2[b] + g * bp;
                if (ba_abs(e->fb_s1[b]) < 1e-15f)
                    e->fb_s1[b] = 0.0f;
                if (ba_abs(e->fb_s2[b]) < 1e-15f)
                    e->fb_s2[b] = 0.0f;
                y = bp * 2.0f * rd;      /* auf 1 in der Mitte des Bands */
                e->fb_y[b] = y;
                y *= e->fb_g[b];
                wl += y * (1.0f - pan[b]);
                wr += y * (1.0f + pan[b]);
                if (ba_abs(y) > lv[b])
                    lv[b] = ba_abs(y);
            }
            wl = ba_soft(wl * 0.55f) * 1.1f;
            wr = ba_soft(wr * 0.55f) * 1.1f;
            l[i] = in[i] * (1.0f - fmix) + wl * fmix;
            r[i] = in[i] * (1.0f - fmix) + wr * fmix;
        }
        for (b = 0; b < BA_NBANDS; b++)  /* für die Anzeige: schnell hoch, langsam herunter */
            e->fb_lvl[b] = lv[b] > e->fb_lvl[b] ? lv[b] : e->fb_lvl[b] * 0.93f;
    } else {
        int b;
        for (b = 0; b < BA_NBANDS; b++)
            e->fb_lvl[b] *= 0.9f;
        fb_gains(e, n, 0.0f);
    }
    /* ---- der Reverb ---- */
    if (e->rv && rmix > 0.002f) {
        /* REVERB: bis zur Mitte ein normaler Hall im Raum des Effekts (bis 35 % nass); ab der Mitte (ex) wird er
         * experimentell: der Raum wächst, der Nachhall wird fast endlos, die Kammlängen schwimmen (der Hall leiert
         * und schimmert), er wird heller und dreckiger und drängt den trockenen Bass zurück */
        float ex = rmix > 0.5f ? (rmix - 0.5f) * 2.0f : 0.0f;
        float wet = rmix < 0.5f ? rmix * 0.7f : 0.35f + 0.65f * ex, dry = 1.0f - 0.55f * wet - 0.25f * ex;
        float sz = (float)fp->rv_size * 0.01f, size = 0.45f + 0.55f * sz, fb = 0.70f + 0.285f * (float)fp->rv_decay * 0.01f;
        float damp = 0.62f - 0.57f * (float)fp->rv_tone * 0.01f, pk = 0.0f, grit = 1.0f + 3.0f * ex * ex;
        int len[12], c, pre;
        rv_pp pp;
        size += (1.0f - size) * ex;
        fb += (0.992f - fb) * ex;
        damp += (0.12f - damp) * ex;
        pre = (int)((0.004f + 0.036f * sz) * e->sr + (float)(RV_PRE_MAX - 400) * ex * ex);
        if (pre >= RV_PRE_MAX)
            pre = RV_PRE_MAX - 1;
        e->rv_ph += (0.13f + 0.5f * ex) * (float)n / e->sr;
        e->rv_ph -= (float)(int)e->rv_ph;
        for (c = 0; c < 12; c++) {       /* ab der Mitte schwimmen die Kämme, jeder mit eigener Phase */
            float q = e->rv_ph + (float)c * 0.137f, w;
            q -= (float)(int)q;
            w = q < 0.5f ? 2.0f * q : 2.0f - 2.0f * q;   /* ein Dreieck 0..1 */
            len[c] = (int)((float)e->rv_clen[c] * size * (1.0f - 0.035f * ex * w));
            if (len[c] < 8)
                len[c] = 8;
        }
        rv_post_setup(e, &pp, l, r, n);
        for (i = 0; i < n; i++) {
            float x = (l[i] + r[i]) * 0.5f, d, sl = 0.0f, sr2 = 0.0f;
            int a;
            e->rv_pre[e->rv_pre_pos] = x;
            d = e->rv_pre[(e->rv_pre_pos - pre + RV_PRE_MAX) % RV_PRE_MAX];
            if (++e->rv_pre_pos >= RV_PRE_MAX)
                e->rv_pre_pos = 0;
            d *= 0.03f;
            for (c = 0; c < 6; c++) {    /* die Kämme */
                int k2;
                for (k2 = 0; k2 < 2; k2++) {
                    int cc = c + 6 * k2;
                    float y;
                    if (e->rv_cpos[cc] >= len[cc])
                        e->rv_cpos[cc] = 0;
                    y = e->rv_comb[cc][e->rv_cpos[cc]];
                    e->rv_cf[cc] = y * (1.0f - damp) + e->rv_cf[cc] * damp + 1e-20f;
                    e->rv_comb[cc][e->rv_cpos[cc]] = d + e->rv_cf[cc] * fb;
                    if (++e->rv_cpos[cc] >= len[cc])
                        e->rv_cpos[cc] = 0;
                    if (k2)
                        sr2 += y;
                    else
                        sl += y;
                }
            }
            for (a = 0; a < 4; a++) {    /* die Allpässe */
                int k2;
                for (k2 = 0; k2 < 2; k2++) {
                    int aa = a + 4 * k2;
                    float *s = k2 ? &sr2 : &sl, bb = e->rv_ap[aa][e->rv_apos[aa]], y = bb - *s;
                    e->rv_ap[aa][e->rv_apos[aa]] = *s + bb * 0.5f + 1e-20f;
                    if (++e->rv_apos[aa] >= e->rv_alen[aa])
                        e->rv_apos[aa] = 0;
                    *s = y;
                }
            }
            sl *= 1.6f;
            sr2 *= 1.6f;
            if (ba_abs(sl) > pk)
                pk = ba_abs(sl);
            if (ex > 0.0f) {             /* ab der Mitte: angezerrt, damit der endlose Hall nicht explodiert */
                sl = ba_soft(sl * grit) / grit * (1.0f + ex);
                sr2 = ba_soft(sr2 * grit) / grit * (1.0f + ex);
            }
            rv_post(e, &sl, &sr2, &pp, i);
            l[i] = l[i] * dry + sl * wet;
            r[i] = r[i] * dry + sr2 * wet;
        }
        e->rv_lvl = pk > e->rv_lvl ? pk : e->rv_lvl * 0.95f;
    } else {
        e->rv_lvl *= 0.9f;
    }
}
