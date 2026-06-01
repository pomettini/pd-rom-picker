#pragma once

#include "pd_api.h"

#define ROM_PICKER_MAX_FILES       256
#define ROM_PICKER_MAX_PATH        256
#define ROM_PICKER_MAX_EXTENSIONS  8

typedef void (*RomPickerCallback)(const char *path, void *userdata);

typedef struct {
    const char      *folder;      // directory to scan
    const char     **extensions;  // NULL-terminated list of valid extensions (no dot)
    RomPickerCallback on_select;  // called when A is pressed on a valid file
    void            *userdata;    // forwarded to on_select
    LCDFont         *font;        // NULL = use current font
} RomPickerConfig;

void rom_picker_init(PlaydateAPI *pd, const RomPickerConfig *config);
void rom_picker_free(void);
void rom_picker_update(void);
