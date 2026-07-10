#ifndef ADSC_TOC_H
#define ADSC_TOC_H

#include "../internal.h"

/* Parse a READ TOC format-0 (LBA) response into the public struct. Pure —
 * unit-testable without hardware. Returns ACCUDISC_ERR_SHORT when the
 * response holds no lead-out descriptor. */
int adsc_toc_parse(const uint8_t *buf, uint32_t len, accudisc_toc *out);

#endif /* ADSC_TOC_H */
