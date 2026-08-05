#define _GNU_SOURCE

#include "plat.h"

#include <dlfcn.h>
#include <link.h>

static int aimdo_match_loaded_module(struct dl_phdr_info *info, size_t size, void *data) {
    const char **search = (const char **)data;
    const char *name = info->dlpi_name;
    const char *basename;

    (void)size;
    if (!name || !*name) {
        return 0;
    }

    basename = strrchr(name, '/');
    basename = basename ? basename + 1 : name;

    if (strcmp(basename, search[0]) == 0) {
        search[1] = name;
        return 1;
    }

    return 0;
}

void *aimdo_find_loaded_module(const char *const *libraries, size_t library_count) {
    for (size_t i = 0; i < library_count; i++) {
        const char *search[2] = { libraries[i], NULL };
        void *module;

        dl_iterate_phdr(aimdo_match_loaded_module, search);
        if ((search[1] && (module = dlopen(search[1], RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD))) ||
            (module = dlopen(search[0], RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD))) {
            return module;
        }
    }

    log(AIMDO_LOG_ERROR, "%s: failed to find an already-loaded module\n", __func__);
    return NULL;
}
