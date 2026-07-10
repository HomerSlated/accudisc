#include <stdio.h>
#include <string.h>

#include <accudisc/accudisc.h>

static void usage(FILE *to)
{
    fprintf(to,
        "usage: accudisc [--device DEV] <command>\n"
        "\n"
        "commands:\n"
        "  info       identify the drive (INQUIRY vendor/product/revision)\n"
        "  version    print the library version\n"
        "\n"
        "options:\n"
        "  --device DEV   optical device (default /dev/sr0)\n"
        "  --version, -V  same as the version command\n");
}

static int cmd_info(const char *device)
{
    int err = 0;
    accudisc_device *dev = accudisc_open(device, 0, &err);

    if (!dev) {
        fprintf(stderr, "accudisc: open %s: %s\n", device,
                accudisc_strerror(err));
        return 1;
    }

    accudisc_drive_id id;
    err = accudisc_drive_identify(dev, &id);
    if (err != ACCUDISC_OK) {
        accudisc_sense sense;
        accudisc_last_sense(dev, &sense);
        fprintf(stderr, "accudisc: identify %s: %s", device,
                accudisc_strerror(err));
        if (sense.valid)
            fprintf(stderr, " (key=0x%x asc=0x%02x ascq=0x%02x)",
                    sense.key, sense.asc, sense.ascq);
        fputc('\n', stderr);
        accudisc_close(dev);
        return 1;
    }

    printf("device %s\n", device);
    printf("vendor %s\n", id.vendor);
    printf("product %s\n", id.product);
    printf("revision %s\n", id.revision);
    accudisc_close(dev);
    return 0;
}

int main(int argc, char **argv)
{
    const char *device = "/dev/sr0";
    const char *command = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--device") && i + 1 < argc)
            device = argv[++i];
        else if (!strcmp(a, "--version") || !strcmp(a, "-V"))
            command = "version";
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(stdout);
            return 0;
        } else if (a[0] != '-' && !command)
            command = a;
        else {
            usage(stderr);
            return 2;
        }
    }

    if (!command) {
        usage(stderr);
        return 2;
    }
    if (!strcmp(command, "version")) {
        printf("accudisc %s\n", accudisc_version_string());
        return 0;
    }
    if (!strcmp(command, "info"))
        return cmd_info(device);

    fprintf(stderr, "accudisc: unknown command '%s'\n", command);
    usage(stderr);
    return 2;
}
