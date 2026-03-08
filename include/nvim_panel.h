/* nvim_panel.h */
#pragma once
#include "trixie.h" /* for Config, OverlayCfg */
#include <stdbool.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

typedef struct {
  int  sev; /* NvDiagSev cast to int (1=error,2=warn,3=info,4=hint) */
  int  line, col;
  char file[256];
  char source[64];
  char msg[256];
} LspDiagSnapshot;

/* Connection lifecycle */
bool nvim_connect(const char *socket_path);
void nvim_disconnect(void);
bool nvim_is_connected(void);

/* File operations */
void nvim_open_file(const char *path, int line);

/* Frame tick — call each compositor frame when overlay is visible */
void nvim_panel_poll(const OverlayCfg *ov_cfg);

void nvim_retry_reset(void);

/* Draw */
void draw_panel_nvim(uint32_t         *px,
                     int               stride,
                     int               px0,
                     int               py0,
                     int               pw,
                     int               ph,
                     int              *cursor,
                     const Config     *cfg,
                     const OverlayCfg *ov_cfg);

/* Key handler */
bool nvim_panel_key(int *cursor, xkb_keysym_t sym, const OverlayCfg *ov_cfg);

/* Diagnostic accessors used by lsp_panel and overlay tab bar */
int  nvim_error_count(void);
int  nvim_warn_count(void);
int  nvim_diag_count(void);
void nvim_diag_get(int   idx,
                   int  *sev,
                   char *file,
                   int   fmax,
                   int  *line,
                   int  *col,
                   char *source,
                   int   smax,
                   char *msg,
                   int   mmax);
