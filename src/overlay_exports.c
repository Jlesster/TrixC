/* overlay_exports.c
 * Provides linkable definitions for the colour constants, layout constants,
 * and TUI primitive wrappers that marvin_panel.c references via extern.
 *
 * This file must be compiled as part of the Trixie target.
 * Add 'src/overlay_exports.c' to the sources list in meson.build.
 *
 * IMPORTANT: include overlay_internal.h BEFORE defining the constants so
 * the ROW_H / PAD macros here match what overlay.c actually uses.
 */

#define _POSIX_C_SOURCE 200809L
#include "overlay_internal.h" /* ROW_H, PAD, BDR, TrixieOverlay full def,
                                   ov_fill_rect, ov_draw_text, tui_* protos  */
#include "trixie.h"
#include <stdint.h>

/* ── Colour constants ───────────────────────────────────────────────────── */
/* Catppuccin Mocha — must match the values used inside overlay.c            */
const uint32_t COL_BASE     = 0x1e1e2e;
const uint32_t COL_SURFACE0 = 0x313244;
const uint32_t COL_SURFACE1 = 0x45475a;
const uint32_t COL_BORDER   = 0x585b70;
const uint32_t COL_FG1      = 0xcdd6f4;
const uint32_t COL_FG2      = 0xbac2de;
const uint32_t COL_FG3      = 0xa6adc8;
const uint32_t COL_ACCENT   = 0xcba6f7;
const uint32_t COL_GREEN    = 0xa6e3a1;
const uint32_t COL_RED      = 0xf38ba8;
const uint32_t COL_YELLOW   = 0xf9e2af;

/* ── Layout constants ───────────────────────────────────────────────────── */
/* g_ov_th is the font cell height set at runtime in overlay.c.
 * marvin_panel.c reads it via extern to compute OV_ROW_H dynamically.      */
/* g_ov_th is already defined (static int) in overlay.c — re-export it via
 * a non-static int so the linker can find it from marvin_panel.c.
 *
 * ADD THIS LINE to overlay.c, replacing:
 *   static int g_ov_th = 14;
 * with:
 *   int g_ov_th = 14;          <- remove 'static'
 * Then marvin_panel.c's  extern const int g_ov_th;  will resolve.          */

/* ── g_server ───────────────────────────────────────────────────────────── */
/* The global server pointer. Define it in main.c (or server.c) as:
 *   TrixieServer *g_server = NULL;
 * and set it after server init:
 *   g_server = &server;
 * marvin_panel.c already has  extern TrixieServer *g_server;               */

/* ── ipc_send_to_nvim ───────────────────────────────────────────────────── */
/* Implement this in nvim_panel.c or ipc.c.  It writes a command line to
 * the nvim bridge socket so trixie.lua receives it as a marvin_cmd event.
 *
 * Minimal implementation to add to nvim_panel.c:
 *
 *   void ipc_send_to_nvim(TrixieServer *s, const char *cmd) {
 *       if(!s || !cmd) return;
 *       // Push as a JSON event to all subscribers; trixie.lua filters by
 *       // event type.  Format matches dispatch_event() in trixie.lua.
 *       char buf[512];
 *       snprintf(buf, sizeof(buf),
 *           "{\"event\":\"marvin_cmd\",\"action\":\"%s\"}\n", cmd);
 *       // Re-use the existing subscriber broadcast:
 *       for(int i = 0; i < s->subscriber_count; i++)
 *           write(s->subscriber_fds[i], buf, strlen(buf));
 *   }
 */

/* ── g_server ───────────────────────────────────────────────────────────── */
/* Global server pointer — set in main.c after server init:
 *   extern TrixieServer *g_server;
 *   g_server = &server;
 */
TrixieServer *g_server = NULL;

/* ── ipc_send_to_nvim ───────────────────────────────────────────────────── */
#include <string.h>
#include <unistd.h>
void ipc_send_to_nvim(TrixieServer *s, const char *cmd) {
  if(!s || !cmd) return;
  char buf[512];
  snprintf(buf, sizeof(buf), "{\"event\":\"marvin_cmd\",\"action\":\"%s\"}\n", cmd);
  for(int i = 0; i < s->subscriber_count; i++)
    write(s->subscriber_fds[i], buf, strlen(buf));
}

/* ── mp_tui_* wrappers ──────────────────────────────────────────────────── */
/* marvin_panel.c calls these via its internal #define aliases.
 * They bridge the uint32_t color values marvin uses to the Color struct
 * that the real tui_* functions (now non-static in overlay.c) expect.      */

/* Forward-declare the real functions (defined in overlay.c, now non-static) */
extern void tui_box(uint32_t   *px,
                    int         stride,
                    int         bx,
                    int         by,
                    int         bw,
                    int         bh,
                    const char *title,
                    const char *hint,
                    Color       ac,
                    Color       bg,
                    int         cw,
                    int         ch);
extern void tui_hsep(uint32_t   *px,
                     int         stride,
                     int         bx,
                     int         by_sep,
                     int         bw,
                     const char *label,
                     Color       ac,
                     Color       bg,
                     int         cw,
                     int         ch);
extern void tui_scrollbar(uint32_t *px,
                          int       stride,
                          int       bx,
                          int       by,
                          int       bw,
                          int       bh,
                          int       scroll,
                          int       total,
                          int       visible,
                          Color     ac,
                          int       cw,
                          int       ch);

static inline Color u32_to_color(uint32_t c) {
  return (Color){ (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff, 0xff };
}

void mp_tui_box(uint32_t *px,
                int       stride,
                int       x,
                int       y,
                int       w,
                int       h,
                uint32_t  bg_col,
                uint32_t  border_col,
                int       cw,
                int       ch) {
  tui_box(px,
          stride,
          x,
          y,
          w,
          h,
          NULL,
          NULL,
          u32_to_color(border_col),
          u32_to_color(bg_col),
          cw,
          ch);
}

void mp_tui_hsep(uint32_t *px, int stride, int x, int y, int bw, int cw, int ch) {
  tui_hsep(px,
           stride,
           x,
           y,
           bw,
           NULL,
           u32_to_color(COL_BORDER),
           u32_to_color(COL_BASE),
           cw,
           ch);
}

void mp_tui_scrollbar(uint32_t *px,
                      int       stride,
                      int       x,
                      int       y,
                      int       w,
                      int       h,
                      int       total,
                      int       first,
                      int       visible,
                      int       cw,
                      int       ch) {
  tui_scrollbar(px,
                stride,
                x,
                y,
                w,
                h,
                first,
                total,
                visible,
                u32_to_color(COL_ACCENT),
                cw,
                ch);
}

/* ── marvin_lua_detect ──────────────────────────────────────────────────── */
/* Detects project type by inspecting files in root.
 * Returns a static string like "rust", "meson", "cmake", "go", etc.
 * overlay.c calls this from overlay_create and overlay_set_cwd.            */
#include <sys/stat.h>

const char *marvin_lua_detect(const char *root) {
  if(!root || !root[0]) return "";
  char        path[1280];
  struct stat st;
#define CHECK(file, result)                          \
  snprintf(path, sizeof(path), "%s/%s", root, file); \
  if(stat(path, &st) == 0) return result;
  CHECK("Cargo.toml", "rust")
  CHECK("meson.build", "meson")
  CHECK("CMakeLists.txt", "cmake")
  CHECK("go.mod", "go")
  CHECK("package.json", "node")
  CHECK("pyproject.toml", "python")
  CHECK("setup.py", "python")
  CHECK("Makefile", "make")
  CHECK("makefile", "make")
#undef CHECK
  return "";
}
