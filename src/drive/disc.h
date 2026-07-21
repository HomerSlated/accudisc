#ifndef ADSC_DISC_H
#define ADSC_DISC_H

#include "../internal.h"

/* Reach the verdict from an already-populated probe: profile, disc_status,
 * erasable and the track census are inputs; kind and reason are outputs.
 * Pure — no device, unit-testable over synthetic combinations.
 *
 * Precedence is load-bearing and settled with cdda2img (2026-07-18, §17.2):
 * medium, then CD profile, then AUDIO, then BLANK. AUDIO outranks BLANK so a
 * burned audio CD-R classifies rippable rather than burnable. */
void adsc_disc_classify(accudisc_disc_probe *p);

#endif /* ADSC_DISC_H */
