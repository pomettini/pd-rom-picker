#include "rom_picker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROW_HEIGHT 21
#define VISIBLE_ROWS 10
#define LIST_X 10
#define LIST_Y 15
#define SCREEN_WIDTH 400
#define REPEAT_INITIAL_DELAY 20 // frames before repeat starts (~667ms at 30fps)
#define REPEAT_INTERVAL 4       // frames between repeats (~133ms at 30fps)

// White pixels on a 50% checkerboard mask — painted over a row after drawing
// text to make it appear dimmed on Playdate's 1-bit display.
// Layout: 8 bytes image (all white), 8 bytes mask (50% active pixels).
static LCDPattern kDimOverlay = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                 0xFF, 0xFF, 0xAA, 0x55, 0xAA, 0x55,
                                 0xAA, 0x55, 0xAA, 0x55};

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
  LCDFont *bold_font;
  // Fallback for entries whose name contains non-ASCII bytes: the preferred
  // Asheville face has no accented glyphs, so those rows render with Roobert.
  LCDFont *accent_font;

  RomEntry files[ROM_PICKER_MAX_FILES];
  int file_count;
  int valid_indices[ROM_PICKER_MAX_FILES]; // indices into files[] for valid
                                           // entries
  int valid_count;

  int cursor; // index into valid_indices[]
  int scroll; // index into files[] of the first visible row

  int repeat_timer;
  int repeat_active;

  float crank_accum;
} s;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// True if any byte falls outside printable ASCII — i.e. the name needs the
// accent-capable fallback font to render correctly.
static int has_non_ascii(const char *str) {
  for (const unsigned char *c = (const unsigned char *)str; *c; c++) {
    if (*c < 0x20 || *c > 0x7E)
      return 1;
  }
  return 0;
}

// Font to draw a given entry name with: the accent fallback when the name has
// non-ASCII bytes (and the fallback loaded), otherwise the preferred face.
static LCDFont *font_for_name(const char *name) {
  if (s.accent_font && has_non_ascii(name))
    return s.accent_font;
  return s.font;
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

  e->valid = matches_extension(filename);
  s.file_count++;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void rom_picker_init(PlaydateAPI *pd, const RomPickerConfig *config) {
  if (!pd || !config) {
    if (pd)
      pd->system->logToConsole(
          "[rom_picker] rom_picker_init: pd and config must not be NULL");
    return;
  }
  if (!config->folder || config->folder[0] == '\0') {
    pd->system->logToConsole(
        "[rom_picker] rom_picker_init: config->folder must not be empty");
    return;
  }
  if (s.pd) {
    pd->system->logToConsole("[rom_picker] rom_picker_init: already "
                             "initialized; call rom_picker_free first");
    rom_picker_free();
  }

  memset(&s, 0, sizeof(s));
  s.pd = pd;
  s.on_select = config->on_select;
  s.userdata = config->userdata;

  const char *fontErr = NULL;
  s.font = pd->graphics->loadFont("/System/Fonts/Asheville-Sans-14-Light.pft",
                                  &fontErr);
  if (!s.font)
    pd->system->logToConsole("[rom_picker] failed to load font: %s",
                             fontErr ? fontErr : "unknown error");
  s.bold_font = pd->graphics->loadFont(
      "/System/Fonts/Asheville-Sans-14-Bold.pft", &fontErr);
  // Asheville has no accented glyphs; Roobert does. Used only for entries whose
  // name contains non-ASCII characters (e.g. "Pokémon"), via font_for_name().
  s.accent_font =
      pd->graphics->loadFont("/System/Fonts/Roobert-11-Medium.pft", &fontErr);

  strncpy(s.folder, config->folder, ROM_PICKER_MAX_PATH - 1);

  // Create each path segment so deeply-nested folders work on first run.
  {
    char tmp[ROM_PICKER_MAX_PATH];
    strncpy(tmp, s.folder, ROM_PICKER_MAX_PATH - 1);
    tmp[ROM_PICKER_MAX_PATH - 1] = '\0';
    size_t len = strlen(tmp);
    if (len > 1 && tmp[len - 1] == '/')
      tmp[--len] = '\0';
    for (char *p = tmp + 1; *p; p++) {
      if (*p == '/') {
        *p = '\0';
        pd->file->mkdir(tmp);
        *p = '/';
      }
    }
    pd->file->mkdir(tmp);
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
  PDButtons current, pushed;
  s.pd->system->getButtonState(&current, &pushed, NULL);

  if (s.valid_count == 0)
    return;

  PDButtons dir = current & (kButtonUp | kButtonDown);
  int move_up = 0, move_down = 0;

  if (pushed & (kButtonUp | kButtonDown)) {
    s.repeat_timer = 0;
    s.repeat_active = 0;
    move_up = !!(pushed & kButtonUp);
    move_down = !!(pushed & kButtonDown);
  } else if (dir) {
    s.repeat_timer++;
    int threshold = s.repeat_active ? REPEAT_INTERVAL : REPEAT_INITIAL_DELAY;
    if (s.repeat_timer >= threshold) {
      s.repeat_timer = 0;
      s.repeat_active = 1;
      move_up = !!(dir & kButtonUp);
      move_down = !!(dir & kButtonDown);
    }
  } else {
    s.repeat_timer = 0;
    s.repeat_active = 0;
  }

  if (move_up && s.cursor > 0) {
    s.cursor--;
    int file_idx = s.valid_indices[s.cursor];
    if (file_idx < s.scroll)
      s.scroll = file_idx;
  }
  if (move_down && s.cursor < s.valid_count - 1) {
    s.cursor++;
    int file_idx = s.valid_indices[s.cursor];
    if (file_idx >= s.scroll + VISIBLE_ROWS)
      s.scroll = file_idx - VISIBLE_ROWS + 1;
  }

  if (!s.pd->system->isCrankDocked()) {
    s.crank_accum += s.pd->system->getCrankChange();
    while (s.crank_accum >= 30.0f) {
      s.crank_accum -= 30.0f;
      if (s.cursor < s.valid_count - 1) {
        s.cursor++;
        int file_idx = s.valid_indices[s.cursor];
        if (file_idx >= s.scroll + VISIBLE_ROWS)
          s.scroll = file_idx - VISIBLE_ROWS + 1;
      }
    }
    while (s.crank_accum <= -30.0f) {
      s.crank_accum += 30.0f;
      if (s.cursor > 0) {
        s.cursor--;
        int file_idx = s.valid_indices[s.cursor];
        if (file_idx < s.scroll)
          s.scroll = file_idx;
      }
    }
  }

  if (pushed & kButtonA) {
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
      const char *line1 = "Please put ROMs in the folder:";
      const char *line2 = s.folder;
      LCDFont *line1_font = s.bold_font ? s.bold_font : s.font;
      int font_h = pd->graphics->getFontHeight(s.font);
      int gap = 10; // half the font height
      int total_h = font_h + gap + font_h;
      int line1_y = (240 - total_h) / 2;
      int line2_y = line1_y + font_h + gap;
      int tw1 = pd->graphics->getTextWidth(line1_font, line1, strlen(line1),
                                           kUTF8Encoding, 0);
      int tw2 = pd->graphics->getTextWidth(s.font, line2, strlen(line2),
                                           kUTF8Encoding, 0);
      pd->graphics->setFont(line1_font);
      pd->graphics->drawText(line1, strlen(line1), kUTF8Encoding,
                             (SCREEN_WIDTH - tw1) / 2, line1_y);
      pd->graphics->setFont(s.font);
      pd->graphics->drawText(line2, strlen(line2), kUTF8Encoding,
                             (SCREEN_WIDTH - tw2) / 2, line2_y);
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

    pd->graphics->setFont(font_for_name(e->name));

    if (is_selected) {
      pd->graphics->fillRect(0, y - 2, SCREEN_WIDTH, ROW_HEIGHT + 2,
                             kColorBlack);
      pd->graphics->setDrawMode(kDrawModeInverted);
      pd->graphics->drawText(e->name, strlen(e->name), kUTF8Encoding, LIST_X,
                             y);
      pd->graphics->setDrawMode(kDrawModeCopy);
    } else if (!e->valid) {
      pd->graphics->drawText(e->name, strlen(e->name), kUTF8Encoding, LIST_X,
                             y);
      pd->graphics->fillRect(0, y - 2, SCREEN_WIDTH, ROW_HEIGHT + 2,
                             (LCDColor)kDimOverlay);
    } else {
      pd->graphics->drawText(e->name, strlen(e->name), kUTF8Encoding, LIST_X,
                             y);
    }
  }

  // scrollbar
  if (s.file_count > VISIBLE_ROWS) {
    int track_h = 240;
    int thumb_h = track_h * VISIBLE_ROWS / s.file_count;
    if (thumb_h < 8)
      thumb_h = 8;
    int thumb_y =
        (track_h - thumb_h) * s.scroll / (s.file_count - VISIBLE_ROWS);
    pd->graphics->fillRect(396, 0, 4, track_h, kColorBlack);
    pd->graphics->fillRect(397, thumb_y, 2, thumb_h, kColorWhite);
  }
}

// ---------------------------------------------------------------------------

void rom_picker_update(void) {
  if (!s.pd) {
    return; // called before rom_picker_init
  }
  draw();
  handle_input();
}
