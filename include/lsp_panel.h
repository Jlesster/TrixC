/* lsp_panel.h */
#pragma once
#include "trixie.h"
#include <stdbool.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

/* Draw */
void draw_panel_lsp(uint32_t         *px,
                    int               stride,
                    int               px0,
                    int               py0,
                    int               pw,
                    int               ph,
                    int              *cursor,
                    const Config     *cfg,
                    const OverlayCfg *ov_cfg);

/* Key handler */
bool lsp_panel_key(int *cursor, xkb_keysym_t sym, const OverlayCfg *ov_cfg);

/* Frame tick — refreshes snapshot from nvim */
void lsp_panel_tick(const OverlayCfg *ov_cfg);

/* Badge string for tab bar, e.g. " 3" or "" */
const char *lsp_tab_badge(void);
