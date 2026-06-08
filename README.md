# pd-rom-picker

A general-purpose Playdate C library for browsing and launching ROM/game files from a directory.

## Features

- Scans a folder using Playdate's file APIs
- Filters files by one or more extensions (e.g. only `.nes` files)
- Skips files with non-ASCII characters in their name
- Invalid or incompatible files are shown at reduced opacity so the user knows they can't be selected
- D-pad navigation, A to confirm selection
- Calls a user-supplied callback with the file path when a game is picked
- Designed to be embedded in any Playdate emulator project

## Files

```
src/
  rom_picker.h   — public API
  rom_picker.c   — implementation
```

## Usage

### 1. Copy the source files

Copy `src/rom_picker.h` and `src/rom_picker.c` into your project and add `rom_picker.c` to your build.

### 2. Define a callback

```c
void on_rom_picked(const char *path, void *userdata) {
    // path is the full path to the selected file, e.g. "/roms/game.nes"
    // load and launch it here
}
```

### 3. Initialize the picker

```c
#include "rom_picker.h"

const char *extensions[] = { "nes", NULL };

RomPickerConfig config = {
    .folder     = "/roms",
    .extensions = extensions,
    .on_select  = on_rom_picked,
    .userdata   = NULL,
};

rom_picker_init(pd, &config);
```

### 4. Call update every frame

```c
int update(void *userdata) {
    rom_picker_update();
    return 1;
}
```

### 5. Clean up when done

```c
rom_picker_free();
```

## API Reference

```c
// Called when the user confirms a selection.
// path     — full path to the chosen file
// userdata — value passed in RomPickerConfig.userdata
typedef void (*RomPickerCallback)(const char *path, void *userdata);

typedef struct {
    const char       *folder;      // Directory to scan (required)
    const char      **extensions;  // NULL-terminated list of valid extensions
                                   // without the dot, e.g. {"nes", "rom", NULL}
                                   // NULL means all files are valid
    RomPickerCallback on_select;      // Called when A is pressed on a valid file
    void             *userdata;       // Forwarded to on_select unchanged
    int               auto_load_single; // 1 = skip UI and load immediately when only one valid file is found (default 0)
} RomPickerConfig;

// Scan folder and initialise internal state. Call once before update.
void rom_picker_init(PlaydateAPI *pd, const RomPickerConfig *config);

// Release internal state. Safe to call rom_picker_init again afterwards.
void rom_picker_free(void);

// Call once per frame from your update callback.
// Reads input, scrolls the list, and draws everything.
void rom_picker_update(void);
```

## Input

| Button | Action |
|--------|--------|
| Up / Down | Move selection |
| Left / Right | Move selection by one page |
| A | Confirm and launch selected file |

## Compatibility rules

A file is shown with full opacity and can be selected when **both** of these are true:

- Its extension matches one of the entries in `extensions` (case-insensitive)
- Its filename contains only printable ASCII characters (0x20 – 0x7E)

All other files are shown at 50% opacity and cannot be selected.

## Limits

| Constant | Default | Meaning |
|----------|---------|---------|
| `ROM_PICKER_MAX_FILES` | 256 | Maximum files shown |
| `ROM_PICKER_MAX_PATH` | 256 | Maximum full path length |
| `ROM_PICKER_MAX_EXTENSIONS` | 8 | Maximum number of valid extensions |
