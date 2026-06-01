#include "rom_picker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROW_HEIGHT 20
#define VISIBLE_ROWS 11
#define LIST_X 10
#define LIST_Y 14
#define SCREEN_WIDTH 400

// White pixels on a 50% checkerboard mask — painted over a row after drawing
// text to make it appear dimmed on Playdate's 1-bit display.
// Layout: 8 bytes image (all white), 8 bytes mask (50% active pixels).
static LCDPattern kDimOverlay = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55};

typedef struct {
  char path[ROM_PICKER_MAX_PATH];
  char name[ROM_PICKER_MAX_PATH];
  int valid;
} RomEntry;

static struct {
  PlaydateAPI *pd;
  char folder[ROM_PICKER_MAX_PATH];
  char extensions[ROM_PICKER_MAX_EXTENSIONS][32];
  int extension_count;
  RomPickerCallback on_select;
  void *userdata;
  LCDFont *font;

  RomEntry files[ROM_PICKER_MAX_FILES];
  int file_count;
  int valid_indices[ROM_PICKER_MAX_FILES]; // indices into files[] for valid
                                           // entries
  int valid_count;

  int cursor; // index into valid_indices[]
  int scroll; // index into files[] of the first visible row
} s;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int is_all_ascii(const char *str) {
  for (const unsigned char *c = (const unsigned char *)str; *c; c++) {
    if (*c < 0x20 || *c > 0x7E)
      return 0;
  }
  return 1;
}

static int matches_extension(const char *filename) {
  if (s.extension_count == 0)
    return 1;

  const char *dot = strrchr(filename, '.');
  if (!dot || dot == filename)
    return 0;
  dot++; // skip the dot

  for (int i = 0; i < s.extension_count; i++) {
    // case-insensitive compare
    const char *a = dot, *b = s.extensions[i];
    while (*a && *b) {
      char ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : *a;
      char cb = (*b >= 'A' && *b <= 'Z') ? (*b + 32) : *b;
      if (ca != cb)
        break;
      a++;
      b++;
    }
    if (*a == '\0' && *b == '\0')
      return 1;
  }
  return 0;
}

static int entry_compare(const void *a, const void *b) {
  return strcmp(((const RomEntry *)a)->name, ((const RomEntry *)b)->name);
}

// ---------------------------------------------------------------------------
// listfiles callback
// ---------------------------------------------------------------------------

static void collect_file(const char *filename, void *userdata) {
  (void)userdata;
  if (s.file_count >= ROM_PICKER_MAX_FILES)
    return;

  // skip directories (Playdate appends '/' to directory names)
  size_t len = strlen(filename);
  if (len > 0 && filename[len - 1] == '/')
    return;

  RomEntry *e = &s.files[s.file_count];

  const char *sep = (s.folder[strlen(s.folder) - 1] == '/') ? "" : "/";
  int written =
      snprintf(e->path, ROM_PICKER_MAX_PATH, "%s%s%s", s.folder, sep, filename);
  if (written < 0 || written >= ROM_PICKER_MAX_PATH)
    return;

  strncpy(e->name, filename, ROM_PICKER_MAX_PATH - 1);
  e->name[ROM_PICKER_MAX_PATH - 1] = '\0';

  e->valid = is_all_ascii(filename) && matches_extension(filename);
  s.file_count++;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void rom_picker_init(PlaydateAPI *pd, const RomPickerConfig *config) {
  if (!pd || !config) {
    if (pd)
      pd->system->logToConsole("[rom_picker] rom_picker_init: pd and config must not be NULL");
    return;
  }
  if (!config->folder || config->folder[0] == '\0') {
    pd->system->logToConsole("[rom_picker] rom_picker_init: config->folder must not be empty");
    return;
  }
  if (s.pd) {
    pd->system->logToConsole("[rom_picker] rom_picker_init: already initialized; call rom_picker_free first");
    rom_picker_free();
  }

  memset(&s, 0, sizeof(s));
  s.pd = pd;
  s.on_select = config->on_select;
  s.userdata = config->userdata;

  const char *fontErr = NULL;
  s.font = pd->graphics->loadFont(
      "/System/Fonts/Asheville-Sans-14-Light.pft", &fontErr);
  if (!s.font)
    pd->system->logToConsole("[rom_picker] failed to load font: %s",
                             fontErr ? fontErr : "unknown error");

  strncpy(s.folder, config->folder, ROM_PICKER_MAX_PATH - 1);

  FileStat st;
  if (pd->file->stat(s.folder, &st) != 0) {
    pd->file->mkdir(s.folder);
  }

  if (config->extensions) {
    for (int i = 0; config->extensions[i] && i < ROM_PICKER_MAX_EXTENSIONS;
         i++) {
      strncpy(s.extensions[i], config->extensions[i], 31);
      s.extensions[i][31] = '\0';
      s.extension_count++;
    }
  }

  pd->file->listfiles(s.folder, collect_file, NULL, 0);

  qsort(s.files, s.file_count, sizeof(RomEntry), entry_compare);

  for (int i = 0; i < s.file_count; i++) {
    if (s.files[i].valid) {
      s.valid_indices[s.valid_count++] = i;
    }
  }

  if (config->auto_load_single && s.valid_count == 1 && s.on_select) {
    s.on_select(s.files[s.valid_indices[0]].path, s.userdata);
  }
}

void rom_picker_free(void) { memset(&s, 0, sizeof(s)); }

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

static void handle_input(void) {
  PDButtons pushed;
  s.pd->system->getButtonState(NULL, &pushed, NULL);

  if (s.valid_count == 0)
    return;

  if (pushed & kButtonUp) {
    if (s.cursor > 0) {
      s.cursor--;
      int file_idx = s.valid_indices[s.cursor];
      if (file_idx < s.scroll)
        s.scroll = file_idx;
    }
  } else if (pushed & kButtonDown) {
    if (s.cursor < s.valid_count - 1) {
      s.cursor++;
      int file_idx = s.valid_indices[s.cursor];
      if (file_idx >= s.scroll + VISIBLE_ROWS)
        s.scroll = file_idx - VISIBLE_ROWS + 1;
    }
  } else if (pushed & kButtonA) {
    if (s.on_select) {
      s.on_select(s.files[s.valid_indices[s.cursor]].path, s.userdata);
    }
  }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static void draw(void) {
  PlaydateAPI *pd = s.pd;

  if (s.font)
    pd->graphics->setFont(s.font);

  pd->graphics->clear(kColorWhite);

  if (s.valid_count == 0) {
    if (s.font) {
      char msg[ROM_PICKER_MAX_PATH + 32];
      snprintf(msg, sizeof(msg), "Please put ROMs in the %s folder", s.folder);
      int tw = pd->graphics->getTextWidth(s.font, msg, strlen(msg),
                                          kASCIIEncoding, 0);
      pd->graphics->drawText(msg, strlen(msg), kASCIIEncoding,
                             (SCREEN_WIDTH - tw) / 2, 113);
    }
    return;
  }

  for (int row = 0; row < VISIBLE_ROWS; row++) {
    int file_idx = s.scroll + row;
    if (file_idx >= s.file_count)
      break;

    RomEntry *e = &s.files[file_idx];
    int y = LIST_Y + row * ROW_HEIGHT;
    int is_selected =
        (s.valid_count > 0 && s.valid_indices[s.cursor] == file_idx);

    if (is_selected) {
      pd->graphics->fillRect(0, y - 2, SCREEN_WIDTH, ROW_HEIGHT + 2,
                             kColorBlack);
      pd->graphics->setDrawMode(kDrawModeInverted);
      pd->graphics->drawText(e->name, strlen(e->name), kASCIIEncoding, LIST_X,
                             y);
      pd->graphics->setDrawMode(kDrawModeCopy);
    } else if (!e->valid) {
      pd->graphics->drawText(e->name, strlen(e->name), kASCIIEncoding, LIST_X,
                             y);
      pd->graphics->fillRect(0, y - 2, SCREEN_WIDTH, ROW_HEIGHT + 2,
                             (LCDColor)kDimOverlay);
    } else {
      pd->graphics->drawText(e->name, strlen(e->name), kASCIIEncoding, LIST_X,
                             y);
    }
  }

  // scrollbar
  if (s.file_count > VISIBLE_ROWS) {
    int track_h = 240 - LIST_Y * 2;
    int thumb_h = track_h * VISIBLE_ROWS / s.file_count;
    if (thumb_h < 8)
      thumb_h = 8;
    int thumb_y =
        LIST_Y + (track_h - thumb_h) * s.scroll / (s.file_count - VISIBLE_ROWS);
    pd->graphics->fillRect(396, LIST_Y, 4, track_h, kColorBlack);
    pd->graphics->fillRect(397, thumb_y, 2, thumb_h, kColorWhite);
  }
}

// ---------------------------------------------------------------------------

void rom_picker_update(void) {
  if (!s.pd) {
    return; // called before rom_picker_init
  }
  handle_input();
  draw();
}
