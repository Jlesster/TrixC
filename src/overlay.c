/* overlay.c — Ratatui-styled TUI dev overlay for Trixie.
 *
 * Seven panels (Tab / 1-7):
 *
 *   [1] Workspace map   — minimap grid + layout/ratio per workspace
 *   [2] Command palette — all keybinds + IPC commands, fuzzy search
 *   [3] Process list    — top-N CPU/mem from /proc (no popen)
 *   [4] Log viewer      — wlr_log ring buffer, colour-coded by severity
 *   [5] Git             — branch, status summary, last 10 commits
 *   [6] Build           — run a build command, stream stdout into log ring
 *   [7] Notes           — scratch pad persisted to ~/.config/trixie/notes.txt
 *
 * Navigation
 * ──────────
 *   Tab / 1-7    switch panel
 *   j / Down     cursor / scroll down
 *   k / Up       cursor / scroll up
 *   Enter        execute selected item (commands); confirm (build)
 *   Escape / `   dismiss
 *   /            enter filter mode (commands panel)
 *   Backspace    delete filter char; delete notes char
 *   e            open notes for editing (notes panel)
 *   r            refresh git status (git panel)
 *   b            trigger build (build panel)
 *   Any printable key while notes in edit mode: append to note
 */

#define _POSIX_C_SOURCE 200809L
#include "trixie.h"
#include <dirent.h>
#include <drm_fourcc.h>
#include <ft2build.h>
#include <unistd.h>
#include FT_FREETYPE_H
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <xkbcommon/xkbcommon.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * §0  Log ring
 * ═══════════════════════════════════════════════════════════════════════════ */

#define LOG_RING_SIZE 512
#define LOG_LINE_MAX  256

static struct {
  char lines[LOG_RING_SIZE][LOG_LINE_MAX];
  int  head;
  int  count;
} g_log_ring;

static void log_ring_push(const char *line) {
  strncpy(g_log_ring.lines[g_log_ring.head], line, LOG_LINE_MAX - 1);
  g_log_ring.lines[g_log_ring.head][LOG_LINE_MAX - 1] = '\0';
  g_log_ring.head = (g_log_ring.head + 1) % LOG_RING_SIZE;
  if(g_log_ring.count < LOG_RING_SIZE) g_log_ring.count++;
}

static const char *log_ring_get(int idx) {
  if(idx < 0 || idx >= g_log_ring.count) return "";
  int real =
      (g_log_ring.head - g_log_ring.count + idx + LOG_RING_SIZE * 2) % LOG_RING_SIZE;
  return g_log_ring.lines[real];
}

static void
overlay_log_handler(enum wlr_log_importance imp, const char *fmt, va_list args) {
  (void)imp;
  char    buf[LOG_LINE_MAX];
  va_list args2;
  va_copy(args2, args); /* copy before first use */
  vsnprintf(buf, sizeof(buf), fmt, args);
  log_ring_push(buf);
  vfprintf(stderr, fmt, args2); /* second use from the copy */
  va_end(args2);
  fputc('\n', stderr);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Process list
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PROC_MAX 32

typedef struct {
  pid_t pid;
  char  comm[32];
  float cpu_pct;
  long  rss_kb;
} ProcEntry;

static ProcEntry g_procs[PROC_MAX];
static int       g_proc_count   = 0;
static int64_t   g_proc_next_ms = 0;
static long      g_clk_tck      = 0;

static int64_t ov_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void refresh_procs(void) {
  int64_t now = ov_now_ms();
  if(now < g_proc_next_ms) return;
  g_proc_next_ms = now + 2000;
  if(!g_clk_tck) g_clk_tck = sysconf(_SC_CLK_TCK);

  long long cpu_total = 0;
  {
    FILE *f = fopen("/proc/stat", "r");
    if(f) {
      long long u, n, s, i, w, r, si, st;
      if(fscanf(f,
                "cpu  %lld %lld %lld %lld %lld %lld %lld %lld",
                &u,
                &n,
                &s,
                &i,
                &w,
                &r,
                &si,
                &st) == 8)
        cpu_total = u + n + s + i + w + r + si + st;
      fclose(f);
    }
  }

  g_proc_count = 0;
  DIR *d       = opendir("/proc");
  if(!d) return;
  struct dirent *ent;
  while((ent = readdir(d)) != NULL && g_proc_count < PROC_MAX) {
    pid_t pid = (pid_t)atoi(ent->d_name);
    if(pid <= 0) continue;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if(!f) continue;
    ProcEntry pe    = { .pid = pid };
    long long utime = 0, stime = 0;
    long      rss          = 0;
    char      comm_buf[64] = { 0 };
    int       matched      = fscanf(f,
                         "%*d (%63[^)]) %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u "
                                    "%lld %lld %*d %*d %*d %*d %*d %*d %*u %*u %ld",
                         comm_buf,
                         &utime,
                         &stime,
                         &rss);
    fclose(f);
    if(matched != 4) continue;
    strncpy(pe.comm, comm_buf, sizeof(pe.comm) - 1);
    pe.cpu_pct =
        cpu_total > 0 ? (float)(utime + stime) * 100.f / (float)cpu_total : 0.f;
    pe.rss_kb               = rss * (long)(sysconf(_SC_PAGESIZE) / 1024);
    g_procs[g_proc_count++] = pe;
  }
  closedir(d);
  for(int i = 1; i < g_proc_count; i++) {
    ProcEntry key = g_procs[i];
    int       j   = i - 1;
    while(j >= 0 && g_procs[j].cpu_pct < key.cpu_pct) {
      g_procs[j + 1] = g_procs[j];
      j--;
    }
    g_procs[j + 1] = key;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Git state
 * ═══════════════════════════════════════════════════════════════════════════ */

#define GIT_LINE_MAX  128
#define GIT_LINES_MAX 64

typedef struct {
  char    branch[GIT_LINE_MAX];
  char    lines[GIT_LINES_MAX][GIT_LINE_MAX]; /* status + log lines */
  int     line_count;
  int64_t fetched_ms;
  bool    valid;
} GitState;

static GitState g_git;

/* Run cmd, capture up to max_lines lines into out[].  Returns line count. */
static int run_capture(const char *cmd, char out[][GIT_LINE_MAX], int max_lines) {
  FILE *f = popen(cmd, "r");
  if(!f) return 0;
  int n = 0;
  while(n < max_lines) {
    if(!fgets(out[n], GIT_LINE_MAX, f)) break;
    /* strip trailing newline */
    size_t l = strlen(out[n]);
    while(l > 0 && (out[n][l - 1] == '\n' || out[n][l - 1] == '\r'))
      out[n][--l] = '\0';
    n++;
  }
  pclose(f);
  return n;
}

static void refresh_git(void) {
  int64_t now = ov_now_ms();
  if(g_git.valid && now - g_git.fetched_ms < 5000) return;

  memset(&g_git, 0, sizeof(g_git));

  /* Branch name */
  {
    char tmp[GIT_LINE_MAX][1];
    (void)tmp; /* silence unused warning */
    FILE *f = popen("git rev-parse --abbrev-ref HEAD 2>/dev/null", "r");
    if(!f) {
      strcpy(g_git.branch, "(not a git repo)");
      return;
    }
    if(!fgets(g_git.branch, sizeof(g_git.branch), f)) {
      pclose(f);
      strcpy(g_git.branch, "(not a git repo)");
      return;
    }
    pclose(f);
    size_t bl = strlen(g_git.branch);
    while(bl > 0 && (g_git.branch[bl - 1] == '\n' || g_git.branch[bl - 1] == '\r'))
      g_git.branch[--bl] = '\0';
  }

  int n = 0;

  /* Short status */
  {
    char st[GIT_LINES_MAX][GIT_LINE_MAX];
    int  sc = run_capture("git status --short 2>/dev/null", st, GIT_LINES_MAX);
    if(sc == 0) {
      strncpy(g_git.lines[n++], "  (clean)", GIT_LINE_MAX - 1);
    } else {
      for(int i = 0; i < sc && n < GIT_LINES_MAX - 1; i++) {
        char buf[GIT_LINE_MAX];
        snprintf(buf, sizeof(buf), "  %s", st[i]);
        strncpy(g_git.lines[n++], buf, GIT_LINE_MAX - 1);
      }
    }
  }

  /* Separator */
  if(n < GIT_LINES_MAX) strncpy(g_git.lines[n++], "", GIT_LINE_MAX - 1);

  /* Last 10 commits — oneline */
  {
    char cl[GIT_LINES_MAX][GIT_LINE_MAX];
    int  cc = run_capture("git log --oneline -10 2>/dev/null", cl, GIT_LINES_MAX);
    if(n < GIT_LINES_MAX)
      strncpy(g_git.lines[n++], "Recent commits:", GIT_LINE_MAX - 1);
    for(int i = 0; i < cc && n < GIT_LINES_MAX; i++) {
      char buf[GIT_LINE_MAX];
      snprintf(buf, sizeof(buf), "  %s", cl[i]);
      strncpy(g_git.lines[n++], buf, GIT_LINE_MAX - 1);
    }
  }

  g_git.line_count = n;
  g_git.fetched_ms = now;
  g_git.valid      = true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Build runner
 * ═══════════════════════════════════════════════════════════════════════════ */

#define BUILD_CMD_MAX 256

typedef struct {
  char    cmd[BUILD_CMD_MAX];
  bool    running;
  bool    done;
  int     exit_code;
  int64_t started_ms;
  int64_t finished_ms;
} BuildState;

static BuildState g_build;

/* Synchronous build — runs popen and streams lines into log ring.
 * For a compositor this is fine; the frame loop continues on the next frame.
 * If you need async, move this to a pthread. */
static void run_build(void) {
  if(!g_build.cmd[0]) return;
  g_build.running    = true;
  g_build.done       = false;
  g_build.exit_code  = -1;
  g_build.started_ms = ov_now_ms();

  char header[LOG_LINE_MAX];
  snprintf(header, sizeof(header), "==> build: %s", g_build.cmd);
  log_ring_push(header);

  FILE *f = popen(g_build.cmd, "r");
  if(!f) {
    log_ring_push("==> build: popen failed");
    g_build.running = false;
    g_build.done    = true;
    return;
  }
  char buf[LOG_LINE_MAX];
  while(fgets(buf, sizeof(buf), f)) {
    size_t l = strlen(buf);
    while(l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r'))
      buf[--l] = '\0';
    log_ring_push(buf);
  }
  int status          = pclose(f);
  g_build.exit_code   = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  g_build.running     = false;
  g_build.done        = true;
  g_build.finished_ms = ov_now_ms();

  char footer[LOG_LINE_MAX];
  snprintf(footer,
           sizeof(footer),
           "==> build finished: exit %d (%.1fs)",
           g_build.exit_code,
           (double)(g_build.finished_ms - g_build.started_ms) / 1000.0);
  log_ring_push(footer);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Notes
 * ═══════════════════════════════════════════════════════════════════════════ */

#define NOTES_MAX 4096

static char g_notes[NOTES_MAX];
static int  g_notes_len  = 0;
static bool g_notes_edit = false;

static void notes_path(char *out, size_t sz) {
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if(xdg)
    snprintf(out, sz, "%s/trixie/notes.txt", xdg);
  else {
    const char *home = getenv("HOME");
    snprintf(out, sz, "%s/.config/trixie/notes.txt", home ? home : "/root");
  }
}

static void notes_load(void) {
  char path[512];
  notes_path(path, sizeof(path));
  FILE *f = fopen(path, "r");
  if(!f) {
    g_notes[0]  = '\0';
    g_notes_len = 0;
    return;
  }
  g_notes_len = (int)fread(g_notes, 1, NOTES_MAX - 1, f);
  if(g_notes_len < 0) g_notes_len = 0;
  g_notes[g_notes_len] = '\0';
  fclose(f);
}

static void notes_save(void) {
  char path[512];
  notes_path(path, sizeof(path));
  FILE *f = fopen(path, "w");
  if(!f) return;
  fwrite(g_notes, 1, (size_t)g_notes_len, f);
  fclose(f);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  FreeType singleton
 * ═══════════════════════════════════════════════════════════════════════════ */

static FT_Library g_ov_ft   = NULL;
static FT_Face    g_ov_face = NULL;
static int        g_ov_asc  = 0;
static int        g_ov_th   = 0;

static void ov_font_init(const char *path, float size_pt) {
  if(!g_ov_ft) FT_Init_FreeType(&g_ov_ft);
  if(g_ov_face) {
    FT_Done_Face(g_ov_face);
    g_ov_face = NULL;
  }
  if(!path || !path[0]) return;
  if(FT_New_Face(g_ov_ft, path, 0, &g_ov_face)) {
    g_ov_face = NULL;
    return;
  }
  if(size_pt <= 0.f) size_pt = 13.f;
  FT_Set_Char_Size(g_ov_face, 0, (FT_F26Dot6)(size_pt * 64.f), 96, 96);
  g_ov_asc = (int)ceilf((float)g_ov_face->size->metrics.ascender / 64.f);
  int desc = (int)floorf((float)g_ov_face->size->metrics.descender / 64.f);
  g_ov_th  = g_ov_asc - desc;
}

static int ov_measure(const char *text) {
  if(!g_ov_face || !text) return 0;
  int w = 0;
  for(const char *p = text; *p; p++) {
    if(FT_Load_Char(g_ov_face, (unsigned char)*p, FT_LOAD_ADVANCE_ONLY)) continue;
    w += (int)(g_ov_face->glyph->advance.x >> 6);
  }
  return w;
}

static void ov_draw_text(uint32_t   *px,
                         int         stride,
                         int         x,
                         int         y,
                         int         clip_w,
                         int         clip_h,
                         const char *text,
                         uint8_t     r,
                         uint8_t     g,
                         uint8_t     b,
                         uint8_t     a) {
  if(!g_ov_face || !text) return;
  int pen = x;
  for(const char *p = text; *p; p++) {
    if(FT_Load_Char(g_ov_face, (unsigned char)*p, FT_LOAD_RENDER)) continue;
    FT_GlyphSlot slot = g_ov_face->glyph;
    int          gx = pen + slot->bitmap_left, gy = y - slot->bitmap_top;
    for(int row = 0; row < (int)slot->bitmap.rows; row++) {
      int py = gy + row;
      if(py < 0 || py >= clip_h) continue;
      for(int col = 0; col < (int)slot->bitmap.width; col++) {
        int px_x = gx + col;
        if(px_x < 0 || px_x >= clip_w) continue;
        uint8_t glyph_a = slot->bitmap.buffer[row * slot->bitmap.pitch + col];
        if(!glyph_a) continue;
        uint8_t   ba  = (uint8_t)((uint32_t)glyph_a * a / 255);
        uint32_t *d   = &px[py * stride + px_x];
        uint32_t  inv = 255 - ba;
        uint8_t   or_ = (uint8_t)((r * ba + ((*d >> 16) & 0xff) * inv) / 255);
        uint8_t   og  = (uint8_t)((g * ba + ((*d >> 8) & 0xff) * inv) / 255);
        uint8_t   ob  = (uint8_t)((b * ba + (*d & 0xff) * inv) / 255);
        *d = (0xffu << 24) | ((uint32_t)or_ << 16) | ((uint32_t)og << 8) | ob;
      }
    }
    pen += (int)(slot->advance.x >> 6);
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  Pixel helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void ov_fill_rect(uint32_t *px,
                         int       stride,
                         int       x,
                         int       y,
                         int       w,
                         int       h,
                         uint8_t   r,
                         uint8_t   g,
                         uint8_t   b,
                         uint8_t   a,
                         int       cw,
                         int       ch) {
  uint32_t c = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  for(int row = y; row < y + h && row < ch; row++) {
    if(row < 0) continue;
    for(int col = x; col < x + w && col < cw; col++) {
      if(col < 0) continue;
      if(a == 255) {
        px[row * stride + col] = c;
      } else {
        uint32_t *d   = &px[row * stride + col];
        uint32_t  inv = 255 - a;
        uint8_t   or_ = (uint8_t)((r * a + ((*d >> 16) & 0xff) * inv) / 255);
        uint8_t   og  = (uint8_t)((g * a + ((*d >> 8) & 0xff) * inv) / 255);
        uint8_t   ob  = (uint8_t)((b * a + (*d & 0xff) * inv) / 255);
        *d = (0xffu << 24) | ((uint32_t)or_ << 16) | ((uint32_t)og << 8) | ob;
      }
    }
  }
}

static void ov_fill_border(uint32_t *px,
                           int       stride,
                           int       x,
                           int       y,
                           int       w,
                           int       h,
                           uint8_t   r,
                           uint8_t   g,
                           uint8_t   b,
                           uint8_t   a,
                           int       cw,
                           int       ch) {
  ov_fill_rect(px, stride, x, y, w, 1, r, g, b, a, cw, ch);
  ov_fill_rect(px, stride, x, y + h - 1, w, 1, r, g, b, a, cw, ch);
  ov_fill_rect(px, stride, x, y, 1, h, r, g, b, a, cw, ch);
  ov_fill_rect(px, stride, x + w - 1, y, 1, h, r, g, b, a, cw, ch);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  wlr_buffer wrapper
 * ═══════════════════════════════════════════════════════════════════════════ */

struct OvRawBuf {
  struct wlr_buffer base;
  uint32_t         *data;
  int               stride;
};
static void ovb_destroy(struct wlr_buffer *b) {
  struct OvRawBuf *rb = wl_container_of(b, rb, base);
  free(rb->data);
  free(rb);
}
static bool ovb_begin(struct wlr_buffer *b,
                      uint32_t           flags,
                      void             **data,
                      uint32_t          *fmt,
                      size_t            *stride) {
  (void)flags;
  struct OvRawBuf *rb = wl_container_of(b, rb, base);
  *data               = rb->data;
  *fmt                = DRM_FORMAT_ARGB8888;
  *stride             = (size_t)rb->stride;
  return true;
}
static void ovb_end(struct wlr_buffer *b) {
  (void)b;
}
static const struct wlr_buffer_impl ovb_impl = {
  .destroy               = ovb_destroy,
  .begin_data_ptr_access = ovb_begin,
  .end_data_ptr_access   = ovb_end,
};
static struct OvRawBuf *ovb_create(int w, int h) {
  struct OvRawBuf *rb = calloc(1, sizeof(*rb));
  rb->stride          = w * 4;
  rb->data            = calloc((size_t)(w * h), 4);
  wlr_buffer_init(&rb->base, &ovb_impl, w, h);
  return rb;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  Panel definitions
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
  PANEL_WORKSPACES = 0,
  PANEL_COMMANDS,
  PANEL_PROCESSES,
  PANEL_LOG,
  PANEL_GIT,
  PANEL_BUILD,
  PANEL_NOTES,
  PANEL_COUNT,
} PanelId;

static const char *panel_names[PANEL_COUNT] = { "[1] WS",   "[2] Cmds", "[3] Procs",
                                                "[4] Log",  "[5] Git",  "[6] Build",
                                                "[7] Notes" };

/* ─── Built-in commands (palette) ──────────────────────────────────────── */

typedef struct {
  const char *label;
  const char *ipc_cmd;
} OvCommand;

static const OvCommand g_commands[] = {
  { "Close focused window",     "close"               },
  { "Toggle fullscreen",        "fullscreen"          },
  { "Toggle float",             "float"               },
  { "Next layout",              "layout next"         },
  { "Previous layout",          "layout prev"         },
  { "BSP layout",               "set_layout bsp"      },
  { "Columns layout",           "set_layout columns"  },
  { "Monocle layout",           "set_layout monocle"  },
  { "Grow main pane",           "grow_main"           },
  { "Shrink main pane",         "shrink_main"         },
  { "Swap with master",         "swap_main"           },
  { "Swap forward",             "swap forward"        },
  { "Swap backward",            "swap back"           },
  { "Next workspace",           "next_workspace"      },
  { "Previous workspace",       "prev_workspace"      },
  { "Move pane to workspace 1", "move_to_workspace 1" },
  { "Move pane to workspace 2", "move_to_workspace 2" },
  { "Move pane to workspace 3", "move_to_workspace 3" },
  { "Reload config",            "reload"              },
  { "DPMS off",                 "dpms off"            },
  { "DPMS on",                  "dpms on"             },
  { "Quit compositor",          "quit"                },
};
#define G_COMMAND_COUNT (int)(sizeof(g_commands) / sizeof(g_commands[0]))

/* ═══════════════════════════════════════════════════════════════════════════
 * §9  TrixieOverlay struct
 * ═══════════════════════════════════════════════════════════════════════════ */

#define OV_FILTER_MAX 64

struct TrixieOverlay {
  struct wlr_scene_buffer *scene_buf;
  int                      w, h;
  bool                     visible;
  PanelId                  panel;
  int                      cursor;
  int                      scroll;
  char                     filter[OV_FILTER_MAX];
  int                      filter_len;
  bool                     filter_mode;
  char                     last_font[256];
  float                    last_size;
  int                      matches[G_COMMAND_COUNT];
  int                      match_count;
  /* build panel */
  char                     build_cmd[BUILD_CMD_MAX];
  int                      build_cmd_editing; /* 0=no,1=typing cmd */
  /* notes */
  bool                     notes_loaded;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * §10  Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

TrixieOverlay *
overlay_create(struct wlr_scene_tree *layer, int w, int h, const Config *cfg) {
  TrixieOverlay *o = calloc(1, sizeof(*o));
  o->w             = w;
  o->h             = h;
  o->panel         = PANEL_WORKSPACES;
  o->scene_buf     = wlr_scene_buffer_create(layer, NULL);
  wlr_scene_node_set_enabled(&o->scene_buf->node, false);
  ov_font_init(cfg->font_path,
               cfg->bar.font_size > 0.f ? cfg->bar.font_size : cfg->font_size);
  strncpy(o->last_font, cfg->font_path, sizeof(o->last_font) - 1);
  o->last_size = cfg->bar.font_size > 0.f ? cfg->bar.font_size : cfg->font_size;

  static bool log_installed = false;
  if(!log_installed) {
    wlr_log_init(WLR_INFO, overlay_log_handler);
    log_installed = true;
  }

  /* Default build command — override with 'b' key prompt */
  strncpy(o->build_cmd, "make -j$(nproc) 2>&1", sizeof(o->build_cmd) - 1);

  return o;
}

void overlay_destroy(TrixieOverlay *o) {
  if(!o) return;
  wlr_scene_node_destroy(&o->scene_buf->node);
  if(g_ov_face) {
    FT_Done_Face(g_ov_face);
    g_ov_face = NULL;
  }
  if(g_ov_ft) {
    FT_Done_FreeType(g_ov_ft);
    g_ov_ft = NULL;
  }
  free(o);
}

void overlay_toggle(TrixieOverlay *o) {
  if(!o) return;
  o->visible = !o->visible;
  wlr_scene_node_set_enabled(&o->scene_buf->node, o->visible);
  /* Lazy-load notes on first show */
  if(o->visible && !o->notes_loaded) {
    notes_load();
    o->notes_loaded = true;
  }
}
bool overlay_visible(TrixieOverlay *o) {
  return o && o->visible;
}
void overlay_resize(TrixieOverlay *o, int w, int h) {
  if(o) {
    o->w = w;
    o->h = h;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §11  Filter / match
 * ═══════════════════════════════════════════════════════════════════════════ */

static void rebuild_matches(TrixieOverlay *o) {
  o->match_count = 0;
  for(int i = 0; i < G_COMMAND_COUNT; i++) {
    if(!o->filter[0]) {
      o->matches[o->match_count++] = i;
      continue;
    }
    const char *h     = g_commands[i].label;
    const char *n     = o->filter;
    bool        found = false;
    for(int hi = 0; h[hi] && !found; hi++) {
      bool match = true;
      for(int ni = 0; n[ni]; ni++) {
        if(!h[hi + ni]) {
          match = false;
          break;
        }
        char hc = h[hi + ni], nc = n[ni];
        if(hc >= 'A' && hc <= 'Z') hc += 32;
        if(nc >= 'A' && nc <= 'Z') nc += 32;
        if(hc != nc) {
          match = false;
          break;
        }
      }
      if(match) found = true;
    }
    if(found) o->matches[o->match_count++] = i;
  }
  if(o->cursor >= o->match_count)
    o->cursor = o->match_count > 0 ? o->match_count - 1 : 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §12  Key handling
 * ═══════════════════════════════════════════════════════════════════════════ */

bool overlay_key(TrixieOverlay *o, xkb_keysym_t sym, uint32_t mods) {
  if(!o || !o->visible) return false;
  (void)mods;

  /* ── Notes edit mode swallows printable keys + backspace ── */
  if(o->panel == PANEL_NOTES && g_notes_edit) {
    if(sym == XKB_KEY_Escape) {
      g_notes_edit = false;
      notes_save();
      return true;
    }
    if(sym == XKB_KEY_Return && g_notes_len < NOTES_MAX - 1) {
      g_notes[g_notes_len++] = '\n';
      g_notes[g_notes_len]   = '\0';
      return true;
    }
    if(sym == XKB_KEY_BackSpace) {
      if(g_notes_len > 0) g_notes[--g_notes_len] = '\0';
      return true;
    }
    if(sym >= 0x20 && sym < 0x7f && g_notes_len < NOTES_MAX - 1) {
      g_notes[g_notes_len++] = (char)sym;
      g_notes[g_notes_len]   = '\0';
      return true;
    }
    return true;
  }

  /* ── Build cmd edit mode ── */
  if(o->panel == PANEL_BUILD && o->build_cmd_editing) {
    if(sym == XKB_KEY_Escape) {
      o->build_cmd_editing = 0;
      return true;
    }
    if(sym == XKB_KEY_Return) {
      o->build_cmd_editing = 0;
      run_build();
      o->panel = PANEL_LOG; /* switch to log to watch output */
      return true;
    }
    int l = (int)strlen(o->build_cmd);
    if(sym == XKB_KEY_BackSpace) {
      if(l > 0) o->build_cmd[l - 1] = '\0';
      return true;
    }
    if(sym >= 0x20 && sym < 0x7f && l < BUILD_CMD_MAX - 1) {
      o->build_cmd[l]     = (char)sym;
      o->build_cmd[l + 1] = '\0';
    }
    return true;
  }

  /* ── Global keys ── */
  if(sym == XKB_KEY_Tab) {
    o->panel       = (PanelId)((o->panel + 1) % PANEL_COUNT);
    o->cursor      = 0;
    o->scroll      = 0;
    o->filter[0]   = '\0';
    o->filter_len  = 0;
    o->filter_mode = false;
    return true;
  }
  if(sym >= XKB_KEY_1 && sym <= XKB_KEY_7) {
    o->panel       = (PanelId)(sym - XKB_KEY_1);
    o->cursor      = 0;
    o->scroll      = 0;
    o->filter[0]   = '\0';
    o->filter_len  = 0;
    o->filter_mode = false;
    return true;
  }
  if(sym == XKB_KEY_Escape || sym == XKB_KEY_grave) {
    o->visible = false;
    wlr_scene_node_set_enabled(&o->scene_buf->node, false);
    return true;
  }
  if(sym == XKB_KEY_j || sym == XKB_KEY_Down) {
    o->cursor++;
    return true;
  }
  if(sym == XKB_KEY_k || sym == XKB_KEY_Up) {
    if(o->cursor > 0) o->cursor--;
    return true;
  }

  /* ── Panel-specific keys ── */
  switch(o->panel) {
    case PANEL_COMMANDS:
      if(sym == XKB_KEY_slash) {
        o->filter_mode = true;
        return true;
      }
      if(o->filter_mode) {
        if(sym == XKB_KEY_BackSpace) {
          if(o->filter_len > 0) o->filter[--o->filter_len] = '\0';
          rebuild_matches(o);
          return true;
        }
        if(sym == XKB_KEY_Return) {
          o->filter_mode = false;
          return true;
        }
        if(sym >= 0x20 && sym < 0x7f && o->filter_len < OV_FILTER_MAX - 1) {
          o->filter[o->filter_len++] = (char)sym;
          o->filter[o->filter_len]   = '\0';
          rebuild_matches(o);
        }
        return true;
      }
      break;
    case PANEL_GIT:
      if(sym == XKB_KEY_r) {
        g_git.valid = false;
        refresh_git();
        return true;
      }
      break;
    case PANEL_BUILD:
      if(sym == XKB_KEY_b) {
        /* clear cmd and enter edit mode */
        o->build_cmd[0]      = '\0';
        o->build_cmd_editing = 1;
        return true;
      }
      if(sym == XKB_KEY_Return) {
        run_build();
        o->panel = PANEL_LOG;
        return true;
      }
      break;
    case PANEL_NOTES:
      if(sym == XKB_KEY_e) {
        g_notes_edit = true;
        return true;
      }
      break;
    default: break;
  }

  return true; /* consume all keys while overlay visible */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §13  Panel renderers
 * ═══════════════════════════════════════════════════════════════════════════ */

#define ROW_H    (g_ov_th + 6)
#define PAD      12
#define HEADER_H (ROW_H + 4)

/* ── Shared cursor-line highlight ─────────────────────────────────────── */
static void draw_cursor_line(uint32_t *px,
                             int       stride,
                             int       px0,
                             int       ry,
                             int       pw,
                             Color     ac,
                             Color     bg,
                             int       cw,
                             int       ch) {
  uint8_t sr = (uint8_t)((int)bg.r + 0x14 < 0xff ? bg.r + 0x14 : 0xff);
  uint8_t sg = (uint8_t)((int)bg.g + 0x14 < 0xff ? bg.g + 0x14 : 0xff);
  uint8_t sb = (uint8_t)((int)bg.b + 0x14 < 0xff ? bg.b + 0x14 : 0xff);
  ov_fill_rect(
      px, stride, px0 + PAD, ry, pw - PAD * 2, ROW_H, sr, sg, sb, 0xff, cw, ch);
  ov_fill_rect(px, stride, px0 + PAD, ry, 2, ROW_H, ac.r, ac.g, ac.b, 0xff, cw, ch);
}

/* ── [1] Workspaces ───────────────────────────────────────────────────── */
static void draw_panel_workspaces(uint32_t     *px,
                                  int           stride,
                                  int           px0,
                                  int           py0,
                                  int           pw,
                                  int           ph,
                                  TwmState     *twm,
                                  const Config *cfg) {
  int cols   = twm->ws_count > 4 ? 4 : twm->ws_count;
  int rows   = (twm->ws_count + cols - 1) / cols;
  int cell_w = (pw - PAD * 2 - (cols - 1) * 8) / cols;
  int cell_h = (ph - HEADER_H - PAD * 2 - (rows - 1) * 8) / rows;
  if(cell_h < 48) cell_h = 48;
  if(cell_w < 80) cell_w = 80;
  Color ac = cfg->colors.active_border;
  Color im = cfg->colors.inactive_border;

  for(int i = 0; i < twm->ws_count; i++) {
    int   col = i % cols, row = i / cols;
    int   cx     = px0 + PAD + col * (cell_w + 8);
    int   cy     = py0 + HEADER_H + PAD + row * (cell_h + 8);
    bool  active = (i == twm->active_ws);
    Color bc     = active ? ac : im;

    ov_fill_rect(px,
                 stride,
                 cx,
                 cy,
                 cell_w,
                 cell_h,
                 0x18,
                 0x18,
                 0x25,
                 0xff,
                 stride,
                 ph + py0);
    ov_fill_border(px,
                   stride,
                   cx,
                   cy,
                   cell_w,
                   cell_h,
                   bc.r,
                   bc.g,
                   bc.b,
                   0xff,
                   stride,
                   ph + py0);

    /* WS number */
    char wnum[8];
    snprintf(wnum, sizeof(wnum), "%d", i + 1);
    ov_draw_text(px,
                 stride,
                 cx + 4,
                 cy + g_ov_asc + 2,
                 stride,
                 ph + py0,
                 wnum,
                 ac.r,
                 ac.g,
                 ac.b,
                 active ? 0xff : 0x80);

    /* Layout + ratio on same line, right-aligned in cell */
    Workspace *ws = &twm->workspaces[i];
    char       lay_buf[32];
    snprintf(lay_buf,
             sizeof(lay_buf),
             "%s %.0f%%",
             layout_label(ws->layout),
             ws->main_ratio * 100.f);
    int lw = ov_measure(lay_buf);
    ov_draw_text(px,
                 stride,
                 cx + cell_w - lw - 4,
                 cy + g_ov_asc + 2,
                 stride,
                 ph + py0,
                 lay_buf,
                 0x58,
                 0x5b,
                 0x70,
                 0xff);

    /* Pane list */
    int max_lines = (cell_h - ROW_H) / (g_ov_th + 2);
    for(int j = 0; j < ws->pane_count && j < max_lines; j++) {
      Pane *p = twm_pane_by_id(twm, ws->panes[j]);
      if(!p) continue;
      bool focused = ws->has_focused && ws->focused == p->id;
      char title[32];
      strncpy(title, p->title[0] ? p->title : p->app_id, sizeof(title) - 1);
      if(ov_measure(title) > cell_w - 8)
        for(int k = (int)strlen(title); k > 0; k--) {
          title[k] = '\0';
          if(ov_measure(title) <= cell_w - 20) {
            strncat(title, "…", sizeof(title) - strlen(title) - 1);
            break;
          }
        }
      int     pty = cy + g_ov_asc + 2 + (j + 1) * (g_ov_th + 2);
      uint8_t fr  = focused ? ac.r : 0xa6;
      uint8_t fg  = focused ? ac.g : 0xad;
      uint8_t fb  = focused ? ac.b : 0xc8;
      ov_draw_text(
          px, stride, cx + 4, pty, stride, ph + py0, title, fr, fg, fb, 0xff);
    }
  }
}

/* ── [2] Commands ─────────────────────────────────────────────────────── */
static void draw_panel_commands(uint32_t      *px,
                                int            stride,
                                int            px0,
                                int            py0,
                                int            pw,
                                int            ph,
                                TrixieOverlay *o,
                                const Config  *cfg) {
  rebuild_matches(o);
  Color ac = cfg->colors.active_border;
  Color bg = cfg->colors.pane_bg;
  int   y  = py0 + HEADER_H + PAD;

  char filter_label[OV_FILTER_MAX + 20];
  if(o->filter_mode)
    snprintf(filter_label, sizeof(filter_label), "/ %s_", o->filter);
  else if(o->filter[0])
    snprintf(
        filter_label, sizeof(filter_label), "filter: %s  (/ to edit)", o->filter);
  else
    strncpy(filter_label,
            "/ to filter  j/k navigate  Enter exec",
            sizeof(filter_label) - 1);

  ov_draw_text(px,
               stride,
               px0 + PAD,
               y + g_ov_asc,
               stride,
               py0 + ph,
               filter_label,
               ac.r,
               ac.g,
               ac.b,
               0xff);
  y += ROW_H + 4;

  if(o->cursor >= o->match_count && o->match_count > 0)
    o->cursor = o->match_count - 1;

  int visible_rows = (ph - (y - py0) - PAD) / ROW_H;
  int scroll       = o->cursor - visible_rows + 1;
  if(scroll < 0) scroll = 0;

  for(int i = 0; i < o->match_count && i < visible_rows; i++) {
    int  mi  = o->matches[i + scroll];
    int  ry  = y + i * ROW_H;
    bool sel = (i + scroll == o->cursor);
    if(sel) draw_cursor_line(px, stride, px0, ry, pw, ac, bg, stride, py0 + ph);
    ov_draw_text(px,
                 stride,
                 px0 + PAD + 8,
                 ry + g_ov_asc + 2,
                 stride,
                 py0 + ph,
                 g_commands[mi].label,
                 sel ? ac.r : 0xa6,
                 sel ? ac.g : 0xad,
                 sel ? ac.b : 0xc8,
                 0xff);
    int hw = ov_measure(g_commands[mi].ipc_cmd);
    ov_draw_text(px,
                 stride,
                 px0 + pw - PAD - hw - 4,
                 ry + g_ov_asc + 2,
                 stride,
                 py0 + ph,
                 g_commands[mi].ipc_cmd,
                 0x58,
                 0x5b,
                 0x70,
                 0xff);
  }
}

/* ── [3] Processes ────────────────────────────────────────────────────── */
static void draw_panel_processes(uint32_t      *px,
                                 int            stride,
                                 int            px0,
                                 int            py0,
                                 int            pw,
                                 int            ph,
                                 TrixieOverlay *o,
                                 const Config  *cfg) {
  refresh_procs();
  Color ac = cfg->colors.active_border;
  Color bg = cfg->colors.pane_bg;
  int   y  = py0 + HEADER_H + PAD;

  /* Header row */
  ov_draw_text(px,
               stride,
               px0 + PAD,
               y + g_ov_asc,
               stride,
               py0 + ph,
               "PID",
               ac.r,
               ac.g,
               ac.b,
               0xff);
  ov_draw_text(px,
               stride,
               px0 + PAD + 60,
               y + g_ov_asc,
               stride,
               py0 + ph,
               "COMMAND",
               ac.r,
               ac.g,
               ac.b,
               0xff);
  ov_draw_text(px,
               stride,
               px0 + pw - PAD - 120,
               y + g_ov_asc,
               stride,
               py0 + ph,
               "CPU%",
               ac.r,
               ac.g,
               ac.b,
               0xff);
  ov_draw_text(px,
               stride,
               px0 + pw - PAD - 56,
               y + g_ov_asc,
               stride,
               py0 + ph,
               "RSS",
               ac.r,
               ac.g,
               ac.b,
               0xff);
  y += ROW_H;

  int visible_rows = (ph - (y - py0) - PAD) / ROW_H;
  if(o->cursor >= g_proc_count && g_proc_count > 0) o->cursor = g_proc_count - 1;

  for(int i = 0; i < g_proc_count && i < visible_rows; i++) {
    ProcEntry *pe  = &g_procs[i];
    bool       sel = (i == o->cursor);
    int        ry  = y + i * ROW_H;
    if(sel) draw_cursor_line(px, stride, px0, ry, pw, ac, bg, stride, py0 + ph);

    char pid_s[16], cpu_s[16], rss_s[16];
    snprintf(pid_s, sizeof(pid_s), "%d", pe->pid);
    snprintf(cpu_s, sizeof(cpu_s), "%.1f", pe->cpu_pct);
    snprintf(rss_s, sizeof(rss_s), "%ldM", pe->rss_kb / 1024);

    uint8_t fr = sel ? ac.r : 0xa6, fg = sel ? ac.g : 0xad, fb = sel ? ac.b : 0xc8;
    ov_draw_text(px,
                 stride,
                 px0 + PAD,
                 ry + g_ov_asc + 2,
                 stride,
                 py0 + ph,
                 pid_s,
                 0x58,
                 0x5b,
                 0x70,
                 0xff);
    ov_draw_text(px,
                 stride,
                 px0 + PAD + 60,
                 ry + g_ov_asc + 2,
                 stride,
                 py0 + ph,
                 pe->comm,
                 fr,
                 fg,
                 fb,
                 0xff);
    ov_draw_text(px,
                 stride,
                 px0 + pw - PAD - 120,
                 ry + g_ov_asc + 2,
                 stride,
                 py0 + ph,
                 cpu_s,
                 0xf3,
                 0x8b,
                 0xa8,
                 0xff);
    ov_draw_text(px,
                 stride,
                 px0 + pw - PAD - 56,
                 ry + g_ov_asc + 2,
                 stride,
                 py0 + ph,
                 rss_s,
                 0xa6,
                 0xe3,
                 0xa1,
                 0xff);
  }
}

/* ── [4] Log ──────────────────────────────────────────────────────────── */
static void draw_panel_log(uint32_t      *px,
                           int            stride,
                           int            px0,
                           int            py0,
                           int            pw,
                           int            ph,
                           TrixieOverlay *o,
                           const Config  *cfg) {
  (void)pw;
  Color ac           = cfg->colors.active_border;
  int   y            = py0 + HEADER_H + PAD;
  int   visible_rows = (ph - (y - py0) - PAD) / ROW_H;
  int   scroll_base  = g_log_ring.count - visible_rows - o->cursor;
  if(scroll_base < 0) scroll_base = 0;

  for(int i = 0; i < visible_rows; i++) {
    int li = scroll_base + i;
    if(li >= g_log_ring.count) break;
    const char *line = log_ring_get(li);
    int         ry   = y + i * ROW_H;
    uint8_t     lr = 0xa6, lg = 0xad, lb = 0xc8;
    if(strstr(line, "ERROR") || strstr(line, "error"))
      lr = 0xf3, lg = 0x8b, lb = 0xa8;
    else if(strstr(line, "WARN") || strstr(line, "warn"))
      lr = 0xf9, lg = 0xe2, lb = 0xaf;
    else if(strstr(line, "==>"))
      lr = ac.r, lg = ac.g, lb = ac.b;
    else if(strstr(line, "INFO"))
      lr = ac.r, lg = ac.g, lb = ac.b;
    ov_draw_text(px,
                 stride,
                 px0 + PAD,
                 ry + g_ov_asc + 2,
                 stride,
                 py0 + ph,
                 line,
                 lr,
                 lg,
                 lb,
                 0xff);
  }
}

/* ── [5] Git ──────────────────────────────────────────────────────────── */
static void draw_panel_git(uint32_t      *px,
                           int            stride,
                           int            px0,
                           int            py0,
                           int            pw,
                           int            ph,
                           TrixieOverlay *o,
                           const Config  *cfg) {
  (void)pw;
  refresh_git();
  Color ac = cfg->colors.active_border;
  int   y  = py0 + HEADER_H + PAD;

  /* Branch line */
  char branch_label[GIT_LINE_MAX + 16];
  snprintf(branch_label,
           sizeof(branch_label),
           "branch: %s  (r to refresh)",
           g_git.branch);
  ov_draw_text(px,
               stride,
               px0 + PAD,
               y + g_ov_asc,
               stride,
               py0 + ph,
               branch_label,
               ac.r,
               ac.g,
               ac.b,
               0xff);
  y += ROW_H;

  /* Separator */
  ov_fill_rect(px,
               stride,
               px0 + PAD,
               y,
               pw - PAD * 2,
               1,
               ac.r,
               ac.g,
               ac.b,
               0x40,
               stride,
               py0 + ph);
  y += 4;

  int visible_rows = (ph - (y - py0) - PAD) / ROW_H;
  int start        = o->cursor; /* scroll via j/k */
  if(start > g_git.line_count - visible_rows)
    start = g_git.line_count - visible_rows;
  if(start < 0) start = 0;

  for(int i = 0; i < visible_rows && (start + i) < g_git.line_count; i++) {
    const char *line = g_git.lines[start + i];
    int         ry   = y + i * ROW_H;
    /* Colour: lines starting with M/A/D/? are status flags */
    uint8_t     lr = 0xa6, lg = 0xad, lb = 0xc8;
    if(line[0] == 'M' || line[1] == 'M')
      lr = 0xf9, lg = 0xe2, lb = 0xaf;
    else if(line[0] == 'A' || line[1] == 'A')
      lr = 0xa6, lg = 0xe3, lb = 0xa1;
    else if(line[0] == 'D' || line[1] == 'D')
      lr = 0xf3, lg = 0x8b, lb = 0xa8;
    else if(line[0] == '?' && line[1] == '?')
      lr = 0x58, lg = 0x5b, lb = 0x70;
    else if(strncmp(line, "Recent", 6) == 0)
      lr = ac.r, lg = ac.g, lb = ac.b;
    ov_draw_text(px,
                 stride,
                 px0 + PAD,
                 ry + g_ov_asc + 2,
                 stride,
                 py0 + ph,
                 line,
                 lr,
                 lg,
                 lb,
                 0xff);
  }
}

/* ── [6] Build ────────────────────────────────────────────────────────── */
static void draw_panel_build(uint32_t      *px,
                             int            stride,
                             int            px0,
                             int            py0,
                             int            pw,
                             int            ph,
                             TrixieOverlay *o,
                             const Config  *cfg) {
  (void)pw;
  Color ac = cfg->colors.active_border;
  int   y  = py0 + HEADER_H + PAD;

  /* Command line */
  char cmd_display[BUILD_CMD_MAX + 20];
  if(o->build_cmd_editing)
    snprintf(cmd_display, sizeof(cmd_display), "cmd: %s_", o->build_cmd);
  else
    snprintf(cmd_display,
             sizeof(cmd_display),
             "cmd: %s  (b=edit  Enter=run)",
             o->build_cmd);

  ov_draw_text(px,
               stride,
               px0 + PAD,
               y + g_ov_asc,
               stride,
               py0 + ph,
               cmd_display,
               ac.r,
               ac.g,
               ac.b,
               0xff);
  y += ROW_H;

  /* Last build status */
  if(g_build.done) {
    char   status[64];
    double elapsed = (double)(g_build.finished_ms - g_build.started_ms) / 1000.0;
    snprintf(status,
             sizeof(status),
             "last exit: %d  (%.1fs)",
             g_build.exit_code,
             elapsed);
    uint8_t sr = g_build.exit_code == 0 ? 0xa6 : 0xf3;
    uint8_t sg = g_build.exit_code == 0 ? 0xe3 : 0x8b;
    uint8_t sb = g_build.exit_code == 0 ? 0xa1 : 0xa8;
    ov_draw_text(px,
                 stride,
                 px0 + PAD,
                 y + g_ov_asc,
                 stride,
                 py0 + ph,
                 status,
                 sr,
                 sg,
                 sb,
                 0xff);
    y += ROW_H;
  } else if(g_build.running) {
    ov_draw_text(px,
                 stride,
                 px0 + PAD,
                 y + g_ov_asc,
                 stride,
                 py0 + ph,
                 "building...",
                 0xf9,
                 0xe2,
                 0xaf,
                 0xff);
    y += ROW_H;
  }

  ov_fill_rect(px,
               stride,
               px0 + PAD,
               y,
               pw - PAD * 2,
               1,
               ac.r,
               ac.g,
               ac.b,
               0x40,
               stride,
               py0 + ph);
  y += 6;

  /* Last N log lines as build output preview */
  int visible_rows = (ph - (y - py0) - PAD) / ROW_H;
  int start        = g_log_ring.count - visible_rows;
  if(start < 0) start = 0;
  for(int i = 0; i < visible_rows; i++) {
    int li = start + i;
    if(li >= g_log_ring.count) break;
    const char *line = log_ring_get(li);
    int         ry   = y + i * ROW_H;
    uint8_t     lr = 0xa6, lg = 0xad, lb = 0xc8;
    if(strstr(line, "error:") || strstr(line, "ERROR"))
      lr = 0xf3, lg = 0x8b, lb = 0xa8;
    else if(strstr(line, "warning:"))
      lr = 0xf9, lg = 0xe2, lb = 0xaf;
    else if(strstr(line, "==>"))
      lr = ac.r, lg = ac.g, lb = ac.b;
    ov_draw_text(px,
                 stride,
                 px0 + PAD,
                 ry + g_ov_asc + 2,
                 stride,
                 py0 + ph,
                 line,
                 lr,
                 lg,
                 lb,
                 0xff);
  }
}

/* ── [7] Notes ────────────────────────────────────────────────────────── */
static void draw_panel_notes(uint32_t      *px,
                             int            stride,
                             int            px0,
                             int            py0,
                             int            pw,
                             int            ph,
                             TrixieOverlay *o,
                             const Config  *cfg) {
  (void)pw;
  Color ac = cfg->colors.active_border;
  int   y  = py0 + HEADER_H + PAD;

  const char *hint = g_notes_edit ? "INSERT MODE — Esc to save & exit"
                                  : "e to edit  Esc to dismiss  (auto-saved)";
  uint8_t     hr   = g_notes_edit ? 0xa6 : ac.r;
  uint8_t     hg   = g_notes_edit ? 0xe3 : ac.g;
  uint8_t     hb   = g_notes_edit ? 0xa1 : ac.b;
  ov_draw_text(
      px, stride, px0 + PAD, y + g_ov_asc, stride, py0 + ph, hint, hr, hg, hb, 0xff);
  y += ROW_H + 4;
  ov_fill_rect(px,
               stride,
               px0 + PAD,
               y,
               pw - PAD * 2,
               1,
               ac.r,
               ac.g,
               ac.b,
               0x40,
               stride,
               py0 + ph);
  y += 6;

  /* Render notes text line by line */
  int  visible_rows = (ph - (y - py0) - PAD - ROW_H) / ROW_H;
  /* Scroll to bottom when editing */
  int  line_idx     = 0;
  char line_buf[256];
  int  buf_i = 0;
  int  row   = 0;
  int  skip  = 0;

  /* Count total lines first for scroll offset */
  int total_lines = 1;
  for(int ci = 0; ci < g_notes_len; ci++)
    if(g_notes[ci] == '\n') total_lines++;
  if(g_notes_edit) skip = total_lines - visible_rows;
  if(skip < 0) skip = 0;
  (void)o; /* scroll via skip above */

  for(int ci = 0; ci <= g_notes_len; ci++) {
    char c = ci < g_notes_len ? g_notes[ci] : '\0';
    if(c == '\n' || c == '\0') {
      line_buf[buf_i] = '\0';
      if(line_idx >= skip && row < visible_rows) {
        int ry = y + row * ROW_H;
        ov_draw_text(px,
                     stride,
                     px0 + PAD,
                     ry + g_ov_asc + 2,
                     stride,
                     py0 + ph,
                     line_buf,
                     0xa6,
                     0xad,
                     0xc8,
                     0xff);
        row++;
      }
      line_idx++;
      buf_i = 0;
    } else if(buf_i < 255) {
      line_buf[buf_i++] = c;
    }
  }

  /* Cursor blink indicator at end in edit mode */
  if(g_notes_edit && row <= visible_rows) {
    int64_t ms = ov_now_ms();
    if((ms / 500) % 2 == 0) {
      int ry = y + (row > 0 ? row - 1 : 0) * ROW_H;
      ov_draw_text(px,
                   stride,
                   px0 + PAD + ov_measure(line_buf),
                   ry + g_ov_asc + 2,
                   stride,
                   py0 + ph,
                   "_",
                   ac.r,
                   ac.g,
                   ac.b,
                   0xff);
    }
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §14  overlay_update
 * ═══════════════════════════════════════════════════════════════════════════ */

void overlay_update(TrixieOverlay *o,
                    TwmState      *twm,
                    const Config  *cfg,
                    BarWorkerPool *pool) {
  if(!o || !o->visible) return;
  (void)pool;
  int w = o->w, h = o->h;
  if(w <= 0 || h <= 0) return;

  float want_size = cfg->bar.font_size > 0.f ? cfg->bar.font_size : cfg->font_size;
  if(strcmp(o->last_font, cfg->font_path) != 0 ||
     fabsf(o->last_size - want_size) > 0.01f) {
    ov_font_init(cfg->font_path, want_size);
    strncpy(o->last_font, cfg->font_path, sizeof(o->last_font) - 1);
    o->last_size = want_size;
  }

  struct OvRawBuf *rb = ovb_create(w, h);
  uint32_t        *px = rb->data;

  Color ac  = cfg->colors.active_border;
  Color bg  = cfg->colors.pane_bg;
  Color fg_ = cfg->colors.bar_fg;

  /* Full-screen opaque backdrop */
  ov_fill_rect(px, w, 0, 0, w, h, bg.r, bg.g, bg.b, 0xff, w, h);

  /* Content panel — 88% wide, 88% tall */
  int pw = w * 22 / 25, ph = h * 22 / 25;
  if(pw < 560) pw = 560;
  if(ph < 360) ph = 360;
  int px0 = (w - pw) / 2, py0 = (h - ph) / 2;

  uint8_t sr = (uint8_t)((int)bg.r + 0x0a < 0xff ? bg.r + 0x0a : 0xff);
  uint8_t sg = (uint8_t)((int)bg.g + 0x0a < 0xff ? bg.g + 0x0a : 0xff);
  uint8_t sb = (uint8_t)((int)bg.b + 0x0a < 0xff ? bg.b + 0x0a : 0xff);
  ov_fill_rect(px, w, px0, py0, pw, ph, sr, sg, sb, 0xff, w, h);
  ov_fill_border(px, w, px0, py0, pw, ph, ac.r, ac.g, ac.b, 0xff, w, h);

  /* Tab bar */
  int tab_y    = py0 + 1;
  int tab_text = tab_y + g_ov_asc + 2;

  ov_draw_text(px, w, px0 + PAD, tab_text, w, h, "trixie", ac.r, ac.g, ac.b, 0xff);
  int sep_x = px0 + PAD + ov_measure("trixie") + 4;
  ov_fill_rect(
      px, w, sep_x, tab_y + 1, 1, g_ov_th + 2, ac.r, ac.g, ac.b, 0x60, w, h);
  sep_x += 6;

  int tab_x = sep_x;
  for(int i = 0; i < PANEL_COUNT; i++) {
    bool        sel = (i == (int)o->panel);
    const char *nm  = panel_names[i];
    int         tw  = ov_measure(nm) + 8;
    if(sel) {
      ov_fill_rect(px,
                   w,
                   tab_x - 2,
                   tab_y,
                   tw + 4,
                   g_ov_th + 4,
                   ac.r,
                   ac.g,
                   ac.b,
                   0xff,
                   w,
                   h);
      ov_draw_text(px, w, tab_x, tab_text, w, h, nm, bg.r, bg.g, bg.b, 0xff);
    } else {
      ov_draw_text(px, w, tab_x, tab_text, w, h, nm, 0x55, 0x55, 0x55, 0xff);
    }
    tab_x += tw + 8;
  }

  /* Rule under tab bar */
  ov_fill_rect(px, w, px0, py0 + HEADER_H - 1, pw, 1, ac.r, ac.g, ac.b, 0xff, w, h);

  /* Active panel */
  switch(o->panel) {
    case PANEL_WORKSPACES:
      draw_panel_workspaces(px, w, px0, py0, pw, ph, twm, cfg);
      break;
    case PANEL_COMMANDS: draw_panel_commands(px, w, px0, py0, pw, ph, o, cfg); break;
    case PANEL_PROCESSES:
      draw_panel_processes(px, w, px0, py0, pw, ph, o, cfg);
      break;
    case PANEL_LOG: draw_panel_log(px, w, px0, py0, pw, ph, o, cfg); break;
    case PANEL_GIT: draw_panel_git(px, w, px0, py0, pw, ph, o, cfg); break;
    case PANEL_BUILD: draw_panel_build(px, w, px0, py0, pw, ph, o, cfg); break;
    case PANEL_NOTES: draw_panel_notes(px, w, px0, py0, pw, ph, o, cfg); break;
    default: break;
  }

  /* Mode line */
  {
    int ml_h = g_ov_th + 6;
    int ml_y = py0 + ph - ml_h;
    ov_fill_rect(px, w, px0, ml_y - 1, pw, 1, ac.r, ac.g, ac.b, 0xff, w, h);
    ov_fill_rect(px, w, px0, ml_y, pw, ml_h, bg.r, bg.g, bg.b, 0xff, w, h);

    static const char *hints[PANEL_COUNT] = {
      "j/k scroll  Tab next panel",
      "/ filter  j/k navigate  Enter exec  Tab next panel",
      "j/k select  Tab next panel",
      "j/k scroll  Tab next panel",
      "r refresh  j/k scroll  Tab next panel",
      "b edit cmd  Enter run  Tab next panel",
      "e edit  Esc save  Tab next panel",
    };
    const char *hint = (o->panel < PANEL_COUNT) ? hints[o->panel] : "";
    ov_draw_text(px,
                 w,
                 px0 + PAD,
                 ml_y + g_ov_asc + 2,
                 w,
                 h,
                 hint,
                 fg_.r,
                 fg_.g,
                 fg_.b,
                 0xa0);
  }

  wlr_scene_buffer_set_buffer(o->scene_buf, &rb->base);
  wlr_buffer_drop(&rb->base);
}
