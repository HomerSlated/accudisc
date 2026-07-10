#include <stdio.h>
#include <string.h>

#include <accudisc/accudisc.h>

int main(int argc, char **argv)
{
    if (argc > 1 && (strcmp(argv[1], "--version") == 0 ||
                     strcmp(argv[1], "-V") == 0)) {
        printf("accudisc %s\n", accudisc_version_string());
        return 0;
    }

    fprintf(stderr, "accudisc %s — Red Book CD-DA reader/writer\n"
                    "usage: accudisc --version\n",
            accudisc_version_string());
    return argc > 1 ? 1 : 0;
}
