/* screenshot.h — wlr-screencopy-based screenshot capture
 *
 * Three capture modes (matching hyprland/grimblast conventions):
 *   SCREENSHOT_FULL    — entire output
 *   SCREENSHOT_REGION  — user-defined box (coordinates provided by caller,
 *                        e.g. after running slurp or trixie's own region picker)
 *   SCREENSHOT_WINDOW  — focused window's geometry
 *
 * Output: PNG written to a path derived from $XDG_PICTURES_DIR (or ~/Pictures)
 * with filename pattern "trixie_YYYY-MM-DD_HH-MM-SS.png".
 *
 * The capture is asynchronous — the caller kicks it off, wlroots delivers the
 * frame via a wl_listener, and then we write the PNG and optionally copy to
 * clipboard.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
  SCREENSHOT_FULL   = 0,
  SCREENSHOT_REGION = 1,
  SCREENSHOT_WINDOW = 2,
} ScreenshotMode;

typedef struct {
  /* Crop rectangle in output-local logical pixels.  Ignored for FULL. */
  int x, y, w, h;
} ScreenshotRegion;

/* Opaque context — one per in-flight capture.
 * Freed automatically after the PNG is written (or on error). */
typedef struct ScreenshotCtx ScreenshotCtx;

/* Kick off a screenshot.  Returns NULL on immediate failure (no outputs, etc.)
 * server must be TrixieServer*. */
ScreenshotCtx *screenshot_capture(void            *server,
                                  ScreenshotMode   mode,
                                  ScreenshotRegion region,
                                  bool             copy_clipboard);

/* Called from action dispatch — convenience wrappers. */
void screenshot_full(void *server);
void screenshot_window(void *server);
/* region: pass zero rect to launch interactive region picker subprocess. */
void screenshot_region(void *server, ScreenshotRegion r);
