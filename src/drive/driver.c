/* Vendor-driver manager: locate, load, verify, attach. The gate order the
 * project mandates — identify, availability, permission (the attach call
 * itself), verified selftest — lives here; everything vendor-specific lives
 * in the external .so the manager loads. */

#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../internal.h"

#ifndef ACCUDISC_DRIVER_DIR_DEFAULT
#define ACCUDISC_DRIVER_DIR_DEFAULT "/usr/local/lib/accudisc/drivers"
#endif

#define DRV_PREFIX "accudisc-drv-"
#define DRV_SUFFIX ".so"

static const char *driver_dir(const char *dir)
{
    const char *env;

    if (dir)
        return dir;
    env = getenv("ACCUDISC_DRIVER_DIR");
    if (env && *env)
        return env;
    return ACCUDISC_DRIVER_DIR_DEFAULT;
}

/* dlopen + entry-point + ABI check. Returns the descriptor or NULL (handle
 * closed on any failure). */
static const accudisc_driver *load_driver(struct accudisc_device *dev,
                                          const char *path, void **handle)
{
    const accudisc_driver *drv;
    accudisc_driver_entry_fn entry;

    *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!*handle) {
        adsc_dev_log(dev, "driver %s: %s", path, dlerror());
        return NULL;
    }
    entry = (accudisc_driver_entry_fn)dlsym(*handle,
                                            ACCUDISC_DRIVER_ENTRY_SYMBOL);
    drv = entry ? entry() : NULL;
    if (!drv || drv->abi != ACCUDISC_DRIVER_ABI || !drv->name ||
        !drv->match || !drv->selftest) {
        adsc_dev_log(dev, "driver %s: %s", path,
                     entry ? "ABI mismatch or malformed descriptor"
                           : "no entry symbol");
        dlclose(*handle);
        *handle = NULL;
        return NULL;
    }
    return drv;
}

static int attach_verified(struct accudisc_device *dev,
                           const accudisc_driver *drv, void *handle)
{
    dev->host.dev = dev;
    if (drv->selftest(&dev->host) != ACCUDISC_OK) {
        adsc_dev_log(dev, "driver %s: selftest failed on %s %s — "
                          "staying on generic MMC",
                     drv->name, dev->id.vendor, dev->id.product);
        dlclose(handle);
        return ACCUDISC_ERR_UNSUPPORTED;
    }
    dev->drv = drv;
    dev->drv_handle = handle;
    adsc_dev_log(dev, "driver %s attached (%s %s, selftest ok)", drv->name,
                 dev->id.vendor, dev->id.product);
    return ACCUDISC_OK;
}

int accudisc_driver_attach(accudisc_device *dev, const char *name,
                           const char *dir)
{
    char path[512];
    const accudisc_driver *drv;
    void *handle;
    int rc;

    if (!dev)
        return ACCUDISC_ERR_INVAL;
    if (dev->drv)
        accudisc_driver_detach(dev);

    rc = adsc_dev_identify(dev);
    if (rc != ACCUDISC_OK)
        return rc;

    const char *d = driver_dir(dir);

    if (name) { /* explicit request */
        snprintf(path, sizeof(path), "%s/%s%s%s", d, DRV_PREFIX, name,
                 DRV_SUFFIX);
        drv = load_driver(dev, path, &handle);
        if (!drv) {
            adsc_dev_log(dev, "requested driver '%s' not found in %s — "
                              "using generic MMC", name, d);
            return ACCUDISC_ERR_NOTFOUND;
        }
        if (!drv->match(&dev->id)) {
            adsc_dev_log(dev, "driver %s does not support %s %s — "
                              "using generic MMC",
                         drv->name, dev->id.vendor, dev->id.product);
            dlclose(handle);
            return ACCUDISC_ERR_NOTFOUND;
        }
        return attach_verified(dev, drv, handle);
    }

    /* Auto: enumerate the directory, first ID match wins. */
    DIR *dp = opendir(d);
    if (!dp) {
        adsc_dev_log(dev, "no driver directory %s — using generic MMC", d);
        return ACCUDISC_ERR_NOTFOUND;
    }
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        size_t n = strlen(de->d_name);

        if (strncmp(de->d_name, DRV_PREFIX, strlen(DRV_PREFIX)) != 0)
            continue;
        if (n < strlen(DRV_SUFFIX) ||
            strcmp(de->d_name + n - strlen(DRV_SUFFIX), DRV_SUFFIX) != 0)
            continue;
        snprintf(path, sizeof(path), "%s/%s", d, de->d_name);
        drv = load_driver(dev, path, &handle);
        if (!drv)
            continue;
        if (!drv->match(&dev->id)) {
            dlclose(handle);
            continue;
        }
        closedir(dp);
        return attach_verified(dev, drv, handle);
    }
    closedir(dp);
    adsc_dev_log(dev, "no driver matches %s %s — using generic MMC",
                 dev->id.vendor, dev->id.product);
    return ACCUDISC_ERR_NOTFOUND;
}

void accudisc_driver_detach(accudisc_device *dev)
{
    if (!dev || !dev->drv)
        return;
    dev->drv = NULL;
    if (dev->drv_handle)
        dlclose(dev->drv_handle);
    dev->drv_handle = NULL;
}

const char *accudisc_access_method(accudisc_device *dev)
{
    if (!dev)
        return "generic MMC";
    if (!dev->drv)
        return "generic MMC";
    snprintf(dev->access, sizeof(dev->access), "driver %s (%s)",
             dev->drv->name,
             dev->drv->description ? dev->drv->description : "");
    return dev->access;
}

/* ---- driver capability delegation --------------------------------------- */

int accudisc_counter_scan_begin(accudisc_device *dev)
{
    if (!dev)
        return ACCUDISC_ERR_INVAL;
    if (!dev->drv || !dev->drv->counter_scan_begin)
        return ACCUDISC_ERR_UNSUPPORTED;
    return dev->drv->counter_scan_begin(&dev->host);
}

int accudisc_counter_scan_read(accudisc_device *dev, accudisc_counters *out)
{
    if (!dev || !out)
        return ACCUDISC_ERR_INVAL;
    if (!dev->drv || !dev->drv->counter_scan_read)
        return ACCUDISC_ERR_UNSUPPORTED;
    return dev->drv->counter_scan_read(&dev->host, out);
}

int accudisc_counter_scan_end(accudisc_device *dev)
{
    if (!dev)
        return ACCUDISC_ERR_INVAL;
    if (!dev->drv || !dev->drv->counter_scan_end)
        return ACCUDISC_ERR_UNSUPPORTED;
    return dev->drv->counter_scan_end(&dev->host);
}

int accudisc_speed_uncap_get(accudisc_device *dev, int *on)
{
    if (!dev || !on)
        return ACCUDISC_ERR_INVAL;
    if (!dev->drv || !dev->drv->speed_uncap_get)
        return ACCUDISC_ERR_UNSUPPORTED;
    return dev->drv->speed_uncap_get(&dev->host, on);
}

int accudisc_speed_uncap_set(accudisc_device *dev, int on)
{
    if (!dev)
        return ACCUDISC_ERR_INVAL;
    if (!dev->drv || !dev->drv->speed_uncap_set)
        return ACCUDISC_ERR_UNSUPPORTED;
    return dev->drv->speed_uncap_set(&dev->host, on ? 1 : 0);
}
