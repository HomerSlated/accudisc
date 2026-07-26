#include <assert.h>
#include <string.h>

#include <accudisc/accudisc.h>

int main(void)
{
    int major = -1, minor = -1, patch = -1;
    accudisc_version(&major, &minor, &patch);
    assert(major == ACCUDISC_VERSION_MAJOR);
    assert(minor == ACCUDISC_VERSION_MINOR);
    assert(patch == ACCUDISC_VERSION_PATCH);
    assert(strlen(accudisc_version_string()) >= 5);

    /* The above compares the header against itself, which cannot fail. This
     * compares it against the version the BUILD used — the one that names the
     * .so and sets its soname. They were independent until 2026-07-26 and had
     * drifted: the header said 0.1.0 while CMake built libaccudisc.so.0.0.1.
     * An ABI policy of "rebuild on a version bump" is not enforceable while a
     * binding can ask two sources and get two answers. CMake now derives its
     * version from the header; this is what keeps that true. */
    assert(strcmp(accudisc_version_string(), ADSC_BUILD_VERSION) == 0);
    return 0;
}
