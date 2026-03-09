/* files_panel.c — File browser panel for the Trixie overlay.
 *
 *   ┌─ cwd ───────────────────────┬─ preview ──────────────────────────────┐
 *   │  📁 ..                      │  README.md                             │
 *   │  📁 src/                    │  ─────────────────────────────────────  │
 *   │ ▶ 📄 overlay.c     38K      │  # Trixie                              │
 *   │  📄 run_panel.c    12K      │  Wayland compositor with TUI overlay   │
 *   │  📄 trixie.h       29K      │  …                                     │
 *   │  📁 build/                  │                                         │
 *   └─────────────────────────────┴─────────────────────────────────────────┘
 *
 * Keys (list focus):
 *   j / k        navigate
 *   Enter        cd into dir / open file in $EDITOR
 *   o            open with xdg-open (images, PDFs, etc.)
 *   l / →        enter directory / open
 *   h / ←        go up (..)
 *   /            filter by name
 *   Esc          clear filter / cancel
 *   .            toggle show hidden files
 *   r            refresh
 *   Tab          focus preview pane (scroll with j/k)
 *
 * Preview pane (Tab focus):
 *   j / k        scroll
 *   Tab / Esc    back to list
 */

#define _POSIX_C_SOURCE 200809L
#include "overlay_internal.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Types
 * ═══════════════════════════════════════════════════════════════════════════ */

#define FB_MAX_ENTRIES      1024
#define FB_NAME_MAX         256
#define FB_PATH_MAX         1024
#define FB_PREVIEW_LINES    256
#define FB_PREVIEW_LINE_MAX 512

typedef enum { FB_DIR = 0, FB_FILE, FB_LINK } FbKind;

typedef struct {
  char   name[FB_NAME_MAX];
  FbKind kind;
  off_t  size;
  bool   hidden;     /* name starts with '.' */
  bool   executable; /* has execute bit */
} FbEntry;

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  State
 * ═══════════════════════════════════════════════════════════════════════════ */

static FbEntry g_fb_entries[FB_MAX_ENTRIES];
static int     g_fb_count       = 0;
static bool    g_fb_show_hidden = false;
static bool    g_fb_dirty       = true;

/* Preview buffer — loaded when selection changes */
static char g_fb_preview[FB_PREVIEW_LINES][FB_PREVIEW_LINE_MAX];
static int  g_fb_preview_count  = 0;
static int  g_fb_preview_scroll = 0;
static bool g_fb_preview_focus  = false;
static char g_fb_preview_for[FB_PATH_MAX]; /* path of last loaded preview */

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static int fb_entry_cmp(const void *a, const void *b) {
  const FbEntry *ea = (const FbEntry *)a;
  const FbEntry *eb = (const FbEntry *)b;
  /* Dirs before files */
  if(ea->kind != eb->kind) return ea->kind - eb->kind;
  return strcasecmp(ea->name, eb->name);
}

/* Human-readable file size: "12K", "3.2M", "1.1G" */
static void fmt_size(off_t sz, char *buf, int bufsz) {
  if(sz >= 1024 * 1024 * 1024)
    snprintf(buf, bufsz, "%.1fG", (double)sz / (1024.0 * 1024.0 * 1024.0));
  else if(sz >= 1024 * 1024)
    snprintf(buf, bufsz, "%.1fM", (double)sz / (1024.0 * 1024.0));
  else if(sz >= 1024)
    snprintf(buf, bufsz, "%ldK", (long)(sz / 1024));
  else
    snprintf(buf, bufsz, "%ldb", (long)sz);
}

/* Returns true if extension looks like a text file we can preview */
static bool is_text_file(const char *name) {
  static const char *exts[] = {
    ".c",         ".h",          ".cc",         ".cpp",     ".cxx",     ".hh",
    ".hpp",       ".rs",         ".go",         ".py",      ".rb",      ".js",
    ".ts",        ".jsx",        ".tsx",        ".java",    ".kt",      ".swift",
    ".m",         ".mm",         ".sh",         ".bash",    ".zsh",     ".fish",
    ".md",        ".txt",        ".rst",        ".adoc",    ".toml",    ".yaml",
    ".yml",       ".json",       ".jsonc",      ".xml",     ".html",    ".css",
    ".scss",      ".less",       ".lua",        ".vim",     ".el",      ".lisp",
    ".mk",        ".cmake",      ".ninja",      ".env",     ".conf",    ".cfg",
    ".ini",       ".rc",         ".sql",        ".graphql", "Makefile", "makefile",
    "Dockerfile", "Vagrantfile", "meson.build", "BUILD",    "BUCK",     NULL
  };
  const char *dot = strrchr(name, '.');
  /* Check exact filenames first */
  for(int i = 0; exts[i]; i++) {
    if(exts[i][0] != '.') {
      if(!strcmp(name, exts[i])) return true;
    }
  }
  if(!dot) return false;
  for(int i = 0; exts[i]; i++) {
    if(exts[i][0] == '.' && !strcasecmp(dot, exts[i])) return true;
  }
  return false;
}

/* Icon for a file entry */
static const char *fb_icon(const FbEntry *e) {
  if(e->kind == FB_DIR) {
    if(!strcmp(e->name, "..")) return " "; /* nf-fa-arrow_up */
    return " ";                            /* nf-fa-folder */
  }
  if(e->kind == FB_LINK) return " "; /* nf-fa-link */

  const char *dot = strrchr(e->name, '.');
  if(dot) {
    if(!strcasecmp(dot, ".c") || !strcasecmp(dot, ".h")) return " "; /* nf-dev-c */
    if(!strcasecmp(dot, ".cc") || !strcasecmp(dot, ".cpp") ||
       !strcasecmp(dot, ".cxx") || !strcasecmp(dot, ".hpp"))
      return " ";                           /* nf-dev-cplusplus */
    if(!strcasecmp(dot, ".rs")) return " "; /* nf-dev-rust */
    if(!strcasecmp(dot, ".go")) return " "; /* nf-dev-go */
    if(!strcasecmp(dot, ".py")) return " "; /* nf-dev-python */
    if(!strcasecmp(dot, ".js") || !strcasecmp(dot, ".mjs"))
      return " ";                           /* nf-dev-javascript */
    if(!strcasecmp(dot, ".ts")) return " "; /* nf-dev-typescript */
    if(!strcasecmp(dot, ".jsx") || !strcasecmp(dot, ".tsx"))
      return " ";                            /* nf-dev-react */
    if(!strcasecmp(dot, ".lua")) return " "; /* nf-seti-lua */
    if(!strcasecmp(dot, ".md") || !strcasecmp(dot, ".mdx"))
      return " "; /* nf-dev-markdown */
    if(!strcasecmp(dot, ".txt") || !strcasecmp(dot, ".rst") ||
       !strcasecmp(dot, ".adoc"))
      return " ";                             /* nf-fa-file_text */
    if(!strcasecmp(dot, ".toml")) return " "; /* nf-seti-config */
    if(!strcasecmp(dot, ".yaml") || !strcasecmp(dot, ".yml"))
      return " "; /* nf-seti-config */
    if(!strcasecmp(dot, ".json") || !strcasecmp(dot, ".jsonc"))
      return " "; /* nf-seti-json */
    if(!strcasecmp(dot, ".sh") || !strcasecmp(dot, ".bash") ||
       !strcasecmp(dot, ".zsh") || !strcasecmp(dot, ".fish"))
      return " "; /* nf-fa-terminal */
    if(!strcasecmp(dot, ".vim") || !strcasecmp(dot, ".nvim"))
      return " "; /* nf-dev-vim */
    if(!strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm"))
      return " "; /* nf-dev-html5 */
    if(!strcasecmp(dot, ".css") || !strcasecmp(dot, ".scss") ||
       !strcasecmp(dot, ".sass"))
      return " "; /* nf-dev-css3 */
    if(!strcasecmp(dot, ".xml") || !strcasecmp(dot, ".svg"))
      return "󰗀 "; /* nf-md-xml */
    if(!strcasecmp(dot, ".png") || !strcasecmp(dot, ".jpg") ||
       !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".gif") ||
       !strcasecmp(dot, ".webp") || !strcasecmp(dot, ".bmp"))
      return " "; /* nf-fa-image */
    if(!strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".flac") ||
       !strcasecmp(dot, ".ogg") || !strcasecmp(dot, ".wav"))
      return " "; /* nf-fa-music */
    if(!strcasecmp(dot, ".mp4") || !strcasecmp(dot, ".mkv") ||
       !strcasecmp(dot, ".webm") || !strcasecmp(dot, ".avi"))
      return " ";                            /* nf-fa-film */
    if(!strcasecmp(dot, ".pdf")) return " "; /* nf-fa-file_pdf */
    if(!strcasecmp(dot, ".zip") || !strcasecmp(dot, ".tar") ||
       !strcasecmp(dot, ".gz") || !strcasecmp(dot, ".xz") ||
       !strcasecmp(dot, ".zst") || !strcasecmp(dot, ".bz2"))
      return " "; /* nf-fa-file_archive */
    if(!strcasecmp(dot, ".env") || !strcasecmp(dot, ".conf") ||
       !strcasecmp(dot, ".cfg") || !strcasecmp(dot, ".ini") ||
       !strcasecmp(dot, ".rc"))
      return " ";                             /* nf-fa-cog */
    if(!strcasecmp(dot, ".lock")) return " "; /* nf-fa-lock */
    if(!strcasecmp(dot, ".log")) return " ";  /* nf-fa-file_text_o */
  }
  /* Special filenames */
  if(!strcasecmp(e->name, "Makefile") || !strcasecmp(e->name, "makefile") ||
     !strcasecmp(e->name, "GNUmakefile"))
    return " "; /* nf-seti-makefile */
  if(!strcasecmp(e->name, "Dockerfile") || !strncasecmp(e->name, "Dockerfile.", 11))
    return " "; /* nf-dev-docker */
  if(!strcasecmp(e->name, "meson.build") || !strcasecmp(e->name, "CMakeLists.txt"))
    return " "; /* nf-fa-cog */
  if(!strcasecmp(e->name, "LICENSE") || !strcasecmp(e->name, "LICENCE"))
    return " "; /* nf-fa-balance_scale */
  if(!strcasecmp(e->name, "README") || !strncasecmp(e->name, "README.", 7))
    return " ";
  if(!strcasecmp(e->name, ".gitignore") || !strcasecmp(e->name, ".gitmodules") ||
     !strcasecmp(e->name, ".gitattributes"))
    return " "; /* nf-dev-git */

  /* Executable? */
  if(e->executable) return " "; /* nf-fa-cog (binary) */

  return " "; /* nf-fa-file_o — generic file */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Directory loading
 * ═══════════════════════════════════════════════════════════════════════════ */

void fb_load_dir(const char *cwd, const char *filter) {
  g_fb_count = 0;

  DIR *d = opendir(cwd);
  if(!d) return;

  /* Always add ".." unless we're at root */
  if(strcmp(cwd, "/") != 0) {
    FbEntry *e = &g_fb_entries[g_fb_count++];
    strncpy(e->name, "..", FB_NAME_MAX - 1);
    e->kind   = FB_DIR;
    e->size   = 0;
    e->hidden = false;
  }

  struct dirent *de;
  while((de = readdir(d)) != NULL && g_fb_count < FB_MAX_ENTRIES) {
    if(!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;

    bool hidden = de->d_name[0] == '.';
    if(hidden && !g_fb_show_hidden) continue;

    /* Apply name filter */
    if(filter && filter[0]) {
      /* Case-insensitive substring match */
      char haystack[FB_NAME_MAX], needle[128];
      strncpy(haystack, de->d_name, FB_NAME_MAX - 1);
      strncpy(needle, filter, 127);
      for(char *p = haystack; *p; p++)
        *p = tolower(*p);
      for(char *p = needle; *p; p++)
        *p = tolower(*p);
      if(!strstr(haystack, needle)) continue;
    }

    FbEntry *e = &g_fb_entries[g_fb_count];
    strncpy(e->name, de->d_name, FB_NAME_MAX - 1);
    e->hidden = hidden;

    /* Stat to get kind and size */
    char full[FB_PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s", cwd, de->d_name);
    struct stat st;
    if(lstat(full, &st) == 0) {
      if(S_ISLNK(st.st_mode))
        e->kind = FB_LINK;
      else if(S_ISDIR(st.st_mode))
        e->kind = FB_DIR;
      else
        e->kind = FB_FILE;
      e->size = st.st_size;
      e->executable =
          (e->kind == FB_FILE) && (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH));
    } else {
      e->kind       = FB_FILE;
      e->size       = 0;
      e->executable = false;
    }
    g_fb_count++;
  }
  closedir(d);

  /* Sort: dirs first, then alphabetical */
  /* Keep ".." pinned at top — sort everything after it */
  int skip = (strcmp(cwd, "/") != 0) ? 1 : 0;
  if(g_fb_count - skip > 1)
    qsort(g_fb_entries + skip, g_fb_count - skip, sizeof(FbEntry), fb_entry_cmp);

  g_fb_dirty = false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  Preview loading
 * ═══════════════════════════════════════════════════════════════════════════ */

static void fb_load_preview(const char *path, const FbEntry *e) {
  if(!strcmp(g_fb_preview_for, path)) return; /* already loaded */

  g_fb_preview_count  = 0;
  g_fb_preview_scroll = 0;
  strncpy(g_fb_preview_for, path, FB_PATH_MAX - 1);

  if(e->kind == FB_DIR) {
    /* List directory contents as preview */
    DIR *d = opendir(path);
    if(!d) return;
    struct dirent *de;
    while((de = readdir(d)) != NULL && g_fb_preview_count < FB_PREVIEW_LINES) {
      if(!strcmp(de->d_name, ".")) continue;
      strncpy(
          g_fb_preview[g_fb_preview_count++], de->d_name, FB_PREVIEW_LINE_MAX - 1);
    }
    closedir(d);
    return;
  }

  if(!is_text_file(e->name)) {
    /* Binary file — show size and type */
    char szbuf[32];
    fmt_size(e->size, szbuf, sizeof(szbuf));
    snprintf(g_fb_preview[0], FB_PREVIEW_LINE_MAX, "[binary file — %s]", szbuf);
    g_fb_preview_count = 1;
    return;
  }

  /* Text file — read up to FB_PREVIEW_LINES lines */
  FILE *f = fopen(path, "r");
  if(!f) {
    snprintf(
        g_fb_preview[0], FB_PREVIEW_LINE_MAX, "[cannot open: %s]", strerror(errno));
    g_fb_preview_count = 1;
    return;
  }
  char buf[FB_PREVIEW_LINE_MAX];
  while(fgets(buf, sizeof(buf), f) && g_fb_preview_count < FB_PREVIEW_LINES) {
    /* Strip trailing newline */
    int len = strlen(buf);
    if(len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
    if(len > 0 && buf[len - 1] == '\r') buf[--len] = '\0';
    /* Replace tabs with spaces */
    for(char *p = buf; *p; p++)
      if(*p == '\t') *p = ' ';
    strncpy(g_fb_preview[g_fb_preview_count++], buf, FB_PREVIEW_LINE_MAX - 1);
  }
  fclose(f);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  Init / refresh
 * ═══════════════════════════════════════════════════════════════════════════ */

void fb_init(char *cwd_buf, size_t cwd_bufsz) {
  if(cwd_buf[0] == '\0') {
    if(!getcwd(cwd_buf, cwd_bufsz)) {
      strncpy(cwd_buf, "/", cwd_bufsz - 1);
    }
  }
  g_fb_dirty          = true;
  g_fb_preview_for[0] = '\0';
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  Key handler
 * ═══════════════════════════════════════════════════════════════════════════ */

bool files_panel_key(int              *cursor,
                     char             *cwd,
                     int               cwd_bufsz,
                     char             *filter,
                     int              *filter_len,
                     bool             *filter_mode,
                     xkb_keysym_t      sym,
                     const OverlayCfg *ov_cfg) {
  /* Filter input mode */
  if(*filter_mode) {
    if(sym == XKB_KEY_Escape || sym == XKB_KEY_Return) {
      *filter_mode = false;
      g_fb_dirty   = true;
      *cursor      = 0;
      return true;
    }
    if(sym == XKB_KEY_BackSpace) {
      if(*filter_len > 0) {
        filter[--(*filter_len)] = '\0';
        g_fb_dirty              = true;
      }
      return true;
    }
    if(sym >= 0x20 && sym < 0x7f && *filter_len < 127) {
      filter[(*filter_len)++] = (char)sym;
      filter[*filter_len]     = '\0';
      g_fb_dirty              = true;
      *cursor                 = 0;
    }
    return true;
  }

  /* Preview pane focus */
  if(g_fb_preview_focus) {
    if(sym == XKB_KEY_Tab || sym == XKB_KEY_Escape || sym == XKB_KEY_h ||
       sym == XKB_KEY_Left) {
      g_fb_preview_focus = false;
      return true;
    }
    if(sym == XKB_KEY_j || sym == XKB_KEY_Down) {
      g_fb_preview_scroll++;
      return true;
    }
    if(sym == XKB_KEY_k || sym == XKB_KEY_Up) {
      if(g_fb_preview_scroll > 0) g_fb_preview_scroll--;
      return true;
    }
    if(sym == XKB_KEY_g) {
      g_fb_preview_scroll = 0;
      return true;
    }
    if(sym == XKB_KEY_G) {
      g_fb_preview_scroll = g_fb_preview_count;
      return true;
    }
    return true;
  }

  /* Tab → preview pane */
  if(sym == XKB_KEY_Tab) {
    g_fb_preview_focus = true;
    return true;
  }

  /* Navigate list */
  if(sym == XKB_KEY_j || sym == XKB_KEY_Down) {
    (*cursor)++;
    return true;
  }
  if(sym == XKB_KEY_k || sym == XKB_KEY_Up) {
    if(*cursor > 0) (*cursor)--;
    return true;
  }

  /* Refresh */
  if(sym == XKB_KEY_r) {
    g_fb_dirty          = true;
    g_fb_preview_for[0] = '\0';
    return true;
  }

  /* Toggle hidden */
  if(sym == XKB_KEY_period) {
    g_fb_show_hidden = !g_fb_show_hidden;
    g_fb_dirty       = true;
    *cursor          = 0;
    return true;
  }

  /* Filter */
  if(sym == XKB_KEY_slash) {
    *filter_mode = true;
    return true;
  }
  if(sym == XKB_KEY_Escape) {
    if(filter[0]) {
      filter[0]   = '\0';
      *filter_len = 0;
      g_fb_dirty  = true;
      *cursor     = 0;
    }
    return true;
  }

  /* Go up (h / Left / Backspace) */
  if(sym == XKB_KEY_h || sym == XKB_KEY_Left || sym == XKB_KEY_BackSpace) {
    /* Go up one directory */
    char *last = strrchr(cwd, '/');
    if(last && last != cwd) {
      *last = '\0';
    } else if(last == cwd && cwd[1] != '\0') {
      cwd[1] = '\0'; /* now "/" */
    }
    g_fb_dirty          = true;
    g_fb_preview_for[0] = '\0';
    *cursor             = 0;
    return true;
  }

  /* Enter dir / open file */
  if(sym == XKB_KEY_Return || sym == XKB_KEY_l || sym == XKB_KEY_Right) {
    if(*cursor < 0 || *cursor >= g_fb_count) return true;
    FbEntry *e = &g_fb_entries[*cursor];
    if(e->kind == FB_DIR) {
      char newpath[FB_PATH_MAX];
      if(!strcmp(e->name, "..")) {
        char *last = strrchr(cwd, '/');
        if(last && last != cwd)
          *last = '\0';
        else if(last == cwd)
          cwd[1] = '\0';
      } else {
        snprintf(newpath, sizeof(newpath), "%s/%s", cwd, e->name);
        strncpy(cwd, newpath, cwd_bufsz - 1);
      }
      g_fb_dirty          = true;
      g_fb_preview_for[0] = '\0';
      *cursor             = 0;
    } else {
      /* Open text file in $EDITOR via spawn */
      char fullpath[FB_PATH_MAX];
      snprintf(fullpath, sizeof(fullpath), "%s/%s", cwd, e->name);
      overlay_open_file(fullpath, 0, ov_cfg);
    }
    return true;
  }

  /* xdg-open */
  if(sym == XKB_KEY_o) {
    if(*cursor >= 0 && *cursor < g_fb_count) {
      FbEntry *e = &g_fb_entries[*cursor];
      char     fullpath[FB_PATH_MAX];
      snprintf(fullpath, sizeof(fullpath), "%s/%s", cwd, e->name);
      char cmd[FB_PATH_MAX + 32];
      snprintf(cmd, sizeof(cmd), "xdg-open \"%s\"", fullpath);
      pid_t pid = fork();
      if(pid == 0) {
        setsid();
        int maxfd = (int)sysconf(_SC_OPEN_MAX);
        for(int fd = 3; fd < maxfd; fd++)
          close(fd);
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(1);
      }
    }
    return true;
  }

  return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  Draw
 * ═══════════════════════════════════════════════════════════════════════════ */

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
                      const Config *cfg) {
  Color ac  = cfg->colors.active_border;
  Color bg  = cfg->colors.pane_bg;
  int   bot = py0 + ph - ROW_H - PAD; /* above mode line */
  int   y   = py0 + HEADER_H + PAD;

  /* Init cwd on first draw */
  if(cwd[0] == '\0') fb_init(cwd, cwd_bufsz);

  /* Reload if dirty */
  if(g_fb_dirty) fb_load_dir(cwd, filter);

  /* ── Breadcrumb + filter bar ── */
  {
    /* Truncate cwd from the left if too wide */
    int  avail_cwd = pw * 55 / 100 - PAD * 2;
    char crumb[FB_PATH_MAX];
    strncpy(crumb, cwd, sizeof(crumb) - 1);
    while(ov_measure(crumb) > avail_cwd && strlen(crumb) > 4) {
      /* Remove first path component: "/a/b/c" → "/b/c" */
      char *slash = strchr(crumb + 1, '/');
      if(slash)
        memmove(crumb, slash, strlen(slash) + 1);
      else
        break;
    }
    /* Prepend "…/" if we trimmed */
    if(strcmp(crumb, cwd) != 0) {
      char tmp[FB_PATH_MAX];
      snprintf(tmp, sizeof(tmp), "…%s", crumb);
      strncpy(crumb, tmp, sizeof(crumb) - 1);
    }

    ov_draw_text(px,
                 stride,
                 px0 + PAD,
                 y + g_ov_asc,
                 stride,
                 bot,
                 crumb,
                 ac.r,
                 ac.g,
                 ac.b,
                 0xd0);

    /* Filter badge right of breadcrumb */
    if(filter[0] || *filter_mode) {
      char fbuf[160];
      snprintf(fbuf, sizeof(fbuf), *filter_mode ? "/ %s▌" : "/ %s", filter);
      int fx = px0 + PAD + ov_measure(crumb) + COL_GAP;
      ov_draw_text(
          px, stride, fx, y + g_ov_asc, stride, bot, fbuf, 0xf9, 0xe2, 0xaf, 0xff);
    } else {
      const char *hint = g_fb_show_hidden ? "  · hidden" : "";
      if(hint[0]) {
        int hx = px0 + PAD + ov_measure(crumb) + COL_GAP;
        ov_draw_text(
            px, stride, hx, y + g_ov_asc, stride, bot, hint, 0x45, 0x47, 0x5a, 0xff);
      }
    }

    /* Entry count right-aligned */
    char cnt[32];
    snprintf(cnt, sizeof(cnt), "%d entries", g_fb_count);
    int cw_ = ov_measure(cnt);
    ov_draw_text(px,
                 stride,
                 px0 + pw * 55 / 100 - PAD - cw_,
                 y + g_ov_asc,
                 stride,
                 bot,
                 cnt,
                 0x45,
                 0x47,
                 0x5a,
                 0xff);

    y += ROW_H;
  }

  /* Thin separator */
  ov_fill_rect(px,
               stride,
               px0 + PAD,
               y,
               pw - PAD * 2,
               1,
               ac.r,
               ac.g,
               ac.b,
               0x30,
               stride,
               bot);
  y += SECTION_GAP;

  /* ── Two-pane split: 55% list | 45% preview ── */
  int split   = pw * 55 / 100;
  int list_x0 = px0;
  int list_w  = split;
  int prev_x0 = px0 + split + PAD;
  int prev_w  = pw - split - PAD * 2;

  /* Vertical divider */
  ov_fill_rect(
      px, stride, px0 + split, y, 1, bot - y, ac.r, ac.g, ac.b, 0x25, stride, bot);

  /* ── File list ── */
  int visible_rows = (bot - y) / ROW_H;
  if(visible_rows < 1) return;

  /* Clamp cursor */
  if(*cursor >= g_fb_count && g_fb_count > 0) *cursor = g_fb_count - 1;
  if(*cursor < 0) *cursor = 0;

  int scroll = *cursor - visible_rows + 2;
  if(scroll < 0) scroll = 0;
  if(scroll > g_fb_count - visible_rows) scroll = g_fb_count - visible_rows;
  if(scroll < 0) scroll = 0;

  /* Column positions within the list pane */
  int icon_x = list_x0 + PAD;
  int name_x =
      icon_x + ov_measure(" ") + 4; /* all nerd font icons are ~1 glyph wide */
  int size_x = list_x0 + list_w - PAD - 48; /* right-aligned size */

  for(int i = 0; i < visible_rows; i++) {
    int idx = i + scroll;
    if(idx >= g_fb_count) break;

    FbEntry *e   = &g_fb_entries[idx];
    bool     sel = (idx == *cursor) && !g_fb_preview_focus;
    int      ry  = y + i * ROW_H;

    if(sel) draw_cursor_line(px, stride, list_x0, ry, list_w, ac, bg, stride, bot);

    /* Focus indicator for preview pane focus */
    if(idx == *cursor && g_fb_preview_focus) {
      ov_fill_rect(px,
                   stride,
                   list_x0 + PAD,
                   ry,
                   2,
                   ROW_H - 1,
                   ac.r,
                   ac.g,
                   ac.b,
                   0x60,
                   stride,
                   bot);
    }

    /* Icon */
    uint8_t ir = e->kind == FB_DIR ? ac.r : (e->hidden ? 0x45 : 0x89);
    uint8_t ig = e->kind == FB_DIR ? ac.g : (e->hidden ? 0x47 : 0xdc);
    uint8_t ib = e->kind == FB_DIR ? ac.b : (e->hidden ? 0x5a : 0xeb);
    ov_draw_text(px,
                 stride,
                 icon_x,
                 ry + g_ov_asc + 2,
                 stride,
                 bot,
                 fb_icon(e),
                 ir,
                 ig,
                 ib,
                 0xff);

    /* Name — dim if hidden */
    uint8_t nr = sel ? ac.r : (e->hidden ? 0x45 : (e->kind == FB_DIR ? 0xcd : 0xa6));
    uint8_t ng = sel ? ac.g : (e->hidden ? 0x47 : (e->kind == FB_DIR ? 0xd6 : 0xad));
    uint8_t nb = sel ? ac.b : (e->hidden ? 0x5a : (e->kind == FB_DIR ? 0xf4 : 0xc8));
    char    trunc_name[FB_NAME_MAX];
    strncpy(trunc_name, e->name, sizeof(trunc_name) - 1);
    int avail_name = size_x - name_x - COL_GAP;
    while(ov_measure(trunc_name) > avail_name && strlen(trunc_name) > 3)
      trunc_name[strlen(trunc_name) - 1] = '\0';
    ov_draw_text(px,
                 stride,
                 name_x,
                 ry + g_ov_asc + 2,
                 stride,
                 bot,
                 trunc_name,
                 nr,
                 ng,
                 nb,
                 e->hidden ? 0xa0 : 0xff);

    /* Size for files */
    if(e->kind == FB_FILE) {
      char szbuf[16];
      fmt_size(e->size, szbuf, sizeof(szbuf));
      int sw = ov_measure(szbuf);
      ov_draw_text(px,
                   stride,
                   size_x + 48 - sw,
                   ry + g_ov_asc + 2,
                   stride,
                   bot,
                   szbuf,
                   0x45,
                   0x47,
                   0x5a,
                   0xff);
    } else if(e->kind == FB_DIR) {
      ov_draw_text(px,
                   stride,
                   size_x,
                   ry + g_ov_asc + 2,
                   stride,
                   bot,
                   "/",
                   0x45,
                   0x47,
                   0x5a,
                   0x60);
    }
  }

  /* Scrollbar hint */
  if(g_fb_count > visible_rows) {
    char sb[16];
    snprintf(sb, sizeof(sb), "%d/%d", *cursor + 1, g_fb_count);
    int sbw = ov_measure(sb);
    ov_draw_text(px,
                 stride,
                 list_x0 + list_w - PAD - sbw,
                 bot - g_ov_th,
                 stride,
                 bot,
                 sb,
                 0x45,
                 0x47,
                 0x5a,
                 0xff);
  }

  /* ── Preview pane ── */
  {
    /* Header */
    const char *prev_title = "preview";
    if(*cursor >= 0 && *cursor < g_fb_count) prev_title = g_fb_entries[*cursor].name;

    uint8_t pr = g_fb_preview_focus ? 0xf9 : ac.r;
    uint8_t pg = g_fb_preview_focus ? 0xe2 : ac.g;
    uint8_t pb = g_fb_preview_focus ? 0xaf : ac.b;

    char ptrunc[FB_NAME_MAX];
    strncpy(ptrunc, prev_title, sizeof(ptrunc) - 1);
    while(ov_measure(ptrunc) > prev_w - 4 && strlen(ptrunc) > 3)
      ptrunc[strlen(ptrunc) - 1] = '\0';
    ov_draw_text(px,
                 stride,
                 prev_x0,
                 y - SECTION_GAP + 2 - ROW_H + g_ov_asc,
                 stride,
                 bot,
                 ptrunc,
                 pr,
                 pg,
                 pb,
                 0xd0);

    /* Load preview if needed */
    if(*cursor >= 0 && *cursor < g_fb_count) {
      FbEntry *e = &g_fb_entries[*cursor];
      char     fullpath[FB_PATH_MAX];
      snprintf(fullpath, sizeof(fullpath), "%s/%s", cwd, e->name);
      fb_load_preview(fullpath, e);
    }

    /* Clamp scroll */
    int pvis       = (bot - y) / ROW_H;
    int max_scroll = g_fb_preview_count - pvis;
    if(max_scroll < 0) max_scroll = 0;
    if(g_fb_preview_scroll > max_scroll) g_fb_preview_scroll = max_scroll;
    if(g_fb_preview_scroll < 0) g_fb_preview_scroll = 0;

    /* Content background tint */
    ov_fill_rect(px,
                 stride,
                 prev_x0 - PAD / 2,
                 y,
                 prev_w + PAD,
                 bot - y,
                 0x10,
                 0x10,
                 0x1c,
                 0x60,
                 stride,
                 bot);

    /* Lines */
    for(int i = 0; i < pvis; i++) {
      int li = i + g_fb_preview_scroll;
      if(li >= g_fb_preview_count) break;
      const char *line = g_fb_preview[li];
      int         ry   = y + i * ROW_H;

      /* Syntax coloring hints — very basic */
      uint8_t lr = 0xa6, lg = 0xad, lb = 0xc8;
      if(line[0] == '#') {
        lr = 0x58;
        lg = 0x5b;
        lb = 0x70;
      } /* comments */
      else if(strstr(line, "TODO:") || strstr(line, "FIXME:") ||
              strstr(line, "HACK:")) {
        lr = 0xf9;
        lg = 0xe2;
        lb = 0xaf;
      } /* warnings */
      else if(!strncmp(line, "fn ", 3) || !strncmp(line, "func ", 5) ||
              !strncmp(line, "def ", 4) || !strncmp(line, "void ", 5) ||
              !strncmp(line, "int ", 4) || !strncmp(line, "static ", 7)) {
        lr = 0x89;
        lg = 0xdc;
        lb = 0xeb;
      } /* function decls */

      char trunc[FB_PREVIEW_LINE_MAX];
      strncpy(trunc, line, sizeof(trunc) - 1);
      while(ov_measure(trunc) > prev_w - 4 && strlen(trunc) > 3)
        trunc[strlen(trunc) - 1] = '\0';

      ov_draw_text(px,
                   stride,
                   prev_x0,
                   ry + g_ov_asc + 2,
                   stride,
                   bot,
                   trunc,
                   lr,
                   lg,
                   lb,
                   0xff);
    }

    /* Scroll indicator */
    if(g_fb_preview_scroll > 0 || g_fb_preview_count > pvis) {
      char ps[24];
      snprintf(ps, sizeof(ps), "↑ ln %d", g_fb_preview_scroll + 1);
      ov_draw_text(px,
                   stride,
                   px0 + pw - PAD - ov_measure(ps),
                   bot - g_ov_th,
                   stride,
                   bot,
                   ps,
                   0x89,
                   0xdc,
                   0xeb,
                   0x80);
    }
  }
}
