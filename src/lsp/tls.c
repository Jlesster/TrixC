/*
 * trixie-lsp.c — LSP server for Trixie compositor configs
 *
 * Implements the Language Server Protocol over stdin/stdout.
 * Supports: initialize, textDocument/didOpen, textDocument/didChange,
 *           textDocument/completion, textDocument/hover,
 *           textDocument/publishDiagnostics
 *
 * Build: cc -O2 -o trixie-lsp trixie-lsp.c
 * Usage: registered as an lspconfig server, nvim spawns it automatically.
 */

#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Minimal JSON builder
 * ═══════════════════════════════════════════════════════════════════════════ */

#define JSON_BUF 1048576 /* 1 MB output buffer */

typedef struct {
  char *s;
  int   len, cap;
} JBuf;

static void jb_init(JBuf *b) {
  b->cap  = JSON_BUF;
  b->s    = malloc(b->cap);
  b->len  = 0;
  b->s[0] = '\0';
}
static void jb_free(JBuf *b) {
  free(b->s);
  b->s = NULL;
}

static void jb_raw(JBuf *b, const char *s) {
  int n = (int)strlen(s);
  while(b->len + n + 1 >= b->cap) {
    b->cap *= 2;
    b->s = realloc(b->s, b->cap);
  }
  memcpy(b->s + b->len, s, n);
  b->len += n;
  b->s[b->len] = '\0';
}

static void jb_str(JBuf *b, const char *s) {
  jb_raw(b, "\"");
  for(; *s; s++) {
    if(*s == '"')
      jb_raw(b, "\\\"");
    else if(*s == '\\')
      jb_raw(b, "\\\\");
    else if(*s == '\n')
      jb_raw(b, "\\n");
    else if(*s == '\r')
      jb_raw(b, "\\r");
    else if(*s == '\t')
      jb_raw(b, "\\t");
    else {
      char c[2] = { *s, 0 };
      jb_raw(b, c);
    }
  }
  jb_raw(b, "\"");
}

static void jb_int(JBuf *b, int n) {
  char tmp[32];
  snprintf(tmp, sizeof(tmp), "%d", n);
  jb_raw(b, tmp);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Minimal JSON parser (just enough to extract fields we need)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Find value of a string field in a flat JSON object.
 * Returns malloc'd string or NULL. Not recursive — good enough for LSP. */
static char *json_str(const char *json, const char *key) {
  char pat[256];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(json, pat);
  if(!p) return NULL;
  p += strlen(pat);
  while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    p++;
  if(*p != ':') return NULL;
  p++;
  while(*p == ' ' || *p == '\t')
    p++;
  if(*p != '"') return NULL;
  p++;
  /* collect until closing unescaped " */
  char buf[65536];
  int  i = 0;
  while(*p && i < (int)sizeof(buf) - 1) {
    if(*p == '\\' && *(p + 1)) {
      p++;
      buf[i++] = *p++;
      continue;
    }
    if(*p == '"') break;
    buf[i++] = *p++;
  }
  buf[i] = '\0';
  return strdup(buf);
}

/* Extract integer field */
static int json_int(const char *json, const char *key, int def) {
  char pat[256];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(json, pat);
  if(!p) return def;
  p += strlen(pat);
  while(*p == ' ' || *p == ':' || *p == ' ')
    p++;
  if(!isdigit((unsigned char)*p) && *p != '-') return def;
  return atoi(p);
}

/* Extract a nested object as a raw string slice (shallow, brace-counted) */
static char *json_obj(const char *json, const char *key) {
  char pat[256];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(json, pat);
  if(!p) return NULL;
  p += strlen(pat);
  while(*p && *p != '{')
    p++;
  if(!*p) return NULL;
  const char *start = p;
  int         depth = 0;
  while(*p) {
    if(*p == '{')
      depth++;
    else if(*p == '}') {
      depth--;
      if(!depth) {
        p++;
        break;
      }
    }
    p++;
  }
  int   len = (int)(p - start);
  char *out = malloc(len + 1);
  memcpy(out, start, len);
  out[len] = '\0';
  return out;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Document store
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_DOCS  32
#define MAX_LINES 4096
#define MAX_LINE  2048

typedef struct {
  char uri[512];
  int  version;
  char lines[MAX_LINES][MAX_LINE];
  int  nlines;
} Doc;

static Doc docs[MAX_DOCS];
static int ndocs = 0;

static Doc *doc_find(const char *uri) {
  for(int i = 0; i < ndocs; i++)
    if(!strcmp(docs[i].uri, uri)) return &docs[i];
  return NULL;
}

static Doc *doc_open(const char *uri) {
  Doc *d = doc_find(uri);
  if(d) return d;
  if(ndocs >= MAX_DOCS) return NULL;
  d = &docs[ndocs++];
  memset(d, 0, sizeof(*d));
  strncpy(d->uri, uri, sizeof(d->uri) - 1);
  return d;
}

static void doc_set_text(Doc *d, const char *text) {
  d->nlines     = 0;
  const char *p = text;
  while(*p && d->nlines < MAX_LINES) {
    int i = 0;
    while(*p && *p != '\n' && i < MAX_LINE - 1)
      d->lines[d->nlines][i++] = *p++;
    d->lines[d->nlines][i] = '\0';
    d->nlines++;
    if(*p == '\n') p++;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Schema
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum { T_STRING, T_INT, T_FLOAT, T_BOOL, T_COLOR, T_ENUM, T_MULTI } KType;

typedef struct {
  const char *key;
  KType       type;
  const char *doc;
  const char *values[16]; /* NULL-terminated list for enum/multi/bool */
} KInfo;

static const char *BOOL_VALS[] = { "true", "false", "yes", "no", "on", "off", NULL };
static const char *BAR_POS[]   = { "top", "bottom", NULL };
static const char *SEP_STYLES[] = { "pipe", "block", "arrow", "none", NULL };
static const char *THEMES[]     = { "catppuccin-mocha",
                                    "catppuccin-latte",
                                    "mocha",
                                    "latte",
                                    "gruvbox",
                                    "gruvbox-dark",
                                    "nord",
                                    "dracula",
                                    "rose-pine",
                                    NULL };
static const char *MODULES[]    = { "workspaces",  "title",   "layout", "clock",
                                    "battery",     "network", "volume", "bluetooth",
                                    "wifi",        "tray",    "cpu",    "memory",
                                    "temperature", "date",    "time",   NULL };

#define K(k, t, d, v) { k, t, d, v }
#define KEND         \
  {                  \
    NULL, 0, NULL, { \
      NULL           \
    }                \
  }

/* We store values as a flat const char** pointer in the struct.
 * Use a union trick: cast the pointer into the values[0] slot. */
typedef struct {
  const char  *key;
  KType        type;
  const char  *doc;
  const char **vals; /* NULL if no fixed value list */
} Schema;

static const Schema SCHEMA[] = {
  /* general */
  { "font",                    T_STRING, "Font family name or absolute .ttf path",                    NULL       },
  { "font_family",             T_STRING, "Alias for font",                                            NULL       },
  { "font_size",               T_FLOAT,  "Base font size in points",                                  NULL       },
  { "gaps_in",                 T_INT,    "Inner gap between tiled windows (px)",                      NULL       },
  { "gaps_out",                T_INT,    "Outer gap around screen edge (px)",                         NULL       },
  { "gap",                     T_INT,    "Alias for gaps_in",                                         NULL       },
  { "gaps",                    T_INT,    "Alias for gaps_in",                                         NULL       },
  { "outer_gap",               T_INT,    "Alias for gaps_out",                                        NULL       },
  { "outer_gaps",              T_INT,    "Alias for gaps_out",                                        NULL       },
  { "border_size",             T_INT,    "Window border thickness (px)",                              NULL       },
  { "border_width",            T_INT,    "Alias for border_size",                                     NULL       },
  { "corner_radius",           T_INT,    "Window corner rounding radius (px)",                        NULL       },
  { "rounding",                T_INT,    "Alias for corner_radius",                                   NULL       },
  { "smart_gaps",
   T_BOOL,                               "Disable gaps when only one window is visible",
   BOOL_VALS                                                                                                     },
  { "saturation",              T_FLOAT,  "Monitor colour saturation multiplier (0.0-2.0)",            NULL       },
  { "shader",                  T_BOOL,   "Enable custom GLSL post-process shader",                    BOOL_VALS  },
  { "background_color",        T_COLOR,  "Root/wallpaper fallback fill colour",                       NULL       },
  { "background",              T_COLOR,  "Alias for background_color",                                NULL       },
  { "active_border",           T_COLOR,  "Focused window border colour",                              NULL       },
  { "active_border_color",     T_COLOR,  "Alias for active_border",                                   NULL       },
  { "inactive_border",         T_COLOR,  "Unfocused window border colour",                            NULL       },
  { "inactive_border_color",   T_COLOR,  "Alias for inactive_border",                                 NULL       },
  { "cursor_theme",            T_STRING, "XCursor theme name (e.g. Adwaita)",                         NULL       },
  { "cursor_size",             T_INT,    "XCursor size in pixels",                                    NULL       },
  { "workspaces",              T_INT,    "Number of workspaces to create at startup",                 NULL       },
  { "workspace_count",         T_INT,    "Alias for workspaces",                                      NULL       },
  { "seat_name",               T_STRING, "libinput seat identifier (e.g. seat0)",                     NULL       },
  { "seat",                    T_STRING, "Alias for seat_name",                                       NULL       },
  { "idle_timeout",            T_INT,    "Seconds of inactivity before idle (0=disabled)",            NULL       },
  { "xwayland",                T_BOOL,   "Enable XWayland compatibility layer",                       BOOL_VALS  },
  { "theme",                   T_ENUM,   "Built-in colour preset name",                               THEMES     },
  { "gesture_swipe_threshold",
   T_FLOAT,                              "Minimum swipe distance to register a gesture",
   NULL                                                                                                          },
  { "gesture_pinch_threshold",
   T_FLOAT,                              "Minimum pinch scale delta to register a gesture",
   NULL                                                                                                          },
  { "source",                  T_STRING, "Path to another config file to include",                    NULL       },
  { "exec-once",               T_STRING, "Shell command to run once at compositor startup",           NULL       },
  { "exec",                    T_STRING, "Shell command to run (restartable)",                        NULL       },
  { "gesture",
   T_STRING,                             "Gesture bind: swipe:N:dir,action or pinch:N:in|out,action",
   NULL                                                                                                          },
  { "bind",                    T_STRING, "Keybind: MODS, keysym, action[, args]",                     NULL       },
  { "binde",                   T_STRING, "Repeating keybind (fires while held)",                      NULL       },
  { "bindm",                   T_STRING, "Mouse-drag keybind",                                        NULL       },
  { "windowrule",              T_STRING, "Window rule: effect, matcher",                              NULL       },
  { "windowrulev2",            T_STRING, "Window rule v2: effect, class:^(pattern)$",                 NULL       },
  { "window_rule",             T_STRING, "Alias for windowrule",                                      NULL       },
  /* decoration */
  { "active_title",            T_COLOR,  "Focused titlebar text colour",                              NULL       },
  { "active_title_color",      T_COLOR,  "Alias for active_title",                                    NULL       },
  { "inactive_title",          T_COLOR,  "Unfocused titlebar text colour",                            NULL       },
  { "inactive_title_color",    T_COLOR,  "Alias for inactive_title",                                  NULL       },
  { "pane_bg",                 T_COLOR,  "Tiled window pane background colour",                       NULL       },
  { "focus_ring",              T_COLOR,  "Focus highlight ring colour (RGBA)",                        NULL       },
  { "focus_ring_color",        T_COLOR,  "Alias for focus_ring",                                      NULL       },
  /* colors */
  { "bar_bg",                  T_COLOR,  "Bar background colour",                                     NULL       },
  { "bar_fg",                  T_COLOR,  "Bar foreground/text colour",                                NULL       },
  { "bar_accent",              T_COLOR,  "Bar accent colour",                                         NULL       },
  /* input/keyboard */
  { "kb_layout",               T_STRING, "XKB keyboard layout (e.g. us, gb)",                         NULL       },
  { "layout",                  T_STRING, "Alias for kb_layout inside input/keyboard block",           NULL       },
  { "kb_variant",              T_STRING, "XKB variant (e.g. dvorak, colemak)",                        NULL       },
  { "variant",                 T_STRING, "Alias for kb_variant",                                      NULL       },
  { "kb_options",              T_STRING, "XKB options (e.g. caps:swapescape)",                        NULL       },
  { "options",                 T_STRING, "Alias for kb_options",                                      NULL       },
  { "repeat_rate",             T_INT,    "Key auto-repeat rate (chars/sec)",                          NULL       },
  { "repeat_delay",            T_INT,    "Delay before auto-repeat begins (ms)",                      NULL       },
  /* bar */
  { "position",                T_ENUM,   "Bar screen edge",                                           BAR_POS    },
  { "height",                  T_INT,    "Bar height in pixels",                                      NULL       },
  { "padding",                 T_INT,    "Bar horizontal padding in pixels",                          NULL       },
  { "glyph_y_offset",          T_INT,    "Vertical pixel nudge for icon glyphs",                      NULL       },
  { "item_spacing",            T_INT,    "Pixels between adjacent bar items",                         NULL       },
  { "pill_radius",             T_INT,    "Workspace pill corner radius (0=square)",                   NULL       },
  { "bg",                      T_COLOR,  "Bar background colour",                                     NULL       },
  { "fg",                      T_COLOR,  "Bar text colour",                                           NULL       },
  { "accent",                  T_COLOR,  "Bar accent colour",                                         NULL       },
  { "dim",                     T_COLOR,  "Bar dimmed/inactive element colour",                        NULL       },
  { "active_ws_fg",            T_COLOR,  "Active workspace label foreground",                         NULL       },
  { "active_ws_bg",            T_COLOR,  "Active workspace label background",                         NULL       },
  { "occupied_ws_fg",          T_COLOR,  "Occupied (inactive) workspace label colour",                NULL       },
  { "inactive_ws_fg",          T_COLOR,  "Empty workspace label colour",                              NULL       },
  { "separator",               T_BOOL,   "Draw separator lines between modules",                      BOOL_VALS  },
  { "separator_top",           T_BOOL,   "Draw separator on the bar top edge",                        BOOL_VALS  },
  { "separator_color",         T_COLOR,  "Separator line colour",                                     NULL       },
  { "separator_style",         T_ENUM,   "Separator glyph style",                                     SEP_STYLES },
  { "sep_style",               T_ENUM,   "Alias for separator_style",                                 SEP_STYLES },
  { "powerline",               T_BOOL,   "Enable powerline arrow separators",                         BOOL_VALS  },
  { "modules_left",            T_MULTI,  "Space/comma list of left-side modules",                     MODULES    },
  { "modules-left",            T_MULTI,  "Alias for modules_left",                                    MODULES    },
  { "modules_center",          T_MULTI,  "Space/comma list of centre modules",                        MODULES    },
  { "modules-center",          T_MULTI,  "Alias for modules_center",                                  MODULES    },
  { "modules_right",           T_MULTI,  "Space/comma list of right-side modules",                    MODULES    },
  { "modules-right",           T_MULTI,  "Alias for modules_right",                                   MODULES    },
  { "col_background",          T_COLOR,  "col. prefix alias for background colour",                   NULL       },
  { "col_text",                T_COLOR,  "col. prefix alias for text colour",                         NULL       },
  { "col_accent",              T_COLOR,  "col. prefix alias for accent colour",                       NULL       },
  /* monitor block */
  { "width",                   T_INT,    "Monitor width in pixels",                                   NULL       },
  { "refresh",                 T_INT,    "Monitor refresh rate in Hz",                                NULL       },
  { "scale",                   T_FLOAT,  "Output HiDPI scale factor (e.g. 1.0, 2.0)",                 NULL       },
  /* scratchpad block */
  { "app_id",                  T_STRING, "app_id / WM_CLASS pattern to match",                        NULL       },
  { "class",                   T_STRING, "Alias for app_id",                                          NULL       },
  { "name",                    T_STRING, "Scratchpad identifier name",                                NULL       },
  { "size",                    T_STRING, "Dimensions as width%, height%",                             NULL       },
  /* bar_module block */
  { "interval",                T_INT,    "Module refresh interval in seconds",                        NULL       },
  { "icon",                    T_STRING, "Nerd Font glyph prefix for this module",                    NULL       },
  { "format",                  T_STRING, "strftime or custom format string",                          NULL       },
  { "color",                   T_COLOR,  "Module text colour override",                               NULL       },
  { "colour",                  T_COLOR,  "British spelling alias for color",                          NULL       },
  /* overlay block */
  { "enabled",                 T_BOOL,   "Enable this subsystem",                                     BOOL_VALS  },
  { "editor",                  T_STRING, "Editor command (e.g. nvim)",                                NULL       },
  { "terminal",                T_STRING, "Terminal emulator command (e.g. foot)",                     NULL       },
  { "nvim_socket",             T_STRING, "Path to Neovim RPC socket",                                 NULL       },
  { "nvim_sock",               T_STRING, "Alias for nvim_socket",                                     NULL       },
  { "nvim_listen_addr",        T_STRING, "Neovim --listen address",                                   NULL       },
  { "nvim_listen",             T_STRING, "Alias for nvim_listen_addr",                                NULL       },
  { "project_root",            T_STRING, "Default project root path (~/... expanded)",                NULL       },
  { "root",                    T_STRING, "Alias for project_root",                                    NULL       },
  { "default_panel",           T_STRING, "Default overlay panel name to open",                        NULL       },
  { "panel",                   T_STRING, "Alias for default_panel",                                   NULL       },
  { "lsp_diagnostics",         T_BOOL,   "Show LSP diagnostics in the overlay",                       BOOL_VALS  },
  { "lsp",                     T_STRING, "LSP server command for this language",                      NULL       },
  { "lsp_poll_ms",             T_INT,    "LSP diagnostic polling interval (ms)",                      NULL       },
  { "lsp_poll",                T_INT,    "Alias for lsp_poll_ms",                                     NULL       },
  /* dev_lang block */
  { "build",                   T_STRING, "Shell command to build the project",                        NULL       },
  { "run",                     T_STRING, "Shell command to run the project",                          NULL       },
  { "test",                    T_STRING, "Shell command to run the test suite",                       NULL       },
  { "fmt",                     T_STRING, "Shell command to format source files",                      NULL       },
  { "lint",                    T_STRING, "Shell command to lint source files",                        NULL       },
  /* workspace block */
  { "ratio",                   T_FLOAT,  "Master-stack size ratio (0.0-1.0)",                         NULL       },
  { NULL,                      0,        NULL,                                                        NULL       }
};

static const Schema *schema_find(const char *key) {
  /* strip col. prefix */
  const char *k = key;
  if(!strncmp(k, "col.", 4)) k += 4;
  for(int i = 0; SCHEMA[i].key; i++)
    if(!strcasecmp(SCHEMA[i].key, k)) return &SCHEMA[i];
  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  Parser / validator
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
  int  lnum; /* 0-based */
  int  col_s;
  int  col_e;
  char key[256];
  char val[1024];
} Entry;

#define MAX_ENTRIES 2048

static int parse_doc(const Doc *d, Entry *out) {
  int n = 0;
  for(int i = 0; i < d->nlines && n < MAX_ENTRIES; i++) {
    const char *raw = d->lines[i];
    char        line[MAX_LINE];
    strncpy(line, raw, MAX_LINE - 1);
    line[MAX_LINE - 1] = '\0';

    /* strip // comments */
    char *slsl = strstr(line, "//");
    if(slsl) *slsl = '\0';

    /* strip # comments but protect #RRGGBB(AA) color literals */
    char *p = line;
    while(*p) {
      if(*p == '#') {
        /* count following hex digits */
        int hx = 0;
        while(isxdigit((unsigned char)p[1 + hx]))
          hx++;
        if(hx == 6 || hx == 8) {
          /* color literal — skip past it */
          p += 1 + hx;
          continue;
        }
        /* comment — truncate here */
        *p = '\0';
        break;
      }
      p++;
    }

    /* skip $variable definitions, blocks, blank lines */
    const char *t = line;
    while(*t == ' ' || *t == '\t')
      t++;
    if(!*t || *t == '$' || *t == '}') continue;

    /* find first '=' not inside parens (to skip rgba(...)=...) */
    int         depth = 0;
    const char *eq    = NULL;
    for(const char *c = t; *c; c++) {
      if(*c == '(')
        depth++;
      else if(*c == ')')
        depth--;
      else if(*c == '=' && depth == 0 && *(c + 1) != '=') {
        eq = c;
        break;
      }
    }
    if(!eq) continue;

    /* extract key: walk back from '=' skipping spaces */
    const char *ke = eq - 1;
    while(ke > t && (*ke == ' ' || *ke == '\t'))
      ke--;
    const char *ks   = t;
    /* key chars: alnum, _, - */
    int         klen = 0;
    const char *kp   = ks;
    while(*kp && (isalnum((unsigned char)*kp) || *kp == '_' || *kp == '-')) {
      kp++;
      klen++;
    }
    /* check that what follows the key (ignoring spaces) is actually '=' */
    const char *after = kp;
    while(*after == ' ' || *after == '\t')
      after++;
    if(*after != '=') continue;
    if(klen == 0 || klen > 255) continue;

    /* extract value: everything after '=' trimmed */
    const char *vs = eq + 1;
    while(*vs == ' ' || *vs == '\t')
      vs++;
    const char *ve = vs + strlen(vs) - 1;
    while(ve > vs && (*ve == ' ' || *ve == '\t' || *ve == '\r'))
      ve--;

    Entry *e = &out[n++];
    e->lnum  = i;
    e->col_s = (int)(ks - line);
    e->col_e = e->col_s + klen;
    strncpy(e->key, ks, klen);
    e->key[klen] = '\0';
    int vlen     = (int)(ve - vs + 1);
    if(vlen < 0) vlen = 0;
    if(vlen > 1023) vlen = 1023;
    strncpy(e->val, vs, vlen);
    e->val[vlen] = '\0';
  }
  return n;
}

static bool is_color(const char *v) {
  if(v[0] == '$') return true; /* variable ref */
  if(v[0] == '#' && strlen(v) >= 7) return true;
  if(!strncasecmp(v, "0x", 2)) return true;
  if(!strncasecmp(v, "rgba(", 5)) return true;
  if(!strncasecmp(v, "rgb(", 4)) return true;
  return false;
}

static bool is_number(const char *v) {
  const char *p = v;
  if(*p == '-') p++;
  if(!isdigit((unsigned char)*p)) return false;
  while(isdigit((unsigned char)*p) || *p == '.')
    p++;
  /* allow unit suffixes */
  if(!strcasecmp(p, "px") || !strcasecmp(p, "hz") || !strcasecmp(p, "ms") ||
     *p == '%' || *p == '\0')
    return true;
  return false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  LSP message I/O
 * ═══════════════════════════════════════════════════════════════════════════ */

static char *read_message(void) {
  /* Read headers one byte at a time to handle \r\n and \n cleanly */
  int  content_length = -1;
  char hdr[256];
  int  hi = 0;
  int  c;

  /* read lines until blank line */
  while((c = fgetc(stdin)) != EOF) {
    if(c == '\r') continue; /* skip CR */
    if(c == '\n') {
      hdr[hi] = '\0';
      if(hi == 0) break; /* blank line = end of headers */
      if(!strncasecmp(hdr, "Content-Length:", 15)) content_length = atoi(hdr + 15);
      hi = 0;
      continue;
    }
    if(hi < 255) hdr[hi++] = (char)c;
  }

  if(content_length <= 0) return NULL;
  char *buf = malloc(content_length + 1);
  if(!buf) return NULL;
  int got = 0;
  while(got < content_length) {
    int r = (int)fread(buf + got, 1, content_length - got, stdin);
    if(r <= 0) {
      free(buf);
      return NULL;
    }
    got += r;
  }
  buf[content_length] = '\0';
  return buf;
}

static void send_message(const char *body) {
  fprintf(stdout, "Content-Length: %d\r\n\r\n%s", (int)strlen(body), body);
  fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  Response builders
 * ═══════════════════════════════════════════════════════════════════════════ */

static void send_response(int id, const char *result) {
  JBuf b;
  jb_init(&b);
  jb_raw(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
  jb_int(&b, id);
  jb_raw(&b, ",\"result\":");
  jb_raw(&b, result);
  jb_raw(&b, "}");
  send_message(b.s);
  jb_free(&b);
}

static void send_notification(const char *method, const char *params) {
  JBuf b;
  jb_init(&b);
  jb_raw(&b, "{\"jsonrpc\":\"2.0\",\"method\":");
  jb_str(&b, method);
  jb_raw(&b, ",\"params\":");
  jb_raw(&b, params);
  jb_raw(&b, "}");
  send_message(b.s);
  jb_free(&b);
}

static void send_null_response(int id) {
  send_response(id, "null");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  Diagnostics
 * ═══════════════════════════════════════════════════════════════════════════ */

/* severity: 1=error 2=warn 3=info 4=hint */
static void push_diagnostics(const Doc *d) {
  Entry entries[MAX_ENTRIES];
  int   n = parse_doc(d, entries);

  JBuf b;
  jb_init(&b);
  jb_raw(&b, "{\"uri\":");
  jb_str(&b, d->uri);
  jb_raw(&b, ",\"diagnostics\":[");

  bool first = true;
  for(int i = 0; i < n; i++) {
    const Entry  *e        = &entries[i];
    const Schema *info     = schema_find(e->key);
    int           sev      = 0;
    char          msg[512] = { 0 };

    if(!info) {
      sev = 2;
      snprintf(msg, sizeof(msg), "Unknown key '%s'", e->key);
    } else if(e->val[0]) {
      if(info->type == T_BOOL) {
        bool ok = false;
        for(int j = 0; BOOL_VALS[j]; j++)
          if(!strcasecmp(e->val, BOOL_VALS[j])) {
            ok = true;
            break;
          }
        if(!ok) {
          sev = 2;
          snprintf(msg,
                   sizeof(msg),
                   "'%s' expects true/false/yes/no/on/off, got '%s'",
                   e->key,
                   e->val);
        }
      } else if(info->type == T_ENUM && info->vals) {
        bool ok = false;
        for(int j = 0; info->vals[j]; j++)
          if(!strcasecmp(e->val, info->vals[j])) {
            ok = true;
            break;
          }
        if(!ok) {
          sev            = 2;
          /* build options list */
          char opts[256] = { 0 };
          for(int j = 0; info->vals[j]; j++) {
            if(j) strncat(opts, ", ", sizeof(opts) - strlen(opts) - 1);
            strncat(opts, info->vals[j], sizeof(opts) - strlen(opts) - 1);
          }
          snprintf(msg,
                   sizeof(msg),
                   "'%s': '%s' not valid. Options: %s",
                   e->key,
                   e->val,
                   opts);
        }
      } else if(info->type == T_INT || info->type == T_FLOAT) {
        if(!is_number(e->val)) {
          sev = 2;
          snprintf(
              msg, sizeof(msg), "'%s' expects a number, got '%s'", e->key, e->val);
        }
      } else if(info->type == T_COLOR) {
        if(!is_color(e->val)) {
          sev = 3;
          snprintf(msg,
                   sizeof(msg),
                   "'%s': '%s' doesn't look like a colour (#RRGGBB, rgba(...), "
                   "0xRRGGBBAA)",
                   e->key,
                   e->val);
        }
      }
    }

    if(!sev) continue;

    if(!first) jb_raw(&b, ",");
    first = false;

    jb_raw(&b, "{\"range\":{\"start\":{\"line\":");
    jb_int(&b, e->lnum);
    jb_raw(&b, ",\"character\":");
    jb_int(&b, e->col_s);
    jb_raw(&b, "},\"end\":{\"line\":");
    jb_int(&b, e->lnum);
    jb_raw(&b, ",\"character\":");
    jb_int(&b, e->col_e);
    jb_raw(&b, "}},\"severity\":");
    jb_int(&b, sev);
    jb_raw(&b, ",\"source\":\"trixie\",\"message\":");
    jb_str(&b, msg);
    jb_raw(&b, "}");
  }

  jb_raw(&b, "]}");
  send_notification("textDocument/publishDiagnostics", b.s);
  jb_free(&b);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §9  Completion
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *ACTIONS[]   = { "exec",
                                   "close",
                                   "fullscreen",
                                   "togglefloating",
                                   "toggle_float",
                                   "togglebar",
                                   "toggle_bar",
                                   "movefocus",
                                   "focus",
                                   "movewindow",
                                   "move",
                                   "workspace",
                                   "movetoworkspace",
                                   "move_to_workspace",
                                   "nextlayout",
                                   "next_layout",
                                   "prevlayout",
                                   "prev_layout",
                                   "growmain",
                                   "grow_main",
                                   "shrinkmain",
                                   "shrink_main",
                                   "nextworkspace",
                                   "next_workspace",
                                   "prevworkspace",
                                   "prev_workspace",
                                   "swapwithmaster",
                                   "swap_main",
                                   "switchvt",
                                   "switch_vt",
                                   "scratchpad",
                                   "reload",
                                   "exit",
                                   "quit",
                                   "emergency_quit",
                                   "resize_ratio",
                                   NULL };
static const char *MODIFIERS[] = { "SUPER", "SHIFT", "CTRL", "ALT",  "META", "HYPER",
                                   "MOD1",  "MOD2",  "MOD3", "MOD4", "MOD5", NULL };
static const char *SECTIONS[]  = { "general",    "decoration", "colors",
                                   "input",      "keyboard",   "bar",
                                   "animations", "overlay",    "monitor",
                                   "scratchpad", "bar_module", "dev_lang",
                                   "workspace",  NULL };
static const char *GESTURE_SPECS[] = { "swipe:3:left",
                                       "swipe:3:right",
                                       "swipe:3:up",
                                       "swipe:3:down",
                                       "swipe:4:left",
                                       "swipe:4:right",
                                       "swipe:4:up",
                                       "swipe:4:down",
                                       "pinch:2:in",
                                       "pinch:2:out",
                                       "pinch:3:in",
                                       "pinch:3:out",
                                       NULL };

static void handle_completion(int id, const char *params) {
  char *td  = json_obj(params, "textDocument");
  char *pos = json_obj(params, "position");
  char *uri = td ? json_str(td, "uri") : NULL;
  int   row = pos ? json_int(pos, "line", 0) : 0;
  int   col = pos ? json_int(pos, "character", 0) : 0;
  free(td);
  free(pos);

  Doc *d = uri ? doc_find(uri) : NULL;
  free(uri);

  JBuf b;
  jb_init(&b);
  jb_raw(&b, "[");
  bool first = true;

#define ITEM(label, kind, detail, doc_str)                              \
  do {                                                                  \
    if(!first) jb_raw(&b, ",");                                         \
    first = false;                                                      \
    jb_raw(&b, "{\"label\":");                                          \
    jb_str(&b, label);                                                  \
    jb_raw(&b, ",\"kind\":");                                           \
    jb_int(&b, kind);                                                   \
    jb_raw(&b, ",\"detail\":");                                         \
    jb_str(&b, detail);                                                 \
    jb_raw(&b, ",\"documentation\":{\"kind\":\"markdown\",\"value\":"); \
    jb_str(&b, doc_str);                                                \
    jb_raw(&b, "}}");                                                   \
  } while(0)

  if(d && row < d->nlines) {
    const char *line = d->lines[row];
    /* get text before cursor */
    char        before[MAX_LINE];
    int         blen = col < MAX_LINE ? col : MAX_LINE - 1;
    strncpy(before, line, blen);
    before[blen] = '\0';

    /* check if we're after an '=' */
    char *eq = strrchr(before, '=');
    if(eq) {
      /* find the key */
      char  key[256] = { 0 };
      char *kend     = eq - 1;
      while(kend > before && (*kend == ' ' || *kend == '\t'))
        kend--;
      char *kstart = kend;
      while(kstart > before && (isalnum((unsigned char)*(kstart - 1)) ||
                                *(kstart - 1) == '_' || *(kstart - 1) == '-'))
        kstart--;
      int klen = (int)(kend - kstart + 1);
      if(klen > 0 && klen < 255) {
        strncpy(key, kstart, klen);
        key[klen] = '\0';
      }

      const Schema *info = schema_find(key);
      if(info) {
        if(info->type == T_BOOL) {
          for(int j = 0; BOOL_VALS[j]; j++)
            ITEM(BOOL_VALS[j], 13, "bool", "");
        } else if((info->type == T_ENUM || info->type == T_MULTI) && info->vals) {
          for(int j = 0; info->vals[j]; j++)
            ITEM(info->vals[j], 13, "value", "");
        } else if(info->type == T_COLOR) {
          ITEM("rgba(rrggbbaa)", 16, "color", "e.g. rgba(1e1e2eff)");
          ITEM("#RRGGBB", 16, "color", "6-digit hex");
          ITEM("#RRGGBBAA", 16, "color", "8-digit hex with alpha");
          ITEM("0xRRGGBBAA", 16, "color", "0x-prefixed with alpha");
        }
      }

      /* bind field completions */
      if(!strcasecmp(key, "bind") || !strcasecmp(key, "binde") ||
         !strcasecmp(key, "bindm")) {
        int ncommas = 0;
        for(char *c = before; *c; c++)
          if(*c == ',') ncommas++;
        if(ncommas == 0) {
          for(int j = 0; MODIFIERS[j]; j++)
            ITEM(MODIFIERS[j], 13, "modifier", "");
        } else if(ncommas == 2) {
          for(int j = 0; ACTIONS[j]; j++)
            ITEM(ACTIONS[j], 3, "action", "");
        }
      }

      /* gesture field completions */
      if(!strcasecmp(key, "gesture")) {
        int ncommas = 0;
        for(char *c = before; *c; c++)
          if(*c == ',') ncommas++;
        if(ncommas == 0) {
          for(int j = 0; GESTURE_SPECS[j]; j++)
            ITEM(GESTURE_SPECS[j], 13, "gesture", "");
        } else if(ncommas == 1) {
          for(int j = 0; ACTIONS[j]; j++)
            ITEM(ACTIONS[j], 3, "action", "");
          ITEM("exec:command", 3, "action", "Run a shell command");
          ITEM("workspace:N", 3, "action", "Switch to workspace N");
        }
      }

    } else {
      /* key completions at line start */
      for(int i = 0; SCHEMA[i].key; i++) {
        char        doc_str[512];
        const char *tname = "string";
        switch(SCHEMA[i].type) {
          case T_INT: tname = "int"; break;
          case T_FLOAT: tname = "float"; break;
          case T_BOOL: tname = "bool"; break;
          case T_COLOR: tname = "color"; break;
          case T_ENUM: tname = "enum"; break;
          case T_MULTI: tname = "multi"; break;
          default: break;
        }
        snprintf(doc_str,
                 sizeof(doc_str),
                 "**%s** _(%s)_\n\n%s",
                 SCHEMA[i].key,
                 tname,
                 SCHEMA[i].doc);
        ITEM(SCHEMA[i].key, 5, tname, doc_str);
      }
      for(int i = 0; SECTIONS[i]; i++)
        ITEM(SECTIONS[i], 14, "section", "");
    }
  }
#undef ITEM

  jb_raw(&b, "]");
  send_response(id, b.s);
  jb_free(&b);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §10  Hover
 * ═══════════════════════════════════════════════════════════════════════════ */

static void handle_hover(int id, const char *params) {
  char *td  = json_obj(params, "textDocument");
  char *pos = json_obj(params, "position");
  char *uri = td ? json_str(td, "uri") : NULL;
  int   row = pos ? json_int(pos, "line", 0) : 0;
  int   col = pos ? json_int(pos, "character", 0) : 0;
  free(td);
  free(pos);

  Doc *d = uri ? doc_find(uri) : NULL;
  free(uri);

  if(!d || row >= d->nlines) {
    send_null_response(id);
    return;
  }

  const char *line = d->lines[row];
  /* find word under cursor */
  int         ws = col, we = col;
  while(ws > 0 && (isalnum((unsigned char)line[ws - 1]) || line[ws - 1] == '_' ||
                   line[ws - 1] == '-'))
    ws--;
  while(line[we] &&
        (isalnum((unsigned char)line[we]) || line[we] == '_' || line[we] == '-'))
    we++;
  if(ws == we) {
    send_null_response(id);
    return;
  }

  char word[256] = { 0 };
  int  wlen      = we - ws;
  if(wlen > 255) wlen = 255;
  strncpy(word, line + ws, wlen);

  const Schema *info = schema_find(word);
  if(!info) {
    send_null_response(id);
    return;
  }

  const char *tname = "string";
  switch(info->type) {
    case T_INT: tname = "int"; break;
    case T_FLOAT: tname = "float"; break;
    case T_BOOL: tname = "bool"; break;
    case T_COLOR: tname = "color"; break;
    case T_ENUM: tname = "enum"; break;
    case T_MULTI: tname = "multi"; break;
    default: break;
  }

  JBuf content;
  jb_init(&content);
  /* build markdown string */
  char md[1024];
  snprintf(md, sizeof(md), "**%s** _(%s)_\n\n%s", word, tname, info->doc);
  if(info->vals) {
    strncat(md, "\n\n**Values:** `", sizeof(md) - strlen(md) - 1);
    for(int i = 0; info->vals[i]; i++) {
      if(i) strncat(md, "`, `", sizeof(md) - strlen(md) - 1);
      strncat(md, info->vals[i], sizeof(md) - strlen(md) - 1);
    }
    strncat(md, "`", sizeof(md) - strlen(md) - 1);
  }

  jb_raw(&content, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
  jb_str(&content, md);
  jb_raw(&content, "}}");
  send_response(id, content.s);
  jb_free(&content);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §11  Main loop
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void) {
  /* LSP requires binary-safe I/O */
  /* use line-buffered stdout so responses flush immediately */
  setvbuf(stdout, NULL, _IONBF, 0);

  for(;;) {
    char *msg = read_message();
    if(!msg) break;

    /* extract method and id */
    char *method = json_str(msg, "method");
    int   id     = json_int(msg, "id", -1);

    if(!method) {
      free(msg);
      continue;
    }

    if(!strcmp(method, "initialize")) {
      send_response(
          id,
          "{"
          "\"capabilities\":{"
          "\"textDocumentSync\":1,"
          "\"completionProvider\":{\"triggerCharacters\":[\"=\",\" \",\",\"]},"
          "\"hoverProvider\":true"
          "},"
          "\"serverInfo\":{\"name\":\"trixie-lsp\",\"version\":\"1.0.0\"}"
          "}");

    } else if(!strcmp(method, "initialized")) {
      /* no-op notification */

    } else if(!strcmp(method, "shutdown")) {
      send_null_response(id);

    } else if(!strcmp(method, "exit")) {
      free(method);
      free(msg);
      return 0;

    } else if(!strcmp(method, "textDocument/didOpen")) {
      char *td   = json_obj(msg, "textDocument");
      char *uri  = td ? json_str(td, "uri") : NULL;
      char *text = td ? json_str(td, "text") : NULL;
      int   ver  = td ? json_int(td, "version", 0) : 0;
      if(uri && text) {
        Doc *d = doc_open(uri);
        if(d) {
          d->version = ver;
          doc_set_text(d, text);
          push_diagnostics(d);
        }
      }
      free(td);
      free(uri);
      free(text);

    } else if(!strcmp(method, "textDocument/didChange")) {
      char       *td   = json_obj(msg, "textDocument");
      char       *uri  = td ? json_str(td, "uri") : NULL;
      int         ver  = td ? json_int(td, "version", 0) : 0;
      /* get first contentChange text */
      char       *text = NULL;
      const char *cc   = strstr(msg, "\"contentChanges\"");
      if(cc) {
        cc = strchr(cc, '[');
        if(cc) {
          char       *obj = json_obj(cc + 1, "text");
          /* obj approach won't work for array — use json_str on the slice */
          const char *tp  = strstr(cc, "\"text\"");
          if(tp) {
            tp += 6;
            while(*tp == ':' || *tp == ' ')
              tp++;
            if(*tp == '"') {
              tp++;
              char buf[65536];
              int  bi = 0;
              while(*tp && bi < 65535) {
                if(*tp == '\\' && *(tp + 1)) {
                  tp++;
                  buf[bi++] = *tp++;
                  continue;
                }
                if(*tp == '"') break;
                buf[bi++] = *tp++;
              }
              buf[bi] = '\0';
              text    = strdup(buf);
            }
          }
          free(obj);
        }
      }
      if(uri && text) {
        Doc *d = doc_find(uri);
        if(!d) d = doc_open(uri);
        if(d) {
          d->version = ver;
          doc_set_text(d, text);
          push_diagnostics(d);
        }
      }
      free(td);
      free(uri);
      free(text);

    } else if(!strcmp(method, "textDocument/didClose")) {
      /* nothing to do — keep the doc in memory */

    } else if(!strcmp(method, "textDocument/completion")) {
      handle_completion(id, msg);

    } else if(!strcmp(method, "textDocument/hover")) {
      handle_hover(id, msg);

    } else if(id >= 0) {
      /* unknown request — send null */
      send_null_response(id);
    }

    free(method);
    free(msg);
  }
  return 0;
}
