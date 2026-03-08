/* run_panel.h — Public API for the Run panel (run_panel.c).
 *
 * Include from overlay.c (and anywhere else that needs to drive runs).
 */
#pragma once

#include "trixie.h"
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/* Auto-populate presets from project files; idempotent. */
void run_configs_init(void);

/* Non-blocking reap of finished child processes.  Call each render frame. */
void run_configs_poll(void);

/* Kill all running processes, join reader threads.  Call from overlay_destroy. */
void run_configs_destroy(void);

/* ── Control ───────────────────────────────────────────────────────────── */

void run_config_start(int idx);
void run_config_stop(int idx);

/* ── Key handler ───────────────────────────────────────────────────────── */

/* Returns true (always consumes keys while run panel is active).
 * cursor is the overlay's shared cursor for this panel. */
bool run_panel_key(int *cursor, xkb_keysym_t sym);

/* ── Draw ──────────────────────────────────────────────────────────────── */

void draw_panel_run(uint32_t     *px,
                    int           stride,
                    int           px0,
                    int           py0,
                    int           pw,
                    int           ph,
                    int          *cursor,
                    const Config *cfg);
