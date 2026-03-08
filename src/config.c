/* config.c — Trixie config parser (Hyprland syntax)
 *
 * SYNTAX
 * ──────
 * Variables:        $name = value
 * Key/value:        key = value
 * Anonymous block:  section { key = value }
 *
 * Directives:
 *   source    = ~/.config/trixie/theme.conf
 *   exec-once = dunst
 *   exec      = nm-applet
 *
 * Keybinds (Hypr
 *   bind = SUPER, t, exec, kitty
 *   bind = SUPER SHIFT, f, togglefloating
 *   bind = , xf86audioraisevolume, exec, wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+
 *
 * Window rules:
 *   windowrule  = float, pavucontrol
 *   windowrule  = size 700 500, pavucontrol
 *   windowrulev2 = float, class:^(pavucontrol)$
 *   windowrulev2 = float, title:^(Picture-in-Picture)$
 *
 * Labeled blocks:
 *   monitor = eDP-1, 1920x1080@144, 0x0, 1.0
 *
 *   monitor eDP-1 {
 *       width = 1920  height = 1080  refresh = 144  scale = 1.0
 *   }
 *
 *   scratchpad music {
 *       app_id = pear-desktop
 *       size   = 80%, 75%
 *   }
 *
 *   bar_module clock { format = "%a %d %b  %H:%M" }
 *
 *   workspace 3 { layout = spiral  ratio = 0.6 }
 *
 * Colors (any of):
 *   rgba(rrggbbaa)   rgb(rrggbb)   0xRRGGBBAA   #RRGGBB   #RRGGBBAA
 *
 * Comments:  # …   // …
 *
 * DESIGN NOTES
 * ────────────
 * • Single-pass character lexer, one-token lookahead.
 * • Variable table is shared across all sourced files (same pointer).
 * • Variables are expanded eagerly at lex time.  Values that reference
 *   undefined variables are stored as empty string with a DEBUG log.
 * • collect_vals() reads ALL remaining word/comma tokens on the current
 *   logical line (stopping at newline, EOF, or '}'), so both
 *   comma-separated and space-separated multi-field values work uniformly.
 * • Unknown keys inside known blocks: warn, continue.
 * • Unknown top-level blocks: skip (brace-counted).
 * • source depth limit: 8 (prevents circular includes).
 * • col. prefix on keys is stripped before matching.
 * • Dimension suffixes (px hz % ms) are accepted and ignored.
 */

#define _POSIX_C_SOURCE 200809L
#include "trixie.h"
#include <ctype.h>
#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Forward declaration — defined in §12b below the parser that calls it. */
static void apply_theme(Config *c, const char *name);

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Variable table
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_VARS     256
#define MAX_VAR_NAME 128
#define MAX_VAR_VAL  512

typedef struct {
  char name[MAX_VAR_NAME];
  char val[MAX_VAR_VAL];
} CfgVar;
typedef struct {
  CfgVar v[MAX_VARS];
  int    n;
} VarTable;

static void vt_set(VarTable *vt, const char *name, const char *val) {
  for(int i = 0; i < vt->n; i++) {
    if(!strcmp(vt->v[i].name, name)) {
      strncpy(vt->v[i].val, val, MAX_VAR_VAL - 1);
      return;
    }
  }
  if(vt->n >= MAX_VARS) {
    wlr_log(WLR_ERROR, "config: variable table full, dropping $%s", name);
    return;
  }
  strncpy(vt->v[vt->n].name, name, MAX_VAR_NAME - 1);
  strncpy(vt->v[vt->n].val, val, MAX_VAR_VAL - 1);
  vt->n++;
}

static const char *vt_get(const VarTable *vt, const char *name) {
  for(int i = 0; i < vt->n; i++)
    if(!strcmp(vt->v[i].name, name)) return vt->v[i].val;
  return NULL;
}

/* Recursively resolve $var → value, up to 16 hops (catches cycles). */
static const char *vt_expand(const VarTable *vt, const char *name, int depth) {
  if(depth > 16) {
    wlr_log(WLR_ERROR, "config: variable cycle at $%s", name);
    return NULL;
  }
  const char *val = vt_get(vt, name);
  if(!val) return NULL;
  if(val[0] == '$') {
    const char *inner = vt_expand(vt, val + 1, depth + 1);
    return inner ? inner : val;
  }
  return val;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Color helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint8_t hex2(const char *s) {
  uint8_t v = 0;
  for(int i = 0; i < 2; i++) {
    v <<= 4;
    char c = s[i];
    if(c >= '0' && c <= '9')
      v |= (uint8_t)(c - '0');
    else if(c >= 'a' && c <= 'f')
      v |= (uint8_t)(c - 'a' + 10);
    else if(c >= 'A' && c <= 'F')
      v |= (uint8_t)(c - 'A' + 10);
  }
  return v;
}

static bool parse_color_str(const char *s, Color *out) {
  if(!s || !*s) return false;
  while(*s == ' ' || *s == '\t')
    s++;

  if(!strncasecmp(s, "rgba(", 5)) {
    const char *h = s + 5;
    int         n = 0;
    while(isxdigit((unsigned char)h[n]))
      n++;
    if(n != 8) return false;
    out->r = hex2(h);
    out->g = hex2(h + 2);
    out->b = hex2(h + 4);
    out->a = hex2(h + 6);
    return true;
  }
  if(!strncasecmp(s, "rgb(", 4)) {
    const char *h = s + 4;
    int         n = 0;
    while(isxdigit((unsigned char)h[n]))
      n++;
    if(n != 6) return false;
    out->r = hex2(h);
    out->g = hex2(h + 2);
    out->b = hex2(h + 4);
    out->a = 0xff;
    return true;
  }
  if(s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    const char *h = s + 2;
    int         n = 0;
    while(isxdigit((unsigned char)h[n]))
      n++;
    if(n == 8) {
      out->r = hex2(h);
      out->g = hex2(h + 2);
      out->b = hex2(h + 4);
      out->a = hex2(h + 6);
      return true;
    }
    if(n == 6) {
      out->r = hex2(h);
      out->g = hex2(h + 2);
      out->b = hex2(h + 4);
      out->a = 0xff;
      return true;
    }
    return false;
  }
  if(s[0] == '#') {
    const char *h = s + 1;
    int         n = 0;
    while(isxdigit((unsigned char)h[n]))
      n++;
    if(n == 8) {
      out->r = hex2(h);
      out->g = hex2(h + 2);
      out->b = hex2(h + 4);
      out->a = hex2(h + 6);
      return true;
    }
    if(n == 6) {
      out->r = hex2(h);
      out->g = hex2(h + 2);
      out->b = hex2(h + 4);
      out->a = 0xff;
      return true;
    }
    return false;
  }
  return false;
}

static Color color_from_str(const char *s) {
  Color c = { 0, 0, 0, 255 };
  if(!parse_color_str(s, &c))
    wlr_log(WLR_DEBUG, "config: unrecognised color '%s'", s ? s : "(null)");
  return c;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Lexer
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum { T_WORD, T_EQ, T_COMMA, T_LBRACE, T_RBRACE, T_NEWLINE, T_EOF } TK;

typedef struct {
  TK   kind;
  char s[1024];
  int  line;
} Token;

typedef struct {
  const char *src;
  int         pos, line;
  VarTable   *vt;
  Token       peek;
  bool        have_peek;
} Lexer;

static void skip_ws(Lexer *l, bool stop_nl) {
  for(;;) {
    char c = l->src[l->pos];
    if(!c) return;
    if(c == '\n') {
      if(stop_nl) return;
      l->line++;
      l->pos++;
      continue;
    }
    if(c == ' ' || c == '\t' || c == '\r') {
      l->pos++;
      continue;
    }
    /* // comment */
    if(c == '/' && l->src[l->pos + 1] == '/') {
      while(l->src[l->pos] && l->src[l->pos] != '\n')
        l->pos++;
      continue;
    }
    /* # comment — but not if it's #RRGGBB or #RRGGBBAA */
    if(c == '#') {
      int j = l->pos + 1, hx = 0;
      while(isxdigit((unsigned char)l->src[j])) {
        j++;
        hx++;
      }
      if(hx == 6 || hx == 8) return; /* color literal */
      while(l->src[l->pos] && l->src[l->pos] != '\n')
        l->pos++;
      continue;
    }
    return;
  }
}

static Token lex_raw(Lexer *l) {
  Token t = { 0 };
  t.line  = l->line;

  skip_ws(l, true);

  char c = l->src[l->pos];
  if(!c) {
    t.kind = T_EOF;
    return t;
  }
  if(c == '\n') {
    l->line++;
    l->pos++;
    t.kind = T_NEWLINE;
    return t;
  }
  if(c == '=') {
    l->pos++;
    t.kind = T_EQ;
    return t;
  }
  if(c == ',') {
    l->pos++;
    t.kind = T_COMMA;
    return t;
  }
  if(c == '{') {
    l->pos++;
    t.kind = T_LBRACE;
    return t;
  }
  if(c == '}') {
    l->pos++;
    t.kind = T_RBRACE;
    return t;
  }
  /* [ ] used in module lists — transparent punctuation, skip silently */
  if(c == '[' || c == ']') {
    l->pos++;
    return lex_raw(l);
  }

  /* quoted string */
  if(c == '"' || c == '\'') {
    char q = c;
    l->pos++;
    int i = 0;
    while(l->src[l->pos] && l->src[l->pos] != q && i < (int)sizeof(t.s) - 1) {
      if(l->src[l->pos] == '\\' && l->src[l->pos + 1]) {
        l->pos++;
        switch(l->src[l->pos]) {
          case 'n': t.s[i++] = '\n'; break;
          case 't': t.s[i++] = '\t'; break;
          default: t.s[i++] = l->src[l->pos]; break;
        }
      } else {
        t.s[i++] = l->src[l->pos];
      }
      l->pos++;
    }
    if(l->src[l->pos] == q) l->pos++;
    t.s[i] = '\0';
    t.kind = T_WORD;
    return t;
  }

  /* $variable */
  if(c == '$') {
    l->pos++;
    int ns = l->pos;
    while(isalnum((unsigned char)l->src[l->pos]) || l->src[l->pos] == '_')
      l->pos++;
    char name[MAX_VAR_NAME] = { 0 };
    int  nlen               = l->pos - ns;
    if(nlen >= MAX_VAR_NAME) nlen = MAX_VAR_NAME - 1;
    memcpy(name, l->src + ns, nlen);

    /* peek: is this a definition ($name = ...)? Return raw $name token. */
    int sp = l->pos, sl = l->line;
    skip_ws(l, true);
    bool is_def = (l->src[l->pos] == '=' && l->src[l->pos + 1] != '=');
    l->pos      = sp;
    l->line     = sl;

    if(is_def) {
      snprintf(t.s, sizeof(t.s), "$%s", name);
      t.kind = T_WORD;
      return t;
    }

    const char *val = vt_expand(l->vt, name, 0);
    if(val)
      strncpy(t.s, val, sizeof(t.s) - 1);
    else
      wlr_log(WLR_DEBUG, "config:%d: undefined $%s", l->line, name);
    t.kind = T_WORD;
    return t;
  }

  /* rgba(...) / rgb(...)  — capture through closing paren as one token */
  if(!strncasecmp(l->src + l->pos, "rgba(", 5) ||
     !strncasecmp(l->src + l->pos, "rgb(", 4)) {
    int i = 0;
    while(l->src[l->pos] && l->src[l->pos] != ')' && i < (int)sizeof(t.s) - 2)
      t.s[i++] = l->src[l->pos++];
    if(l->src[l->pos] == ')') t.s[i++] = l->src[l->pos++];
    t.s[i] = '\0';
    t.kind = T_WORD;
    return t;
  }

  /* bare word — alnum plus chars that appear in paths, keysyms, colors */
  if(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '#' || c == '@' ||
     c == '~' || c == '.' || c == '/' || c == '%' || c == ':' || c == '+') {
    int i = 0;
    while(l->src[l->pos] && l->src[l->pos] != '=' && l->src[l->pos] != ',' &&
          l->src[l->pos] != '{' && l->src[l->pos] != '}' && l->src[l->pos] != '\n' &&
          l->src[l->pos] != '\r' && l->src[l->pos] != '"' &&
          l->src[l->pos] != '\'' && l->src[l->pos] != '#' && /* inline # comment */
          l->src[l->pos] != ' ' && l->src[l->pos] != '\t' &&
          i < (int)sizeof(t.s) - 1) {
      if(l->src[l->pos] == '/' && l->src[l->pos + 1] == '/') break;
      t.s[i++] = l->src[l->pos++];
    }
    /* rtrim */
    while(i > 0 && (t.s[i - 1] == ' ' || t.s[i - 1] == '\t'))
      i--;
    t.s[i] = '\0';
    if(i == 0) {
      l->pos++;
      return lex_raw(l);
    }
    t.kind = T_WORD;
    return t;
  }

  wlr_log(WLR_DEBUG, "config:%d: skipping unexpected char '%c'", l->line, c);
  l->pos++;
  return lex_raw(l);
}

static Token lex_peek(Lexer *l) {
  if(!l->have_peek) {
    l->peek      = lex_raw(l);
    l->have_peek = true;
  }
  return l->peek;
}
static Token lex_next(Lexer *l) {
  Token t      = lex_peek(l);
  l->have_peek = false;
  return t;
}
static bool lex_eat(Lexer *l, TK k) {
  if(lex_peek(l).kind == k) {
    lex_next(l);
    return true;
  }
  return false;
}
static void lex_skip_nl(Lexer *l) {
  while(lex_peek(l).kind == T_NEWLINE)
    lex_next(l);
}
static void lex_skip_to_nl(Lexer *l) {
  for(;;) {
    TK k = lex_peek(l).kind;
    if(k == T_NEWLINE || k == T_EOF) return;
    lex_next(l);
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Value helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static double str_to_num(const char *s) {
  return (s && *s) ? atof(s) : 0.0;
}

static bool str_to_bool(const char *s) {
  return s && (!strcasecmp(s, "true") || !strcasecmp(s, "yes") ||
               !strcasecmp(s, "on") || !strcmp(s, "1"));
}

static const char *strip_col(const char *k) {
  return !strncmp(k, "col.", 4) ? k + 4 : k;
}

static void strtrim(char *s) {
  if(!s) return;
  char *p = s;
  while(*p == ' ' || *p == '\t')
    memmove(p, p + 1, strlen(p));
  int n = (int)strlen(s);
  while(n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
    s[--n] = '\0';
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  collect_vals
 *
 * Reads ALL remaining T_WORD tokens on the current logical line into cv.s[].
 * Commas are eaten as separators; spaces already split words in the lexer.
 * Stops at T_NEWLINE, T_EOF, T_RBRACE.
 * `first` (already consumed by the caller) goes into cv.s[0].
 *
 * This single mechanism handles:
 *   "a, b, c"         → [a][b][c]
 *   "a b c"           → [a][b][c]
 *   "size 900 550"    → [size][900][550]
 *   "80%, 75%"        → [80%][75%]
 *   "[workspaces]"    → [workspaces]   (brackets stripped by lexer)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define CV_MAX 32
typedef struct {
  char s[CV_MAX][512];
  int  n;
} CVals;

static CVals collect_vals(Lexer *l, const char *first) {
  CVals cv = { 0 };
  if(first && *first) {
    strncpy(cv.s[cv.n], first, 511);
    strtrim(cv.s[cv.n]);
    cv.n++;
  }
  for(;;) {
    TK pk = lex_peek(l).kind;
    if(pk == T_NEWLINE || pk == T_EOF || pk == T_RBRACE || pk == T_LBRACE ||
       pk == T_EQ)
      break;
    if(pk == T_COMMA) {
      lex_next(l);
      continue;
    }
    if(pk == T_WORD) {
      if(cv.n >= CV_MAX) {
        lex_next(l);
        continue;
      }
      Token t = lex_next(l);
      strncpy(cv.s[cv.n], t.s, 511);
      strtrim(cv.s[cv.n]);
      cv.n++;
    } else
      break;
  }
  return cv;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  Geometry helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool parse_WxH(const char *s, int *w, int *h) {
  char buf[64];
  strncpy(buf, s, 63);
  buf[63] = '\0';
  char *x = strchr(buf, 'x');
  if(!x) x = strchr(buf, 'X');
  if(!x) return false;
  *x = '\0';
  *w = atoi(buf);
  *h = atoi(x + 1);
  return true;
}

static float parse_pct_or_float(const char *s) {
  if(!s || !*s) return 0.f;
  float v = (float)atof(s);
  if(strchr(s, '%')) v /= 100.f;
  return v;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  Forward declarations
 * ═══════════════════════════════════════════════════════════════════════════ */

static void parse_block(
    Lexer *l, Config *c, const char *block, const char *label, const char *file_dir);
static void do_source(
    Config *c, VarTable *vt, const char *path, const char *file_dir, int depth);

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  Keybind parser — Hyprland syntax only
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint32_t parse_mods(const char *s) {
  uint32_t mods = 0;
  char     buf[256];
  strncpy(buf, s, 255);
  buf[255] = '\0';
  for(char *tok = strtok(buf, " +,\t"); tok; tok = strtok(NULL, " +,\t")) {
    if(!strcasecmp(tok, "SUPER") || !strcasecmp(tok, "MOD4"))
      mods |= MOD_SUPER;
    else if(!strcasecmp(tok, "CTRL") || !strcasecmp(tok, "CONTROL"))
      mods |= MOD_CTRL;
    else if(!strcasecmp(tok, "ALT") || !strcasecmp(tok, "MOD1"))
      mods |= MOD_ALT;
    else if(!strcasecmp(tok, "SHIFT"))
      mods |= MOD_SHIFT;
  }
  return mods;
}

static void parse_bind(Config *c, Lexer *l, const char *eq_val) {
  CVals cv = collect_vals(l, eq_val);
  for(int i = 0; i < cv.n; i++)
    strtrim(cv.s[i]);

  if(cv.n < 3) {
    wlr_log(
        WLR_DEBUG, "config: bind: too few fields (%d), need mods,key,action", cv.n);
    return;
  }

  uint32_t mods = parse_mods(cv.s[0]);
  char     sym_str[128];
  strncpy(sym_str, cv.s[1], 127);
  sym_str[127]        = '\0';
  const char *act_str = cv.s[2];
  int         arg1    = 3;

  for(char *p = sym_str; *p; p++)
    *p = (char)tolower((unsigned char)*p);

  xkb_keysym_t sym = xkb_keysym_from_name(sym_str, XKB_KEYSYM_CASE_INSENSITIVE);
  if(sym == XKB_KEY_NoSymbol) {
    wlr_log(WLR_DEBUG, "config: bind: unknown keysym '%s'", sym_str);
    return;
  }

#define ARG(n) (cv.n > arg1 + (n) ? cv.s[arg1 + (n)] : "")

  Action act = { 0 };

  const char *a0 = (cv.n > arg1) ? cv.s[arg1] : "";
  const char *a1 = (cv.n > arg1 + 1) ? cv.s[arg1 + 1] : "";
  (void)a1;

  if(!strcasecmp(act_str, "exec")) {
    act.kind = ACTION_EXEC;
    for(int i = arg1; i < cv.n; i++) {
      if(i > arg1)
        strncat(act.exec_cmd, " ", sizeof(act.exec_cmd) - strlen(act.exec_cmd) - 1);
      strncat(
          act.exec_cmd, cv.s[i], sizeof(act.exec_cmd) - strlen(act.exec_cmd) - 1);
    }
  } else if(!strcasecmp(act_str, "killactive") || !strcasecmp(act_str, "close")) {
    act.kind = ACTION_CLOSE;
  } else if(!strcasecmp(act_str, "fullscreen")) {
    act.kind = ACTION_FULLSCREEN;
  } else if(!strcasecmp(act_str, "togglefloating") ||
            !strcasecmp(act_str, "toggle_float")) {
    act.kind = ACTION_TOGGLE_FLOAT;
  } else if(!strcasecmp(act_str, "togglebar") ||
            !strcasecmp(act_str, "toggle_bar")) {
    act.kind = ACTION_TOGGLE_BAR;
  } else if(!strcasecmp(act_str, "movefocus") || !strcasecmp(act_str, "focus")) {
    if(!strcasecmp(a0, "l") || !strcasecmp(a0, "left"))
      act.kind = ACTION_FOCUS_LEFT;
    else if(!strcasecmp(a0, "r") || !strcasecmp(a0, "right"))
      act.kind = ACTION_FOCUS_RIGHT;
    else if(!strcasecmp(a0, "u") || !strcasecmp(a0, "up"))
      act.kind = ACTION_FOCUS_UP;
    else if(!strcasecmp(a0, "d") || !strcasecmp(a0, "down"))
      act.kind = ACTION_FOCUS_DOWN;
  } else if(!strcasecmp(act_str, "movewindow") || !strcasecmp(act_str, "move")) {
    if(!strcasecmp(a0, "l") || !strcasecmp(a0, "left"))
      act.kind = ACTION_MOVE_LEFT;
    else if(!strcasecmp(a0, "r") || !strcasecmp(a0, "right"))
      act.kind = ACTION_MOVE_RIGHT;
    else if(!strcasecmp(a0, "u") || !strcasecmp(a0, "up"))
      act.kind = ACTION_MOVE_UP;
    else if(!strcasecmp(a0, "d") || !strcasecmp(a0, "down"))
      act.kind = ACTION_MOVE_DOWN;
  } else if(!strcasecmp(act_str, "workspace")) {
    act.kind = ACTION_WORKSPACE;
    act.n    = *a0 ? atoi(a0) : 1;
  } else if(!strcasecmp(act_str, "movetoworkspace") ||
            !strcasecmp(act_str, "move_to_workspace")) {
    act.kind = ACTION_MOVE_TO_WS;
    act.n    = *a0 ? atoi(a0) : 1;
  } else if(!strcasecmp(act_str, "nextlayout") ||
            !strcasecmp(act_str, "next_layout")) {
    act.kind = ACTION_NEXT_LAYOUT;
  } else if(!strcasecmp(act_str, "prevlayout") ||
            !strcasecmp(act_str, "prev_layout")) {
    act.kind = ACTION_PREV_LAYOUT;
  } else if(!strcasecmp(act_str, "growmain") || !strcasecmp(act_str, "grow_main")) {
    act.kind = ACTION_GROW_MAIN;
  } else if(!strcasecmp(act_str, "shrinkmain") ||
            !strcasecmp(act_str, "shrink_main")) {
    act.kind = ACTION_SHRINK_MAIN;
  } else if(!strcasecmp(act_str, "nextworkspace") ||
            !strcasecmp(act_str, "next_workspace")) {
    act.kind = ACTION_NEXT_WS;
  } else if(!strcasecmp(act_str, "prevworkspace") ||
            !strcasecmp(act_str, "prev_workspace")) {
    act.kind = ACTION_PREV_WS;
  } else if(!strcasecmp(act_str, "exit") || !strcasecmp(act_str, "quit")) {
    act.kind = ACTION_QUIT;
  } else if(!strcasecmp(act_str, "reload")) {
    act.kind = ACTION_RELOAD;
  } else if(!strcasecmp(act_str, "swapwithmaster") ||
            !strcasecmp(act_str, "swap_main")) {
    act.kind = ACTION_SWAP_MAIN;
  } else if(!strcasecmp(act_str, "switchvt") || !strcasecmp(act_str, "switch_vt")) {
    act.kind = ACTION_SWITCH_VT;
    act.n    = *a0 ? atoi(a0) : 1;
  } else if(!strcasecmp(act_str, "scratchpad")) {
    act.kind = ACTION_SCRATCHPAD;
    if(*a0) strncpy(act.name, a0, sizeof(act.name) - 1);
  } else if(!strcasecmp(act_str, "emergency_quit") ||
            !strcasecmp(act_str, "emergencyquit")) {
    act.kind = ACTION_EMERGENCY_QUIT;
    /* FIX/FEATURE: resize_ratio action (Feature 3) */
  } else if(!strcasecmp(act_str, "resize_ratio")) {
    act.kind        = ACTION_RESIZE_RATIO;
    act.ratio_delta = *a0 ? (float)atof(a0) : 0.05f;
  } else {
    wlr_log(WLR_DEBUG, "config: bind: unknown action '%s'", act_str);
    return;
  }

  if(c->keybind_count >= MAX_KEYBINDS) {
    wlr_log(WLR_ERROR, "config: MAX_KEYBINDS reached, bind dropped");
    return;
  }
  c->keybinds[c->keybind_count].mods   = mods;
  c->keybinds[c->keybind_count].sym    = sym;
  c->keybinds[c->keybind_count].action = act;
  wlr_log(WLR_DEBUG,
          "config: keybind[%d] mods=0x%x sym='%s' action=%d",
          c->keybind_count,
          mods,
          sym_str,
          act.kind);
  c->keybind_count++;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §9  Window rule parser — Hyprland syntax
 * ═══════════════════════════════════════════════════════════════════════════ */

static int apply_wr_effect(WinRule *r, CVals *cv, int ei) {
  const char *rs       = cv->s[ei];
  int         consumed = 1;

  if(!strncasecmp(rs, "float", 5)) {
    r->float_rule = true;
  } else if(!strncasecmp(rs, "fullscreen", 10)) {
    r->fullscreen_rule = true;
  } else if(!strncasecmp(rs, "noborder", 8)) {
    r->noborder = true;
  } else if(!strncasecmp(rs, "notitle", 7)) {
    r->notitle = true;
  } else if(!strncasecmp(rs, "workspace", 9)) {
    if(ei + 1 < cv->n) {
      r->forced_ws = atoi(cv->s[ei + 1]);
      consumed     = 2;
    }
  } else if(!strncasecmp(rs, "opacity", 7)) {
    if(ei + 1 < cv->n) {
      r->opacity = (float)atof(cv->s[ei + 1]);
      consumed   = 2;
    }
  } else if(!strncasecmp(rs, "size", 4)) {
    if(ei + 2 < cv->n) {
      if(!parse_WxH(cv->s[ei + 1], &r->forced_w, &r->forced_h)) {
        r->forced_w = atoi(cv->s[ei + 1]);
        r->forced_h = atoi(cv->s[ei + 2]);
        consumed    = 3;
      } else {
        consumed = 2;
      }
    } else if(ei + 1 < cv->n) {
      parse_WxH(cv->s[ei + 1], &r->forced_w, &r->forced_h);
      consumed = 2;
    }
  } else if(!strncasecmp(rs, "position", 8)) {
    if(ei + 2 < cv->n && !parse_WxH(cv->s[ei + 1], &r->forced_x, &r->forced_y)) {
      r->forced_x = atoi(cv->s[ei + 1]);
      r->forced_y = atoi(cv->s[ei + 2]);
      consumed    = 3;
    } else if(ei + 1 < cv->n) {
      parse_WxH(cv->s[ei + 1], &r->forced_x, &r->forced_y);
      consumed = 2;
    }
  } else {
    wlr_log(WLR_DEBUG, "config: unknown windowrule effect '%s'", rs);
  }
  return consumed;
}

static void strip_v2_pattern(char *s) {
  if(*s == '^') memmove(s, s + 1, strlen(s));
  int n = (int)strlen(s);
  while(n > 0 && (s[n - 1] == '$' || s[n - 1] == ')' || s[n - 1] == '('))
    s[--n] = '\0';
  if(*s == '(') memmove(s, s + 1, strlen(s));
}

static void parse_windowrule(Config *c, Lexer *l, const char *first, bool v2) {
  if(c->win_rule_count >= MAX_WIN_RULES) {
    wlr_log(WLR_ERROR, "config: MAX_WIN_RULES reached");
    lex_skip_to_nl(l);
    return;
  }

  CVals cv = collect_vals(l, first);
  if(cv.n < 2) {
    wlr_log(WLR_DEBUG, "config: windowrule: need at least effect and matcher");
    return;
  }
  for(int i = 0; i < cv.n; i++)
    strtrim(cv.s[i]);

  WinRule *r = &c->win_rules[c->win_rule_count++];
  memset(r, 0, sizeof(*r));
  r->forced_ws = -1;

  const char *m = cv.s[cv.n - 1];
  if(v2) {
    char pat[256];
    strncpy(pat, m, 255);
    pat[255] = '\0';
    if(!strncasecmp(pat, "title:", 6)) {
      char tmp[256];
      strncpy(tmp, pat + 6, 255);
      strip_v2_pattern(tmp);
      snprintf(r->app_id, sizeof(r->app_id), "title:%s", tmp);
    } else {
      const char *p = pat;
      if(!strncasecmp(p, "class:", 6))
        p += 6;
      else if(!strncasecmp(p, "app_id:", 7))
        p += 7;
      char tmp[256];
      strncpy(tmp, p, 255);
      strip_v2_pattern(tmp);
      strncpy(r->app_id, tmp, sizeof(r->app_id) - 1);
    }
  } else {
    if(!strncasecmp(m, "title:", 6))
      snprintf(r->app_id, sizeof(r->app_id), "title:%s", m + 6);
    else if(!strncasecmp(m, "app_id:", 7))
      strncpy(r->app_id, m + 7, sizeof(r->app_id) - 1);
    else if(!strncasecmp(m, "class:", 6))
      strncpy(r->app_id, m + 6, sizeof(r->app_id) - 1);
    else
      strncpy(r->app_id, m, sizeof(r->app_id) - 1);
  }

  bool any = false;
  for(int i = 0; i < cv.n - 1;) {
    int consumed = apply_wr_effect(r, &cv, i);
    any          = true;
    i += consumed;
  }

  if(!any) {
    wlr_log(WLR_DEBUG, "config: windowrule '%s': no effects, discarding", r->app_id);
    c->win_rule_count--;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §10  Monitor parser
 * ═══════════════════════════════════════════════════════════════════════════ */

static void parse_monitor(Config *c, Lexer *l, const char *first) {
  if(c->monitor_count >= MAX_MONITORS) {
    wlr_log(WLR_ERROR, "config: MAX_MONITORS reached");
    lex_skip_to_nl(l);
    return;
  }
  CVals       cv = collect_vals(l, first);
  MonitorCfg *m  = &c->monitors[c->monitor_count++];
  memset(m, 0, sizeof(*m));
  m->refresh = 60;
  m->scale   = 1.0f;

  if(cv.n >= 1) strncpy(m->name, cv.s[0], sizeof(m->name) - 1);
  if(cv.n >= 2 && strcasecmp(cv.s[1], "preferred") && strcasecmp(cv.s[1], "auto")) {
    char buf[64];
    strncpy(buf, cv.s[1], 63);
    char *at = strchr(buf, '@');
    if(at) {
      m->refresh = atoi(at + 1);
      *at        = '\0';
    }
    parse_WxH(buf, &m->width, &m->height);
  }
  if(cv.n >= 3 && strcasecmp(cv.s[2], "auto")) {
    int px = 0, py = 0;
    if(parse_WxH(cv.s[2], &px, &py)) {
      m->pos_x = px;
      m->pos_y = py;
    }
  }
  if(cv.n >= 4) {
    m->scale = (float)atof(cv.s[3]);
    if(m->scale < 0.1f) m->scale = 1.0f;
  }
  wlr_log(WLR_INFO,
          "config: monitor '%s' %dx%d@%d pos=%dx%d scale=%.2f",
          m->name,
          m->width,
          m->height,
          m->refresh,
          m->pos_x,
          m->pos_y,
          m->scale);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §11  Block parser
 * ═══════════════════════════════════════════════════════════════════════════ */

static void skip_block(Lexer *l) {
  int d = 1;
  while(d > 0) {
    Token t = lex_next(l);
    if(t.kind == T_EOF) return;
    if(t.kind == T_LBRACE) d++;
    if(t.kind == T_RBRACE) d--;
  }
}

#define OPEN_MONITOR(lbl_)                               \
  do {                                                   \
    if(c->monitor_count < MAX_MONITORS) {                \
      MonitorCfg *_m = &c->monitors[c->monitor_count++]; \
      memset(_m, 0, sizeof(*_m));                        \
      _m->refresh = 60;                                  \
      _m->scale   = 1.f;                                 \
      strncpy(_m->name, (lbl_), sizeof(_m->name) - 1);   \
      parse_block(l, c, "monitor", (lbl_), file_dir);    \
    } else                                               \
      skip_block(l);                                     \
  } while(0)

#define OPEN_SCRATCHPAD(lbl_)                                      \
  do {                                                             \
    if(c->scratchpad_count < MAX_SCRATCHPADS) {                    \
      ScratchpadCfg *_sp = &c->scratchpads[c->scratchpad_count++]; \
      memset(_sp, 0, sizeof(*_sp));                                \
      _sp->width_pct  = 0.6f;                                      \
      _sp->height_pct = 0.6f;                                      \
      strncpy(_sp->name, (lbl_), sizeof(_sp->name) - 1);           \
      strncpy(_sp->app_id, (lbl_), sizeof(_sp->app_id) - 1);       \
      parse_block(l, c, "scratchpad", (lbl_), file_dir);           \
    } else                                                         \
      skip_block(l);                                               \
  } while(0)

#define OPEN_BAR_MODULE(lbl_)                                             \
  do {                                                                    \
    if(c->bar.module_cfg_count < MAX_BAR_MODULE_CFGS) {                   \
      BarModuleCfg *_mc = &c->bar.module_cfgs[c->bar.module_cfg_count++]; \
      memset(_mc, 0, sizeof(*_mc));                                       \
      _mc->interval = 5;                                                  \
      strncpy(_mc->name, (lbl_), sizeof(_mc->name) - 1);                  \
      parse_block(l, c, "bar_module", (lbl_), file_dir);                  \
    } else                                                                \
      skip_block(l);                                                      \
  } while(0)

static void parse_block(Lexer      *l,
                        Config     *c,
                        const char *block,
                        const char *label,
                        const char *file_dir) {
  (void)label;
  for(;;) {
    lex_skip_nl(l);
    Token t = lex_peek(l);
    if(t.kind == T_EOF || t.kind == T_RBRACE) break;
    if(t.kind != T_WORD) {
      lex_next(l);
      continue;
    }

    Token key_tok = lex_next(l);
    char  key[512];
    strncpy(key, key_tok.s, 511);
    strtrim(key);

    /* ── $variable definition ── */
    if(key[0] == '$') {
      if(!lex_eat(l, T_EQ)) {
        lex_skip_to_nl(l);
        continue;
      }
      Token vt2 = lex_next(l);
      vt_set(l->vt, key + 1, vt2.s);
      wlr_log(WLR_DEBUG, "config: $%s = %s", key + 1, vt2.s);
      lex_skip_to_nl(l);
      continue;
    }

    /* ── anonymous block:  keyword { ── */
    if(lex_peek(l).kind == T_LBRACE) {
      lex_next(l);
      parse_block(l, c, key, "", file_dir);
      lex_eat(l, T_RBRACE);
      continue;
    }

    /* ── labeled block WITHOUT =:  "keyword label {" ── */
    if(lex_peek(l).kind == T_WORD) {
      Token lbl_tok = lex_next(l);
      if(lex_peek(l).kind == T_LBRACE) {
        lex_next(l);
        const char *lbl = lbl_tok.s;
        if(!strcmp(key, "monitor")) {
          OPEN_MONITOR(lbl);
        } else if(!strcmp(key, "scratchpad")) {
          OPEN_SCRATCHPAD(lbl);
        } else if(!strcmp(key, "bar_module")) {
          OPEN_BAR_MODULE(lbl);
          /* FEATURE 1: "workspace N { layout = ... ratio = ... }" */
        } else if(!strcmp(key, "workspace") && !block[0]) {
          int ws_idx = atoi(lbl) - 1;
          if(ws_idx >= 0 && ws_idx < MAX_WORKSPACES) {
            for(;;) {
              lex_skip_nl(l);
              Token tk = lex_peek(l);
              if(tk.kind == T_EOF || tk.kind == T_RBRACE) break;
              if(tk.kind != T_WORD) {
                lex_next(l);
                continue;
              }
              Token kk = lex_next(l);
              if(!lex_eat(l, T_EQ)) {
                lex_skip_to_nl(l);
                continue;
              }
              Token vv = lex_next(l);
              if(!strcmp(kk.s, "layout")) {
                Layout lv = LAYOUT_BSP;
                if(!strcasecmp(vv.s, "spiral"))
                  lv = LAYOUT_SPIRAL;
                else if(!strcasecmp(vv.s, "columns"))
                  lv = LAYOUT_COLUMNS;
                else if(!strcasecmp(vv.s, "rows"))
                  lv = LAYOUT_ROWS;
                else if(!strcasecmp(vv.s, "threecol"))
                  lv = LAYOUT_THREECOL;
                else if(!strcasecmp(vv.s, "monocle"))
                  lv = LAYOUT_MONOCLE;
                c->ws_layout[ws_idx]     = lv;
                c->ws_layout_set[ws_idx] = true;
              } else if(!strcmp(kk.s, "ratio")) {
                c->ws_ratio[ws_idx]     = (float)atof(vv.s);
                c->ws_ratio_set[ws_idx] = true;
              }
              lex_skip_to_nl(l);
            }
          } else {
            skip_block(l);
          }
        } else {
          wlr_log(WLR_DEBUG, "config: unknown labeled block '%s %s'", key, lbl);
          skip_block(l);
        }
        lex_eat(l, T_RBRACE);
        continue;
      }
      {
        char bv[512];
        strncpy(bv, lbl_tok.s, 511);
        if(!strcasecmp(key, "windowrule") || !strcasecmp(key, "window_rule"))
          parse_windowrule(c, l, bv, false);
        else if(!strcasecmp(key, "windowrulev2") ||
                !strcasecmp(key, "window_rule_v2"))
          parse_windowrule(c, l, bv, true);
        else if(!strncasecmp(key, "bind", 4))
          parse_bind(c, l, bv);
        else if(!strcmp(key, "monitor") && !block[0])
          parse_monitor(c, l, bv);
        else
          wlr_log(WLR_DEBUG,
                  "config:%d: bare '%s %s' unrecognised",
                  key_tok.line,
                  key,
                  bv);
        lex_skip_to_nl(l);
        continue;
      }
    }

    /* ── key = value ── */
    lex_skip_nl(l);
    if(lex_peek(l).kind == T_LBRACE) {
      lex_next(l);
      parse_block(l, c, key, "", file_dir);
      lex_eat(l, T_RBRACE);
      continue;
    }
    if(!lex_eat(l, T_EQ)) {
      wlr_log(WLR_DEBUG, "config:%d: expected '=' after '%s'", key_tok.line, key);
      lex_skip_to_nl(l);
      continue;
    }

    if(lex_peek(l).kind == T_NEWLINE || lex_peek(l).kind == T_EOF) {
      continue;
    }

    Token       val_tok = lex_next(l);
    const char *val     = val_tok.s;

    /* "keyword = label {" — labeled block */
    if(lex_peek(l).kind == T_LBRACE) {
      lex_next(l);
      if(!strcmp(key, "monitor")) {
        OPEN_MONITOR(val);
      } else if(!strcmp(key, "scratchpad")) {
        OPEN_SCRATCHPAD(val);
      } else if(!strcmp(key, "bar_module")) {
        OPEN_BAR_MODULE(val);
      } else {
        wlr_log(WLR_DEBUG, "config: unknown labeled block '%s = %s'", key, val);
        skip_block(l);
      }
      lex_eat(l, T_RBRACE);
      continue;
    }

    /* ════════════════════════════════════════════════════════════
     * §11a  Top-level directives
     * ════════════════════════════════════════════════════════════ */

    if(!strcmp(key, "source")) {
      do_source(c, l->vt, val, file_dir, 0);
      lex_skip_to_nl(l);
      continue;
    }
    if(!strcmp(key, "exec-once") || !strcmp(key, "exec_once")) {
      CVals cv       = collect_vals(l, val);
      char  cmd[256] = { 0 };
      for(int i = 0; i < cv.n; i++) {
        if(i) strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, cv.s[i], sizeof(cmd) - strlen(cmd) - 1);
      }
      if(c->exec_once_count < 16)
        strncpy(c->exec_once[c->exec_once_count++], cmd, 255);
      lex_skip_to_nl(l);
      continue;
    }
    if(!strcmp(key, "exec") && !block[0]) {
      CVals cv       = collect_vals(l, val);
      char  cmd[256] = { 0 };
      for(int i = 0; i < cv.n; i++) {
        if(i) strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, cv.s[i], sizeof(cmd) - strlen(cmd) - 1);
      }
      if(c->exec_count < 16) strncpy(c->exec[c->exec_count++], cmd, 255);
      lex_skip_to_nl(l);
      continue;
    }
    if(!strncasecmp(key, "bind", 4)) {
      parse_bind(c, l, val);
      lex_skip_to_nl(l);
      continue;
    }
    if(!strcmp(key, "monitor") && !block[0]) {
      parse_monitor(c, l, val);
      lex_skip_to_nl(l);
      continue;
    }
    if(!strcasecmp(key, "windowrule") || !strcasecmp(key, "window_rule")) {
      parse_windowrule(c, l, val, false);
      lex_skip_to_nl(l);
      continue;
    }
    if(!strcasecmp(key, "windowrulev2") || !strcasecmp(key, "window_rule_v2")) {
      parse_windowrule(c, l, val, true);
      lex_skip_to_nl(l);
      continue;
    }

    /* ════════════════════════════════════════════════════════════
     * §11b  Block-specific key=value
     * ════════════════════════════════════════════════════════════ */

    const char *bk = strip_col(key);

    /* ── top-level / general ── */
    if(!block[0] || !strcmp(block, "general")) {
      if(!strcmp(bk, "font") || !strcmp(bk, "font_family"))
        strncpy(c->font_path, val, sizeof(c->font_path) - 1);
      else if(!strcmp(bk, "font_size"))
        c->font_size = (float)str_to_num(val);
      else if(!strcmp(bk, "gaps_in") || !strcmp(bk, "gap") || !strcmp(bk, "gaps"))
        c->gap = (int)str_to_num(val);
      else if(!strcmp(bk, "gaps_out") || !strcmp(bk, "outer_gap") ||
              !strcmp(bk, "outer_gaps"))
        c->outer_gap = (int)str_to_num(val);
      else if(!strcmp(bk, "background_color") || !strcmp(bk, "background"))
        c->colors.background = color_from_str(val);
      else if(!strcmp(bk, "border_size") || !strcmp(bk, "border_width"))
        c->border_width = (int)str_to_num(val);
      else if(!strcmp(bk, "corner_radius") || !strcmp(bk, "rounding"))
        c->corner_radius = (int)str_to_num(val);
      else if(!strcmp(bk, "smart_gaps"))
        c->smart_gaps = str_to_bool(val);
      else if(!strcmp(bk, "cursor_theme"))
        strncpy(c->cursor_theme, val, sizeof(c->cursor_theme) - 1);
      else if(!strcmp(bk, "cursor_size"))
        c->cursor_size = (int)str_to_num(val);
      else if(!strcmp(bk, "workspaces") || !strcmp(bk, "workspace_count"))
        c->workspaces = (int)str_to_num(val);
      else if(!strcmp(bk, "seat_name") || !strcmp(bk, "seat"))
        strncpy(c->seat_name, val, sizeof(c->seat_name) - 1);
      else if(!strcmp(bk, "idle_timeout"))
        c->idle_timeout = (int)str_to_num(val);
      else if(!strcmp(bk, "xwayland"))
        c->xwayland = str_to_bool(val);
      else if(!strcmp(bk, "theme"))
        apply_theme(c, val);
      else if(!strcmp(bk, "active_border") || !strcmp(bk, "active_border_color"))
        c->colors.active_border = color_from_str(val);
      else if(!strcmp(bk, "inactive_border") || !strcmp(bk, "inactive_border_color"))
        c->colors.inactive_border = color_from_str(val);
      else if(block[0])
        wlr_log(WLR_DEBUG, "config: unknown key '%s' in '%s'", key, block);
    }

    /* ── decoration ── */
    else if(!strcmp(block, "decoration")) {
      if(!strcmp(bk, "rounding") || !strcmp(bk, "corner_radius"))
        c->corner_radius = (int)str_to_num(val);
      else if(!strcmp(bk, "border_size") || !strcmp(bk, "border_width"))
        c->border_width = (int)str_to_num(val);
      else if(!strcmp(bk, "active_border"))
        c->colors.active_border = color_from_str(val);
      else if(!strcmp(bk, "inactive_border"))
        c->colors.inactive_border = color_from_str(val);
      else if(!strcmp(bk, "active_title") || !strcmp(bk, "active_title_color"))
        c->colors.active_title = color_from_str(val);
      else if(!strcmp(bk, "inactive_title") || !strcmp(bk, "inactive_title_color"))
        c->colors.inactive_title = color_from_str(val);
      else if(!strcmp(bk, "pane_bg") || !strcmp(bk, "background"))
        c->colors.pane_bg = color_from_str(val);
      else if(!strcmp(bk, "focus_ring") || !strcmp(bk, "focus_ring_color"))
        c->colors.focus_ring = color_from_str(val);
      else
        wlr_log(WLR_DEBUG, "config: unknown key '%s' in decoration", key);
    }

    /* ── colors ── */
    else if(!strcmp(block, "colors") || !strcmp(block, "color")) {
      if(!strcmp(bk, "active_border"))
        c->colors.active_border = color_from_str(val);
      else if(!strcmp(bk, "inactive_border"))
        c->colors.inactive_border = color_from_str(val);
      else if(!strcmp(bk, "active_title"))
        c->colors.active_title = color_from_str(val);
      else if(!strcmp(bk, "inactive_title"))
        c->colors.inactive_title = color_from_str(val);
      else if(!strcmp(bk, "pane_bg") || !strcmp(bk, "background"))
        c->colors.pane_bg = color_from_str(val);
      else if(!strcmp(bk, "bar_bg")) {
        c->colors.bar_bg = color_from_str(val);
        c->bar.bg        = c->colors.bar_bg;
      } else if(!strcmp(bk, "bar_fg")) {
        c->colors.bar_fg = color_from_str(val);
        c->bar.fg        = c->colors.bar_fg;
      } else if(!strcmp(bk, "bar_accent")) {
        c->colors.bar_accent = color_from_str(val);
        c->bar.accent        = c->colors.bar_accent;
      } else if(!strcmp(bk, "focus_ring"))
        c->colors.focus_ring = color_from_str(val);
      else
        wlr_log(WLR_DEBUG, "config: unknown key '%s' in colors", key);
    }

    /* ── input / keyboard ── */
    else if(!strcmp(block, "input") || !strcmp(block, "keyboard")) {
      if(!strcmp(bk, "kb_layout") || !strcmp(bk, "layout"))
        strncpy(c->keyboard.kb_layout, val, sizeof(c->keyboard.kb_layout) - 1);
      else if(!strcmp(bk, "kb_variant") || !strcmp(bk, "variant"))
        strncpy(c->keyboard.kb_variant, val, sizeof(c->keyboard.kb_variant) - 1);
      else if(!strcmp(bk, "kb_options") || !strcmp(bk, "options"))
        strncpy(c->keyboard.kb_options, val, sizeof(c->keyboard.kb_options) - 1);
      else if(!strcmp(bk, "repeat_rate"))
        c->keyboard.repeat_rate = (int)str_to_num(val);
      else if(!strcmp(bk, "repeat_delay"))
        c->keyboard.repeat_delay = (int)str_to_num(val);
      else
        wlr_log(WLR_DEBUG, "config: unknown key '%s' in %s", key, block);
    }

    /* ── bar ── */
    else if(!strcmp(block, "bar")) {
      BarCfg *b = &c->bar;
      if(!strcmp(bk, "position"))
        b->position = !strcasecmp(val, "top") ? BAR_TOP : BAR_BOTTOM;
      else if(!strcmp(bk, "height"))
        b->height = (int)str_to_num(val);
      else if(!strcmp(bk, "padding"))
        b->padding = (int)str_to_num(val);
      else if(!strcmp(bk, "glyph_y_offset"))
        b->glyph_y_offset = (int)str_to_num(val);
      else if(!strcmp(bk, "item_spacing"))
        b->item_spacing = (int)str_to_num(val);
      else if(!strcmp(bk, "pill_radius"))
        b->pill_radius = (int)str_to_num(val);
      else if(!strcmp(bk, "font_size"))
        b->font_size = (float)str_to_num(val);
      else if(!strcmp(bk, "bg") || !strcmp(bk, "background") ||
              !strcmp(bk, "col_background")) {
        Color col        = color_from_str(val);
        b->bg            = col;
        c->colors.bar_bg = col;
      } else if(!strcmp(bk, "fg") || !strcmp(bk, "text") ||
                !strcmp(bk, "col_text")) {
        Color col        = color_from_str(val);
        b->fg            = col;
        c->colors.bar_fg = col;
      } else if(!strcmp(bk, "accent") || !strcmp(bk, "col_accent")) {
        Color col            = color_from_str(val);
        b->accent            = col;
        c->colors.bar_accent = col;
      } else if(!strcmp(bk, "dim"))
        b->dim = color_from_str(val);
      else if(!strcmp(bk, "active_ws_fg"))
        b->active_ws_fg = color_from_str(val);
      else if(!strcmp(bk, "active_ws_bg"))
        b->active_ws_bg = color_from_str(val);
      else if(!strcmp(bk, "occupied_ws_fg"))
        b->occupied_ws_fg = color_from_str(val);
      else if(!strcmp(bk, "inactive_ws_fg"))
        b->inactive_ws_fg = color_from_str(val);
      else if(!strcmp(bk, "separator"))
        b->separator = str_to_bool(val);
      else if(!strcmp(bk, "separator_top"))
        b->separator_top = str_to_bool(val);
      else if(!strcmp(bk, "separator_color"))
        b->separator_color = color_from_str(val);
      else if(!strcmp(bk, "separator_style") || !strcmp(bk, "sep_style")) {
        if(!strcasecmp(val, "pipe"))
          b->sep_style = SEP_PIPE;
        else if(!strcasecmp(val, "block"))
          b->sep_style = SEP_BLOCK;
        else if(!strcasecmp(val, "arrow"))
          b->sep_style = SEP_ARROW;
        else
          b->sep_style = SEP_NONE;
      } else if(!strcmp(bk, "powerline")) {
        b->sep_style = str_to_bool(val) ? SEP_ARROW : SEP_NONE;
        /* FIX 3: modules_* reset-in-loop bug — reset once, before the copy loop */
      } else if(!strcmp(bk, "modules_left") || !strcmp(bk, "modules-left")) {
        CVals cv          = collect_vals(l, val);
        b->modules_left_n = 0; /* reset once */
        for(int i = 0; i < cv.n && b->modules_left_n < MAX_BAR_MODS; i++)
          if(cv.s[i][0]) strncpy(b->modules_left[b->modules_left_n++], cv.s[i], 63);
      } else if(!strcmp(bk, "modules_center") || !strcmp(bk, "modules-center")) {
        CVals cv            = collect_vals(l, val);
        b->modules_center_n = 0;
        for(int i = 0; i < cv.n && b->modules_center_n < MAX_BAR_MODS; i++)
          if(cv.s[i][0])
            strncpy(b->modules_center[b->modules_center_n++], cv.s[i], 63);
      } else if(!strcmp(bk, "modules_right") || !strcmp(bk, "modules-right")) {
        CVals cv           = collect_vals(l, val);
        b->modules_right_n = 0;
        for(int i = 0; i < cv.n && b->modules_right_n < MAX_BAR_MODS; i++)
          if(cv.s[i][0])
            strncpy(b->modules_right[b->modules_right_n++], cv.s[i], 63);
      } else
        wlr_log(WLR_DEBUG, "config: unknown key '%s' in bar", key);
    }

    /* ── animations (accepted, silently ignored) ── */
    else if(!strcmp(block, "animations")) {
      (void)val;
    }

    /* ── monitor block form ── */
    else if(!strcmp(block, "monitor") && c->monitor_count > 0) {
      MonitorCfg *m = &c->monitors[c->monitor_count - 1];
      if(!strcmp(bk, "width"))
        m->width = (int)str_to_num(val);
      else if(!strcmp(bk, "height"))
        m->height = (int)str_to_num(val);
      else if(!strcmp(bk, "refresh"))
        m->refresh = (int)str_to_num(val);
      else if(!strcmp(bk, "scale"))
        m->scale = (float)str_to_num(val);
      else if(!strcmp(bk, "position")) {
        CVals pv = collect_vals(l, val);
        if(pv.n == 1)
          parse_WxH(pv.s[0], &m->pos_x, &m->pos_y);
        else if(pv.n >= 2) {
          m->pos_x = (int)str_to_num(pv.s[0]);
          m->pos_y = (int)str_to_num(pv.s[1]);
        }
      } else
        wlr_log(WLR_DEBUG, "config: unknown key '%s' in monitor", key);
    }

    /* ── scratchpad block form ── */
    else if(!strcmp(block, "scratchpad") && c->scratchpad_count > 0) {
      ScratchpadCfg *sp = &c->scratchpads[c->scratchpad_count - 1];
      if(!strcmp(bk, "app_id") || !strcmp(bk, "class"))
        strncpy(sp->app_id, val, sizeof(sp->app_id) - 1);
      else if(!strcmp(bk, "name"))
        strncpy(sp->name, val, sizeof(sp->name) - 1);
      else if(!strcmp(bk, "width")) {
        float v       = parse_pct_or_float(val);
        sp->width_pct = (v > 1.f) ? v / 100.f : v;
        if(sp->width_pct < 0.05f) sp->width_pct = 0.6f;
        if(sp->width_pct > 1.0f) sp->width_pct = 1.0f;
      } else if(!strcmp(bk, "height")) {
        float v        = parse_pct_or_float(val);
        sp->height_pct = (v > 1.f) ? v / 100.f : v;
        if(sp->height_pct < 0.05f) sp->height_pct = 0.6f;
        if(sp->height_pct > 1.0f) sp->height_pct = 1.0f;
      } else if(!strcmp(bk, "size")) {
        CVals sv = collect_vals(l, val);
        if(sv.n >= 1) {
          float w       = parse_pct_or_float(sv.s[0]);
          sp->width_pct = (w > 1.f) ? w / 100.f : w;
        }
        if(sv.n >= 2) {
          float h        = parse_pct_or_float(sv.s[1]);
          sp->height_pct = (h > 1.f) ? h / 100.f : h;
        }
      } else
        wlr_log(WLR_DEBUG, "config: unknown key '%s' in scratchpad", key);
    }

    /* ── bar_module block form ── */
    else if(!strcmp(block, "bar_module") && c->bar.module_cfg_count > 0) {
      BarModuleCfg *mc = &c->bar.module_cfgs[c->bar.module_cfg_count - 1];
      if(!strcmp(bk, "exec"))
        strncpy(mc->exec, val, sizeof(mc->exec) - 1);
      else if(!strcmp(bk, "interval"))
        mc->interval = (int)str_to_num(val);
      else if(!strcmp(bk, "icon"))
        strncpy(mc->icon, val, sizeof(mc->icon) - 1);
      else if(!strcmp(bk, "format"))
        strncpy(mc->format, val, sizeof(mc->format) - 1);
      else if(!strcmp(bk, "color") || !strcmp(bk, "colour")) {
        mc->color     = color_from_str(val);
        mc->has_color = true;
      } else
        wlr_log(WLR_DEBUG, "config: unknown key '%s' in bar_module", key);
    }

    else {
      wlr_log(
          WLR_DEBUG, "config: key '%s' in unknown block '%s', ignored", key, block);
    }

    lex_skip_to_nl(l);
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §12  source directive
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_SOURCE_DEPTH 8

static void do_source(
    Config *c, VarTable *vt, const char *path_str, const char *file_dir, int depth) {
  if(depth >= MAX_SOURCE_DEPTH) {
    wlr_log(WLR_ERROR, "config: source depth limit, skipping '%s'", path_str);
    return;
  }
  char path[512] = { 0 };
  if(!strncmp(path_str, "~/", 2)) {
    const char *home = getenv("HOME");
    if(!home) home = "/root";
    snprintf(path, sizeof(path), "%s/%s", home, path_str + 2);
  } else if(path_str[0] != '/' && file_dir && file_dir[0]) {
    snprintf(path, sizeof(path), "%s/%s", file_dir, path_str);
  } else {
    strncpy(path, path_str, sizeof(path) - 1);
  }

  FILE *f = fopen(path, "r");
  if(!f) {
    wlr_log(WLR_ERROR, "config: source '%s' not found", path);
    return;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  rewind(f);
  if(sz <= 0) {
    fclose(f);
    return;
  }
  char *src = malloc((size_t)sz + 1);
  if(!src) {
    fclose(f);
    return;
  }
  fread(src, 1, (size_t)sz, f);
  src[sz] = '\0';
  fclose(f);

  wlr_log(WLR_INFO, "config: sourcing '%s' (%ld bytes)", path, sz);

  char dir[512] = { 0 };
  strncpy(dir, path, sizeof(dir) - 1);
  char *sl = strrchr(dir, '/');
  if(sl)
    *sl = '\0';
  else
    dir[0] = '\0';

  Lexer l2 = { 0 };
  l2.src   = src;
  l2.line  = 1;
  l2.vt    = vt;
  parse_block(&l2, c, "", "", dir);
  free(src);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §12b  Built-in theme presets
 * ═══════════════════════════════════════════════════════════════════════════ */

static void apply_theme(Config *c, const char *name) {
  BarCfg *b = &c->bar;

  b->pill_radius   = 0;
  c->corner_radius = 0;
  b->separator     = true;
  b->separator_top = (b->position == BAR_BOTTOM);
  b->sep_style     = SEP_PIPE;

  if(!strcasecmp(name, "catppuccin-mocha") || !strcasecmp(name, "mocha")) {
    c->colors.active_border   = color_hex(0x94e2d5);
    c->colors.inactive_border = color_hex(0x313244);
    c->colors.active_title    = color_hex(0x94e2d5);
    c->colors.inactive_title  = color_hex(0x585b70);
    c->colors.pane_bg         = color_hex(0x1e1e2e);
    c->colors.background      = color_hex(0x1e1e2e);
    c->colors.bar_bg          = color_hex(0x181825);
    c->colors.bar_fg          = color_hex(0xcdd6f4);
    c->colors.bar_accent      = color_hex(0x94e2d5);
    c->colors.focus_ring      = color_hex(0x94e2d5);
    b->bg                     = color_hex(0x181825);
    b->fg                     = color_hex(0xcdd6f4);
    b->accent                 = color_hex(0x94e2d5);
    b->dim                    = color_hex(0x1e1e2e);
    b->active_ws_fg           = color_hex(0x181825);
    b->active_ws_bg           = color_hex(0x94e2d5);
    b->occupied_ws_fg         = color_hex(0x94e2d5);
    b->inactive_ws_fg         = color_hex(0x45475a);
    b->separator_color        = color_hex(0x313244);

  } else if(!strcasecmp(name, "catppuccin-latte") || !strcasecmp(name, "latte")) {
    c->colors.active_border   = color_hex(0x7287fd);
    c->colors.inactive_border = color_hex(0xacb0be);
    c->colors.active_title    = color_hex(0x7287fd);
    c->colors.inactive_title  = color_hex(0x9ca0b0);
    c->colors.pane_bg         = color_hex(0xeff1f5);
    c->colors.background      = color_hex(0xeff1f5);
    c->colors.bar_bg          = color_hex(0xe6e9ef);
    c->colors.bar_fg          = color_hex(0x5c5f77);
    c->colors.bar_accent      = color_hex(0x7287fd);
    c->colors.focus_ring      = color_hex(0x7287fd);
    b->bg                     = color_hex(0xe6e9ef);
    b->fg                     = color_hex(0x5c5f77);
    b->accent                 = color_hex(0x7287fd);
    b->dim                    = color_hex(0x9ca0b0);
    b->active_ws_fg           = color_hex(0xeff1f5);
    b->active_ws_bg           = color_hex(0x7287fd);
    b->occupied_ws_fg         = color_hex(0x7287fd);
    b->inactive_ws_fg         = color_hex(0x9ca0b0);
    b->separator_color        = color_hex(0xbcc0cc);

  } else if(!strcasecmp(name, "gruvbox") || !strcasecmp(name, "gruvbox-dark")) {
    c->colors.active_border   = color_hex(0xd79921);
    c->colors.inactive_border = color_hex(0x504945);
    c->colors.active_title    = color_hex(0xd79921);
    c->colors.inactive_title  = color_hex(0x7c6f64);
    c->colors.pane_bg         = color_hex(0x1d2021);
    c->colors.background      = color_hex(0x1d2021);
    c->colors.bar_bg          = color_hex(0x282828);
    c->colors.bar_fg          = color_hex(0xebdbb2);
    c->colors.bar_accent      = color_hex(0xd79921);
    c->colors.focus_ring      = color_hex(0xd79921);
    b->bg                     = color_hex(0x282828);
    b->fg                     = color_hex(0xebdbb2);
    b->accent                 = color_hex(0xd79921);
    b->dim                    = color_hex(0x7c6f64);
    b->active_ws_fg           = color_hex(0x1d2021);
    b->active_ws_bg           = color_hex(0xd79921);
    b->occupied_ws_fg         = color_hex(0xd79921);
    b->inactive_ws_fg         = color_hex(0x7c6f64);
    b->separator_color        = color_hex(0x3c3836);

  } else if(!strcasecmp(name, "nord")) {
    c->colors.active_border   = color_hex(0x88c0d0);
    c->colors.inactive_border = color_hex(0x3b4252);
    c->colors.active_title    = color_hex(0x88c0d0);
    c->colors.inactive_title  = color_hex(0x4c566a);
    c->colors.pane_bg         = color_hex(0x2e3440);
    c->colors.background      = color_hex(0x2e3440);
    c->colors.bar_bg          = color_hex(0x3b4252);
    c->colors.bar_fg          = color_hex(0xd8dee9);
    c->colors.bar_accent      = color_hex(0x88c0d0);
    c->colors.focus_ring      = color_hex(0x88c0d0);
    b->bg                     = color_hex(0x3b4252);
    b->fg                     = color_hex(0xd8dee9);
    b->accent                 = color_hex(0x88c0d0);
    b->dim                    = color_hex(0x4c566a);
    b->active_ws_fg           = color_hex(0x2e3440);
    b->active_ws_bg           = color_hex(0x88c0d0);
    b->occupied_ws_fg         = color_hex(0x88c0d0);
    b->inactive_ws_fg         = color_hex(0x4c566a);
    b->separator_color        = color_hex(0x434c5e);

  } else if(!strcasecmp(name, "tokyo-night") || !strcasecmp(name, "tokyonight")) {
    c->colors.active_border   = color_hex(0x7aa2f7);
    c->colors.inactive_border = color_hex(0x292e42);
    c->colors.active_title    = color_hex(0x7aa2f7);
    c->colors.inactive_title  = color_hex(0x565f89);
    c->colors.pane_bg         = color_hex(0x1a1b26);
    c->colors.background      = color_hex(0x1a1b26);
    c->colors.bar_bg          = color_hex(0x16161e);
    c->colors.bar_fg          = color_hex(0xa9b1d6);
    c->colors.bar_accent      = color_hex(0x7aa2f7);
    c->colors.focus_ring      = color_hex(0x7aa2f7);
    b->bg                     = color_hex(0x16161e);
    b->fg                     = color_hex(0xa9b1d6);
    b->accent                 = color_hex(0x7aa2f7);
    b->dim                    = color_hex(0x565f89);
    b->active_ws_fg           = color_hex(0x1a1b26);
    b->active_ws_bg           = color_hex(0x7aa2f7);
    b->occupied_ws_fg         = color_hex(0x7aa2f7);
    b->inactive_ws_fg         = color_hex(0x565f89);
    b->separator_color        = color_hex(0x292e42);

  } else {
    wlr_log(WLR_DEBUG,
            "config: unknown theme '%s' — try catppuccin-mocha, catppuccin-latte, "
            "gruvbox, nord, tokyo-night",
            name);
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §13  Defaults
 * ═══════════════════════════════════════════════════════════════════════════ */

void config_defaults(Config *c) {
  memset(c, 0, sizeof(*c));
  strncpy(c->font_path,
          "/usr/share/fonts/TTF/JetBrainsMono-Regular.ttf",
          sizeof(c->font_path) - 1);
  c->font_size     = 13.f;
  c->gap           = 2;
  c->outer_gap     = 0;
  c->border_width  = 1;
  c->corner_radius = 0;
  c->smart_gaps    = true;
  strncpy(c->cursor_theme, "default", sizeof(c->cursor_theme) - 1);
  c->cursor_size = 24;
  c->workspaces  = 9;
  strncpy(c->seat_name, "seat0", sizeof(c->seat_name) - 1);
  c->idle_timeout = 0;
  c->xwayland     = false;

  c->colors.active_border   = color_hex(0x94e2d5);
  c->colors.inactive_border = color_hex(0x313244);
  c->colors.active_title    = color_hex(0x94e2d5);
  c->colors.inactive_title  = color_hex(0x6c7086);
  c->colors.pane_bg         = color_hex(0x1e1e2e);
  c->colors.background      = color_hex(0x1e1e2e);
  c->colors.bar_bg          = color_hex(0x181825);
  c->colors.bar_fg          = color_hex(0xcdd6f4);
  c->colors.bar_accent      = color_hex(0x94e2d5);
  c->colors.focus_ring      = color_hex(0x94e2d5);

  BarCfg *b          = &c->bar;
  b->position        = BAR_BOTTOM;
  b->height          = 20;
  b->padding         = 6;
  b->glyph_y_offset  = 0;
  b->item_spacing    = 4;
  b->pill_radius     = 0;
  b->bg              = color_hex(0x181825);
  b->fg              = color_hex(0xcdd6f4);
  b->accent          = color_hex(0x94e2d5);
  b->dim             = color_hex(0x313244);
  b->active_ws_fg    = color_hex(0x181825);
  b->active_ws_bg    = color_hex(0x94e2d5);
  b->occupied_ws_fg  = color_hex(0x94e2d5);
  b->inactive_ws_fg  = color_hex(0x45475a);
  b->separator       = true;
  b->separator_top   = true;
  b->separator_color = color_hex(0x313244);
  b->sep_style       = SEP_PIPE;

  strncpy(b->modules_left[0], "workspaces", 63);
  strncpy(b->modules_left[1], "title", 63);
  b->modules_left_n = 2;
  strncpy(b->modules_center[0], "layout", 63);
  b->modules_center_n = 1;
  strncpy(b->modules_right[0], "battery", 63);
  strncpy(b->modules_right[1], "network", 63);
  strncpy(b->modules_right[2], "clock", 63);
  b->modules_right_n = 3;

  c->keyboard.repeat_rate  = 25;
  c->keyboard.repeat_delay = 600;
  c->keybind_count         = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §14  Fallback keybinds
 * ═══════════════════════════════════════════════════════════════════════════ */

void config_apply_fallback_keybinds(Config *c) {
  if(c->keybind_count > 0) return;
  int k = 0;
#define KB(mods_, sym_, act_)                                                        \
  do {                                                                               \
    c->keybinds[k].mods   = mods_;                                                   \
    c->keybinds[k].sym    = xkb_keysym_from_name(sym_, XKB_KEYSYM_CASE_INSENSITIVE); \
    c->keybinds[k].action = act_;                                                    \
    k++;                                                                             \
  } while(0)

  {
    Action a = { 0 };
    a.kind   = ACTION_EXEC;
    strncpy(a.exec_cmd, "foot", sizeof(a.exec_cmd) - 1);
    KB(MOD_SUPER, "Return", a);
  }
  {
    Action a = { 0 };
    a.kind   = ACTION_CLOSE;
    KB(MOD_SUPER, "q", a);
  }
  {
    Action a = { 0 };
    a.kind   = ACTION_PREV_WS;
    KB(MOD_SUPER, "Left", a);
  }
  {
    Action a = { 0 };
    a.kind   = ACTION_NEXT_WS;
    KB(MOD_SUPER, "Right", a);
  }
  for(int i = 1; i <= 9; i++) {
    char s[4];
    snprintf(s, 4, "%d", i);
    Action a = { 0 };
    a.kind   = ACTION_WORKSPACE;
    a.n      = i;
    KB(MOD_SUPER, s, a);
  }
#undef KB
  c->keybind_count = k;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §15  Fontconfig resolver
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool fc_resolve(const char *q, int weight, int slant, char *out, int olen) {
  if(!q || !*q) return false;
  if(q[0] == '/') {
    FILE *f = fopen(q, "r");
    if(f) {
      fclose(f);
      strncpy(out, q, olen - 1);
      return true;
    }
    wlr_log(WLR_ERROR, "config: font path '%s' not found", q);
    return false;
  }
  FcInit();
  FcConfig *fc = FcConfigGetCurrent();
  if(strchr(q, '.')) {
    FcFontSet *fs = FcConfigGetFonts(fc, FcSetSystem);
    if(fs)
      for(int i = 0; i < fs->nfont; i++) {
        FcChar8 *file = NULL;
        if(FcPatternGetString(fs->fonts[i], FC_FILE, 0, &file) == FcResultMatch) {
          const char *base = strrchr((char *)file, '/');
          base             = base ? base + 1 : (char *)file;
          if(!strcasecmp(base, q)) {
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
  FcResult   res;
  FcPattern *match = FcFontMatch(fc, pat, &res);
  FcPatternDestroy(pat);
  if(!match) return false;
  FcChar8 *file = NULL;
  bool     ok   = false;
  if(FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
    strncpy(out, (char *)file, olen - 1);
    ok = true;
  }
  FcPatternDestroy(match);
  return ok;
}

static void config_resolve_fonts(Config *c) {
  char raw[256];
  strncpy(raw, c->font_path, sizeof(raw) - 1);
  char resolved[256] = { 0 };
  if(fc_resolve(
         raw, FC_WEIGHT_REGULAR, FC_SLANT_ROMAN, resolved, sizeof(resolved))) {
    strncpy(c->font_path, resolved, sizeof(c->font_path) - 1);
    wlr_log(WLR_INFO, "config: font regular → %s", c->font_path);
  } else
    wlr_log(WLR_ERROR, "config: could not resolve font '%s'", raw);

  if(fc_resolve(raw,
                FC_WEIGHT_BOLD,
                FC_SLANT_ROMAN,
                c->font_path_bold,
                sizeof(c->font_path_bold)))
    wlr_log(WLR_INFO, "config: font bold    → %s", c->font_path_bold);
  else {
    strncpy(c->font_path_bold, c->font_path, sizeof(c->font_path_bold) - 1);
    wlr_log(WLR_INFO, "config: font bold    → (same as regular)");
  }

  if(fc_resolve(raw,
                FC_WEIGHT_REGULAR,
                FC_SLANT_ITALIC,
                c->font_path_italic,
                sizeof(c->font_path_italic)))
    wlr_log(WLR_INFO, "config: font italic  → %s", c->font_path_italic);
  else {
    strncpy(c->font_path_italic, c->font_path, sizeof(c->font_path_italic) - 1);
    wlr_log(WLR_INFO, "config: font italic  → (same as regular)");
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §16  Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

void config_load(Config *c, const char *path) {
  config_defaults(c);
  FILE *f = fopen(path, "r");
  if(!f) {
    wlr_log(WLR_INFO, "config: '%s' not found, using defaults", path);
    config_apply_fallback_keybinds(c);
    config_resolve_fonts(c);
    return;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  rewind(f);
  char *src = malloc((size_t)sz + 1);
  if(!src) {
    fclose(f);
    return;
  }
  fread(src, 1, (size_t)sz, f);
  src[sz] = '\0';
  fclose(f);

  char dir[512] = { 0 };
  strncpy(dir, path, sizeof(dir) - 1);
  char *sl = strrchr(dir, '/');
  if(sl)
    *sl = '\0';
  else
    dir[0] = '\0';

  VarTable vt = { 0 };
  Lexer    l  = { 0 };
  l.src       = src;
  l.line      = 1;
  l.vt        = &vt;
  parse_block(&l, c, "", "", dir);
  free(src);

  config_apply_fallback_keybinds(c);
  config_resolve_fonts(c);
  wlr_log(WLR_INFO,
          "config: loaded '%s'  keybinds=%d monitors=%d scratchpads=%d"
          " win_rules=%d exec_once=%d",
          path,
          c->keybind_count,
          c->monitor_count,
          c->scratchpad_count,
          c->win_rule_count,
          c->exec_once_count);

  {
    char left[256] = { 0 }, center[256] = { 0 }, right[256] = { 0 };
    for(int i = 0; i < c->bar.modules_left_n; i++) {
      if(i) strncat(left, ",", 255);
      strncat(left, c->bar.modules_left[i], 255);
    }
    for(int i = 0; i < c->bar.modules_center_n; i++) {
      if(i) strncat(center, ",", 255);
      strncat(center, c->bar.modules_center[i], 255);
    }
    for(int i = 0; i < c->bar.modules_right_n; i++) {
      if(i) strncat(right, ",", 255);
      strncat(right, c->bar.modules_right[i], 255);
    }
    wlr_log(WLR_INFO,
            "config: bar modules  left(%d)=[%s]  center(%d)=[%s]  right(%d)=[%s]",
            c->bar.modules_left_n,
            left[0] ? left : "(none)",
            c->bar.modules_center_n,
            center[0] ? center : "(none)",
            c->bar.modules_right_n,
            right[0] ? right : "(none)");
    wlr_log(WLR_INFO,
            "config: bar position=%s height=%d",
            c->bar.position == BAR_TOP ? "top" : "bottom",
            c->bar.height);
  }
}

static char        cfg_path_buf[512];
static const char *get_config_path(void) {
  if(cfg_path_buf[0]) return cfg_path_buf;
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if(xdg)
    snprintf(cfg_path_buf, sizeof(cfg_path_buf), "%s/trixie/trixie.conf", xdg);
  else {
    const char *home = getenv("HOME");
    if(!home) home = "/root";
    snprintf(
        cfg_path_buf, sizeof(cfg_path_buf), "%s/.config/trixie/trixie.conf", home);
  }
  return cfg_path_buf;
}

void config_reload(Config *c) {
  config_load(c, get_config_path());
}

void config_set_theme(Config *c, const char *name) {
  apply_theme(c, name);
}
