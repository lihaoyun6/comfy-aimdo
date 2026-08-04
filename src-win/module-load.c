#include "plat.h"

#include <windows.h>

void *aimdo_find_loaded_module(const char *const *libraries, size_t library_count) {
    for (size_t i = 0; i < library_count; i++) {
        HMODULE module = GetModuleHandleA(libraries[i]);

        if (module) {
            return (void *)module;
        }
    }

    log(AIMDO_LOG_ERROR, "%s: failed to find an already-loaded module\n", __func__);
    return NULL;
}
