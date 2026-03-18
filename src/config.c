/* config.c — Runtime config defaults + fontconfig font resolution.
 * There is no file parser. Lua owns all configuration via trixie.set(). */
#define _POSIX_C_SOURCE 200809L
#include "trixie.h"
#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <string.h>
#include <strings.h> /* strcasecmp */
#include <strings.h>

static bool fc_resolve(const char *q, int weight, int slant, char *out,
                       int olen) {
  if (!q || !*q)
    return false;
  if (q[0] == '/') {
    FILE *f = fopen(q, "r");
    if (!f)
      return false;
    fclose(f);
    strncpy(out, q, olen - 1);
    return true;
  }
  FcInit();
  FcConfig *fc = FcConfigGetCurrent();
  if (strchr(q, '.')) {
    FcFontSet *fs = FcConfigGetFonts(fc, FcSetSystem);
    if (fs)
      for (int i = 0; i < fs->nfont; i++) {
        FcChar8 *file = NULL;
        if (FcPatternGetString(fs->fonts[i], FC_FILE, 0, &file) ==
            FcResultMatch) {
          const char *base = strrchr((char *)file, '/');
          base = base ? base + 1 : (char *)file;
          if (!strcasecmp(base, q)) {
            strncpy(out, (char *)file, olen - 1);
            return true;
          }
        }
      }
  }
  FcPattern *pat = FcNameParse((FcChar8 *)q);
  FcPatternAddInteger(pat, FC_WEIGHT, weight);
  FcPatternAddInteger(pat, FC_SLANT, slant);
  FcConfigSubstitute(fc, pat, FcMatchPattern);
  FcDefaultSubstitute(pat);
  FcResult res;
  FcPattern *match = FcFontMatch(fc, pat, &res);
  FcPatternDestroy(pat);
  if (!match)
    return false;
  FcChar8 *file = NULL;
  bool ok = false;
  if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
    strncpy(out, (char *)file, olen - 1);
    ok = true;
  }
  FcPatternDestroy(match);
  return ok;
}

static void resolve_fonts(Config *c) {
  char raw[256];
  strncpy(raw, c->font_path, sizeof(raw) - 1);
  char resolved[256] = {0};
  if (fc_resolve(raw, FC_WEIGHT_REGULAR, FC_SLANT_ROMAN, resolved,
                 sizeof(resolved)))
    strncpy(c->font_path, resolved, sizeof(c->font_path) - 1);
  else
    wlr_log(WLR_ERROR, "config: could not resolve font '%s'", raw);
  if (!fc_resolve(raw, FC_WEIGHT_BOLD, FC_SLANT_ROMAN, c->font_path_bold,
                  sizeof(c->font_path_bold)))
    strncpy(c->font_path_bold, c->font_path, sizeof(c->font_path_bold) - 1);
  if (!fc_resolve(raw, FC_WEIGHT_REGULAR, FC_SLANT_ITALIC, c->font_path_italic,
                  sizeof(c->font_path_italic)))
    strncpy(c->font_path_italic, c->font_path, sizeof(c->font_path_italic) - 1);
}

void config_defaults(Config *c) {
  memset(c, 0, sizeof(*c));
  strncpy(c->font_path, "JetBrainsMono Nerd Font", sizeof(c->font_path) - 1);
  c->font_size = 13.f;
  c->gap = 0;
  c->outer_gap = 0;
  c->border_width = 1;
  c->smart_gaps = true;
  c->workspaces = 9;
  strncpy(c->cursor_theme, "default", sizeof(c->cursor_theme) - 1);
  c->cursor_size = 24;
  strncpy(c->kb_layout, "us", sizeof(c->kb_layout) - 1);
  c->repeat_rate = 25;
  c->repeat_delay = 600;
  /* Catppuccin Mocha defaults — Lua overrides on startup */
  c->colors.active_border = color_hex(0xcba6f7);
  c->colors.inactive_border = color_hex(0x313244);
  c->colors.active_title = color_hex(0xcba6f7);
  c->colors.inactive_title = color_hex(0x6c7086);
  c->colors.pane_bg = color_hex(0x1e1e2e);
  c->colors.background = color_hex(0x1e1e2e);
  c->saturation = 1.f;
  c->shader_enabled = true;
  strncpy(c->seat_name, "seat0", sizeof(c->seat_name) - 1);
  resolve_fonts(c);
}
