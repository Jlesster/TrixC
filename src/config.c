/* config.c — config file parser and hot-reload
 *
 * Parses the same format as config/parser.rs:
 *   - // line comments (and # line comments)
 *   - $var = value  /  $var references
 *   - rgb(rrggbb) / rgba(rrggbbaa) colors
 *   - #rrggbb / #rrggbbaa colors
 *   - 6px, 144hz, 70%, 200ms dimension values
 *   - [a, b, c] bracketed arrays
 *   - source = "path" directives
 *   - true/yes/on, false/no/off booleans
 *   - labeled blocks: monitor eDP-1 { }, scratchpad term { }
 *   - last-value-wins for duplicate keys
 */
#include "trixie.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ── Variable table ───────────────────────────────────────────────────────── */

#define MAX_VARS     128
#define MAX_VAR_NAME 64
#define MAX_VAR_VAL  256

typedef struct {
  char name[MAX_VAR_NAME];
  char val[MAX_VAR_VAL];
} Var;

typedef struct {
  Var vars[MAX_VARS];
  int count;
} VarTable;

static void var_set(VarTable *vt, const char *name, const char *val) {
  for(int i = 0; i < vt->count; i++) {
    if(!strcmp(vt->vars[i].name, name)) {
      strncpy(vt->vars[i].val, val, MAX_VAR_VAL - 1);
      return;
    }
  }
  if(vt->count >= MAX_VARS) return;
  strncpy(vt->vars[vt->count].name, name, MAX_VAR_NAME - 1);
  strncpy(vt->vars[vt->count].val, val, MAX_VAR_VAL - 1);
  vt->count++;
}

static const char *var_get(const VarTable *vt, const char *name) {
  for(int i = 0; i < vt->count; i++)
    if(!strcmp(vt->vars[i].name, name)) return vt->vars[i].val;
  return NULL;
}

/* ── Lexer ────────────────────────────────────────────────────────────────── */

typedef enum {
  T_IDENT,
  T_STRING,
  T_INT,
  T_FLOAT,
  T_COLOR,
  T_DIM_PX,
  T_DIM_HZ,
  T_DIM_PCT,
  T_DIM_MS,
  T_EQ,
  T_LBRACE,
  T_RBRACE,
  T_LBRACKET,
  T_RBRACKET,
  T_COMMA,
  T_EOF,
  T_ERR,
} TokKind;

typedef struct {
  TokKind kind;
  char    s[512];     /* T_IDENT, T_STRING */
  double  d;          /* T_INT, T_FLOAT, T_DIM_* */
  uint8_t r, g, b, a; /* T_COLOR */
} Tok;

typedef struct {
  const char *src;
  int         pos;
  int         line;
  VarTable   *vars;
} Lexer;

static void lex_init(Lexer *l, const char *src, VarTable *vars) {
  l->src  = src;
  l->pos  = 0;
  l->line = 1;
  l->vars = vars;
}

static void skip_ws_comments(Lexer *l) {
  for(;;) {
    /* whitespace */
    while(l->src[l->pos] && isspace((unsigned char)l->src[l->pos])) {
      if(l->src[l->pos] == '\n') l->line++;
      l->pos++;
    }
    /* // comment */
    if(l->src[l->pos] == '/' && l->src[l->pos + 1] == '/') {
      while(l->src[l->pos] && l->src[l->pos] != '\n')
        l->pos++;
      continue;
    }
    /* # comment — but only at start of token (not inside #rrggbb) */
    if(l->src[l->pos] == '#' && (l->pos == 0 || l->src[l->pos - 1] == '\n' ||
                                 isspace((unsigned char)l->src[l->pos - 1]))) {
      /* peek: if followed by 6/8 hex digits it's a color, not a comment */
      int j  = l->pos + 1;
      int hx = 0;
      while(isxdigit((unsigned char)l->src[j])) {
        j++;
        hx++;
      }
      if(hx == 6 || hx == 8) break; /* it's a color */
      while(l->src[l->pos] && l->src[l->pos] != '\n')
        l->pos++;
      continue;
    }
    break;
  }
}

static int parse_hex2(const char *s) {
  int hi = 0, lo = 0;
  if(*s >= '0' && *s <= '9')
    hi = *s - '0';
  else if(*s >= 'a' && *s <= 'f')
    hi = *s - 'a' + 10;
  else if(*s >= 'A' && *s <= 'F')
    hi = *s - 'A' + 10;
  s++;
  if(*s >= '0' && *s <= '9')
    lo = *s - '0';
  else if(*s >= 'a' && *s <= 'f')
    lo = *s - 'a' + 10;
  else if(*s >= 'A' && *s <= 'F')
    lo = *s - 'A' + 10;
  return (hi << 4) | lo;
}

static uint32_t parse_hex_n(const char *s, int n) {
  uint32_t v = 0;
  for(int i = 0; i < n; i++) {
    v <<= 4;
    char c = s[i];
    if(c >= '0' && c <= '9')
      v |= c - '0';
    else if(c >= 'a' && c <= 'f')
      v |= c - 'a' + 10;
    else if(c >= 'A' && c <= 'F')
      v |= c - 'A' + 10;
  }
  return v;
}

static Tok lex_next(Lexer *l) {
  skip_ws_comments(l);
  Tok         t   = { 0 };
  const char *src = l->src;
  int         p   = l->pos;

  if(!src[p]) {
    t.kind = T_EOF;
    return t;
  }

  switch(src[p]) {
    case '=':
      l->pos++;
      t.kind = T_EQ;
      return t;
    case '{':
      l->pos++;
      t.kind = T_LBRACE;
      return t;
    case '}':
      l->pos++;
      t.kind = T_RBRACE;
      return t;
    case '[':
      l->pos++;
      t.kind = T_LBRACKET;
      return t;
    case ']':
      l->pos++;
      t.kind = T_RBRACKET;
      return t;
    case ',':
      l->pos++;
      t.kind = T_COMMA;
      return t;
    case '|':
      l->pos++;
      t.kind = T_IDENT;
      strcpy(t.s, "|");
      return t;
  }

  /* quoted string */
  if(src[p] == '"') {
    p++;
    int i = 0;
    while(src[p] && src[p] != '"') {
      if(src[p] == '\\' && src[p + 1]) {
        p++;
        switch(src[p]) {
          case 'n': t.s[i++] = '\n'; break;
          case 't': t.s[i++] = '\t'; break;
          default: t.s[i++] = src[p]; break;
        }
      } else {
        t.s[i++] = src[p];
      }
      p++;
      if(i >= (int)sizeof(t.s) - 1) break;
    }
    if(src[p] == '"') p++;
    t.s[i] = '\0';
    t.kind = T_STRING;
    l->pos = p;
    return t;
  }

  /* rgb(...) / rgba(...) color */
  if(!strncmp(src + p, "rgba(", 5) || !strncmp(src + p, "rgb(", 4)) {
    int is_rgba = (src[p + 3] == 'a');
    p += is_rgba ? 5 : 4;
    int hs = p;
    while(isxdigit((unsigned char)src[p]))
      p++;
    int hlen = p - hs;
    if(src[p] == ')') p++;
    if(is_rgba && hlen == 8) {
      uint32_t v = parse_hex_n(src + hs, 8);
      t.r        = (v >> 24) & 0xff;
      t.g        = (v >> 16) & 0xff;
      t.b        = (v >> 8) & 0xff;
      t.a        = v & 0xff;
    } else if(!is_rgba && hlen == 6) {
      uint32_t v = parse_hex_n(src + hs, 6);
      t.r        = (v >> 16) & 0xff;
      t.g        = (v >> 8) & 0xff;
      t.b        = v & 0xff;
      t.a        = 0xff;
    } else {
      t.kind = T_ERR;
      l->pos = p;
      return t;
    }
    t.kind = T_COLOR;
    l->pos = p;
    return t;
  }

  /* #rrggbb / #rrggbbaa color */
  if(src[p] == '#') {
    int hs = p + 1;
    int j  = hs;
    while(isxdigit((unsigned char)src[j]))
      j++;
    int hlen = j - hs;
    if(hlen == 6) {
      uint32_t v = parse_hex_n(src + hs, 6);
      t.r        = (v >> 16) & 0xff;
      t.g        = (v >> 8) & 0xff;
      t.b        = v & 0xff;
      t.a        = 0xff;
      t.kind     = T_COLOR;
      l->pos     = j;
      return t;
    } else if(hlen == 8) {
      uint32_t v = parse_hex_n(src + hs, 8);
      t.r        = (v >> 24) & 0xff;
      t.g        = (v >> 16) & 0xff;
      t.b        = (v >> 8) & 0xff;
      t.a        = v & 0xff;
      t.kind     = T_COLOR;
      l->pos     = j;
      return t;
    }
    /* fall through: treat as error */
    t.kind = T_ERR;
    l->pos = p + 1;
    return t;
  }

  /* $var_ref or $var_def (we emit as T_IDENT with the resolved value, or
   * as the var name prefixed with '$' so the parser can detect var defs) */
  if(src[p] == '$') {
    p++;
    int ns = p;
    while(isalnum((unsigned char)src[p]) || src[p] == '_')
      p++;
    char name[MAX_VAR_NAME] = { 0 };
    int  nlen               = p - ns;
    if(nlen >= MAX_VAR_NAME) nlen = MAX_VAR_NAME - 1;
    memcpy(name, src + ns, nlen);
    l->pos = p;
    /* check if this is a definition ($name = ...) */
    skip_ws_comments(l);
    if(l->src[l->pos] == '=' && l->src[l->pos + 1] != '=') {
      /* var def — return special token so parser can store it */
      t.kind = T_IDENT;
      snprintf(t.s, sizeof(t.s), "$%s", name);
      return t;
    }
    /* var ref — resolve immediately */
    const char *val = var_get(l->vars, name);
    if(!val) {
      wlr_log(WLR_DEBUG, "config: undefined variable $%s", name);
      t.kind = T_IDENT;
      t.s[0] = '\0';
      return t;
    }
    strncpy(t.s, val, sizeof(t.s) - 1);
    /* try to classify the resolved value */
    if(t.s[0] == '#') {
      int hs2 = 1;
      int j2  = hs2;
      while(isxdigit((unsigned char)t.s[j2]))
        j2++;
      int hlen2 = j2 - hs2;
      if(hlen2 == 6) {
        uint32_t v = parse_hex_n(t.s + 1, 6);
        t.r        = (v >> 16) & 0xff;
        t.g        = (v >> 8) & 0xff;
        t.b        = v & 0xff;
        t.a        = 0xff;
        t.kind     = T_COLOR;
        return t;
      } else if(hlen2 == 8) {
        uint32_t v = parse_hex_n(t.s + 1, 8);
        t.r        = (v >> 24) & 0xff;
        t.g        = (v >> 16) & 0xff;
        t.b        = (v >> 8) & 0xff;
        t.a        = v & 0xff;
        t.kind     = T_COLOR;
        return t;
      }
    }
    t.kind = T_IDENT;
    return t;
  }

  /* number (possibly with unit) */
  if(isdigit((unsigned char)src[p]) ||
     (src[p] == '-' && isdigit((unsigned char)src[p + 1]))) {
    int ns = p;
    if(src[p] == '-') p++;
    while(isdigit((unsigned char)src[p]))
      p++;
    int is_float = (src[p] == '.');
    if(is_float) {
      p++;
      while(isdigit((unsigned char)src[p]))
        p++;
    }
    char nbuf[64] = { 0 };
    int  nlen     = p - ns;
    if(nlen >= 64) nlen = 63;
    memcpy(nbuf, src + ns, nlen);
    double v = atof(nbuf);
    if(!strncmp(src + p, "px", 2)) {
      t.kind = T_DIM_PX;
      t.d    = v;
      p += 2;
    } else if(!strncmp(src + p, "ms", 2)) {
      t.kind = T_DIM_MS;
      t.d    = v;
      p += 2;
    } else if(!strncasecmp(src + p, "hz", 2)) {
      t.kind = T_DIM_HZ;
      t.d    = v;
      p += 2;
    } else if(src[p] == '%') {
      t.kind = T_DIM_PCT;
      t.d    = v;
      p++;
    } else if(is_float) {
      t.kind = T_FLOAT;
      t.d    = v;
    } else {
      t.kind = T_INT;
      t.d    = v;
    }
    l->pos = p;
    return t;
  }

  /* ident (includes keybind combos: SUPER+SHIFT:q, eDP-1, wl-paste, etc.) */
  if(isalpha((unsigned char)src[p]) || src[p] == '_' || src[p] == '-') {
    int ns = p;
    while(isalnum((unsigned char)src[p]) || src[p] == '_' || src[p] == '-' ||
          src[p] == '+' || src[p] == ':' || src[p] == '.' || src[p] == '@' ||
          src[p] == '/')
      p++;
    int ilen = p - ns;
    if(ilen >= (int)sizeof(t.s)) ilen = (int)sizeof(t.s) - 1;
    memcpy(t.s, src + ns, ilen);
    t.s[ilen] = '\0';
    t.kind    = T_IDENT;
    l->pos    = p;
    return t;
  }

  /* unknown */
  t.kind = T_ERR;
  l->pos = p + 1;
  return t;
}

/* ── Token stream with one-token lookahead ────────────────────────────────── */

typedef struct {
  Lexer lex;
  Tok   cur;
  bool  have_cur;
} TokStream;

static void ts_init(TokStream *ts, const char *src, VarTable *vars) {
  lex_init(&ts->lex, src, vars);
  ts->have_cur = false;
}

static Tok ts_peek(TokStream *ts) {
  if(!ts->have_cur) {
    ts->cur      = lex_next(&ts->lex);
    ts->have_cur = true;
  }
  return ts->cur;
}

static Tok ts_next(TokStream *ts) {
  Tok t        = ts_peek(ts);
  ts->have_cur = false;
  return t;
}

static bool ts_eat(TokStream *ts, TokKind k) {
  if(ts_peek(ts).kind == k) {
    ts_next(ts);
    return true;
  }
  return false;
}

/* ── Value helpers for the config layer ──────────────────────────────────── */

/* Parse one scalar value token into a string representation (for simple
 * string-based config consumers) and return the token. */
static Tok parse_scalar(TokStream *ts) {
  return ts_next(ts);
}

/* ── Forward declare ──────────────────────────────────────────────────────── */
static void parse_block_body(TokStream  *ts,
                             Config     *c,
                             VarTable   *vt,
                             const char *block,
                             const char *label,
                             const char *file_dir);

/* ── Color from token ─────────────────────────────────────────────────────── */
static Color tok_color(Tok t) {
  return (Color){ t.r, t.g, t.b, t.a };
}

/* ── Bool from ident string ───────────────────────────────────────────────── */
static bool tok_bool(const char *s) {
  return !strcasecmp(s, "true") || !strcasecmp(s, "yes") || !strcasecmp(s, "on");
}

/* ── Skip a full value (scalar or array) ─────────────────────────────────── */
static void skip_value(TokStream *ts) {
  Tok t = ts_peek(ts);
  if(t.kind == T_LBRACKET) {
    ts_next(ts);
    while(ts_peek(ts).kind != T_RBRACKET && ts_peek(ts).kind != T_EOF)
      ts_next(ts);
    ts_eat(ts, T_RBRACKET);
    return;
  }
  /* scalar, possibly followed by commas for an array */
  ts_next(ts);
  while(ts_peek(ts).kind == T_COMMA) {
    ts_next(ts);
    Tok n = ts_peek(ts);
    if(n.kind == T_RBRACE || n.kind == T_LBRACE || n.kind == T_EOF) break;
    ts_next(ts);
  }
}

/* ── Source directive ─────────────────────────────────────────────────────── */
static void
do_source(Config *c, VarTable *vt, const char *path_str, const char *file_dir);

/* ── Key-value handler ────────────────────────────────────────────────────── */

/* Collect a comma-separated list of ident/string tokens into a string array.
 * Used for modules_left/center/right. Returns count. */
static int collect_array(TokStream *ts, char out[][64], int max) {
  int n = 0;
  Tok t = ts_peek(ts);
  /* bracketed: [a, b, c] */
  if(t.kind == T_LBRACKET) {
    ts_next(ts);
    while(ts_peek(ts).kind != T_RBRACKET && ts_peek(ts).kind != T_EOF) {
      Tok v = ts_next(ts);
      if((v.kind == T_IDENT || v.kind == T_STRING) && n < max)
        strncpy(out[n++], v.s, 63);
      ts_eat(ts, T_COMMA);
    }
    ts_eat(ts, T_RBRACKET);
    return n;
  }
  /* unbracketed comma list */
  while(1) {
    Tok v = ts_peek(ts);
    if(v.kind == T_RBRACE || v.kind == T_EOF || v.kind == T_LBRACE) break;
    if(v.kind == T_IDENT || v.kind == T_STRING) {
      if(n < max) strncpy(out[n++], v.s, 63);
      ts_next(ts);
    } else
      break;
    if(!ts_eat(ts, T_COMMA)) break;
  }
  return n;
}

static void handle_kv(Config     *c,
                      VarTable   *vt,
                      TokStream  *ts,
                      const char *block,
                      const char *label,
                      const char *key,
                      Tok         val,
                      const char *file_dir) {

  /* ── top-level ──────────────────────────────────────────────────────── */
  if(!block[0]) {
    if(!strcmp(key, "source")) {
      if(val.kind == T_STRING || val.kind == T_IDENT)
        do_source(c, vt, val.s, file_dir);
      return;
    }
    if(!strcmp(key, "font"))
      strncpy(c->font_path, val.s, sizeof(c->font_path) - 1);
    else if(!strcmp(key, "font_size"))
      c->font_size = (float)val.d;
    else if(!strcmp(key, "gap"))
      c->gap = (int)val.d;
    else if(!strcmp(key, "border_width"))
      c->border_width = (int)val.d;
    else if(!strcmp(key, "corner_radius"))
      c->corner_radius = (int)val.d;
    else if(!strcmp(key, "smart_gaps"))
      c->smart_gaps = tok_bool(val.s);
    else if(!strcmp(key, "cursor_theme"))
      strncpy(c->cursor_theme, val.s, sizeof(c->cursor_theme) - 1);
    else if(!strcmp(key, "cursor_size"))
      c->cursor_size = (int)val.d;
    else if(!strcmp(key, "workspaces"))
      c->workspaces = (int)val.d;
    else if(!strcmp(key, "seat_name") || !strcmp(key, "seat"))
      strncpy(c->seat_name, val.s, sizeof(c->seat_name) - 1);
    else if(!strcmp(key, "exec_once") && c->exec_once_count < 16)
      strncpy(c->exec_once[c->exec_once_count++], val.s, 255);
    else if(!strcmp(key, "exec") && c->exec_count < 16)
      strncpy(c->exec[c->exec_count++], val.s, 255);
    else if(!strcmp(key, "keybind")) {
      /* keybind = COMBO, action [, args...] — already consumed first
       * token (the combo) into val. Read remaining comma-sep args. */
      char args[16][256] = { 0 };
      int  argc          = 0;
      strncpy(args[argc++], val.s, 255); /* combo */
      while(ts_peek(ts).kind == T_COMMA) {
        ts_next(ts);
        Tok a = ts_peek(ts);
        if(a.kind == T_RBRACE || a.kind == T_EOF || a.kind == T_LBRACE) break;
        if(argc < 16) {
          Tok av = ts_next(ts);
          if(av.kind == T_IDENT || av.kind == T_STRING)
            strncpy(args[argc++], av.s, 255);
          else if(av.kind == T_INT || av.kind == T_FLOAT || av.kind == T_DIM_PX ||
                  av.kind == T_DIM_HZ || av.kind == T_DIM_PCT || av.kind == T_DIM_MS)
            snprintf(args[argc++], 255, "%.10g", av.d);
        } else
          ts_next(ts);
      }
      if(argc < 2) return;
      /* parse combo: SUPER+SHIFT:key */
      char combo[128];
      strncpy(combo, args[0], 127);
      uint32_t mods    = 0;
      char    *sym_str = combo;
      char    *sep     = strrchr(combo, ':');
      if(!sep) sep = strrchr(combo, '+'); /* fallback */
      if(sep) {
        *sep    = '\0';
        sym_str = sep + 1;
        char *m = strtok(combo, "+:");
        while(m) {
          if(!strcasecmp(m, "super") || !strcasecmp(m, "mod4"))
            mods |= MOD_SUPER;
          else if(!strcasecmp(m, "ctrl") || !strcasecmp(m, "control"))
            mods |= MOD_CTRL;
          else if(!strcasecmp(m, "alt") || !strcasecmp(m, "mod1"))
            mods |= MOD_ALT;
          else if(!strcasecmp(m, "shift"))
            mods |= MOD_SHIFT;
          m = strtok(NULL, "+:");
        }
      }
      xkb_keysym_t sym = xkb_keysym_from_name(sym_str, XKB_KEYSYM_CASE_INSENSITIVE);
      if(sym == XKB_KEY_NoSymbol) return;

      const char *act_str = args[1];
      const char *arg1    = argc > 2 ? args[2] : NULL;
      Action      act     = { 0 };

      if(!strcmp(act_str, "exec")) {
        act.kind = ACTION_EXEC;
        if(arg1) strncpy(act.exec_cmd, arg1, sizeof(act.exec_cmd) - 1);
        int ai = 0;
        for(int i = 3; i < argc && ai < MAX_EXEC_ARGS; i++)
          strncpy(act.exec_args[ai++], args[i], 255);
        act.exec_argc = ai;
      } else if(!strcmp(act_str, "close"))
        act.kind = ACTION_CLOSE;
      else if(!strcmp(act_str, "fullscreen"))
        act.kind = ACTION_FULLSCREEN;
      else if(!strcmp(act_str, "toggle_float"))
        act.kind = ACTION_TOGGLE_FLOAT;
      else if(!strcmp(act_str, "toggle_bar"))
        act.kind = ACTION_TOGGLE_BAR;
      else if(!strcmp(act_str, "focus")) {
        if(arg1) {
          if(!strcmp(arg1, "left"))
            act.kind = ACTION_FOCUS_LEFT;
          else if(!strcmp(arg1, "right"))
            act.kind = ACTION_FOCUS_RIGHT;
          else if(!strcmp(arg1, "up"))
            act.kind = ACTION_FOCUS_UP;
          else if(!strcmp(arg1, "down"))
            act.kind = ACTION_FOCUS_DOWN;
        }
      } else if(!strcmp(act_str, "move")) {
        if(arg1) {
          if(!strcmp(arg1, "left"))
            act.kind = ACTION_MOVE_LEFT;
          else if(!strcmp(arg1, "right"))
            act.kind = ACTION_MOVE_RIGHT;
        }
      } else if(!strcmp(act_str, "workspace")) {
        act.kind = ACTION_WORKSPACE;
        act.n    = arg1 ? atoi(arg1) : 1;
      } else if(!strcmp(act_str, "move_to_workspace")) {
        act.kind = ACTION_MOVE_TO_WS;
        act.n    = arg1 ? atoi(arg1) : 1;
      } else if(!strcmp(act_str, "next_layout"))
        act.kind = ACTION_NEXT_LAYOUT;
      else if(!strcmp(act_str, "prev_layout"))
        act.kind = ACTION_PREV_LAYOUT;
      else if(!strcmp(act_str, "grow_main"))
        act.kind = ACTION_GROW_MAIN;
      else if(!strcmp(act_str, "shrink_main"))
        act.kind = ACTION_SHRINK_MAIN;
      else if(!strcmp(act_str, "next_workspace"))
        act.kind = ACTION_NEXT_WS;
      else if(!strcmp(act_str, "prev_workspace"))
        act.kind = ACTION_PREV_WS;
      else if(!strcmp(act_str, "quit"))
        act.kind = ACTION_QUIT;
      else if(!strcmp(act_str, "reload"))
        act.kind = ACTION_RELOAD;
      else if(!strcmp(act_str, "scratchpad")) {
        act.kind = ACTION_SCRATCHPAD;
        if(arg1) strncpy(act.name, arg1, sizeof(act.name) - 1);
      } else
        return;

      if(c->keybind_count < MAX_KEYBINDS) {
        c->keybinds[c->keybind_count].mods   = mods;
        c->keybinds[c->keybind_count].sym    = sym;
        c->keybinds[c->keybind_count].action = act;
        c->keybind_count++;
      }
    }
    return;
  }

  /* ── bar {} ──────────────────────────────────────────────────────────── */
  if(!strcmp(block, "bar")) {
    BarCfg *b = &c->bar;
    if(!strcmp(key, "position"))
      b->position = !strcmp(val.s, "top") ? BAR_TOP : BAR_BOTTOM;
    else if(!strcmp(key, "height"))
      b->height = (int)val.d;
    else if(!strcmp(key, "item_spacing"))
      b->item_spacing = (int)val.d;
    else if(!strcmp(key, "pill_radius"))
      b->pill_radius = (int)val.d;
    else if(!strcmp(key, "font_size"))
      b->font_size = (float)val.d;
    else if(!strcmp(key, "bg"))
      b->bg = tok_color(val);
    else if(!strcmp(key, "fg"))
      b->fg = tok_color(val);
    else if(!strcmp(key, "accent"))
      b->accent = tok_color(val);
    else if(!strcmp(key, "dim"))
      b->dim = tok_color(val);
    else if(!strcmp(key, "active_ws_fg"))
      b->active_ws_fg = tok_color(val);
    else if(!strcmp(key, "active_ws_bg"))
      b->active_ws_bg = tok_color(val);
    else if(!strcmp(key, "occupied_ws_fg"))
      b->occupied_ws_fg = tok_color(val);
    else if(!strcmp(key, "inactive_ws_fg"))
      b->inactive_ws_fg = tok_color(val);
    else if(!strcmp(key, "separator"))
      b->separator = tok_bool(val.s);
    else if(!strcmp(key, "separator_color"))
      b->separator_color = tok_color(val);
    else if(!strcmp(key, "modules_left")) {
      /* val was already consumed — re-parse from stream */
      b->modules_left_n = 0;
      if(val.kind == T_IDENT || val.kind == T_STRING) {
        strncpy(b->modules_left[b->modules_left_n++], val.s, 63);
        while(ts_eat(ts, T_COMMA)) {
          Tok v = ts_next(ts);
          if((v.kind == T_IDENT || v.kind == T_STRING) &&
             b->modules_left_n < MAX_BAR_MODS)
            strncpy(b->modules_left[b->modules_left_n++], v.s, 63);
        }
      } else if(val.kind == T_LBRACKET) {
        while(ts_peek(ts).kind != T_RBRACKET && ts_peek(ts).kind != T_EOF) {
          Tok v = ts_next(ts);
          if((v.kind == T_IDENT || v.kind == T_STRING) &&
             b->modules_left_n < MAX_BAR_MODS)
            strncpy(b->modules_left[b->modules_left_n++], v.s, 63);
          ts_eat(ts, T_COMMA);
        }
        ts_eat(ts, T_RBRACKET);
      }
    } else if(!strcmp(key, "modules_center")) {
      b->modules_center_n = 0;
      if(val.kind == T_IDENT || val.kind == T_STRING) {
        strncpy(b->modules_center[b->modules_center_n++], val.s, 63);
        while(ts_eat(ts, T_COMMA)) {
          Tok v = ts_next(ts);
          if((v.kind == T_IDENT || v.kind == T_STRING) &&
             b->modules_center_n < MAX_BAR_MODS)
            strncpy(b->modules_center[b->modules_center_n++], v.s, 63);
        }
      } else if(val.kind == T_LBRACKET) {
        while(ts_peek(ts).kind != T_RBRACKET && ts_peek(ts).kind != T_EOF) {
          Tok v = ts_next(ts);
          if((v.kind == T_IDENT || v.kind == T_STRING) &&
             b->modules_center_n < MAX_BAR_MODS)
            strncpy(b->modules_center[b->modules_center_n++], v.s, 63);
          ts_eat(ts, T_COMMA);
        }
        ts_eat(ts, T_RBRACKET);
      }
    } else if(!strcmp(key, "modules_right")) {
      b->modules_right_n = 0;
      if(val.kind == T_IDENT || val.kind == T_STRING) {
        strncpy(b->modules_right[b->modules_right_n++], val.s, 63);
        while(ts_eat(ts, T_COMMA)) {
          Tok v = ts_next(ts);
          if((v.kind == T_IDENT || v.kind == T_STRING) &&
             b->modules_right_n < MAX_BAR_MODS)
            strncpy(b->modules_right[b->modules_right_n++], v.s, 63);
        }
      } else if(val.kind == T_LBRACKET) {
        while(ts_peek(ts).kind != T_RBRACKET && ts_peek(ts).kind != T_EOF) {
          Tok v = ts_next(ts);
          if((v.kind == T_IDENT || v.kind == T_STRING) &&
             b->modules_right_n < MAX_BAR_MODS)
            strncpy(b->modules_right[b->modules_right_n++], v.s, 63);
          ts_eat(ts, T_COMMA);
        }
        ts_eat(ts, T_RBRACKET);
      }
    }
    return;
  }

  /* ── colors {} ──────────────────────────────────────────────────────── */
  if(!strcmp(block, "colors")) {
    if(!strcmp(key, "active_border"))
      c->colors.active_border = tok_color(val);
    else if(!strcmp(key, "inactive_border"))
      c->colors.inactive_border = tok_color(val);
    else if(!strcmp(key, "pane_bg"))
      c->colors.pane_bg = tok_color(val);
    else if(!strcmp(key, "bar_bg"))
      c->bar.bg = tok_color(val);
    else if(!strcmp(key, "bar_fg"))
      c->bar.fg = tok_color(val);
    else if(!strcmp(key, "bar_accent"))
      c->bar.accent = tok_color(val);
    return;
  }

  /* ── keyboard {} ─────────────────────────────────────────────────────── */
  if(!strcmp(block, "keyboard")) {
    if(!strcmp(key, "layout"))
      strncpy(c->keyboard.kb_layout, val.s, sizeof(c->keyboard.kb_layout) - 1);
    else if(!strcmp(key, "variant"))
      strncpy(c->keyboard.kb_variant, val.s, sizeof(c->keyboard.kb_variant) - 1);
    else if(!strcmp(key, "options"))
      strncpy(c->keyboard.kb_options, val.s, sizeof(c->keyboard.kb_options) - 1);
    else if(!strcmp(key, "repeat_rate"))
      c->keyboard.repeat_rate = (int)val.d;
    else if(!strcmp(key, "repeat_delay"))
      c->keyboard.repeat_delay = (int)val.d;
    return;
  }

  /* ── monitor <label> {} ──────────────────────────────────────────────── */
  if(!strcmp(block, "monitor") && c->monitor_count > 0) {
    MonitorCfg *m = &c->monitors[c->monitor_count - 1];
    if(!strcmp(key, "width"))
      m->width = (int)val.d;
    else if(!strcmp(key, "height"))
      m->height = (int)val.d;
    else if(!strcmp(key, "refresh"))
      m->refresh = (int)val.d;
    else if(!strcmp(key, "scale"))
      m->scale = (float)val.d;
    else if(!strcmp(key, "position")) {
      m->pos_x = (int)val.d;
      ts_eat(ts, T_COMMA);
      Tok y    = ts_next(ts);
      m->pos_y = (int)y.d;
    }
    return;
  }

  /* ── scratchpad <label> {} ───────────────────────────────────────────── */
  if(!strcmp(block, "scratchpad") && c->scratchpad_count > 0) {
    ScratchpadCfg *sp = &c->scratchpads[c->scratchpad_count - 1];
    if(!strcmp(key, "app_id"))
      strncpy(sp->app_id, val.s, sizeof(sp->app_id) - 1);
    else if(!strcmp(key, "width")) {
      float v = (float)val.d;
      if(val.kind == T_DIM_PCT) v /= 100.0f;
      sp->width_pct = v > 1.0f ? 1.0f : v < 0.1f ? 0.1f : v;
    } else if(!strcmp(key, "height")) {
      float v = (float)val.d;
      if(val.kind == T_DIM_PCT) v /= 100.0f;
      sp->height_pct = v > 1.0f ? 1.0f : v < 0.1f ? 0.1f : v;
    }
    return;
  }

  /* ── animations {} / general {} / unknown ─────────────────────────────── */
  /* silently accepted */
  (void)label;
}

/* ── Block body parser ────────────────────────────────────────────────────── */

static void parse_block_body(TokStream  *ts,
                             Config     *c,
                             VarTable   *vt,
                             const char *block,
                             const char *label,
                             const char *file_dir) {
  while(1) {
    Tok t = ts_peek(ts);
    if(t.kind == T_RBRACE || t.kind == T_EOF) break;

    if(t.kind != T_IDENT) {
      ts_next(ts);
      continue;
    } /* skip garbage */

    Tok  key_tok = ts_next(ts);
    char key[256];
    strncpy(key, key_tok.s, 255);

    /* $var definition inside a block */
    if(key[0] == '$') {
      if(!ts_eat(ts, T_EQ)) continue;
      Tok  v                 = ts_next(ts);
      char vbuf[MAX_VAR_VAL] = { 0 };
      if(v.kind == T_IDENT || v.kind == T_STRING)
        strncpy(vbuf, v.s, MAX_VAR_VAL - 1);
      else if(v.kind == T_COLOR)
        snprintf(vbuf, MAX_VAR_VAL, "#%02x%02x%02x%02x", v.r, v.g, v.b, v.a);
      else
        snprintf(vbuf, MAX_VAR_VAL, "%.10g", v.d);
      var_set(vt, key + 1, vbuf);
      continue;
    }

    Tok next = ts_peek(ts);

    /* labeled sub-block: scratchpad term { } */
    if(next.kind == T_IDENT || next.kind == T_STRING) {
      /* peek two ahead: is there a '{' after the label? */
      Tok label_tok = ts_next(ts);
      Tok after     = ts_peek(ts);
      if(after.kind == T_LBRACE) {
        ts_next(ts); /* consume { */
        /* handle monitor/scratchpad block starts */
        if(!strcmp(key, "monitor") && c->monitor_count < MAX_MONITORS) {
          MonitorCfg *m = &c->monitors[c->monitor_count++];
          memset(m, 0, sizeof(*m));
          m->refresh = 60;
          m->scale   = 1.0f;
          strncpy(m->name, label_tok.s, sizeof(m->name) - 1);
        }
        if(!strcmp(key, "scratchpad") && c->scratchpad_count < MAX_SCRATCHPADS) {
          ScratchpadCfg *sp = &c->scratchpads[c->scratchpad_count++];
          memset(sp, 0, sizeof(*sp));
          sp->width_pct  = 0.6f;
          sp->height_pct = 0.6f;
          strncpy(sp->name, label_tok.s, sizeof(sp->name) - 1);
          strncpy(sp->app_id, label_tok.s, sizeof(sp->app_id) - 1);
        }
        parse_block_body(ts, c, vt, key, label_tok.s, file_dir);
        ts_eat(ts, T_RBRACE);
        continue;
      }
      /* not a block — treat as key = label_tok (bare value, no '=') */
      handle_kv(c, vt, ts, block, label, key, label_tok, file_dir);
      continue;
    }

    /* unlabeled block: colors { } */
    if(next.kind == T_LBRACE) {
      ts_next(ts);
      parse_block_body(ts, c, vt, key, "", file_dir);
      ts_eat(ts, T_RBRACE);
      continue;
    }

    /* assignment: key = value */
    if(!ts_eat(ts, T_EQ)) continue;

    /* for modules_* the value may start with '[' — pass the bracket tok */
    Tok val;
    if(ts_peek(ts).kind == T_LBRACKET) {
      val = ts_next(ts); /* consume '[' — handle_kv sees T_LBRACKET */
    } else {
      val = ts_next(ts);
    }

    /* resolve var ref */
    if(val.kind == T_IDENT && val.s[0] == '$') {
      const char *rv = var_get(vt, val.s + 1);
      if(rv) strncpy(val.s, rv, sizeof(val.s) - 1);
    }

    handle_kv(c, vt, ts, block, label, key, val, file_dir);
  }
}

/* ── Source directive implementation ─────────────────────────────────────── */

static void
do_source(Config *c, VarTable *vt, const char *path_str, const char *file_dir) {
  char path[512] = { 0 };
  if(!strncmp(path_str, "~/", 2)) {
    const char *home = getenv("HOME");
    if(!home) home = "/root";
    snprintf(path, sizeof(path), "%s/%s", home, path_str + 2);
  } else if(path_str[0] != '/' && file_dir) {
    snprintf(path, sizeof(path), "%s/%s", file_dir, path_str);
  } else {
    strncpy(path, path_str, sizeof(path) - 1);
  }

  FILE *f = fopen(path, "r");
  if(!f) {
    wlr_log(WLR_DEBUG, "config: source '%s' not found", path);
    return;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  rewind(f);
  char *src = malloc(sz + 1);
  if(!src) {
    fclose(f);
    return;
  }
  fread(src, 1, sz, f);
  src[sz] = '\0';
  fclose(f);

  /* derive dir for nested sources */
  char dir[512] = { 0 };
  strncpy(dir, path, sizeof(dir) - 1);
  char *slash = strrchr(dir, '/');
  if(slash)
    *slash = '\0';
  else
    dir[0] = '\0';

  TokStream ts2;
  ts_init(&ts2, src, vt);
  parse_block_body(&ts2, c, vt, "", "", dir);
  free(src);
}

/* ── Top-level file parse ─────────────────────────────────────────────────── */

static void
parse_file_src(Config *c, VarTable *vt, const char *src, const char *file_dir) {
  /* Pass 1: collect $variables */
  for(const char *line = src; *line;) {
    const char *end = strchr(line, '\n');
    if(!end) end = line + strlen(line);
    /* find first non-space */
    while(line < end && isspace((unsigned char)*line))
      line++;
    if(*line == '$') {
      const char *p  = line + 1;
      const char *ns = p;
      while(p < end && (isalnum((unsigned char)*p) || *p == '_'))
        p++;
      if(p < end) {
        char name[MAX_VAR_NAME] = { 0 };
        int  nl                 = (int)(p - ns);
        if(nl >= MAX_VAR_NAME) nl = MAX_VAR_NAME - 1;
        memcpy(name, ns, nl);
        /* skip ws and '=' */
        while(p < end && isspace((unsigned char)*p))
          p++;
        if(*p == '=') {
          p++;
          while(p < end && isspace((unsigned char)*p))
            p++;
          /* grab rest of line (strip comment) */
          const char *vs = p;
          while(p < end && !(p[0] == '/' && p[1] == '/'))
            p++;
          while(p > vs && isspace((unsigned char)p[-1]))
            p--;
          char val[MAX_VAR_VAL] = { 0 };
          int  vl               = (int)(p - vs);
          if(vl >= MAX_VAR_VAL) vl = MAX_VAR_VAL - 1;
          memcpy(val, vs, vl);
          var_set(vt, name, val);
        }
      }
    }
    line = (*end == '\n') ? end + 1 : end;
  }

  /* Pass 2: parse */
  TokStream ts;
  ts_init(&ts, src, vt);
  parse_block_body(&ts, c, vt, "", "", file_dir);
}

/* ── Defaults ─────────────────────────────────────────────────────────────── */

void config_defaults(Config *c) {
  memset(c, 0, sizeof(*c));

  strncpy(c->font_path,
          "/usr/share/fonts/TTF/JetBrainsMono-Regular.ttf",
          sizeof(c->font_path) - 1);
  c->font_size     = 14.0f;
  c->gap           = 4;
  c->border_width  = 1;
  c->corner_radius = 0;
  c->smart_gaps    = false;
  strncpy(c->cursor_theme, "default", sizeof(c->cursor_theme) - 1);
  c->cursor_size = 24;
  c->workspaces  = 9;
  strncpy(c->seat_name, "seat0", sizeof(c->seat_name) - 1);

  c->colors.active_border   = color_hex(0xb4befe);
  c->colors.inactive_border = color_hex(0x45475a);
  c->colors.pane_bg         = color_hex(0x11111b);

  BarCfg *b          = &c->bar;
  b->position        = BAR_BOTTOM;
  b->height          = 28;
  b->item_spacing    = 4;
  b->pill_radius     = 4;
  b->bg              = color_hex(0x181825);
  b->fg              = color_hex(0xa6adc8);
  b->accent          = color_hex(0xb4befe);
  b->dim             = color_hex(0x585b70);
  b->active_ws_fg    = color_hex(0x11111b);
  b->active_ws_bg    = color_hex(0xb4befe);
  b->occupied_ws_fg  = color_hex(0xb4befe);
  b->inactive_ws_fg  = color_hex(0x585b70);
  b->separator       = false;
  b->separator_color = color_hex(0x313244);

  strncpy(b->modules_left[0], "workspaces", 63);
  b->modules_left_n = 1;
  strncpy(b->modules_center[0], "clock", 63);
  b->modules_center_n = 1;
  strncpy(b->modules_right[0], "layout", 63);
  strncpy(b->modules_right[1], "battery", 63);
  strncpy(b->modules_right[2], "network", 63);
  b->modules_right_n = 3;

  c->keyboard.repeat_rate  = 25;
  c->keyboard.repeat_delay = 600;

  int k = 0;
#define KB(m, sym_str, act)                                           \
  do {                                                                \
    c->keybinds[k].mods = (m);                                        \
    c->keybinds[k].sym =                                              \
        xkb_keysym_from_name((sym_str), XKB_KEYSYM_CASE_INSENSITIVE); \
    c->keybinds[k].action = (act);                                    \
    k++;                                                              \
  } while(0)

  KB(MOD_SUPER, "Return", ((Action){ .kind = ACTION_EXEC, .exec_cmd = "foot" }));
  KB(MOD_SUPER, "q", ((Action){ .kind = ACTION_CLOSE }));
  KB(MOD_SUPER, "f", ((Action){ .kind = ACTION_FULLSCREEN }));
  KB(MOD_SUPER | MOD_SHIFT, "space", ((Action){ .kind = ACTION_TOGGLE_FLOAT }));
  KB(MOD_SUPER | MOD_SHIFT, "b", ((Action){ .kind = ACTION_TOGGLE_BAR }));
  KB(MOD_SUPER, "h", ((Action){ .kind = ACTION_FOCUS_LEFT }));
  KB(MOD_SUPER, "l", ((Action){ .kind = ACTION_FOCUS_RIGHT }));
  KB(MOD_SUPER, "k", ((Action){ .kind = ACTION_FOCUS_UP }));
  KB(MOD_SUPER, "j", ((Action){ .kind = ACTION_FOCUS_DOWN }));
  KB(MOD_SUPER | MOD_SHIFT, "h", ((Action){ .kind = ACTION_MOVE_LEFT }));
  KB(MOD_SUPER | MOD_SHIFT, "l", ((Action){ .kind = ACTION_MOVE_RIGHT }));
  KB(MOD_SUPER, "Tab", ((Action){ .kind = ACTION_NEXT_LAYOUT }));
  KB(MOD_SUPER, "equal", ((Action){ .kind = ACTION_GROW_MAIN }));
  KB(MOD_SUPER, "minus", ((Action){ .kind = ACTION_SHRINK_MAIN }));
  KB(MOD_SUPER | MOD_CTRL, "Right", ((Action){ .kind = ACTION_NEXT_WS }));
  KB(MOD_SUPER | MOD_CTRL, "Left", ((Action){ .kind = ACTION_PREV_WS }));
  for(int i = 1; i <= 9; i++) {
    char sym[4];
    snprintf(sym, sizeof(sym), "%d", i);
    c->keybinds[k].mods   = MOD_SUPER;
    c->keybinds[k].sym    = xkb_keysym_from_name(sym, XKB_KEYSYM_CASE_INSENSITIVE);
    c->keybinds[k].action = (Action){ .kind = ACTION_WORKSPACE, .n = i };
    k++;
    c->keybinds[k].mods   = MOD_SUPER | MOD_SHIFT;
    c->keybinds[k].sym    = xkb_keysym_from_name(sym, XKB_KEYSYM_CASE_INSENSITIVE);
    c->keybinds[k].action = (Action){ .kind = ACTION_MOVE_TO_WS, .n = i };
    k++;
  }
#undef KB
  c->keybind_count = k;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void config_load(Config *c, const char *path) {
  config_defaults(c);

  FILE *f = fopen(path, "r");
  if(!f) {
    wlr_log(WLR_INFO, "config: no file at %s, using defaults", path);
    return;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  rewind(f);
  char *src = malloc(sz + 1);
  if(!src) {
    fclose(f);
    return;
  }
  fread(src, 1, sz, f);
  src[sz] = '\0';
  fclose(f);

  char dir[512] = { 0 };
  strncpy(dir, path, sizeof(dir) - 1);
  char *slash = strrchr(dir, '/');
  if(slash)
    *slash = '\0';
  else
    dir[0] = '\0';

  VarTable vt = { 0 };
  parse_file_src(c, &vt, src, dir);
  free(src);

  wlr_log(WLR_INFO,
          "config: loaded %s (%d keybinds, %d monitors, %d scratchpads)",
          path,
          c->keybind_count,
          c->monitor_count,
          c->scratchpad_count);
}

static char config_path_buf[512];

static const char *get_config_path(void) {
  if(config_path_buf[0]) return config_path_buf;
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if(xdg)
    snprintf(config_path_buf, sizeof(config_path_buf), "%s/trixie/trixie.conf", xdg);
  else {
    const char *home = getenv("HOME");
    if(!home) home = "/root";
    snprintf(config_path_buf,
             sizeof(config_path_buf),
             "%s/.config/trixie/trixie.conf",
             home);
  }
  return config_path_buf;
}

void config_reload(Config *c) {
  int  exec_count_save = c->exec_count;
  char exec_save[16][256];
  memcpy(exec_save, c->exec, sizeof(exec_save));
  config_load(c, get_config_path());
  (void)exec_count_save;
}
