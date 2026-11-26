/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (BA-55) */
/* BA-55 – jeder Parameter von ganz links nach ganz rechts: ändert sich der Klang? (Pegel und Helligkeit)
 *   cc -O2 -Idsp dsp/ba55_core.c tools/knobcheck.c -lm -o knobcheck && ./knobcheck */
#include "ba55_core.h"
#include <math.h>
#include <stdio.h>

#define SR 44100
static ba_engine E;
static ba_patch P;

/* eine Note, 0.8 s: Pegel (dB) und Helligkeit (Anteil der Differenzen: grob die Höhen) */
static void measure(const ba_patch *src, int seq, double *db, double *hi, double *wid)
{
    int i, j;
    double s = 0, d = 0, side = 0;
    float pl = 0;
    P = *src;
    ba_init(&E, SR, &P);
    if (seq)
        ba_play(&E, 1);
    else
        ba_note_on(&E, 48, 100);
    for (i = 0; i < SR * 8 / 10; i += 128) {
        float l[128], r[128];
        if (i / 128 == SR / 2 / 128 && !seq)
            ba_note_off(&E, 48);
        ba_render(&E, l, r, 128);
        for (j = 0; j < 128; j++) {
            float m = 0.5f * (l[j] + r[j]);
            s += m * m;
            d += (m - pl) * (m - pl);
            side += 0.25 * (l[j] - r[j]) * (l[j] - r[j]);
            pl = m;
        }
    }
    *db = 10 * log10(s / (SR * 0.8) + 1e-12);
    *hi = s > 0 ? d / s : 0;
    *wid = 10 * log10(side / (SR * 0.8) + 1e-12);
}

int main(void)
{
    int id, dead = 0;
    ba_patch base;
    ba_patch_init(&base);
    base.p[P_LFOLVL] = 60;               /* der LFO wirkt, damit seine Regler etwas ändern */
    for (id = 0; id < BA_NP; id++) {
        ba_patch a = base, b = base;
        double da, ha, wa, db, hb, wb;
        a.p[id] = 0;
        b.p[id] = 127;
        measure(&a, id >= P_LAST, &da, &ha, &wa);
        measure(&b, id >= P_LAST, &db, &hb, &wb);
        if (fabs(da - db) < 0.3 && fabs(ha - hb) < 0.01 * (ha + hb + 1e-6) && fabs(wa - wb) < 0.5) {
            printf("NO EFFECT %-24s  %6.1f dB %6.3f | %6.1f dB %6.3f\n", BA_PARAMS[id].name, da, ha, db, hb);
            dead++;
        } else {
            printf("ok        %-24s  %6.1f dB %6.3f w%6.1f | %6.1f dB %6.3f w%6.1f\n", BA_PARAMS[id].name, da, ha, wa, db, hb, wb);
        }
    }
    printf("%d without effect\n", dead);
    return 0;
}
