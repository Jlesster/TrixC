/* files_panel.h — File browser panel public API */
#pragma once

#include "trixie.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

/* Initialise cwd (sets to getcwd() if empty). */
void fb_init(char *cwd_buf, size_t cwd_bufsz);

/* Key handler. Returns true (consumes all keys while panel is active). */
bool files_panel_key(int              *cursor,
                     char             *cwd,
                     int               cwd_bufsz,
                     char             *filter,
                     int              *filter_len,
                     bool             *filter_mode,
                     xkb_keysym_t      sym,
                     const OverlayCfg *ov_cfg);

/* Draw. */
void draw_panel_files(uint32_t     *px,
                      int           stride,
                      int           px0,
                      int           py0,
                      int           pw,
                      int           ph,
                      int          *cursor,
                      char         *cwd,
                      int           cwd_bufsz,
                      char         *filter,
                      int          *filter_len,
                      bool         *filter_mode,
                      const Config *cfg);
