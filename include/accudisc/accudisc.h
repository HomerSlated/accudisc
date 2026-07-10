/* accudisc.h — public API for libaccudisc.
 *
 * This header is the ABI contract: the CLI and all language bindings are
 * built against it exclusively. Keep it C-only, self-contained, and free of
 * internal types.
 */

#ifndef ACCUDISC_H
#define ACCUDISC_H

#ifdef __cplusplus
extern "C" {
#endif

#define ACCUDISC_VERSION_MAJOR 0
#define ACCUDISC_VERSION_MINOR 0
#define ACCUDISC_VERSION_PATCH 1

#if defined(_WIN32)
#  define ACCUDISC_API __declspec(dllexport)
#else
#  define ACCUDISC_API __attribute__((visibility("default")))
#endif

/* Version of the library actually linked (compare against the macros
 * above to detect header/library skew). */
ACCUDISC_API const char *accudisc_version_string(void);
ACCUDISC_API void accudisc_version(int *major, int *minor, int *patch);

#ifdef __cplusplus
}
#endif

#endif /* ACCUDISC_H */
