#include <accudisc/accudisc.h>

#define ADSC_STR2(x) #x
#define ADSC_STR(x) ADSC_STR2(x)

const char *accudisc_version_string(void)
{
    return ADSC_STR(ACCUDISC_VERSION_MAJOR) "."
           ADSC_STR(ACCUDISC_VERSION_MINOR) "."
           ADSC_STR(ACCUDISC_VERSION_PATCH);
}

void accudisc_version(int *major, int *minor, int *patch)
{
    if (major) *major = ACCUDISC_VERSION_MAJOR;
    if (minor) *minor = ACCUDISC_VERSION_MINOR;
    if (patch) *patch = ACCUDISC_VERSION_PATCH;
}
