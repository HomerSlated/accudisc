#ifndef ADSC_TOC_H
#define ADSC_TOC_H

#include "../internal.h"

/* Parse a READ TOC format-0 (LBA) response into the public struct. Pure —
 * unit-testable without hardware. Returns ACCUDISC_ERR_SHORT when the
 * response holds no lead-out descriptor. */
int adsc_toc_parse(const uint8_t *buf, uint32_t len, accudisc_toc *out);

/* Fold a parsed full TOC (READ TOC format 2) down into the same flat track
 * model format 0 produces. Pure — unit-testable without hardware. info may be
 * NULL; when given, its session and disc-type fields are filled (source and
 * degrade are the caller's to set). Returns ACCUDISC_ERR_SHORT when the lead-in
 * carried no lead-out (A2) or no usable track point. */
int adsc_toc_from_fulltoc(const accudisc_fulltoc *ft, accudisc_toc *out,
                          accudisc_toc_info *info);

#endif /* ADSC_TOC_H */
