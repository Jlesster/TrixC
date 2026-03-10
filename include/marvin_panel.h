/* marvin_panel.h — Public API for the Marvin project-manager TUI panel.
 *
 * Include in overlay.c (and anywhere else that calls into Marvin).
 * Never include overlay_internal.h from outside the overlay subsystem.
 *
 * Ownership rules
 * ───────────────
 * • All static state (g_marvin_proj, g_marvin_tab, g_wiz, …) lives in
 *   marvin_panel.c.
 * • overlay.c owns TrixieOverlay and passes o->scroll / o->fb_cwd through.
 * • The four symbols below are the only cross-file interface.
 */
#pragma once

#include "trixie.h"
#include <stdbool.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

/* ── Sub-tab indices (4 tabs: project / tasks / wizard / console) ────────── */
typedef enum {
  MARVIN_TAB_PROJECT = 0,
  MARVIN_TAB_TASKS,
  MARVIN_TAB_WIZARD,
  MARVIN_TAB_CONSOLE,
  MARVIN_TAB_COUNT,
} MarvinTab;

/* ── Global tab/cursor state — read by overlay.c for the panel context
 *    string in the chrome header.  Written only by marvin_panel.c.       ── */
extern MarvinTab g_marvin_tab;
extern int       g_marvin_cursor;

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Poll project detection (throttled).  Pass o->fb_cwd as cwd_hint. */
void marvin_poll(const char *cwd_hint);

/* Handle a key event while PANEL_MARVIN is active.
 * Returns true (key consumed) unconditionally, matching overlay.c convention. */
bool marvin_panel_key(TrixieOverlay *o, xkb_keysym_t sym, uint32_t mods);

/* Render the full Marvin panel into the supplied pixel buffer.
 * bx/by/bw/bh is the OUTER content box (border included), same convention
 * as every other draw_panel_* function in overlay.c. */
void draw_panel_marvin(uint32_t      *px,
                       int            stride,
                       int            bx,
                       int            by,
                       int            bw,
                       int            bh,
                       TrixieOverlay *o,
                       const Config  *cfg);

/* Quick-run by action id ("build","run","brun","test","clean",…).
 * Safe to call from overlay.c key shortcuts that bypass the Marvin panel. */
void mv_run_by_id(const char *id);

/* Write a short context string for the chrome header into buf (max sz bytes).
 * Describes the active sub-tab / project name / running action.           */
void marvin_panel_ctx(char *buf, size_t sz);

/* Returns true while the Marvin panel has an active text-input field
 * (e.g. the wizard prompt), so overlay.c can show "INPUT" mode.           */
bool marvin_panel_in_input(void);
