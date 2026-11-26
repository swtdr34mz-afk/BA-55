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
    if (at > BA_RV_FLOATS)
        return;
    for (i = 0; i < at; i++)
        rv[i] = 0.0f;
    e->rv = rv;
}

float ba_band_level(const ba_engine *e, int b) { return b >= 0 && b < BA_NBANDS ? ba_clamp(e->fb_lvl[b], 0.0f, 1.0f) : 0.0f; }
float ba_band_gain(const ba_engine *e, int b) { return b >= 0 && b < BA_NBANDS ? e->fb_g[b] : 0.0f; }
float ba_reverb_level(const ba_engine *e) { return ba_clamp(e->rv_lvl, 0.0f, 1.0f); }

/* die Pegel der Bänder für dieses Stück, mit der Modulation */
static void fb_gains(ba_engine *e, int n, float in_env)
{
    const uint8_t *p = e->pt->p;
    int mode = ba_list_index(P_FBMOD, p[P_FBMOD]), b;
    float depth = nrm(p[P_FBDEPTH]), rate = nrm(p[P_FBRATE]);
    float pos = 0.0f;
    if (mode == FM_LFO) {                /* eine Welle läuft durch die Bänder: 0.05 .. 12 Hz */
        e->fb_ph += 0.05f * ba_exp2(rate * 7.9f) * (float)n / e->sr;
        e->fb_ph -= (float)(int)e->fb_ph;
    } else if (mode == FM_CLOCK) {       /* ein Lichtpunkt springt im Takt von Band zu Band: 1 Takt .. 1/32 pro Band */
        static const float DIV[6] = {4.0f, 2.0f, 1.0f, 0.5f, 0.25f, 0.125f};
        float beats = DIV[(int)(rate * 5.0f + 0.5f)];
        e->fb_ph += ba_bpm(e) / 60.0f / beats * (float)n / e->sr;
        if (e->fb_ph >= (float)BA_NBANDS)
            e->fb_ph -= (float)BA_NBANDS;
        if (!e->running && e->fb_ph < 0.0f)
            e->fb_ph = 0.0f;
    } else if (mode == FM_ENV) {         /* der Hüllkurvenfolger: laut öffnet die Bänder von unten nach oben */
        float rel = ba_expf(-(float)n / ((0.02f + 0.6f * (1.0f - rate)) * e->sr));
        e->fb_env = in_env > e->fb_env ? e->fb_env + (in_env - e->fb_env) * 0.5f : e->fb_env * rel + in_env * (1.0f - rel);
        pos = ba_clamp(e->fb_env * 3.0f, 0.0f, 1.0f) * (float)BA_NBANDS;
    }
    for (b = 0; b < BA_NBANDS; b++) {
        float v = nrm(p[P_FB1 + b]), g = v * v * 2.0f, m = 1.0f;   /* 90 = 0 dB, 127 = +6 dB, 0 = zu */
        switch (mode) {
        case FM_LFO: {
            float w = e->fb_ph + (float)b * 0.1f, tri;
            w -= (float)(int)w;
            tri = 1.0f - 2.0f * ba_abs(w - 0.5f) * 2.0f;  /* -1..1 */
            tri = tri * (1.5f - 0.5f * tri * tri);
            m = 1.0f - depth * (0.5f - 0.5f * tri);
            break;
        }
        case FM_CLOCK: {
            int cur = (int)e->fb_ph;
            m = b == cur ? 1.0f + depth * 0.6f : 1.0f - depth * 0.9f;
            break;
        }
        case FM_ENV: m = 1.0f - depth * ba_clamp((float)b + 1.0f - pos, 0.0f, 1.0f); break;
        default: break;
        }
        e->fb_g[b] = g * m;
    }
}

static void fx_render(ba_engine *e, const float *in, float *l, float *r, int n)
{
    const uint8_t *p = e->pt->p;
    float fmix = nrm(p[P_FBMIX]), rmix = nrm(p[P_RVMIX]);
    int i;
    for (i = 0; i < n; i++)
        l[i] = r[i] = in[i];
    /* ---- die Filterbank ---- */
    if (fmix > 0.002f) {
        float drv = nrm(p[P_FBDRIVE]), gin = 1.0f + drv * drv * 15.0f;   /* bis +24 dB */
        float k = bipn(p[P_FBRESO]), spr = nrm(p[P_FBSPREAD]) * 0.9f, gg[BA_NBANDS], rr[BA_NBANDS], pan[BA_NBANDS];
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
        float size = 0.45f + 0.55f * nrm(p[P_RVSIZE]), fb = 0.70f + 0.285f * nrm(p[P_RVDECAY]);
        float damp = 0.62f - 0.57f * nrm(p[P_RVTONE]), pk = 0.0f;
        int len[12], c, pre = (int)((0.004f + 0.036f * nrm(p[P_RVSIZE])) * e->sr);
        if (pre >= RV_PRE_MAX)
            pre = RV_PRE_MAX - 1;
        for (c = 0; c < 12; c++)
            len[c] = (int)((float)e->rv_clen[c] * size);
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
            l[i] = l[i] * (1.0f - 0.55f * rmix) + sl * rmix;
            r[i] = r[i] * (1.0f - 0.55f * rmix) + sr2 * rmix;
        }
        e->rv_lvl = pk > e->rv_lvl ? pk : e->rv_lvl * 0.95f;
    } else {
        e->rv_lvl *= 0.9f;
    }
}
