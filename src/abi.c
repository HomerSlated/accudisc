/* Caller-declared struct size negotiation — API_PLAN §7.1.
 *
 * accudisc_read_req and accudisc_read_stats are caller-allocated, transparent,
 * cross an FFI boundary, and have both grown in place (32->40->56 and
 * 80->104->128 bytes across this repo's history). The CLI never noticed because
 * it is rebuilt with the library; a binding compiled against one header and
 * loaded against a different .so would notice by running off the end of a
 * struct. Each therefore declares its own size and the library honours it.
 *
 * The model is the kernel's copy_struct_from_user: extend short, verify long.
 * The asymmetry between import and export is not an oversight — it follows
 * from which side writes. See the block comment above accudisc_read_req in the
 * public header for the caller-facing statement of the same rules.
 */

#include <string.h>

#include "internal.h"

int adsc_abi_import(void *dst, size_t dst_size,
                    const void *src, uint32_t src_size)
{
    if (!dst || !src || dst_size < sizeof(uint32_t))
        return ACCUDISC_ERR_INVAL;

    /* Zero is what a caller that never set the field produces. Refusing it is
     * the whole reason the field can be trusted: forgetting fails on the first
     * call rather than being read as "a struct of no size" and papered over. */
    if (src_size < sizeof(uint32_t))
        return ACCUDISC_ERR_ABI;

    memset(dst, 0, dst_size);

    if (src_size > dst_size) {
        /* The caller is newer than we are. Their extra fields are past our
         * end, so we cannot act on them — but we can tell whether they are
         * ASKING us to. All-zero means "declared, not used", which is safe to
         * ignore; anything set is a request we would silently drop. */
        const unsigned char *tail = (const unsigned char *)src + dst_size;
        for (uint32_t i = 0; i < src_size - dst_size; i++)
            if (tail[i])
                return ACCUDISC_ERR_ABI;
        src_size = (uint32_t)dst_size;
    }

    memcpy(dst, src, src_size);

    /* Normalise: everything downstream of here sees this build's layout, so it
     * must also see this build's size rather than the caller's. */
    uint32_t norm = (uint32_t)dst_size;
    memcpy(dst, &norm, sizeof norm);
    return ACCUDISC_OK;
}

int adsc_abi_export(uint32_t want, size_t have, size_t *n_out)
{
    if (!n_out || have < sizeof(uint32_t))
        return ACCUDISC_ERR_INVAL;
    if (want < sizeof(uint32_t))
        return ACCUDISC_ERR_ABI;
    /* Refused rather than truncated. Writing only what we have would leave the
     * caller's trailing counters at whatever they initialised — and a zero
     * meaning "this build never computed it" is indistinguishable from a zero
     * meaning "none observed", which is precisely the class of wrong answer
     * that nothing downstream can catch. */
    if (want > have)
        return ACCUDISC_ERR_ABI;
    *n_out = want;
    return ACCUDISC_OK;
}
