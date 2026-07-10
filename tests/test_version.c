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
    return 0;
}
