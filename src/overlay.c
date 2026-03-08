/* overlay.c — Ratatui-styled TUI dev overlay for Trixie.
 *
 * Ten panels (Tab / 1-9,0):
 *
 *   [1] Workspace map   — minimap grid + layout/ratio per workspace
 *   [2] Command palette — all keybinds + IPC commands, fuzzy search
 *   [3] Process list    — top-N CPU/RSS; Enter=SIGTERM, K=SIGKILL,
 *                         s=sort-cycle (CPU/RSS/PID), sparkline bars
 *   [4] Log viewer      — wlr_log ring; colour-coded; / regex filter; c=clear
 *   [5] Git             — branch/status; d=inline diff; s/u=stage/unstage;
 *                         split view: status left, diff right; r=refresh
 *   [6] Build           — async pthread; auto-detects Meson/Cargo/go/Maven/
 *                         Gradle/Make; error list (e=toggle); Enter=jump
 *                         to error in $EDITOR; C/Rust/Go/Java parsers
 *   [7] Notes           — multi-file (8), Markdown render, [/]=switch,
 *                         n=rename, N=new, d=delete, e=edit
 *   [8] Search          — ripgrep/grep file search; live results; Enter=open
 *                         in $EDITOR at match line; f=toggle file-only mode
 *   [9] Run             — named run configs (per-lang presets + custom);
 *                         async process per config; Enter=start/stop;
 *                         stdout tail in log ring; a=add, d=delete config
 *   [0] Deps            — language-aware dependency viewer: Cargo.toml,
 *                         go.mod, pom.xml, build.gradle; u=check outdated
 *
 * Global keys
 * ───────────
 *   Tab / 1-9,0  switch panel
 *   j / Down     cursor down / scroll
 *   k / Up       cursor up / scroll
 *   Enter        execute / confirm / open
 *   Escape / `   dismiss or exit sub-mode
 *   /            search / filter mode
 *   Backspace    delete char in input mode
 */

#define _POSIX_C_SOURCE 200809L
#include "overlay_internal.h"
#include "run_panel.h"
#include "files_panel.h"
#include "nvim_panel.h"
#include "lsp_panel.h"
#include <dirent.h>
#include <drm_fourcc.h>
#include <ft2build.h>
#include <unistd.h>
#include FT_FREETYPE_H
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <xkbcommon/xkbcommon.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * §0  Log ring
 * ═══════════════════════════════════════════════════════════════════════════ */

LogRing g_log_ring;

void log_ring_push(const char *line) {
  strncpy(g_log_ring.lines[g_log_ring.head], line, LOG_LINE_MAX - 1);
  g_log_ring.lines[g_log_ring.head][LOG_LINE_MAX - 1] = '\0';
  g_log_ring.head = (g_log_ring.head + 1) % LOG_RING_SIZE;
  if(g_log_ring.count < LOG_RING_SIZE) g_log_ring.count++;
}

const char *log_ring_get(int idx) {
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
 * §1  Process list — sparklines, sort, kill
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PROC_MAX  48
#define PROC_HIST 16 /* sparkline history depth */

typedef enum { PROC_SORT_CPU = 0, PROC_SORT_RSS, PROC_SORT_PID } ProcSort;
static ProcSort g_proc_sort = PROC_SORT_CPU;

typedef struct {
  pid_t     pid;
  char      comm[32];
  float     cpu_pct;
  long      rss_kb;
  float     cpu_hist[PROC_HIST]; /* ring of recent samples */
  int       hist_head;
  int       hist_count;
  /* raw jiffies from last sample for delta */
  long long prev_total_jiff;
  long long prev_proc_jiff;
} ProcEntry;

static ProcEntry g_procs[PROC_MAX];
static int       g_proc_count   = 0;
static int64_t   g_proc_next_ms = 0;
static long      g_clk_tck      = 0;

int64_t ov_now_ms(void) {
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

  /* Keep old entries for history merging */
  ProcEntry old[PROC_MAX];
  int       old_count = g_proc_count;
  memcpy(old, g_procs, sizeof(ProcEntry) * (size_t)old_count);

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

    long long proc_jiff  = utime + stime;
    /* Find previous entry for delta */
    long long prev_total = 0, prev_proc = 0;
    for(int oi = 0; oi < old_count; oi++) {
      if(old[oi].pid == pid) {
        prev_total = old[oi].prev_total_jiff;
        prev_proc  = old[oi].prev_proc_jiff;
        /* carry history */
        memcpy(pe.cpu_hist, old[oi].cpu_hist, sizeof(pe.cpu_hist));
        pe.hist_head  = old[oi].hist_head;
        pe.hist_count = old[oi].hist_count;
        break;
      }
    }
    pe.prev_total_jiff = cpu_total;
    pe.prev_proc_jiff  = proc_jiff;

    long long dtotal = cpu_total - prev_total;
    long long dproc  = proc_jiff - prev_proc;
    pe.cpu_pct       = (dtotal > 0) ? (float)dproc * 100.f / (float)dtotal : 0.f;
    if(pe.cpu_pct < 0.f) pe.cpu_pct = 0.f;

    /* Push to sparkline history */
    pe.cpu_hist[pe.hist_head] = pe.cpu_pct;
    pe.hist_head              = (pe.hist_head + 1) % PROC_HIST;
    if(pe.hist_count < PROC_HIST) pe.hist_count++;

    pe.rss_kb               = rss * (long)(sysconf(_SC_PAGESIZE) / 1024);
    g_procs[g_proc_count++] = pe;
  }
  closedir(d);

  /* Sort */
  for(int i = 1; i < g_proc_count; i++) {
    ProcEntry key    = g_procs[i];
    int       j      = i - 1;
    bool      before = false;
    while(j >= 0) {
      switch(g_proc_sort) {
        case PROC_SORT_CPU: before = g_procs[j].cpu_pct < key.cpu_pct; break;
        case PROC_SORT_RSS: before = g_procs[j].rss_kb < key.rss_kb; break;
        case PROC_SORT_PID: before = g_procs[j].pid > key.pid; break;
      }
      if(!before) break;
      g_procs[j + 1] = g_procs[j];
      j--;
    }
    g_procs[j + 1] = key;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Git state — status, diff, stage/unstage
 * ═══════════════════════════════════════════════════════════════════════════ */

#define GIT_LINE_MAX  128
#define GIT_LINES_MAX 64
#define GIT_DIFF_MAX  256
#define GIT_FILE_MAX  32

typedef struct {
  char xy[3]; /* two-char status code e.g. "M ", " M", "??" */
  char path[GIT_LINE_MAX];
  bool staged; /* index modified */
} GitFile;

typedef struct {
  char    branch[GIT_LINE_MAX];
  char    lines[GIT_LINES_MAX][GIT_LINE_MAX]; /* log lines */
  int     line_count;
  GitFile files[GIT_FILE_MAX];
  int     file_count;
  char    diff[GIT_DIFF_MAX][GIT_LINE_MAX]; /* diff of selected file */
  int     diff_count;
  int     diff_for; /* file index the diff belongs to (-1 = none) */
  bool    show_diff;
  int64_t fetched_ms;
  bool    valid;
  char    root[1024]; /* git root used for this snapshot — invalidate on change */
} GitState;

static GitState g_git;

/* Set by overlay_set_cwd() and initialised in overlay_create().
 * git panel always runs against the git root of this directory. */
static char g_git_cwd[1024] = { 0 };

static int run_capture(const char *cmd, char out[][GIT_LINE_MAX], int max_lines) {
  FILE *f = popen(cmd, "r");
  if(!f) return 0;
  int n = 0;
  while(n < max_lines) {
    if(!fgets(out[n], GIT_LINE_MAX, f)) break;
    size_t l = strlen(out[n]);
    while(l > 0 && (out[n][l - 1] == '\n' || out[n][l - 1] == '\r'))
      out[n][--l] = '\0';
    n++;
  }
  pclose(f);
  return n;
}

static void git_load_diff(int file_idx) {
  if(file_idx < 0 || file_idx >= g_git.file_count) return;
  if(g_git.diff_for == file_idx && g_git.diff_count > 0) return; /* cached */

  GitFile *gf = &g_git.files[file_idx];
  char     cmd[512];
  /* staged diff vs HEAD, or working-tree diff */
  if(gf->staged)
    snprintf(cmd, sizeof(cmd), "git diff --cached -- '%s' 2>/dev/null", gf->path);
  else
    snprintf(cmd, sizeof(cmd), "git diff -- '%s' 2>/dev/null", gf->path);

  g_git.diff_count = run_capture(cmd, g_git.diff, GIT_DIFF_MAX);
  g_git.diff_for   = file_idx;
}

/* Resolve the git root for a given directory. Fills `out` (size outsz).
 * Returns true on success. */
static bool git_find_root(const char *dir, char *out, size_t outsz) {
  char old_cwd[1024] = { 0 };
  getcwd(old_cwd, sizeof(old_cwd));
  if(chdir(dir) != 0) return false;
  FILE *f = popen("git rev-parse --show-toplevel 2>/dev/null", "r");
  bool ok = false;
  if(f) {
    if(fgets(out, (int)outsz, f)) {
      size_t l = strlen(out);
      while(l > 0 && (out[l-1] == '\n' || out[l-1] == '\r')) out[--l] = '\0';
      ok = l > 0;
    }
    pclose(f);
  }
  if(old_cwd[0]) chdir(old_cwd);
  return ok;
}

static void refresh_git(void) {
  int64_t now = ov_now_ms();

  /* Resolve current git root from g_git_cwd */
  char new_root[1024] = { 0 };
  const char *search_dir = g_git_cwd[0] ? g_git_cwd : ".";
  git_find_root(search_dir, new_root, sizeof(new_root));

  /* Invalidate cache when root changes or cwd changed under us */
  if(strcmp(new_root, g_git.root) != 0) {
    memset(&g_git, 0, sizeof(g_git));
    g_git.diff_for = -1;
    strncpy(g_git.root, new_root, sizeof(g_git.root) - 1);
  }

  if(g_git.valid && now - g_git.fetched_ms < 5000) return;

  /* Save/restore cwd so git commands run in the right repo */
  char old_cwd[1024] = { 0 };
  getcwd(old_cwd, sizeof(old_cwd));
  if(g_git.root[0]) chdir(g_git.root);

  memset(&g_git.branch,    0, sizeof(g_git.branch));
  memset(&g_git.lines,     0, sizeof(g_git.lines));
  memset(&g_git.files,     0, sizeof(g_git.files));
  g_git.line_count  = 0;
  g_git.file_count  = 0;
  g_git.diff_count  = 0;
  g_git.diff_for    = -1;
  g_git.show_diff   = false;
  /* Branch name */
  {
    FILE *f = popen("git rev-parse --abbrev-ref HEAD 2>/dev/null", "r");
    if(!f) {
      strcpy(g_git.branch, "(not a git repo)");
      if(old_cwd[0]) chdir(old_cwd);
      return;
    }
    if(!fgets(g_git.branch, sizeof(g_git.branch), f)) {
      pclose(f);
      strcpy(g_git.branch, "(not a git repo)");
      if(old_cwd[0]) chdir(old_cwd);
      return;
    }
    pclose(f);
    size_t bl = strlen(g_git.branch);
    while(bl > 0 && (g_git.branch[bl - 1] == '\n' || g_git.branch[bl - 1] == '\r'))
      g_git.branch[--bl] = '\0';
  }

  /* Porcelain status → populate file list */
  {
    char st[GIT_FILE_MAX][GIT_LINE_MAX];
    int  sc = run_capture("git status --porcelain 2>/dev/null", st, GIT_FILE_MAX);
    for(int i = 0; i < sc && g_git.file_count < GIT_FILE_MAX; i++) {
      if(strlen(st[i]) < 4) continue;
      GitFile *gf = &g_git.files[g_git.file_count++];
      gf->xy[0]   = st[i][0];
      gf->xy[1]   = st[i][1];
      gf->xy[2]   = '\0';
      strncpy(gf->path, st[i] + 3, sizeof(gf->path) - 1);
      gf->staged = (st[i][0] != ' ' && st[i][0] != '?');
    }
    if(g_git.file_count == 0) {
      strncpy(g_git.lines[g_git.line_count++], "  (clean)", GIT_LINE_MAX - 1);
    }
  }

  /* Last 10 commits */
  {
    char cl[GIT_LINES_MAX][GIT_LINE_MAX];
    int  cc = run_capture("git log --oneline -10 2>/dev/null", cl, GIT_LINES_MAX);
    if(g_git.line_count < GIT_LINES_MAX)
      strncpy(g_git.lines[g_git.line_count++], "Recent commits:", GIT_LINE_MAX - 1);
    for(int i = 0; i < cc && g_git.line_count < GIT_LINES_MAX; i++) {
      char buf[GIT_LINE_MAX];
      snprintf(buf, sizeof(buf), "  %s", cl[i]);
      strncpy(g_git.lines[g_git.line_count++], buf, GIT_LINE_MAX - 1);
    }
  }

  g_git.fetched_ms = now;
  g_git.valid      = true;

  if(old_cwd[0]) chdir(old_cwd);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Build runner — async pthread, error parser, auto-detect build system
 * ═══════════════════════════════════════════════════════════════════════════ */

#define BUILD_CMD_MAX  256
#define BUILD_ERR_MAX  128 /* max parsed error entries per build */
#define BUILD_ERR_LINE 256 /* max chars in an error message */

typedef struct {
  char file[256];
  int  line; /* 1-based; 0 = no location */
  int  col;
  char msg[BUILD_ERR_LINE];
  bool is_warning;
} BuildError;

typedef struct {
  char            cmd[BUILD_CMD_MAX];
  atomic_bool     running;
  bool            done;
  int             exit_code;
  int64_t         started_ms;
  int64_t         finished_ms;
  /* parsed diagnostics */
  BuildError      errors[BUILD_ERR_MAX];
  int             err_count;
  pthread_mutex_t err_lock;
  pthread_t       thread;
  bool            thread_valid;
} BuildState;

static BuildState g_build;

/* ── Error-line parsers ─────────────────────────────────────────────────
 * Each returns true and fills *out if the line matches the language pattern.
 *
 * C / C++ (gcc/clang):   file:line:col: error|warning: msg
 * Rust (rustc / cargo):  --> file:line:col   OR   error[...]: msg
 * Go:                    file:line:col: msg
 * Java (javac / Maven):  file:line: error: msg  OR  [ERROR] file:[line,col]
 */

static bool parse_error_c(const char *ln, BuildError *out) {
  /* pattern: <file>:<line>:<col>: error|warning: <msg> */
  const char *p = ln;
  char        file[256];
  int         fi = 0;
  /* extract file — stop at ':' followed by digit */
  while(*p && fi < 255) {
    if(*p == ':' && *(p + 1) >= '0' && *(p + 1) <= '9') {
      file[fi] = '\0';
      p++;
      break;
    }
    file[fi++] = *p++;
  }
  if(!fi || !*p) return false;
  int line = 0, col = 0;
  if(sscanf(p, "%d:%d:", &line, &col) < 1) return false;
  /* advance past line:col: */
  while(*p && *p != ':')
    p++;
  if(*p == ':') p++;
  while(*p && *p != ':')
    p++;
  if(*p == ':') p++;
  while(*p == ' ')
    p++;
  bool warn = (strncmp(p, "warning", 7) == 0);
  bool err  = (strncmp(p, "error", 5) == 0) || (strncmp(p, "fatal", 5) == 0);
  if(!warn && !err) return false;
  /* skip to message after next ': ' */
  while(*p && !(*p == ':' && *(p + 1) == ' '))
    p++;
  if(*p) p += 2;
  strncpy(out->file, file, sizeof(out->file) - 1);
  out->line       = line;
  out->col        = col;
  out->is_warning = warn;
  strncpy(out->msg, p, sizeof(out->msg) - 1);
  return true;
}

static bool parse_error_rust(const char *ln, BuildError *out) {
  /* rustc: "  --> src/main.rs:10:5" */
  const char *p = ln;
  while(*p == ' ')
    p++;
  if(strncmp(p, "--> ", 4) == 0) {
    p += 4;
    char file[256];
    int  fi = 0;
    while(*p && *p != ':' && fi < 255)
      file[fi++] = *p++;
    file[fi] = '\0';
    int line = 0, col = 0;
    if(*p == ':') {
      p++;
      sscanf(p, "%d:%d", &line, &col);
    }
    strncpy(out->file, file, sizeof(out->file) - 1);
    out->line       = line;
    out->col        = col;
    out->is_warning = false;
    strncpy(out->msg, "(see above)", sizeof(out->msg) - 1);
    return fi > 0 && line > 0;
  }
  /* cargo: "error[E0xxx]: message" or "warning: message" at start of line */
  if(strncmp(ln, "error", 5) == 0 || strncmp(ln, "warning", 7) == 0) {
    bool        warn  = (ln[0] == 'w');
    const char *colon = strchr(ln, ':');
    if(!colon) return false;
    colon++;
    while(*colon == ' ')
      colon++;
    if(!*colon) return false;
    out->file[0]    = '\0';
    out->line       = 0;
    out->col        = 0;
    out->is_warning = warn;
    strncpy(out->msg, colon, sizeof(out->msg) - 1);
    return true;
  }
  return false;
}

static bool parse_error_go(const char *ln, BuildError *out) {
  /* go: "./file.go:10:5: message" */
  const char *p = ln;
  if(*p == '.') p++;
  if(*p == '/') p++;
  char file[256];
  int  fi = 0;
  while(*p && *p != ':' && fi < 255)
    file[fi++] = *p++;
  file[fi] = '\0';
  if(!fi || *p != ':') return false;
  p++;
  int line = 0, col = 0;
  if(sscanf(p, "%d:%d:", &line, &col) < 1) return false;
  if(line <= 0) return false;
  while(*p && *p != ':')
    p++;
  if(*p) p++;
  while(*p && *p != ':')
    p++;
  if(*p) p++;
  while(*p == ' ')
    p++;
  /* basic sanity: file should end in .go */
  if(!strstr(file, ".go")) return false;
  strncpy(out->file, file, sizeof(out->file) - 1);
  out->line       = line;
  out->col        = col;
  out->is_warning = false;
  strncpy(out->msg, p, sizeof(out->msg) - 1);
  return true;
}

static bool parse_error_java(const char *ln, BuildError *out) {
  /* javac: "File.java:10: error: message" */
  const char *p = ln;
  /* Maven [ERROR]: skip prefix */
  if(strncmp(p, "[ERROR] ", 8) == 0)
    p += 8;
  else if(strncmp(p, "[WARNING] ", 10) == 0)
    p += 10;
  char file[256];
  int  fi = 0;
  while(*p && *p != ':' && fi < 255)
    file[fi++] = *p++;
  file[fi] = '\0';
  if(!fi || *p != ':') return false;
  p++;
  int line = 0;
  if(sscanf(p, "%d:", &line) < 1) return false;
  if(line <= 0) return false;
  if(!strstr(file, ".java")) return false;
  while(*p && *p != ':')
    p++;
  if(*p) p++;
  while(*p == ' ')
    p++;
  bool warn = (strncmp(p, "warning", 7) == 0);
  bool err  = (strncmp(p, "error", 5) == 0);
  if(warn || err) {
    while(*p && !(*p == ':' && *(p + 1) == ' '))
      p++;
    if(*p) p += 2;
  }
  strncpy(out->file, file, sizeof(out->file) - 1);
  out->line       = line;
  out->col        = 0;
  out->is_warning = warn;
  strncpy(out->msg, p, sizeof(out->msg) - 1);
  return true;
}

static void build_try_parse_error(const char *ln) {
  BuildError e  = { 0 };
  bool       ok = parse_error_c(ln, &e) || parse_error_rust(ln, &e) ||
            parse_error_go(ln, &e) || parse_error_java(ln, &e);
  if(!ok) return;
  pthread_mutex_lock(&g_build.err_lock);
  if(g_build.err_count < BUILD_ERR_MAX) g_build.errors[g_build.err_count++] = e;
  pthread_mutex_unlock(&g_build.err_lock);
}

/* ── Auto-detect build system ────────────────────────────────────────── */
static void build_autodetect(char *cmd_out, size_t sz) {
  struct stat st;
  if(stat("build.ninja", &st) == 0 || stat("meson.build", &st) == 0) {
    snprintf(cmd_out, sz, "meson compile -C builddir 2>&1");
    return;
  }
  if(stat("Cargo.toml", &st) == 0) {
    snprintf(cmd_out, sz, "cargo build 2>&1");
    return;
  }
  if(stat("go.mod", &st) == 0) {
    snprintf(cmd_out, sz, "go build ./... 2>&1");
    return;
  }
  if(stat("pom.xml", &st) == 0) {
    snprintf(cmd_out, sz, "mvn compile -q 2>&1");
    return;
  }
  if(stat("build.gradle", &st) == 0 || stat("build.gradle.kts", &st) == 0) {
    snprintf(cmd_out, sz, "./gradlew build 2>&1");
    return;
  }
  if(stat("Makefile", &st) == 0 || stat("makefile", &st) == 0) {
    snprintf(cmd_out, sz, "make -j$(nproc) 2>&1");
    return;
  }
  /* fallback */
  snprintf(cmd_out, sz, "make -j$(nproc) 2>&1");
}

/* ── Async build thread ──────────────────────────────────────────────── */
static void *build_thread(void *arg) {
  (void)arg;

  char header[LOG_LINE_MAX];
  snprintf(header, sizeof(header), "==> build: %s", g_build.cmd);
  log_ring_push(header);

  FILE *f = popen(g_build.cmd, "r");
  if(!f) {
    log_ring_push("==> build: popen failed");
    atomic_store(&g_build.running, false);
    g_build.done = true;
    return NULL;
  }

  char buf[LOG_LINE_MAX];
  while(fgets(buf, sizeof(buf), f)) {
    size_t l = strlen(buf);
    while(l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r'))
      buf[--l] = '\0';
    log_ring_push(buf);
    build_try_parse_error(buf);
  }

  int status          = pclose(f);
  g_build.exit_code   = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  g_build.finished_ms = ov_now_ms();
  atomic_store(&g_build.running, false);
  g_build.done = true;

  char footer[LOG_LINE_MAX];
  snprintf(footer,
           sizeof(footer),
           "==> build finished: exit %d (%.1fs)  errors: %d",
           g_build.exit_code,
           (double)(g_build.finished_ms - g_build.started_ms) / 1000.0,
           g_build.err_count);
  log_ring_push(footer);
  return NULL;
}

static void run_build(void) {
  if(!g_build.cmd[0]) build_autodetect(g_build.cmd, sizeof(g_build.cmd));
  if(atomic_load(&g_build.running)) return; /* already in progress */

  /* Reset state */
  pthread_mutex_lock(&g_build.err_lock);
  g_build.err_count = 0;
  memset(g_build.errors, 0, sizeof(g_build.errors));
  pthread_mutex_unlock(&g_build.err_lock);

  g_build.done        = false;
  g_build.exit_code   = -1;
  g_build.started_ms  = ov_now_ms();
  g_build.finished_ms = 0;
  atomic_store(&g_build.running, true);

  if(g_build.thread_valid) {
    pthread_join(g_build.thread, NULL);
    g_build.thread_valid = false;
  }
  if(pthread_create(&g_build.thread, NULL, build_thread, NULL) == 0)
    g_build.thread_valid = true;
  else {
    atomic_store(&g_build.running, false);
    log_ring_push("==> build: pthread_create failed");
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Notes — multi-file with Markdown rendering
 * ═══════════════════════════════════════════════════════════════════════════ */

#define NOTES_MAX      8192 /* bytes per note */
#define NOTES_FILES    8    /* max open notes */
#define NOTES_NAME_MAX 64

typedef struct {
  char name[NOTES_NAME_MAX]; /* display name / filename stem */
  char text[NOTES_MAX];
  int  len;
  bool dirty;
} NoteFile;

static NoteFile g_notes_files[NOTES_FILES];
static int      g_notes_count  = 0;
static int      g_notes_active = 0; /* which note is shown */
static bool     g_notes_edit   = false;
static bool     g_notes_rename = false; /* typing a new name */
static char     g_notes_rename_buf[NOTES_NAME_MAX];
static int      g_notes_rename_len = 0;

/* Convenience accessor */
static inline NoteFile *cur_note(void) {
  return (g_notes_count > 0) ? &g_notes_files[g_notes_active] : NULL;
}

static void notes_dir(char *out, size_t sz) {
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if(xdg)
    snprintf(out, sz, "%s/trixie/notes", xdg);
  else {
    const char *home = getenv("HOME");
    snprintf(out, sz, "%s/.config/trixie/notes", home ? home : "/root");
  }
}

/* Legacy single-file path (for migration) */
static void notes_legacy_path(char *out, size_t sz) {
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if(xdg)
    snprintf(out, sz, "%s/trixie/notes.txt", xdg);
  else {
    const char *home = getenv("HOME");
    snprintf(out, sz, "%s/.config/trixie/notes.txt", home ? home : "/root");
  }
}

static void notes_file_path(const NoteFile *nf, char *out, size_t sz) {
  char dir[512];
  notes_dir(dir, sizeof(dir));
  snprintf(out, sz, "%s/%s.md", dir, nf->name);
}

static void notes_save_one(NoteFile *nf) {
  if(!nf->dirty) return;
  char dir[512], path[600];
  notes_dir(dir, sizeof(dir));
  mkdir(dir, 0755);
  notes_file_path(nf, path, sizeof(path));
  FILE *f = fopen(path, "w");
  if(!f) return;
  fwrite(nf->text, 1, (size_t)nf->len, f);
  fclose(f);
  nf->dirty = false;
}

static void notes_save(void) {
  for(int i = 0; i < g_notes_count; i++)
    notes_save_one(&g_notes_files[i]);
}

static void note_add(const char *name, const char *text, int len) {
  if(g_notes_count >= NOTES_FILES) return;
  NoteFile *nf = &g_notes_files[g_notes_count++];
  strncpy(nf->name, name, NOTES_NAME_MAX - 1);
  if(text && len > 0) {
    int copy = len < NOTES_MAX - 1 ? len : NOTES_MAX - 1;
    memcpy(nf->text, text, (size_t)copy);
    nf->len        = copy;
    nf->text[copy] = '\0';
  }
  nf->dirty = false;
}

static void notes_load(void) {
  char dir[512];
  notes_dir(dir, sizeof(dir));
  mkdir(dir, 0755);

  /* Migrate legacy notes.txt if notes dir is empty */
  g_notes_count  = 0;
  g_notes_active = 0;

  DIR *d = opendir(dir);
  if(d) {
    struct dirent *ent;
    while((ent = readdir(d)) != NULL && g_notes_count < NOTES_FILES) {
      const char *nm = ent->d_name;
      size_t      nl = strlen(nm);
      if(nl < 4 || strcmp(nm + nl - 3, ".md") != 0) continue;
      char stem[NOTES_NAME_MAX];
      int  sl = (int)(nl - 3);
      if(sl >= NOTES_NAME_MAX) sl = NOTES_NAME_MAX - 1;
      strncpy(stem, nm, (size_t)sl);
      stem[sl] = '\0';
      char path[700];
      snprintf(path, sizeof(path), "%s/%s", dir, nm);
      FILE *f = fopen(path, "r");
      if(!f) continue;
      char buf[NOTES_MAX];
      int  rd = (int)fread(buf, 1, NOTES_MAX - 1, f);
      if(rd < 0) rd = 0;
      buf[rd] = '\0';
      fclose(f);
      note_add(stem, buf, rd);
    }
    closedir(d);
  }

  /* Migrate legacy single file */
  if(g_notes_count == 0) {
    char legacy[512];
    notes_legacy_path(legacy, sizeof(legacy));
    FILE *f = fopen(legacy, "r");
    if(f) {
      char buf[NOTES_MAX];
      int  rd = (int)fread(buf, 1, NOTES_MAX - 1, f);
      if(rd < 0) rd = 0;
      buf[rd] = '\0';
      fclose(f);
      note_add("notes", buf, rd);
      notes_save_one(&g_notes_files[0]);
    }
  }

  /* Always have at least one note */
  if(g_notes_count == 0) note_add("notes", NULL, 0);
}

static void notes_new(const char *name) {
  if(g_notes_count >= NOTES_FILES) return;
  notes_save();
  note_add(name, NULL, 0);
  g_notes_active                      = g_notes_count - 1;
  g_notes_files[g_notes_active].dirty = true;
  notes_save_one(&g_notes_files[g_notes_active]);
}

static void notes_delete_current(void) {
  if(g_notes_count <= 1) return; /* keep at least one */
  NoteFile *nf = &g_notes_files[g_notes_active];
  char      path[700];
  notes_file_path(nf, path, sizeof(path));
  unlink(path);
  /* Shift array */
  for(int i = g_notes_active; i < g_notes_count - 1; i++)
    g_notes_files[i] = g_notes_files[i + 1];
  g_notes_count--;
  if(g_notes_active >= g_notes_count) g_notes_active = g_notes_count - 1;
}

/* ── Markdown token types for rendering ─────────────────────────────── */
typedef enum {
  MD_NORMAL,
  MD_H1,     /* # */
  MD_H2,     /* ## */
  MD_H3,     /* ### */
  MD_BULLET, /* - or * at start */
  MD_CODE,   /* ```...``` block or `inline` */
  MD_BOLD,   /* **text** */
  MD_ITALIC, /* *text* or _text_ */
  MD_HR,     /* --- or *** */
} MdStyle;

/* Analyse a single display line and return its dominant style */
static MdStyle md_line_style(const char *ln) {
  if(strncmp(ln, "### ", 4) == 0) return MD_H3;
  if(strncmp(ln, "## ", 3) == 0) return MD_H2;
  if(strncmp(ln, "# ", 2) == 0) return MD_H1;
  if(strncmp(ln, "- ", 2) == 0 || strncmp(ln, "* ", 2) == 0) return MD_BULLET;
  if(strncmp(ln, "```", 3) == 0) return MD_CODE;
  if(strncmp(ln, "---", 3) == 0 || strncmp(ln, "***", 3) == 0) return MD_HR;
  return MD_NORMAL;
}

/* Strip markdown prefix chars to get display text */
static const char *md_display(const char *ln, MdStyle s) {
  switch(s) {
    case MD_H1: return ln + 2;
    case MD_H2: return ln + 3;
    case MD_H3: return ln + 4;
    case MD_BULLET: return ln + 2;
    case MD_CODE: return ln + 3;
    default: return ln;
  }
}

/* Map style to a colour (r,g,b) — Catppuccin-flavoured */
static void
md_colour(MdStyle s, bool in_code_block, uint8_t *r, uint8_t *g, uint8_t *b) {
  if(in_code_block) {
    *r = 0xa6;
    *g = 0xe3;
    *b = 0xa1;
    return;
  } /* green */
  switch(s) {
    case MD_H1:
      *r = 0xcb;
      *g = 0xa6;
      *b = 0xf7;
      return; /* mauve */
    case MD_H2:
      *r = 0x89;
      *g = 0xdc;
      *b = 0xeb;
      return; /* sky */
    case MD_H3:
      *r = 0x74;
      *g = 0xc7;
      *b = 0xec;
      return; /* sapphire */
    case MD_BULLET:
      *r = 0xf9;
      *g = 0xe2;
      *b = 0xaf;
      return; /* yellow */
    case MD_CODE:
      *r = 0xa6;
      *g = 0xe3;
      *b = 0xa1;
      return; /* green */
    case MD_HR:
      *r = 0x58;
      *g = 0x5b;
      *b = 0x70;
      return; /* surface2 */
    default:
      *r = 0xcd;
      *g = 0xd6;
      *b = 0xf4;
      return; /* text */
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  Search panel — ripgrep/grep live file search
 * ═══════════════════════════════════════════════════════════════════════════ */

#define SEARCH_RESULTS_MAX 256
#define SEARCH_QUERY_MAX   128
#define SEARCH_LINE_MAX    256

typedef struct {
  char file[256];
  int  line;
  char text[SEARCH_LINE_MAX]; /* matching line content */
} SearchResult;

typedef struct {
  char            query[SEARCH_QUERY_MAX];
  int             query_len;
  SearchResult    results[SEARCH_RESULTS_MAX];
  int             result_count;
  bool            running;   /* search in progress */
  bool            file_only; /* -l mode: filenames only */
  int64_t         last_run_ms;
  pthread_t       thread;
  bool            thread_valid;
  pthread_mutex_t lock;
  /* double-buffer: write to pending, swap on done */
  SearchResult    pending[SEARCH_RESULTS_MAX];
  int             pending_count;
  bool            pending_ready;
} SearchState;

static SearchState g_search;

static bool find_in_path(const char *prog) {
  const char *path_env = getenv("PATH");
  if(!path_env) return false;
  char path_copy[4096];
  strncpy(path_copy, path_env, sizeof(path_copy) - 1);
  char *save = NULL;
  char *dir  = strtok_r(path_copy, ":", &save);
  while(dir) {
    char full[1024];
    snprintf(full, sizeof(full), "%s/%s", dir, prog);
    if(access(full, X_OK) == 0) return true;
    dir = strtok_r(NULL, ":", &save);
  }
  return false;
}

static void *search_thread(void *arg) {
  (void)arg;
  char query[SEARCH_QUERY_MAX];
  bool file_only;
  pthread_mutex_lock(&g_search.lock);
  strncpy(query, g_search.query, sizeof(query) - 1);
  file_only              = g_search.file_only;
  g_search.pending_count = 0;
  pthread_mutex_unlock(&g_search.lock);

  if(!query[0]) {
    pthread_mutex_lock(&g_search.lock);
    g_search.pending_count = 0;
    g_search.pending_ready = true;
    g_search.running       = false;
    pthread_mutex_unlock(&g_search.lock);
    return NULL;
  }

  /* Probe for rg once via access() — no shell spawn needed */
  bool has_rg = find_in_path("rg");

  char cmd[512];
  if(has_rg) {
    if(file_only)
      snprintf(cmd, sizeof(cmd),
               "rg -l --color=never -i '%s' 2>/dev/null | head -256", query);
    else
      snprintf(cmd, sizeof(cmd),
               "rg -n --color=never -i --no-heading '%s' 2>/dev/null | head -256", query);
  } else {
    if(file_only)
      snprintf(cmd, sizeof(cmd),
               "grep -r -l -i '%s' . 2>/dev/null | head -256", query);
    else
      snprintf(cmd, sizeof(cmd),
               "grep -r -n -i '%s' . 2>/dev/null | head -256", query);
  }

  FILE *f = popen(cmd, "r");
  int   n = 0;
  if(f) {
    char line[512];
    while(fgets(line, sizeof(line), f) && n < SEARCH_RESULTS_MAX) {
      size_t l = strlen(line);
      while(l > 0 && (line[l-1] == '\n' || line[l-1] == '\r'))
        line[--l] = '\0';

      SearchResult *sr = &g_search.pending[n];
      if(file_only) {
        strncpy(sr->file, line, sizeof(sr->file) - 1);
        sr->line    = 0;
        sr->text[0] = '\0';
      } else {
        char *colon1 = strchr(line, ':');
        if(!colon1) continue;
        *colon1 = '\0';
        strncpy(sr->file, line, sizeof(sr->file) - 1);
        char *colon2 = strchr(colon1 + 1, ':');
        if(!colon2) {
          sr->line = 0;
          strncpy(sr->text, colon1 + 1, sizeof(sr->text) - 1);
        } else {
          *colon2  = '\0';
          sr->line = atoi(colon1 + 1);
          strncpy(sr->text, colon2 + 1, sizeof(sr->text) - 1);
        }
      }
      n++;
    }
    pclose(f);
  }

  pthread_mutex_lock(&g_search.lock);
  g_search.pending_count = n;
  g_search.pending_ready = true;
  g_search.running       = false;
  pthread_mutex_unlock(&g_search.lock);
  return NULL;
}

static void search_run(void) {
  if(g_search.running) return;
  g_search.running = true;
  if(g_search.thread_valid) {
    pthread_join(g_search.thread, NULL);
    g_search.thread_valid = false;
  }
  if(pthread_create(&g_search.thread, NULL, search_thread, NULL) == 0)
    g_search.thread_valid = true;
  else
    g_search.running = false;
}

/* Call from render loop to swap pending results */
static void search_poll(void) {
  if(!g_search.pending_ready) return;
  pthread_mutex_lock(&g_search.lock);
  memcpy(g_search.results,
         g_search.pending,
         sizeof(SearchResult) * (size_t)g_search.pending_count);
  g_search.result_count  = g_search.pending_count;
  g_search.pending_ready = false;
  pthread_mutex_unlock(&g_search.lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  Deps panel — dependency inspector
 * ═══════════════════════════════════════════════════════════════════════════ */

#define DEPS_MAX      128
#define DEPS_LINE_MAX 128

typedef enum { DEPS_UNKNOWN, DEPS_RUST, DEPS_GO, DEPS_MAVEN, DEPS_GRADLE } DepsLang;

typedef struct {
  char name[64];
  char version[32];
  char latest[32]; /* empty = not checked */
  bool outdated;
} DepEntry;

typedef struct {
  DepsLang  lang;
  DepEntry  entries[DEPS_MAX];
  int       count;
  bool      checking; /* outdated check running */
  bool      valid;
  int64_t   fetched_ms;
  pthread_t thread;
  bool      thread_valid;
} DepsState;

static DepsState g_deps;

static DepsLang deps_detect(void) {
  struct stat st;
  if(stat("Cargo.toml", &st) == 0) return DEPS_RUST;
  if(stat("go.mod", &st) == 0) return DEPS_GO;
  if(stat("pom.xml", &st) == 0) return DEPS_MAVEN;
  if(stat("build.gradle", &st) == 0 || stat("build.gradle.kts", &st) == 0)
    return DEPS_GRADLE;
  return DEPS_UNKNOWN;
}

static void deps_parse_cargo(void) {
  FILE *f = fopen("Cargo.toml", "r");
  if(!f) return;
  char line[256];
  bool in_deps = false;
  while(fgets(line, sizeof(line), f) && g_deps.count < DEPS_MAX) {
    if(strncmp(line, "[dependencies]", 14) == 0 ||
       strncmp(line, "[dev-dependencies]", 18) == 0 ||
       strncmp(line, "[build-dependencies]", 20) == 0) {
      in_deps = true;
      continue;
    }
    if(line[0] == '[') {
      in_deps = false;
      continue;
    }
    if(!in_deps) continue;
    /* name = "version" or name = { version = "..." } */
    char *eq = strchr(line, '=');
    if(!eq) continue;
    char name[64] = { 0 }, ver[32] = { 0 };
    int  nlen = (int)(eq - line);
    while(nlen > 0 && line[nlen - 1] == ' ')
      nlen--;
    strncpy(name, line, (size_t)(nlen < 63 ? nlen : 63));
    /* skip leading spaces in value */
    char *val = eq + 1;
    while(*val == ' ')
      val++;
    if(*val == '"') {
      val++;
      char *end = strchr(val, '"');
      if(end) {
        strncpy(ver, val, (size_t)((end - val) < 31 ? (end - val) : 31));
      }
    } else if(strstr(val, "version")) {
      char *vs = strstr(val, "\"");
      if(vs) {
        vs++;
        char *ve = strchr(vs, '"');
        if(ve) strncpy(ver, vs, (size_t)((ve - vs) < 31 ? (ve - vs) : 31));
      }
    } else
      continue;
    if(!name[0]) continue;
    strncpy(g_deps.entries[g_deps.count].name, name, 63);
    strncpy(g_deps.entries[g_deps.count].version, ver, 31);
    g_deps.count++;
  }
  fclose(f);
}

static void deps_parse_go(void) {
  char lines[DEPS_MAX][DEPS_LINE_MAX];
  int  n = run_capture("go list -m all 2>/dev/null", lines, DEPS_MAX);
  for(int i = 1; i < n && g_deps.count < DEPS_MAX; i++) { /* skip first (self) */
    char *sp = strchr(lines[i], ' ');
    if(!sp) continue;
    *sp = '\0';
    strncpy(g_deps.entries[g_deps.count].name, lines[i], 63);
    strncpy(g_deps.entries[g_deps.count].version, sp + 1, 31);
    g_deps.count++;
  }
}

static void deps_parse_maven(void) {
  char lines[DEPS_MAX][DEPS_LINE_MAX];
  int  n = run_capture("mvn dependency:list -q 2>/dev/null | grep '\\[INFO\\]' | "
                       "grep ':' | head -128",
                      lines,
                      DEPS_MAX);
  for(int i = 0; i < n && g_deps.count < DEPS_MAX; i++) {
    /* [INFO]    groupId:artifactId:type:version:scope */
    char *p = strstr(lines[i], "   ");
    if(!p) continue;
    while(*p == ' ')
      p++;
    /* extract artifactId and version */
    char  parts[5][64] = { { 0 } };
    int   pi           = 0;
    char *tok          = strtok(p, ":");
    while(tok && pi < 5) {
      strncpy(parts[pi++], tok, 63);
      tok = strtok(NULL, ":");
    }
    if(pi < 4) continue;
    strncpy(g_deps.entries[g_deps.count].name, parts[1], 63);
    strncpy(g_deps.entries[g_deps.count].version, parts[3], 31);
    g_deps.count++;
  }
}

static void *deps_check_thread(void *arg) {
  (void)arg;
  /* Language-specific outdated check */
  if(g_deps.lang == DEPS_RUST) {
    /* cargo outdated --format json would be ideal; use plain text */
    char lines[DEPS_MAX][DEPS_LINE_MAX];
    int  n = run_capture("cargo outdated 2>/dev/null | tail -n +3", lines, DEPS_MAX);
    for(int i = 0; i < n; i++) {
      /* Name  Current  Compat  Latest  Kind  Platform */
      char name[64] = { 0 }, cur[32] = { 0 }, latest[32] = { 0 };
      if(sscanf(lines[i], "%63s %31s %*s %31s", name, cur, latest) < 3) continue;
      if(strcmp(latest, "---") == 0) continue;
      for(int j = 0; j < g_deps.count; j++) {
        if(strcmp(g_deps.entries[j].name, name) == 0) {
          strncpy(g_deps.entries[j].latest, latest, 31);
          g_deps.entries[j].outdated = (strcmp(cur, latest) != 0);
          break;
        }
      }
    }
  } else if(g_deps.lang == DEPS_GO) {
    char lines[DEPS_MAX][DEPS_LINE_MAX];
    int  n = run_capture("go list -u -m all 2>/dev/null", lines, DEPS_MAX);
    for(int i = 1; i < n; i++) {
      /* module version [newversion] */
      char name[64] = { 0 }, ver[32] = { 0 }, latest[32] = { 0 };
      sscanf(lines[i], "%63s %31s %31s", name, ver, latest);
      /* latest has brackets: [v1.2.3] */
      if(latest[0] == '[') {
        size_t ll = strlen(latest);
        if(ll > 2) {
          memmove(latest, latest + 1, ll - 2);
          latest[ll - 2] = '\0';
        }
        for(int j = 0; j < g_deps.count; j++) {
          if(strcmp(g_deps.entries[j].name, name) == 0) {
            strncpy(g_deps.entries[j].latest, latest, 31);
            g_deps.entries[j].outdated = true;
            break;
          }
        }
      }
    }
  }
  g_deps.checking = false;
  return NULL;
}

static void deps_load(void) {
  int64_t now = ov_now_ms();
  if(g_deps.valid && now - g_deps.fetched_ms < 30000) return;
  g_deps.count = 0;
  g_deps.lang  = deps_detect();
  switch(g_deps.lang) {
    case DEPS_RUST: deps_parse_cargo(); break;
    case DEPS_GO: deps_parse_go(); break;
    case DEPS_MAVEN: deps_parse_maven(); break;
    default: break;
  }
  g_deps.valid      = true;
  g_deps.fetched_ms = now;
}

static void deps_check_outdated(void) {
  if(g_deps.checking) return;
  g_deps.checking = true;
  if(g_deps.thread_valid) {
    pthread_join(g_deps.thread, NULL);
    g_deps.thread_valid = false;
  }
  if(pthread_create(&g_deps.thread, NULL, deps_check_thread, NULL) == 0)
    g_deps.thread_valid = true;
  else
    g_deps.checking = false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  Log filter state
 * ═══════════════════════════════════════════════════════════════════════════ */

#define LOG_FILTER_MAX 64
static char g_log_filter[LOG_FILTER_MAX];
static int  g_log_filter_len  = 0;
static bool g_log_filter_mode = false;


FT_Library g_ov_ft        = NULL;
FT_Face    g_ov_face      = NULL;  /* primary text face   */
FT_Face    g_ov_icon_face = NULL;  /* fallback icon face  */
int        g_ov_asc       = 0;
int        g_ov_th        = 0;

/* ── UTF-8 decoder ──────────────────────────────────────────────────────────
 * Decodes one Unicode codepoint from *pp, advancing *pp past the bytes used.
 * Returns the codepoint, or the raw byte on invalid sequences (safe fallback).
 * ─────────────────────────────────────────────────────────────────────────── */
static uint32_t utf8_next(const char **pp) {
  const unsigned char *p = (const unsigned char *)*pp;
  uint32_t cp;
  int      extra;
  if(*p < 0x80) {
    cp = *p++; extra = 0;
  } else if((*p & 0xe0) == 0xc0) {
    cp = *p++ & 0x1f; extra = 1;
  } else if((*p & 0xf0) == 0xe0) {
    cp = *p++ & 0x0f; extra = 2;
  } else if((*p & 0xf8) == 0xf0) {
    cp = *p++ & 0x07; extra = 3;
  } else {
    cp = *p++; extra = 0; /* invalid byte — pass through */
  }
  while(extra-- > 0 && (*p & 0xc0) == 0x80)
    cp = (cp << 6) | (*p++ & 0x3f);
  *pp = (const char *)p;
  return cp;
}

/* ── Icon-font fallback paths (checked in order at init) ───────────────────
 * Any Nerd Font or symbols-only font works here.  We try common distro paths.
 * ─────────────────────────────────────────────────────────────────────────── */
static const char *ICON_FONT_CANDIDATES[] = {
  /* Arch/Manjaro */
  "/usr/share/fonts/TTF/NerdFontsSymbolsOnly.ttf",
  "/usr/share/fonts/TTF/Symbols-2048-em Nerd Font Complete.ttf",
  "/usr/share/fonts/nerd-fonts-symbols/NerdFontsSymbolsOnly.ttf",
  /* Ubuntu/Debian */
  "/usr/share/fonts/truetype/nerd-fonts/NerdFontsSymbolsOnly.ttf",
  /* Fedora */
  "/usr/share/fonts/NerdFontsSymbolsOnly.ttf",
  /* User-local */
  NULL, /* filled from $HOME at runtime */
  NULL,
};

static void ov_load_icon_face(float size_pt) {
  /* Fill home-relative candidate */
  static char home_path[512];
  const char *home = getenv("HOME");
  if(home) {
    snprintf(home_path, sizeof(home_path),
             "%s/.local/share/fonts/NerdFontsSymbolsOnly.ttf", home);
    /* Last slot before terminal NULL */
    ICON_FONT_CANDIDATES[5] = home_path;
  }

  if(g_ov_icon_face) { FT_Done_Face(g_ov_icon_face); g_ov_icon_face = NULL; }

  for(int i = 0; ICON_FONT_CANDIDATES[i]; i++) {
    if(FT_New_Face(g_ov_ft, ICON_FONT_CANDIDATES[i], 0, &g_ov_icon_face) == 0) {
      FT_Set_Char_Size(g_ov_icon_face, 0, (FT_F26Dot6)(size_pt * 64.f), 96, 96);
      wlr_log(WLR_INFO, "overlay: icon font → %s", ICON_FONT_CANDIDATES[i]);
      return;
    }
  }

  /* Last resort: if the primary face itself is a Nerd Font it already has the
   * icons — leave g_ov_icon_face NULL and the render loop will retry on the
   * primary face for any codepoint above U+E000. */
  wlr_log(WLR_DEBUG, "overlay: no dedicated icon font found; "
                     "icons will use primary face (works if it is a Nerd Font)");
}

static void ov_font_init(const char *path, float size_pt) {
  if(!g_ov_ft) FT_Init_FreeType(&g_ov_ft);
  if(g_ov_face) { FT_Done_Face(g_ov_face); g_ov_face = NULL; }
  if(!path || !path[0]) return;
  if(FT_New_Face(g_ov_ft, path, 0, &g_ov_face)) { g_ov_face = NULL; return; }
  if(size_pt <= 0.f) size_pt = 13.f;
  FT_Set_Char_Size(g_ov_face, 0, (FT_F26Dot6)(size_pt * 64.f), 96, 96);
  g_ov_asc = (int)ceilf((float)g_ov_face->size->metrics.ascender / 64.f);
  int desc = (int)floorf((float)g_ov_face->size->metrics.descender / 64.f);
  g_ov_th  = g_ov_asc - desc;
  ov_load_icon_face(size_pt);
}

/* Load a glyph for codepoint cp.  Tries primary face first; falls back to the
 * icon face for codepoints in the PUA / high Unicode ranges used by Nerd Fonts
 * (U+E000–U+F8FF, U+F0000–U+FFFFF, U+100000–U+10FFFF).
 * Returns the face the glyph was loaded from, or NULL on failure.            */
static FT_Face ov_load_glyph(uint32_t cp, int load_flags) {
  bool is_icon = (cp >= 0xE000 && cp <= 0xF8FF)   /* BMP PUA           */
              || (cp >= 0xE000 && cp <= 0xF8FF)
              || (cp >= 0x1F300)                    /* emoji / misc syms */
              || (cp >= 0xF0000);                   /* Supplementary PUA */

  /* Try primary face unless it's a known icon codepoint */
  if(!is_icon && g_ov_face) {
    FT_UInt idx = FT_Get_Char_Index(g_ov_face, cp);
    if(idx && FT_Load_Glyph(g_ov_face, idx, load_flags) == 0)
      return g_ov_face;
  }
  /* Try icon face */
  if(g_ov_icon_face) {
    FT_UInt idx = FT_Get_Char_Index(g_ov_icon_face, cp);
    if(idx && FT_Load_Glyph(g_ov_icon_face, idx, load_flags) == 0)
      return g_ov_icon_face;
  }
  /* Final fallback: primary face for any codepoint (renders .notdef box) */
  if(g_ov_face) {
    if(FT_Load_Char(g_ov_face, cp, load_flags) == 0) return g_ov_face;
  }
  return NULL;
}

int ov_measure(const char *text) {
  if(!g_ov_face || !text) return 0;
  int         w = 0;
  const char *p = text;
  while(*p) {
    uint32_t  cp   = utf8_next(&p);
    FT_Face   face = ov_load_glyph(cp, FT_LOAD_ADVANCE_ONLY);
    if(face) w += (int)(face->glyph->advance.x >> 6);
  }
  return w;
}

void ov_draw_text(uint32_t   *px,
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
  int         pen = x;
  const char *p   = text;
  while(*p) {
    uint32_t cp   = utf8_next(&p);
    FT_Face  face = ov_load_glyph(cp, FT_LOAD_RENDER);
    if(!face) continue;
    FT_GlyphSlot slot = face->glyph;
    int          gx   = pen + slot->bitmap_left;
    int          gy   = y - slot->bitmap_top;
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
        uint8_t   og  = (uint8_t)((g * ba + ((*d >> 8)  & 0xff) * inv) / 255);
        uint8_t   ob  = (uint8_t)((b * ba + (*d & 0xff)          * inv) / 255);
        *d = (0xffu << 24) | ((uint32_t)or_ << 16) | ((uint32_t)og << 8) | ob;
      }
    }
    pen += (int)(slot->advance.x >> 6);
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  Pixel helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

void ov_fill_rect(uint32_t *px,
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

void ov_fill_border(uint32_t *px,
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
  PANEL_SEARCH,
  PANEL_RUN,
  PANEL_DEPS,
  PANEL_FILES,
  PANEL_NVIM,
  PANEL_LSP,
  PANEL_COUNT,
} PanelId;


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
  int                      build_cmd_editing;
  int                      build_err_cursor;
  bool                     build_show_errors;
  /* notes */
  bool                     notes_loaded;
  /* search panel */
  bool                     search_active;
  /* file browser panel */
  char                     fb_cwd[1024];
  char                     fb_filter[128];
  int                      fb_filter_len;
  bool                     fb_filter_mode;
  /* workspace panel */
  int                      ws_cursor;        /* selected workspace index */
  int                      pending_ws_switch; /* -1 = none, else ws index to switch to */
  /* nvim / lsp panels */
  int                      nvim_cursor;
  int                      lsp_cursor;
  /* cached config pointer (set in overlay_create, valid for compositor lifetime) */
  const Config            *cfg;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * §9b  Dev-integration helpers — must precede overlay_create
 *
 * These are defined here rather than after §11 because overlay_create()
 * calls panel_name_to_id() and lsp_tab_badge() is referenced in
 * overlay_update().  Placing them here keeps a single definition with no
 * forward-declaration gymnastics.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Returns the best editor to use: config → $EDITOR → nvim */
static const char *overlay_editor(const OverlayCfg *ov_cfg) {
  if(ov_cfg && ov_cfg->editor[0]) return ov_cfg->editor;
  const char *env = getenv("EDITOR");
  return (env && env[0]) ? env : "nvim";
}

/* Returns the terminal emulator to use: config → $TERM_PROGRAM → autodetect */
static const char *overlay_terminal(const OverlayCfg *ov_cfg) {
  if(ov_cfg && ov_cfg->terminal[0]) return ov_cfg->terminal;
  const char *env = getenv("TERM_PROGRAM");
  if(env && env[0]) return env;
  static const char *candidates[] = {
    "/usr/bin/kitty", "/usr/bin/foot", "/usr/bin/alacritty",
    "/usr/bin/wezterm", "/usr/bin/xterm", NULL
  };
  for(int i = 0; candidates[i]; i++) {
    if(access(candidates[i], X_OK) == 0) {
      const char *slash = strrchr(candidates[i], '/');
      return slash ? slash + 1 : candidates[i];
    }
  }
  return "xterm";
}

/* Open a file at an optional line.  Prefers the live nvim socket; falls back
 * to spawning a terminal with $EDITOR.  Used by search, files, and build
 * panels instead of ad-hoc system() calls.                                   */
void overlay_open_file(const char *path, int line, const OverlayCfg *ov_cfg) {
  /* Prefer the live nvim bridge when available */
  if(ov_cfg && ov_cfg->lsp_diagnostics && nvim_is_connected()) {
    nvim_open_file(path, line);
    return;
  }

  const char *editor = overlay_editor(ov_cfg);
  const char *term   = overlay_terminal(ov_cfg);

  /* Build the shell command string */
  char cmd[1280];
  if(line > 0)
    snprintf(cmd, sizeof(cmd), "%s -e %s +%d \"%s\"", term, editor, line, path);
  else
    snprintf(cmd, sizeof(cmd), "%s -e %s \"%s\"", term, editor, path);

  pid_t pid = fork();
  if(pid == 0) {
    setsid(); /* detach from compositor's process group */
    int maxfd = (int)sysconf(_SC_OPEN_MAX);
    for(int fd = 3; fd < maxfd; fd++) close(fd);
    execl("/bin/sh", "sh", "-c", cmd, NULL);
    _exit(1);
  }
  /* Parent returns immediately; log failure but don't crash */
  if(pid < 0) log_ring_push("==> overlay: fork() failed opening file");
}

/* Map a panel name string (from config default_panel) to its PanelId */
static int panel_name_to_id(const char *name) {
  if(!name || !name[0])                                               return PANEL_RUN;
  if(!strcasecmp(name, "workspaces") || !strcasecmp(name, "ws"))     return PANEL_WORKSPACES;
  if(!strcasecmp(name, "commands")   || !strcasecmp(name, "cmd"))    return PANEL_COMMANDS;
  if(!strcasecmp(name, "processes")  || !strcasecmp(name, "proc"))   return PANEL_PROCESSES;
  if(!strcasecmp(name, "log")        || !strcasecmp(name, "logs"))   return PANEL_LOG;
  if(!strcasecmp(name, "git"))                                        return PANEL_GIT;
  if(!strcasecmp(name, "build"))                                      return PANEL_BUILD;
  if(!strcasecmp(name, "notes"))                                      return PANEL_NOTES;
  if(!strcasecmp(name, "search")     || !strcasecmp(name, "find"))   return PANEL_SEARCH;
  if(!strcasecmp(name, "run"))                                        return PANEL_RUN;
  if(!strcasecmp(name, "deps")       || !strcasecmp(name, "dep"))    return PANEL_DEPS;
  if(!strcasecmp(name, "files"))                                      return PANEL_FILES;
  if(!strcasecmp(name, "nvim")       || !strcasecmp(name, "neovim")) return PANEL_NVIM;
  if(!strcasecmp(name, "lsp")        || !strcasecmp(name, "diag"))   return PANEL_LSP;
  return PANEL_RUN;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §10  Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

TrixieOverlay *
overlay_create(struct wlr_scene_tree *layer, int w, int h, const Config *cfg) {
  TrixieOverlay *o = calloc(1, sizeof(*o));
  o->w                  = w;
  o->h                  = h;
  o->cfg                = cfg;
  o->panel              = PANEL_WORKSPACES;
  o->pending_ws_switch  = -1;
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

  pthread_mutex_init(&g_build.err_lock, NULL);
  atomic_init(&g_build.running, false);
  pthread_mutex_init(&g_search.lock, NULL);

  build_autodetect(g_build.cmd, sizeof(g_build.cmd));
  strncpy(o->build_cmd, g_build.cmd, sizeof(o->build_cmd) - 1);

  /* Apply project root before run_configs_init so stat() finds the right files */
  if(cfg->overlay.project_root[0]) {
    if(chdir(cfg->overlay.project_root) == 0) {
      wlr_log(WLR_INFO, "overlay: project_root → %s", cfg->overlay.project_root);
      strncpy(o->fb_cwd,   cfg->overlay.project_root, sizeof(o->fb_cwd)   - 1);
      strncpy(g_git_cwd,   cfg->overlay.project_root, sizeof(g_git_cwd)   - 1);
    } else {
      wlr_log(WLR_ERROR, "overlay: chdir('%s') failed", cfg->overlay.project_root);
    }
  }

  run_configs_init(cfg->overlay);
  deps_load();

  /* Apply default_panel */
  if(cfg->overlay.default_panel[0])
    o->panel = (PanelId)panel_name_to_id(cfg->overlay.default_panel);

  return o;
}
/* Exposed for nvim_panel.c overlay_quickfix_json() */
int overlay_build_err_count(void) {
    return g_build.err_count;
}

void overlay_build_err_get(int idx, char *file, int fsz,
                           int *line, int *col,
                           char *msg,  int msz, bool *is_warning) {
    if(idx < 0 || idx >= g_build.err_count) {
        if(file) file[0] = '\0';
        if(line) *line = 0;
        if(col)  *col  = 0;
        if(msg)  msg[0] = '\0';
        if(is_warning) *is_warning = false;
        return;
    }
    BuildError *e = &g_build.errors[idx];
    if(file) { strncpy(file, e->file, fsz - 1); file[fsz-1] = '\0'; }
    if(line) *line = e->line;
    if(col)  *col  = e->col;
    if(msg)  { strncpy(msg, e->msg, msz - 1); msg[msz-1] = '\0'; }
    if(is_warning) *is_warning = e->is_warning;
}
void overlay_destroy(TrixieOverlay *o) {
  if(!o) return;
  if(g_build.thread_valid) {
    pthread_join(g_build.thread, NULL);
    g_build.thread_valid = false;
  }
  pthread_mutex_destroy(&g_build.err_lock);
  if(g_search.thread_valid) {
    pthread_join(g_search.thread, NULL);
    g_search.thread_valid = false;
  }
  pthread_mutex_destroy(&g_search.lock);
  if(g_deps.thread_valid) {
    pthread_join(g_deps.thread, NULL);
    g_deps.thread_valid = false;
  }
  /* Clean up run panel (kills processes, joins reader threads) */
  run_configs_destroy();
  nvim_disconnect();
  notes_save();
  wlr_scene_node_destroy(&o->scene_buf->node);
  if(g_ov_face) {
    FT_Done_Face(g_ov_face);
    g_ov_face = NULL;
  }
  if(g_ov_icon_face) {
    FT_Done_Face(g_ov_icon_face);
    g_ov_icon_face = NULL;
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
/* ── overlay_set_cwd ────────────────────────────────────────────────────────
 * Called by the IPC "overlay cd <path>" command (and internally when the file
 * browser navigates).  Updates the file browser cwd, invalidates the git
 * cache so the git panel picks up the new repo, and re-runs language
 * detection so the run panel reflects the new project.                       */
void overlay_set_cwd(TrixieOverlay *o, const char *path) {
  if(!path || !path[0]) return;

  /* Expand ~/  */
  char expanded[1024] = { 0 };
  if(!strncmp(path, "~/", 2)) {
    const char *home = getenv("HOME");
    if(!home) home = "/root";
    snprintf(expanded, sizeof(expanded), "%s/%s", home, path + 2);
  } else {
    strncpy(expanded, path, sizeof(expanded) - 1);
  }

  /* Update file browser and git cwd */
  if(o) strncpy(o->fb_cwd, expanded, sizeof(o->fb_cwd) - 1);
  strncpy(g_git_cwd, expanded, sizeof(g_git_cwd) - 1);

  /* Invalidate git cache so next panel open re-queries */
  g_git.valid = false;

  /* Re-run language detection for the new directory */
  if(o && o->cfg) run_configs_init(o->cfg->overlay);

  log_ring_push("  cwd changed");
  wlr_log(WLR_DEBUG, "overlay: cwd → %s", expanded);
}

/* ── overlay_notify ─────────────────────────────────────────────────────────
 * Push a message into the overlay log ring.  Shows up in the Log panel.
 * Called by the IPC "overlay notify <title> <msg>" command.                  */
void overlay_notify(TrixieOverlay *o, const char *title, const char *msg) {
  (void)o;
  char line[LOG_LINE_MAX];
  if(title && title[0] && msg && msg[0])
    snprintf(line, sizeof(line), "  %s: %s", title, msg);
  else if(msg && msg[0])
    snprintf(line, sizeof(line), "  %s", msg);
  else if(title && title[0])
    snprintf(line, sizeof(line), "  %s", title);
  else
    return;
  log_ring_push(line);
}

/* ── overlay_show_panel ─────────────────────────────────────────────────────
 * Switch to a named panel, showing the overlay if hidden.                    */
void overlay_show_panel(TrixieOverlay *o, const char *name) {
  if(!o || !name || !name[0]) return;
  int id = panel_name_to_id(name);
  o->panel = (PanelId)id;
  if(!o->visible) {
    o->visible = true;
    wlr_scene_node_set_enabled(&o->scene_buf->node, true);
    if(!o->notes_loaded) { notes_load(); o->notes_loaded = true; }
  }
}

/* ── overlay_git_invalidate ─────────────────────────────────────────────────
 * Force the git panel to re-query on next open.                              */
void overlay_git_invalidate(TrixieOverlay *o) {
  (void)o;
  g_git.valid = false;
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
static void git_run_async(const char *cmd) {
  pid_t pid = fork();
  if(pid == 0) {
    setsid();
    int maxfd = (int)sysconf(_SC_OPEN_MAX);
    for(int fd = 3; fd < maxfd; fd++) close(fd);
    execl("/bin/sh", "sh", "-c", cmd, NULL);
    _exit(1);
  }
  if(pid < 0) log_ring_push("==> git: fork() failed");
}

bool overlay_key(TrixieOverlay *o, xkb_keysym_t sym, uint32_t mods) {
  if(!o || !o->visible) return false;
  (void)mods;

  /* ── Notes rename mode swallows keys ── */
  if(o->panel == PANEL_NOTES && g_notes_rename) {
    if(sym == XKB_KEY_Escape) {
      g_notes_rename = false;
      return true;
    }
    if(sym == XKB_KEY_Return && g_notes_rename_len > 0) {
      g_notes_rename_buf[g_notes_rename_len] = '\0';
      if(g_notes_count == 0) {
        notes_new(g_notes_rename_buf);
      } else {
        /* rename in-place: delete old file, update name, save */
        NoteFile *nf = cur_note();
        char      old_path[700];
        notes_file_path(nf, old_path, sizeof(old_path));
        unlink(old_path);
        strncpy(nf->name, g_notes_rename_buf, NOTES_NAME_MAX - 1);
        nf->dirty = true;
        notes_save_one(nf);
      }
      g_notes_rename = false;
      return true;
    }
    if(sym == XKB_KEY_BackSpace) {
      if(g_notes_rename_len > 0) g_notes_rename_buf[--g_notes_rename_len] = '\0';
      return true;
    }
    if(sym >= 0x20 && sym < 0x7f && g_notes_rename_len < NOTES_NAME_MAX - 1) {
      /* restrict to filename-safe chars */
      char c = (char)sym;
      if(c == ' ') c = '_';
      g_notes_rename_buf[g_notes_rename_len++] = c;
      g_notes_rename_buf[g_notes_rename_len]   = '\0';
    }
    return true;
  }

  /* ── Notes edit mode swallows printable keys + backspace ── */
  if(o->panel == PANEL_NOTES && g_notes_edit) {
    NoteFile *nf = cur_note();
    if(!nf) {
      g_notes_edit = false;
      return true;
    }
    if(sym == XKB_KEY_Escape) {
      g_notes_edit = false;
      nf->dirty    = true;
      notes_save_one(nf);
      return true;
    }
    if(sym == XKB_KEY_Return && nf->len < NOTES_MAX - 1) {
      nf->text[nf->len++] = '\n';
      nf->text[nf->len]   = '\0';
      return true;
    }
    if(sym == XKB_KEY_BackSpace) {
      if(nf->len > 0) nf->text[--nf->len] = '\0';
      return true;
    }
    if(sym >= 0x20 && sym < 0x7f && nf->len < NOTES_MAX - 1) {
      nf->text[nf->len++] = (char)sym;
      nf->text[nf->len]   = '\0';
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
      if(o->build_cmd[0]) strncpy(g_build.cmd, o->build_cmd, BUILD_CMD_MAX - 1);
      run_build();
      o->panel = PANEL_LOG;
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
    o->panel         = (PanelId)((o->panel + 1) % PANEL_COUNT);
    o->cursor        = 0;
    o->scroll        = 0;
    o->filter[0]     = '\0';
    o->filter_len    = 0;
    o->filter_mode   = false;
    o->search_active = false;
    return true;
  }
  if(sym >= XKB_KEY_1 && sym <= XKB_KEY_9) {
    o->panel         = (PanelId)(sym - XKB_KEY_1);
    o->cursor        = 0;
    o->scroll        = 0;
    o->filter[0]     = '\0';
    o->filter_len    = 0;
    o->filter_mode   = false;
    o->search_active = false;
    return true;
  }
  if(sym == XKB_KEY_0) {
    o->panel         = PANEL_DEPS;
    o->cursor        = 0;
    o->scroll        = 0;
    o->search_active = false;
    return true;
  }
  if(sym == XKB_KEY_minus) {
    o->panel         = PANEL_FILES;
    o->cursor        = 0;
    o->scroll        = 0;
    o->fb_filter[0]  = '\0';
    o->fb_filter_len = 0;
    o->fb_filter_mode= false;
    return true;
  }
  /* F = also jump to files panel */
  if(sym == XKB_KEY_F) {
    o->panel  = PANEL_FILES;
    o->cursor = 0;
    return true;
  }
  /* N = nvim panel */
  if(sym == XKB_KEY_N) {
    o->panel  = PANEL_NVIM;
    o->cursor = 0;
    return true;
  }
  /* L = LSP diagnostics panel */
  if(sym == XKB_KEY_L) {
    o->panel  = PANEL_LSP;
    o->cursor = 0;
    return true;
  }
  if(sym == XKB_KEY_Escape || sym == XKB_KEY_grave) {
    o->visible = false;
    wlr_scene_node_set_enabled(&o->scene_buf->node, false);
    return true;
  }
  if(sym == XKB_KEY_j || sym == XKB_KEY_Down) {
    if(o->panel != PANEL_WORKSPACES) { o->cursor++; return true; }
  }
  if(sym == XKB_KEY_k || sym == XKB_KEY_Up) {
    if(o->panel != PANEL_WORKSPACES) { if(o->cursor > 0) o->cursor--; return true; }
  }

  /* ── Panel-specific keys ── */
  switch(o->panel) {
    case PANEL_WORKSPACES:
      /* j/k navigate workspace grid; Enter queues switch; 1-9 direct-jump */
      if(sym == XKB_KEY_j || sym == XKB_KEY_Down) {
        o->ws_cursor++;
        return true;
      }
      if(sym == XKB_KEY_k || sym == XKB_KEY_Up) {
        if(o->ws_cursor > 0) o->ws_cursor--;
        return true;
      }
      if(sym == XKB_KEY_Return) {
        o->pending_ws_switch = o->ws_cursor;
        return true;
      }
      break;
      if(sym == XKB_KEY_Return) {
        /* SIGTERM selected process */
        if(o->cursor >= 0 && o->cursor < g_proc_count) {
          kill(g_procs[o->cursor].pid, SIGTERM);
          char msg[64];
          snprintf(msg,
                   sizeof(msg),
                   "==> SIGTERM pid %d (%s)",
                   g_procs[o->cursor].pid,
                   g_procs[o->cursor].comm);
          log_ring_push(msg);
        }
        return true;
      }
      if(sym == XKB_KEY_K) {
        if(o->cursor >= 0 && o->cursor < g_proc_count) {
          kill(g_procs[o->cursor].pid, SIGKILL);
          char msg[64];
          snprintf(msg,
                   sizeof(msg),
                   "==> SIGKILL pid %d (%s)",
                   g_procs[o->cursor].pid,
                   g_procs[o->cursor].comm);
          log_ring_push(msg);
        }
        return true;
      }
      if(sym == XKB_KEY_s) {
        g_proc_sort    = (ProcSort)((g_proc_sort + 1) % 3);
        g_proc_next_ms = 0; /* force refresh */
        return true;
      }
      break;
    case PANEL_LOG:
      if(sym == XKB_KEY_slash) {
        g_log_filter_mode = true;
        return true;
      }
      if(sym == XKB_KEY_c && !g_log_filter_mode) {
        g_log_ring.head  = 0;
        g_log_ring.count = 0;
        return true;
      }
      if(g_log_filter_mode) {
        if(sym == XKB_KEY_Escape || sym == XKB_KEY_Return) {
          g_log_filter_mode = false;
          return true;
        }
        if(sym == XKB_KEY_BackSpace) {
          if(g_log_filter_len > 0) g_log_filter[--g_log_filter_len] = '\0';
          return true;
        }
        if(sym >= 0x20 && sym < 0x7f && g_log_filter_len < LOG_FILTER_MAX - 1) {
          g_log_filter[g_log_filter_len++] = (char)sym;
          g_log_filter[g_log_filter_len]   = '\0';
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
      if(sym == XKB_KEY_d) {
        /* Toggle diff view for selected file */
        if(o->cursor >= 0 && o->cursor < g_git.file_count) {
          if(g_git.show_diff && g_git.diff_for == o->cursor) {
            g_git.show_diff = false;
          } else {
            git_load_diff(o->cursor);
            g_git.show_diff = true;
          }
        }
        return true;
      }
      if(sym == XKB_KEY_s) {
        /* Stage selected file */
        if(o->cursor >= 0 && o->cursor < g_git.file_count) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "git add -- '%s' 2>/dev/null",
                 g_git.files[o->cursor].path);
        git_run_async(cmd);
        g_git.valid = false;
        }
        return true;
      }
      if(sym == XKB_KEY_u) {
        /* Unstage selected file */
        if(o->cursor >= 0 && o->cursor < g_git.file_count) {
          char cmd[512];
          snprintf(cmd,
                   sizeof(cmd),
                   "git restore --staged -- '%s' 2>/dev/null",
                   g_git.files[o->cursor].path);
          system(cmd);
          g_git.valid = false;
        }
        return true;
      }
      break;
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
    case PANEL_SEARCH:
      if(sym == XKB_KEY_slash || (!o->search_active && sym >= 0x20 && sym < 0x7f)) {
        o->search_active = true;
        if(sym != XKB_KEY_slash && g_search.query_len < SEARCH_QUERY_MAX - 1) {
          g_search.query[g_search.query_len++] = (char)sym;
          g_search.query[g_search.query_len]   = '\0';
          search_run();
        }
        return true;
      }
      if(o->search_active) {
        if(sym == XKB_KEY_Escape) {
          o->search_active = false;
          return true;
        }
        if(sym == XKB_KEY_Return) {
          o->search_active = false;
          return true;
        }
        if(sym == XKB_KEY_BackSpace) {
          if(g_search.query_len > 0) {
            g_search.query[--g_search.query_len] = '\0';
            search_run();
          }
          return true;
        }
        if(sym >= 0x20 && sym < 0x7f && g_search.query_len < SEARCH_QUERY_MAX - 1) {
          g_search.query[g_search.query_len++] = (char)sym;
          g_search.query[g_search.query_len]   = '\0';
          search_run();
        }
        return true;
      }
      if(sym == XKB_KEY_Return && !o->search_active) {
        /* Open selected result in editor */
        search_poll();
        if(o->cursor >= 0 && o->cursor < g_search.result_count) {
          SearchResult *sr = &g_search.results[o->cursor];
          overlay_open_file(sr->file, sr->line, o->cfg ? &o->cfg->overlay : NULL);
        }
        return true;
      }
      if(sym == XKB_KEY_f) {
        g_search.file_only = !g_search.file_only;
        if(g_search.query_len > 0) search_run();
        return true;
      }
      break;
    case PANEL_RUN:
      return run_panel_key(&o->cursor, sym);
    case PANEL_NVIM:
      return nvim_panel_key(&o->nvim_cursor, sym, &o->cfg->overlay);
    case PANEL_LSP:
      return lsp_panel_key(&o->lsp_cursor, sym, &o->cfg->overlay);
    case PANEL_DEPS:
      if(sym == XKB_KEY_u) {
        deps_check_outdated();
        return true;
      }
      if(sym == XKB_KEY_r) {
        g_deps.valid = false;
        deps_load();
        return true;
      }
      break;
    case PANEL_FILES: {
      bool handled = files_panel_key(&o->cursor,
                             o->fb_cwd, sizeof(o->fb_cwd),
                             o->fb_filter, &o->fb_filter_len, &o->fb_filter_mode,
                             sym, &o->cfg->overlay);
      /* Sync git cwd whenever the file browser changes directory */
      if(handled && strcmp(g_git_cwd, o->fb_cwd) != 0) {
        strncpy(g_git_cwd, o->fb_cwd, sizeof(g_git_cwd) - 1);
        g_git.valid = false;
      }
      return handled;
    }
    case PANEL_BUILD:
      if(sym == XKB_KEY_b) {
        o->build_cmd[0]      = '\0';
        o->build_cmd_editing = 1;
        return true;
      }
      if(sym == XKB_KEY_e) {
        /* Toggle between error list and log view */
        o->build_show_errors = !o->build_show_errors;
        o->build_err_cursor  = 0;
        return true;
      }
      if(sym == XKB_KEY_Return) {
        if(o->build_show_errors && g_build.err_count > 0) {
          /* Jump to error in editor via $EDITOR file:line */
          pthread_mutex_lock(&g_build.err_lock);
          int ei = o->build_err_cursor;
          if(ei < g_build.err_count) {
            BuildError *be = &g_build.errors[ei];
            if(be->file[0] && be->line > 0)
              overlay_open_file(be->file, be->line, o->cfg ? &o->cfg->overlay : NULL);
          }
          pthread_mutex_unlock(&g_build.err_lock);
        } else {
          if(o->build_cmd[0]) strncpy(g_build.cmd, o->build_cmd, BUILD_CMD_MAX - 1);
          run_build();
          o->build_show_errors = false;
          o->panel             = PANEL_LOG;
        }
        return true;
      }
      if(sym == XKB_KEY_j || sym == XKB_KEY_Down) {
        if(o->build_show_errors) {
          pthread_mutex_lock(&g_build.err_lock);
          int mx = g_build.err_count - 1;
          pthread_mutex_unlock(&g_build.err_lock);
          if(o->build_err_cursor < mx) o->build_err_cursor++;
          return true;
        }
      }
      if(sym == XKB_KEY_k || sym == XKB_KEY_Up) {
        if(o->build_show_errors) {
          if(o->build_err_cursor > 0) o->build_err_cursor--;
          return true;
        }
      }
      break;
    case PANEL_NOTES:
      if(sym == XKB_KEY_e && !g_notes_edit) {
        g_notes_edit = true;
        return true;
      }
      if(sym == XKB_KEY_bracketleft) {
        /* previous note */
        if(g_notes_active > 0) {
          notes_save_one(cur_note());
          g_notes_active--;
        }
        return true;
      }
      if(sym == XKB_KEY_bracketright) {
        /* next note */
        if(g_notes_active < g_notes_count - 1) {
          notes_save_one(cur_note());
          g_notes_active++;
        }
        return true;
      }
      if(sym == XKB_KEY_n) {
        /* new note or rename current */
        g_notes_rename     = true;
        g_notes_rename_len = 0;
        /* pre-fill with current name for rename */
        NoteFile *nf       = cur_note();
        if(nf) {
          strncpy(g_notes_rename_buf, nf->name, NOTES_NAME_MAX - 1);
          g_notes_rename_len = (int)strlen(g_notes_rename_buf);
        } else {
          g_notes_rename_buf[0] = '\0';
        }
        return true;
      }
      if(sym == XKB_KEY_N) {
        /* capital N = new blank note */
        g_notes_rename        = true;
        g_notes_rename_len    = 0;
        g_notes_rename_buf[0] = '\0';
        return true;
      }
      if(sym == XKB_KEY_d && !g_notes_edit) {
        notes_delete_current();
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

#define ROW_H    (g_ov_th * 2)
#define PAD      16
#define HEADER_H (ROW_H + 6)

/* ── Shared cursor-line highlight ─────────────────────────────────────── */
void draw_cursor_line(uint32_t *px,
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
                                  int           ws_cursor,
                                  const Config *cfg) {
  Color ac = cfg->colors.active_border;
  Color im = cfg->colors.inactive_border;

  int cols = twm->ws_count > 5 ? 5 : (twm->ws_count > 0 ? twm->ws_count : 1);
  int rows = (twm->ws_count + cols - 1) / cols;

  /* Clamp ws_cursor */
  if(ws_cursor >= twm->ws_count) ws_cursor = twm->ws_count - 1;
  if(ws_cursor < 0)              ws_cursor = 0;

  int content_top = py0 + HEADER_H + PAD;
  int content_bot = py0 + ph - ROW_H - PAD;
  int avail_w     = pw - PAD * 2;
  int avail_h     = content_bot - content_top;
  if(avail_h < 1) return;

  int gap    = 8;
  int cell_w = (avail_w - gap * (cols - 1)) / cols;
  int cell_h = (avail_h - gap * (rows - 1)) / rows;
  if(cell_w < 4) cell_w = 4;
  if(cell_h < 4) cell_h = 4;

  /* Key hint above the grid */
  ov_draw_text(px, stride, px0 + PAD, content_top - ROW_H / 2 + g_ov_asc,
               stride, py0 + ph,
               "j/k navigate  Enter switch workspace",
               ac.r, ac.g, ac.b, 0x50);

  for(int i = 0; i < twm->ws_count; i++) {
    int col = i % cols, row = i / cols;
    int cx  = px0 + PAD + col * (cell_w + gap);
    int cy  = content_top + row * (cell_h + gap);

    if(cy + cell_h > content_bot) break;

    bool  active   = (i == twm->active_ws);
    bool  selected = (i == ws_cursor);   /* cursor highlight */
    bool  urgent   = (twm->ws_urgent_mask >> i) & 1;
    Color bc       = active ? ac : im;
    int   bw       = cfg->border_width > 0 ? cfg->border_width : 1;

    /* Cell background */
    ov_fill_rect(px, stride, cx, cy, cell_w, cell_h,
                 0x18, 0x18, 0x25, 0xff, stride, py0 + ph);

    /* Active workspace tint */
    if(active)
      ov_fill_rect(px, stride, cx, cy, cell_w, cell_h,
                   ac.r, ac.g, ac.b, 0x12, stride, py0 + ph);

    /* Cursor selection tint — distinct from active */
    if(selected && !active)
      ov_fill_rect(px, stride, cx, cy, cell_w, cell_h,
                   0x89, 0xdc, 0xeb, 0x18, stride, py0 + ph);

    if(urgent)
      ov_fill_rect(px, stride, cx, cy, cell_w, cell_h,
                   0xf3, 0x8b, 0xa8, 0x18, stride, py0 + ph);

    /* Border — thickness respects cfg->border_width */
    uint8_t br = urgent ? 0xf3 : (selected ? 0x89 : bc.r);
    uint8_t bg = urgent ? 0x8b : (selected ? 0xdc : bc.g);
    uint8_t bb = urgent ? 0xa8 : (selected ? 0xeb : bc.b);
    for(int t = 0; t < bw; t++) {
      ov_fill_rect(px, stride, cx+t, cy+t, cell_w-t*2, 1, br, bg, bb, 0xff, stride, py0+ph);
      ov_fill_rect(px, stride, cx+t, cy+cell_h-1-t, cell_w-t*2, 1, br, bg, bb, 0xff, stride, py0+ph);
      ov_fill_rect(px, stride, cx+t, cy+t, 1, cell_h-t*2, br, bg, bb, 0xff, stride, py0+ph);
      ov_fill_rect(px, stride, cx+cell_w-1-t, cy+t, 1, cell_h-t*2, br, bg, bb, 0xff, stride, py0+ph);
    }

    /* WS number with  icon — top-left */
    char wnum[16];
    const char *ws_icon = active ? "󰮯 " : "󰖰 "; /* nf-md-monitor / nf-md-monitor_off */
    snprintf(wnum, sizeof(wnum), "%s%d", ws_icon, i + 1);
    ov_draw_text(px, stride, cx + 6, cy + g_ov_asc + 4,
                 stride, py0 + ph, wnum,
                 ac.r, ac.g, ac.b, active ? 0xff : 0x70);

    /* Layout + ratio — top-right */
    Workspace *ws = &twm->workspaces[i];
    char       lay_buf[32];
    snprintf(lay_buf, sizeof(lay_buf), "%s %.0f%%",
             layout_label(ws->layout), ws->main_ratio * 100.f);
    int lw = ov_measure(lay_buf);
    ov_draw_text(px, stride, cx + cell_w - lw - 6, cy + g_ov_asc + 4,
                 stride, py0 + ph, lay_buf, 0x58, 0x5b, 0x70, 0xff);

    /* Pane count badge below header row */
    if(ws->pane_count > 0) {
      char cnt[8];
      snprintf(cnt, sizeof(cnt), "%d", ws->pane_count);
      ov_draw_text(px, stride, cx + 6,
                   cy + g_ov_asc + 4 + ROW_H,
                   stride, py0 + ph, cnt,
                   0x58, 0x5b, 0x70, 0xff);
    }

    /* Separator line under ws header */
    ov_fill_rect(px, stride, cx + 4, cy + ROW_H + 6, cell_w - 8, 1,
                 bc.r, bc.g, bc.b, 0x30, stride, py0 + ph);

    /* Pane list — uses remaining cell height below the separator */
    int list_top  = cy + ROW_H + 10;
    int list_bot  = cy + cell_h - 4;
    int line_h    = g_ov_th + 3;
    int max_lines = (list_bot - list_top) / line_h;

    for(int j = 0; j < ws->pane_count && j < max_lines; j++) {
      Pane *p = twm_pane_by_id(twm, ws->panes[j]);
      if(!p) continue;
      bool focused = ws->has_focused && ws->focused == p->id;

      /* Left pip for focused pane */
      if(focused)
        ov_fill_rect(px, stride, cx + 4, list_top + j * line_h,
                     2, line_h - 1, ac.r, ac.g, ac.b, 0xff, stride, py0 + ph);

      /* Window icon: 󰖯 focused, 󰖰 unfocused */
      const char *win_icon = focused ? "󰖯 " : "󰖰 ";
      int         icon_w   = ov_measure(win_icon);

      char title[64];
      strncpy(title, p->title[0] ? p->title : p->app_id, sizeof(title) - 1);
      int max_w = cell_w - 16 - icon_w;
      while(ov_measure(title) > max_w && strlen(title) > 3)
        title[strlen(title) - 1] = '\0';

      int     pty = list_top + j * line_h + g_ov_asc;
      uint8_t fr  = focused ? ac.r : 0xa6;
      uint8_t fg  = focused ? ac.g : 0xad;
      uint8_t fb  = focused ? ac.b : 0xc8;
      ov_draw_text(px, stride, cx + 10, pty,
                   stride, py0 + ph, win_icon, fr, fg, fb,
                   focused ? 0xff : 0x60);
      ov_draw_text(px, stride, cx + 10 + icon_w, pty,
                   stride, py0 + ph, title, fr, fg, fb,
                   focused ? 0xff : 0xcc);
    }

    /* "+N more" if truncated */
    int overflow = ws->pane_count - max_lines;
    if(overflow > 0 && max_lines >= 0) {
      char more[16];
      snprintf(more, sizeof(more), "+%d more", overflow);
      ov_draw_text(px, stride, cx + 10,
                   list_top + max_lines * line_h + g_ov_asc,
                   stride, py0 + ph, more, 0x45, 0x47, 0x5a, 0xff);
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
  run_configs_poll();
  Color ac = cfg->colors.active_border;
  Color bg = cfg->colors.pane_bg;
  int   y  = py0 + HEADER_H + PAD;

  /*
   * Column layout — fixed pixel widths anchored from left, making them
   * independent of panel width variations and always matching headers.
   *
   * We define each column's LEFT edge explicitly so headers and data rows
   * both use the same X coordinates — no offset drift.
   *
   *  PID(6ch)  NAME(24ch)        CPU%(6ch)  RSS(6ch)  GRAPH(fill)
   *  C0        C1                C2         C3        C4
   */
  int C0      = px0 + PAD;
  int C1      = C0  + 64;                /* 6 chars of PID */
  int C2      = C1  + (pw - PAD*2 - 64 - 200) * 55 / 100; /* after NAME col */
  int C3      = C2  + 72;               /* CPU% col width */
  int C4      = C3  + 72;               /* RSS  col width */
  int spark_w = px0 + pw - PAD - C4;   /* graph fills remaining width */
  if(spark_w < 40) spark_w = 40;

  /* Unified vertical text offset — same for headers AND data rows */
#define PROC_TY(row_y) ((row_y) + (ROW_H - g_ov_th) / 2 + g_ov_asc)

  /* ── Column headers ── */
  static const char *sort_labels[] = { "CPU%", "RSS", "PID" };
  char sort_ind[32];
  snprintf(sort_ind, sizeof(sort_ind), "sort:%s  s=cycle", sort_labels[g_proc_sort]);

  ov_draw_text(px, stride, C0, PROC_TY(y), stride, py0 + ph,
               "PID",     ac.r, ac.g, ac.b, 0x80);
  ov_draw_text(px, stride, C1, PROC_TY(y), stride, py0 + ph,
               "COMMAND", ac.r, ac.g, ac.b, 0x80);
  ov_draw_text(px, stride, C2, PROC_TY(y), stride, py0 + ph,
               "CPU%",    ac.r, ac.g, ac.b, 0x80);
  ov_draw_text(px, stride, C3, PROC_TY(y), stride, py0 + ph,
               "RSS",     ac.r, ac.g, ac.b, 0x80);
  ov_draw_text(px, stride, C4, PROC_TY(y), stride, py0 + ph,
               "GRAPH",   ac.r, ac.g, ac.b, 0x80);

  /* Sort indicator right-aligned */
  int si_w = ov_measure(sort_ind);
  ov_draw_text(px, stride, px0 + pw - PAD - si_w, PROC_TY(y), stride, py0 + ph,
               sort_ind, 0x58, 0x5b, 0x70, 0xff);

  /* Header underline */
  ov_fill_rect(px, stride, px0 + PAD, y + ROW_H - 2, pw - PAD * 2, 1,
               ac.r, ac.g, ac.b, 0x30, stride, py0 + ph);
  y += ROW_H;

  int visible_rows = (ph - (y - py0) - ROW_H - PAD) / ROW_H;
  if(o->cursor >= g_proc_count && g_proc_count > 0) o->cursor = g_proc_count - 1;

  int scroll = o->cursor - visible_rows + 1;
  if(scroll < 0) scroll = 0;

  for(int i = 0; i < g_proc_count && i < visible_rows; i++) {
    ProcEntry *pe  = &g_procs[i + scroll];
    bool       sel = (i + scroll == o->cursor);
    int        ry  = y + i * ROW_H;
    if(sel) draw_cursor_line(px, stride, px0, ry, pw, ac, bg, stride, py0 + ph);

    char pid_s[16], cpu_s[12], rss_s[12];
    snprintf(pid_s, sizeof(pid_s), "%d", pe->pid);
    snprintf(cpu_s, sizeof(cpu_s), "%.1f", pe->cpu_pct);
    if(pe->rss_kb >= 1024)
      snprintf(rss_s, sizeof(rss_s), "%ldM", pe->rss_kb / 1024);
    else
      snprintf(rss_s, sizeof(rss_s), "%ldK", pe->rss_kb);

    uint8_t nr = sel ? ac.r : 0xa6;
    uint8_t ng = sel ? ac.g : 0xad;
    uint8_t nb = sel ? ac.b : 0xc8;

    /* PID */
    ov_draw_text(px, stride, C0, PROC_TY(ry), stride, py0 + ph,
                 pid_s, 0x58, 0x5b, 0x70, 0xff);

    /* Command — truncate to fit column */
    {
      char comm[64];
      strncpy(comm, pe->comm, sizeof(comm) - 1);
      int avail = C2 - C1 - COL_GAP;
      while(ov_measure(comm) > avail && strlen(comm) > 3)
        comm[strlen(comm) - 1] = '\0';
      ov_draw_text(px, stride, C1, PROC_TY(ry), stride, py0 + ph,
                   comm, nr, ng, nb, 0xff);
    }

    /* CPU% — colour by heat */
    uint8_t cr = pe->cpu_pct > 50.f ? 0xf3 : (pe->cpu_pct > 20.f ? 0xf9 : 0xa6);
    uint8_t cg = pe->cpu_pct > 50.f ? 0x8b : (pe->cpu_pct > 20.f ? 0xe2 : 0xe3);
    uint8_t cb = pe->cpu_pct > 50.f ? 0xa8 : (pe->cpu_pct > 20.f ? 0xaf : 0xa1);
    ov_draw_text(px, stride, C2, PROC_TY(ry), stride, py0 + ph,
                 cpu_s, cr, cg, cb, 0xff);

    /* RSS */
    ov_draw_text(px, stride, C3, PROC_TY(ry), stride, py0 + ph,
                 rss_s, 0xa6, 0xe3, 0xa1, 0xff);

    /* Sparkline — fills the GRAPH column */
    {
      int   bar_count = pe->hist_count < PROC_HIST ? pe->hist_count : PROC_HIST;
      int   bar_w     = spark_w / (PROC_HIST + 1);
      if(bar_w < 2) bar_w = 2;
      int   bar_max   = ROW_H - 4;
      int   hist_start = (pe->hist_head - bar_count + PROC_HIST * 2) % PROC_HIST;
      float hmax       = 0.1f;
      for(int hi = 0; hi < bar_count; hi++) {
        float v = pe->cpu_hist[(hist_start + hi) % PROC_HIST];
        if(v > hmax) hmax = v;
      }
      for(int hi = 0; hi < bar_count; hi++) {
        float   v    = pe->cpu_hist[(hist_start + hi) % PROC_HIST] / hmax;
        int     bh   = (int)(v * bar_max);
        if(bh < 1) bh = 1;
        int     bx   = C4 + hi * (bar_w + 1);
        int     by   = ry + ROW_H - 2 - bh;
        float   pct  = pe->cpu_hist[(hist_start + hi) % PROC_HIST];
        uint8_t br   = pct > 50.f ? 0xf3 : (pct > 20.f ? 0xf9 : 0xa6);
        uint8_t bg2  = pct > 50.f ? 0x8b : (pct > 20.f ? 0xe2 : 0xe3);
        uint8_t bb   = pct > 50.f ? 0xa8 : (pct > 20.f ? 0xaf : 0xa1);
        ov_fill_rect(px, stride, bx, by, bar_w, bh,
                     br, bg2, bb, sel ? 0xff : 0xcc, stride, py0 + ph);
      }
    }
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
  Color ac = cfg->colors.active_border;
  int   y  = py0 + HEADER_H + PAD;

  /* Filter bar */
  char filter_disp[LOG_FILTER_MAX + 32];
  if(g_log_filter_mode)
    snprintf(filter_disp, sizeof(filter_disp), "/ %s_", g_log_filter);
  else if(g_log_filter[0])
    snprintf(filter_disp,
             sizeof(filter_disp),
             "filter: %s  (/ edit  c clear-log)",
             g_log_filter);
  else
    snprintf(filter_disp, sizeof(filter_disp), "/ filter  c clear  j/k scroll");

  uint8_t fr = g_log_filter[0] ? 0xf9 : ac.r;
  uint8_t fg = g_log_filter[0] ? 0xe2 : ac.g;
  uint8_t fb = g_log_filter[0] ? 0xaf : ac.b;
  ov_draw_text(px,
               stride,
               px0 + PAD,
               y + g_ov_asc,
               stride,
               py0 + ph,
               filter_disp,
               fr,
               fg,
               fb,
               0xff);
  y += ROW_H;

  int visible_rows = (ph - (y - py0) - PAD) / ROW_H;

  /* Collect filtered indices */
  int indices[LOG_RING_SIZE];
  int idx_count = 0;
  for(int i = 0; i < g_log_ring.count && idx_count < LOG_RING_SIZE; i++) {
    const char *line = log_ring_get(i);
    if(g_log_filter[0] && !strstr(line, g_log_filter)) continue;
    indices[idx_count++] = i;
  }

  int scroll_base = idx_count - visible_rows - o->cursor;
  if(scroll_base < 0) scroll_base = 0;

  for(int i = 0; i < visible_rows; i++) {
    int fi = scroll_base + i;
    if(fi >= idx_count) break;
    const char *line = log_ring_get(indices[fi]);
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
  (void)pw;
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
  refresh_git();
  Color ac = cfg->colors.active_border;
  Color bg = cfg->colors.pane_bg;
  int   y  = py0 + HEADER_H + PAD;

  /* Branch + hint */
  char branch_label[GIT_LINE_MAX + 48];
  snprintf(branch_label,
           sizeof(branch_label),
           "  %s  r=refresh  s=stage  u=unstage  d=diff",  /* nf-dev-git_branch */
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

  int avail_h      = ph - (y - py0) - PAD;
  int visible_rows = avail_h / ROW_H;

  if(g_git.show_diff && g_git.diff_count > 0) {
    /* Split view: left = file list (40%), right = diff (60%) */
    int left_w  = pw * 2 / 5;
    int right_w = pw - left_w - PAD;
    int right_x = px0 + left_w + PAD;

    /* Vertical separator */
    ov_fill_rect(px,
                 stride,
                 px0 + left_w,
                 y,
                 1,
                 avail_h,
                 ac.r,
                 ac.g,
                 ac.b,
                 0x40,
                 stride,
                 py0 + ph);

    /* Left: file list */
    int scroll = o->cursor - visible_rows + 1;
    if(scroll < 0) scroll = 0;
    for(int i = 0; i < visible_rows && (i + scroll) < g_git.file_count; i++) {
      GitFile *gf  = &g_git.files[i + scroll];
      bool     sel = (i + scroll == o->cursor);
      int      ry  = y + i * ROW_H;
      if(sel)
        draw_cursor_line(px, stride, px0, ry, left_w, ac, bg, stride, py0 + ph);

      uint8_t xr, xg, xb;
      const char *git_icon;
      char xy0 = gf->xy[0], xy1 = gf->xy[1];
      if(xy0 == 'A') {
        git_icon = " "; xr = 0xa6; xg = 0xe3; xb = 0xa1; /* nf-fa-plus_circle — added */
      } else if(xy0 == 'M') {
        git_icon = " "; xr = 0xf9; xg = 0xe2; xb = 0xaf; /* nf-fa-pencil — modified */
      } else if(xy0 == 'D') {
        git_icon = " "; xr = 0xf3; xg = 0x8b; xb = 0xa8; /* nf-fa-minus_circle — deleted */
      } else if(xy0 == 'R') {
        git_icon = " "; xr = 0x89; xg = 0xdc; xb = 0xeb; /* nf-fa-exchange — renamed */
      } else if(xy0 == '?' && xy1 == '?') {
        git_icon = " "; xr = 0x58; xg = 0x5b; xb = 0x70; /* nf-fa-question_circle — untracked */
      } else if(xy0 == '!') {
        git_icon = " "; xr = 0x45; xg = 0x47; xb = 0x5a; /* nf-fa-ban — ignored */
      } else {
        git_icon = " "; xr = 0x58; xg = 0x5b; xb = 0x70; /* nf-fa-file_o — other */
      }

      /* Stage indicator pip */
      if(gf->staged)
        ov_fill_rect(px, stride, px0 + PAD, ry + 2, 2, ROW_H - 4,
                     ac.r, ac.g, ac.b, 0xff, stride, py0 + ph);

      ov_draw_text(px, stride, px0 + PAD + 6, ry + g_ov_asc + 2, stride, py0 + ph,
                   git_icon, sel ? ac.r : xr, sel ? ac.g : xg, sel ? ac.b : xb, 0xff);

      int icon_w = ov_measure(git_icon);
      /* Truncate path to fit pane */
      char pathbuf[GIT_LINE_MAX];
      strncpy(pathbuf, gf->path, sizeof(pathbuf) - 1);
      int avail = left_w - PAD * 2 - icon_w - 10;
      while(ov_measure(pathbuf) > avail && strlen(pathbuf) > 3)
        pathbuf[strlen(pathbuf) - 1] = '\0';
      ov_draw_text(px, stride, px0 + PAD + 6 + icon_w + 4, ry + g_ov_asc + 2,
                   stride, py0 + ph,
                   pathbuf, sel ? ac.r : 0xa6, sel ? ac.g : 0xad, sel ? ac.b : 0xc8, 0xff);
    }

    /* Right: diff content */
    int diff_scroll = 0; /* TODO: separate diff scroll */
    for(int i = 0; i < visible_rows && (i + diff_scroll) < g_git.diff_count; i++) {
      const char *dl = g_git.diff[i + diff_scroll];
      int         ry = y + i * ROW_H;
      uint8_t     lr = 0xa6, lg = 0xad, lb = 0xc8;
      if(dl[0] == '+' && dl[1] != '+') {
        lr = 0xa6;
        lg = 0xe3;
        lb = 0xa1;
      } else if(dl[0] == '-' && dl[1] != '-') {
        lr = 0xf3;
        lg = 0x8b;
        lb = 0xa8;
      } else if(dl[0] == '@') {
        lr = 0x89;
        lg = 0xdc;
        lb = 0xeb;
      } else if(strncmp(dl, "diff ", 5) == 0 || strncmp(dl, "index ", 6) == 0 ||
                strncmp(dl, "--- ", 4) == 0 || strncmp(dl, "+++ ", 4) == 0) {
        lr = ac.r;
        lg = ac.g;
        lb = ac.b;
      }
      /* Truncate long diff lines */
      char dtrunc[GIT_LINE_MAX];
      strncpy(dtrunc, dl, sizeof(dtrunc) - 1);
      while(ov_measure(dtrunc) > right_w - PAD && strlen(dtrunc) > 3)
        dtrunc[strlen(dtrunc) - 1] = '\0';
      ov_draw_text(px,
                   stride,
                   right_x + PAD,
                   ry + g_ov_asc + 2,
                   stride,
                   py0 + ph,
                   dtrunc,
                   lr,
                   lg,
                   lb,
                   0xff);
    }
  } else {
    /* Normal view: file list then log */
    int scroll = o->cursor - visible_rows + 1;
    if(scroll < 0) scroll = 0;

    /* File list section */
    int fi_rows =
        g_git.file_count < visible_rows ? g_git.file_count : visible_rows / 2;
    for(int i = 0; i < fi_rows && (i + scroll) < g_git.file_count; i++) {
      GitFile *gf  = &g_git.files[i + scroll];
      bool     sel = (i + scroll == o->cursor);
      int      ry  = y + i * ROW_H;
      if(sel) draw_cursor_line(px, stride, px0, ry, pw, ac, bg, stride, py0 + ph);

      uint8_t xr, xg, xb;
      const char *git_icon2;
      char xy0 = gf->xy[0], xy1 = gf->xy[1];
      if(xy0 == 'A' || xy1 == 'A') {
        git_icon2 = " "; xr = 0xa6; xg = 0xe3; xb = 0xa1;
      } else if(xy0 == 'M' || xy1 == 'M') {
        git_icon2 = " "; xr = 0xf9; xg = 0xe2; xb = 0xaf;
      } else if(xy0 == 'D' || xy1 == 'D') {
        git_icon2 = " "; xr = 0xf3; xg = 0x8b; xb = 0xa8;
      } else if(xy0 == 'R' || xy1 == 'R') {
        git_icon2 = " "; xr = 0x89; xg = 0xdc; xb = 0xeb;
      } else if(xy0 == '?' && xy1 == '?') {
        git_icon2 = " "; xr = 0x58; xg = 0x5b; xb = 0x70;
      } else {
        git_icon2 = " "; xr = 0x58; xg = 0x5b; xb = 0x70;
      }

      /* Staged indicator pip */
      if(gf->staged)
        ov_fill_rect(px, stride, px0 + PAD, ry + 2, 2, ROW_H - 4,
                     ac.r, ac.g, ac.b, 0xff, stride, py0 + ph);

      ov_draw_text(px, stride, px0 + PAD + 6, ry + g_ov_asc + 2, stride, py0 + ph,
                   git_icon2, sel ? ac.r : xr, sel ? ac.g : xg, sel ? ac.b : xb, 0xff);

      int iw2 = ov_measure(git_icon2);
      char pathbuf2[GIT_LINE_MAX];
      strncpy(pathbuf2, gf->path, sizeof(pathbuf2) - 1);
      int avail2 = pw - PAD * 2 - iw2 - 10;
      while(ov_measure(pathbuf2) > avail2 && strlen(pathbuf2) > 3)
        pathbuf2[strlen(pathbuf2) - 1] = '\0';
      ov_draw_text(px, stride, px0 + PAD + 6 + iw2 + 4, ry + g_ov_asc + 2,
                   stride, py0 + ph, pathbuf2,
                   sel ? ac.r : 0xa6, sel ? ac.g : 0xad, sel ? ac.b : 0xc8, 0xff);
    }

    /* Separator + log */
    int log_y = y + fi_rows * ROW_H + 4;
    ov_fill_rect(px,
                 stride,
                 px0 + PAD,
                 log_y - 2,
                 pw - PAD * 2,
                 1,
                 ac.r,
                 ac.g,
                 ac.b,
                 0x30,
                 stride,
                 py0 + ph);
    int log_rows = (ph - (log_y - py0) - PAD) / ROW_H;
    for(int i = 0; i < log_rows && i < g_git.line_count; i++) {
      const char *line = g_git.lines[i];
      int         ry   = log_y + i * ROW_H;
      uint8_t     lr = 0xa6, lg = 0xad, lb = 0xc8;
      if(strncmp(line, "Recent", 6) == 0) {
        lr = ac.r;
        lg = ac.g;
        lb = ac.b;
      }
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
  Color ac = cfg->colors.active_border;
  Color bg = cfg->colors.pane_bg;
  int   y  = py0 + HEADER_H + PAD;

  /* ── Command line ── */
  char cmd_display[BUILD_CMD_MAX + 32];
  if(o->build_cmd_editing)
    snprintf(cmd_display, sizeof(cmd_display), "cmd: %s_", o->build_cmd);
  else
    snprintf(cmd_display,
             sizeof(cmd_display),
             "cmd: %s  (b=edit  Enter=run  e=errors)",
             o->build_cmd[0] ? o->build_cmd : g_build.cmd);
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

  /* ── Build status ── */
  bool running = atomic_load(&g_build.running);
  if(running) {
    static const char *spin[] = { "󰪞", "󰪟", "󰪠", "󰪡", "󰪢", "󰪣", "󰪤", "󰪥" };
    int64_t            si     = (ov_now_ms() / 80) % 8;
    char               status[64];
    double             elapsed = (double)(ov_now_ms() - g_build.started_ms) / 1000.0;
    snprintf(status, sizeof(status), "%s  building...  %.1fs", spin[si], elapsed);
    ov_draw_text(px,
                 stride,
                 px0 + PAD,
                 y + g_ov_asc,
                 stride,
                 py0 + ph,
                 status,
                 0xf9,
                 0xe2,
                 0xaf,
                 0xff);
    y += ROW_H;
  } else if(g_build.done) {
    char   status[80];
    double elapsed = (double)(g_build.finished_ms - g_build.started_ms) / 1000.0;
    pthread_mutex_lock(&g_build.err_lock);
    int ec = g_build.err_count;
    pthread_mutex_unlock(&g_build.err_lock);
    snprintf(status,
             sizeof(status),
             "exit: %d  %.1fs  %d diagnostic%s",
             g_build.exit_code,
             elapsed,
             ec,
             ec == 1 ? "" : "s");
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
  }

  /* ── Separator ── */
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

  int visible_rows = (ph - (y - py0) - PAD) / ROW_H;

  /* ── Error list mode (toggled with 'e') ── */
  if(o->build_show_errors) {
    pthread_mutex_lock(&g_build.err_lock);
    int  ec = g_build.err_count;
    char hdr[64];
    snprintf(hdr,
             sizeof(hdr),
             "%d diagnostic%s  (Enter=jump  e=log)",
             ec,
             ec == 1 ? "" : "s");
    ov_draw_text(px,
                 stride,
                 px0 + PAD,
                 y + g_ov_asc,
                 stride,
                 py0 + ph,
                 hdr,
                 ac.r,
                 ac.g,
                 ac.b,
                 0xff);
    y += ROW_H;
    visible_rows--;

    int scroll = o->build_err_cursor - visible_rows + 1;
    if(scroll < 0) scroll = 0;

    for(int i = 0; i < visible_rows && (i + scroll) < ec; i++) {
      BuildError *be  = &g_build.errors[i + scroll];
      bool        sel = (i + scroll == o->build_err_cursor);
      int         ry  = y + i * ROW_H;
      if(sel) draw_cursor_line(px, stride, px0, ry, pw, ac, bg, stride, py0 + ph);

      uint8_t ir = be->is_warning ? 0xf9 : 0xf3;
      uint8_t ig = be->is_warning ? 0xe2 : 0x8b;
      uint8_t ib = be->is_warning ? 0xaf : 0xa8;
      ov_draw_text(px,
                   stride,
                   px0 + PAD + 4,
                   ry + g_ov_asc + 2,
                   stride,
                   py0 + ph,
                   be->is_warning ? "W" : "E",
                   ir,
                   ig,
                   ib,
                   0xff);

      char loc[128];
      if(be->line > 0)
        snprintf(loc, sizeof(loc), "%s:%d", be->file, be->line);
      else
        strncpy(loc, be->file[0] ? be->file : "(unknown)", sizeof(loc) - 1);
      ov_draw_text(px,
                   stride,
                   px0 + PAD + 20,
                   ry + g_ov_asc + 2,
                   stride,
                   py0 + ph,
                   loc,
                   0x89,
                   0xdc,
                   0xeb,
                   0xff);

      int loc_w = ov_measure(loc) + 24 + PAD;
      int msg_w = pw - loc_w - PAD * 2;
      if(msg_w > 40) {
        char msg[BUILD_ERR_LINE];
        strncpy(msg, be->msg, sizeof(msg) - 1);
        while(ov_measure(msg) > msg_w && strlen(msg) > 4)
          msg[strlen(msg) - 1] = '\0';
        ov_draw_text(px,
                     stride,
                     px0 + PAD + loc_w,
                     ry + g_ov_asc + 2,
                     stride,
                     py0 + ph,
                     msg,
                     sel ? ac.r : 0xa6,
                     sel ? ac.g : 0xad,
                     sel ? ac.b : 0xc8,
                     0xff);
      }
    }
    pthread_mutex_unlock(&g_build.err_lock);
    return;
  }

  /* ── Log preview (default) ── */
  int start = g_log_ring.count - visible_rows;
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
  Color ac = cfg->colors.active_border;
  int   y  = py0 + HEADER_H + PAD;

  /* ── Tab bar for notes ── */
  {
    int tx = px0 + PAD;
    for(int i = 0; i < g_notes_count; i++) {
      bool        sel = (i == g_notes_active);
      const char *nm  = g_notes_files[i].name;
      int         nw  = ov_measure(nm) + 10;
      if(sel) {
        ov_fill_rect(px,
                     stride,
                     tx - 2,
                     y,
                     nw + 4,
                     ROW_H,
                     ac.r,
                     ac.g,
                     ac.b,
                     0xff,
                     stride,
                     py0 + ph);
        ov_draw_text(px,
                     stride,
                     tx + 2,
                     y + g_ov_asc + 1,
                     stride,
                     py0 + ph,
                     nm,
                     cfg->colors.pane_bg.r,
                     cfg->colors.pane_bg.g,
                     cfg->colors.pane_bg.b,
                     0xff);
      } else {
        ov_fill_rect(px,
                     stride,
                     tx - 2,
                     y,
                     nw + 4,
                     ROW_H,
                     0x31,
                     0x32,
                     0x44,
                     0xff,
                     stride,
                     py0 + ph);
        ov_draw_text(px,
                     stride,
                     tx + 2,
                     y + g_ov_asc + 1,
                     stride,
                     py0 + ph,
                     nm,
                     0x58,
                     0x5b,
                     0x70,
                     0xff);
      }
      tx += nw + 6;
    }
  }
  y += ROW_H + 2;

  /* ── Hint / mode line ── */
  const char *hint;
  uint8_t     hr, hg, hb;
  if(g_notes_rename) {
    hint = "rename/new:  (Enter=confirm  Esc=cancel)";
    hr   = 0xf9;
    hg   = 0xe2;
    hb   = 0xaf;
  } else if(g_notes_edit) {
    hint = "INSERT — Esc saves";
    hr   = 0xa6;
    hg   = 0xe3;
    hb   = 0xa1;
  } else {
    hint = "e=edit  n=rename  N=new  d=del  [/]=switch  Markdown rendered";
    hr   = ac.r;
    hg   = ac.g;
    hb   = ac.b;
  }
  ov_draw_text(
      px, stride, px0 + PAD, y + g_ov_asc, stride, py0 + ph, hint, hr, hg, hb, 0xff);
  y += ROW_H;

  /* ── Rename input box ── */
  if(g_notes_rename) {
    char rbuf[NOTES_NAME_MAX + 8];
    snprintf(rbuf, sizeof(rbuf), "> %s_", g_notes_rename_buf);
    ov_draw_text(px,
                 stride,
                 px0 + PAD,
                 y + g_ov_asc,
                 stride,
                 py0 + ph,
                 rbuf,
                 0xf9,
                 0xe2,
                 0xaf,
                 0xff);
    y += ROW_H;
  }

  /* ── Separator ── */
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

  /* ── Render note content with Markdown ── */
  NoteFile *nf = cur_note();
  if(!nf) return;

  int  visible_rows = (ph - (y - py0) - PAD - ROW_H) / ROW_H;
  int  line_idx     = 0;
  char line_buf[512];
  int  buf_i         = 0;
  int  row           = 0;
  bool in_code_block = false;

  /* Count total lines for scroll-to-bottom in edit mode */
  int total_lines = 1;
  for(int ci = 0; ci < nf->len; ci++)
    if(nf->text[ci] == '\n') total_lines++;
  int skip = 0;
  if(g_notes_edit) {
    skip = total_lines - visible_rows;
    if(skip < 0) skip = 0;
  } else {
    skip = o->scroll;
    if(skip > total_lines - visible_rows) skip = total_lines - visible_rows;
    if(skip < 0) skip = 0;
  }

  for(int ci = 0; ci <= nf->len; ci++) {
    char c = ci < nf->len ? nf->text[ci] : '\0';
    if(c == '\n' || c == '\0') {
      line_buf[buf_i] = '\0';

      /* Toggle code-block fence */
      if(strncmp(line_buf, "```", 3) == 0) in_code_block = !in_code_block;

      if(line_idx >= skip && row < visible_rows) {
        MdStyle     ms     = in_code_block ? MD_CODE : md_line_style(line_buf);
        const char *disp   = md_display(line_buf, ms);
        int         indent = 0;
        if(ms == MD_BULLET) indent = 8; /* extra indent for bullets */
        /* Bullet marker */
        if(ms == MD_BULLET) {
          ov_draw_text(px,
                       stride,
                       px0 + PAD - 2,
                       y + row * ROW_H + g_ov_asc + 2,
                       stride,
                       py0 + ph,
                       "•",
                       0xf9,
                       0xe2,
                       0xaf,
                       0xff);
        }
        /* Horizontal rule */
        if(ms == MD_HR) {
          int ry = y + row * ROW_H + (ROW_H / 2);
          ov_fill_rect(px,
                       stride,
                       px0 + PAD,
                       ry,
                       pw - PAD * 2,
                       1,
                       0x58,
                       0x5b,
                       0x70,
                       0xff,
                       stride,
                       py0 + ph);
        } else {
          uint8_t lr, lg, lb;
          md_colour(ms, in_code_block && ms != MD_CODE, &lr, &lg, &lb);

          /* Code-block background tint */
          if(in_code_block || ms == MD_CODE) {
            ov_fill_rect(px,
                         stride,
                         px0 + PAD,
                         y + row * ROW_H,
                         pw - PAD * 2,
                         ROW_H,
                         0x18,
                         0x18,
                         0x25,
                         0x80,
                         stride,
                         py0 + ph);
          }

          /* H1 size simulation: draw twice offset by 1 px */
          int draw_x = px0 + PAD + indent;
          int draw_y = y + row * ROW_H + g_ov_asc + 2;
          if(ms == MD_H1) {
            ov_draw_text(px,
                         stride,
                         draw_x + 1,
                         draw_y,
                         stride,
                         py0 + ph,
                         disp,
                         lr,
                         lg,
                         lb,
                         0x80);
          }
          ov_draw_text(
              px, stride, draw_x, draw_y, stride, py0 + ph, disp, lr, lg, lb, 0xff);
        }
        row++;
      }
      line_idx++;
      buf_i = 0;
    } else if(buf_i < 511) {
      line_buf[buf_i++] = c;
    }
  }

  /* ── Blinking cursor in edit mode ── */
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

/* ── [8] Search ───────────────────────────────────────────────────────── */
static void draw_panel_search(uint32_t      *px,
                              int            stride,
                              int            px0,
                              int            py0,
                              int            pw,
                              int            ph,
                              TrixieOverlay *o,
                              const Config  *cfg) {
  search_poll(); /* swap in fresh results */
  Color ac = cfg->colors.active_border;
  Color bg = cfg->colors.pane_bg;
  int   y  = py0 + HEADER_H + PAD;

  /* Query bar */
  char        qdisp[SEARCH_QUERY_MAX + 48];
  const char *mode_tag = g_search.file_only ? "[files] " : "[text]  ";
  if(o->search_active)
    snprintf(qdisp, sizeof(qdisp), "%s> %s_", mode_tag, g_search.query);
  else if(g_search.query[0])
    snprintf(qdisp,
             sizeof(qdisp),
             "%s  %s  (/ to edit  f=toggle mode  Enter=open)",
             mode_tag,
             g_search.query);
  else
    snprintf(qdisp,
             sizeof(qdisp),
             "%s  / to search  f=toggle files/text  Enter=open",
             mode_tag);

  uint8_t qr = o->search_active ? 0xf9 : ac.r;
  uint8_t qg = o->search_active ? 0xe2 : ac.g;
  uint8_t qb = o->search_active ? 0xaf : ac.b;
  ov_draw_text(px,
               stride,
               px0 + PAD,
               y + g_ov_asc,
               stride,
               py0 + ph,
               qdisp,
               qr,
               qg,
               qb,
               0xff);
  y += ROW_H;

  /* Result count */
  if(g_search.running) {
    ov_draw_text(px,
                 stride,
                 px0 + PAD,
                 y + g_ov_asc,
                 stride,
                 py0 + ph,
                 "searching...",
                 0xf9,
                 0xe2,
                 0xaf,
                 0xff);
  } else if(g_search.result_count > 0) {
    char cnt[48];
    snprintf(cnt,
             sizeof(cnt),
             "%d result%s",
             g_search.result_count,
             g_search.result_count == 1 ? "" : "s");
    ov_draw_text(px,
                 stride,
                 px0 + PAD,
                 y + g_ov_asc,
                 stride,
                 py0 + ph,
                 cnt,
                 0x58,
                 0x5b,
                 0x70,
                 0xff);
  }
  y += ROW_H;

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

  int visible_rows = (ph - (y - py0) - PAD) / ROW_H;
  if(o->cursor >= g_search.result_count && g_search.result_count > 0)
    o->cursor = g_search.result_count - 1;

  int scroll = o->cursor - visible_rows + 1;
  if(scroll < 0) scroll = 0;

  for(int i = 0; i < visible_rows && (i + scroll) < g_search.result_count; i++) {
    SearchResult *sr  = &g_search.results[i + scroll];
    bool          sel = (i + scroll == o->cursor);
    int           ry  = y + i * ROW_H;
    if(sel) draw_cursor_line(px, stride, px0, ry, pw, ac, bg, stride, py0 + ph);

    /* file:line */
    char loc[300];
    if(sr->line > 0)
      snprintf(loc, sizeof(loc), "%s:%d", sr->file, sr->line);
    else
      strncpy(loc, sr->file, sizeof(loc) - 1);

    ov_draw_text(px,
                 stride,
                 px0 + PAD + 4,
                 ry + g_ov_asc + 2,
                 stride,
                 py0 + ph,
                 loc,
                 0x89,
                 0xdc,
                 0xeb,
                 0xff);

    if(sr->text[0]) {
      int  lw = ov_measure(loc) + PAD + 8;
      char txt[SEARCH_LINE_MAX];
      strncpy(txt, sr->text, sizeof(txt) - 1);
      /* strip leading whitespace */
      char *tp = txt;
      while(*tp == ' ' || *tp == '\t')
        tp++;
      int avail = pw - lw - PAD * 2;
      while(ov_measure(tp) > avail && strlen(tp) > 3)
        tp[strlen(tp) - 1] = '\0';
      ov_draw_text(px,
                   stride,
                   px0 + PAD + lw,
                   ry + g_ov_asc + 2,
                   stride,
                   py0 + ph,
                   tp,
                   sel ? ac.r : 0xa6,
                   sel ? ac.g : 0xad,
                   sel ? ac.b : 0xc8,
                   0xff);
    }
  }
}

/* ── [0] Deps ─────────────────────────────────────────────────────────── */
static void draw_panel_deps(uint32_t      *px,
                            int            stride,
                            int            px0,
                            int            py0,
                            int            pw,
                            int            ph,
                            TrixieOverlay *o,
                            const Config  *cfg) {
  deps_load();
  Color ac = cfg->colors.active_border;
  Color bg = cfg->colors.pane_bg;
  int   y  = py0 + HEADER_H + PAD;

  static const char *lang_names[] = { "?", "Rust", "Go", "Maven", "Gradle" };
  const char        *ln = (g_deps.lang < 5) ? lang_names[g_deps.lang] : "?";
  char               hdr[128];
  if(g_deps.checking)
    snprintf(
        hdr, sizeof(hdr), "%s — %d deps  checking outdated...", ln, g_deps.count);
  else
    snprintf(hdr,
             sizeof(hdr),
             "%s — %d deps  u=check outdated  r=refresh",
             ln,
             g_deps.count);
  ov_draw_text(px,
               stride,
               px0 + PAD,
               y + g_ov_asc,
               stride,
               py0 + ph,
               hdr,
               ac.r,
               ac.g,
               ac.b,
               0xff);
  y += ROW_H;

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

  /* Column headers */
  ov_draw_text(px,
               stride,
               px0 + PAD,
               y + g_ov_asc,
               stride,
               py0 + ph,
               "PACKAGE",
               ac.r,
               ac.g,
               ac.b,
               0xff);
  ov_draw_text(px,
               stride,
               px0 + pw - PAD - 160,
               y + g_ov_asc,
               stride,
               py0 + ph,
               "CURRENT",
               ac.r,
               ac.g,
               ac.b,
               0xff);
  ov_draw_text(px,
               stride,
               px0 + pw - PAD - 64,
               y + g_ov_asc,
               stride,
               py0 + ph,
               "LATEST",
               ac.r,
               ac.g,
               ac.b,
               0xff);
  y += ROW_H;

  int visible_rows = (ph - (y - py0) - PAD) / ROW_H;
  if(o->cursor >= g_deps.count && g_deps.count > 0) o->cursor = g_deps.count - 1;
  int scroll = o->cursor - visible_rows + 1;
  if(scroll < 0) scroll = 0;

  for(int i = 0; i < visible_rows && (i + scroll) < g_deps.count; i++) {
    DepEntry *de  = &g_deps.entries[i + scroll];
    bool      sel = (i + scroll == o->cursor);
    int       ry  = y + i * ROW_H;
    if(sel) draw_cursor_line(px, stride, px0, ry, pw, ac, bg, stride, py0 + ph);

    uint8_t nr = sel ? ac.r : 0xa6;
    uint8_t ng = sel ? ac.g : 0xad;
    uint8_t nb = sel ? ac.b : 0xc8;
    if(de->outdated) {
      nr = 0xf9;
      ng = 0xe2;
      nb = 0xaf;
    } /* yellow if outdated */

    ov_draw_text(px,
                 stride,
                 px0 + PAD + 4,
                 ry + g_ov_asc + 2,
                 stride,
                 py0 + ph,
                 de->name,
                 nr,
                 ng,
                 nb,
                 0xff);
    ov_draw_text(px,
                 stride,
                 px0 + pw - PAD - 160,
                 ry + g_ov_asc + 2,
                 stride,
                 py0 + ph,
                 de->version,
                 0x58,
                 0x5b,
                 0x70,
                 0xff);
    if(de->latest[0]) {
      uint8_t lr = de->outdated ? 0xa6 : 0x58;
      uint8_t lg = de->outdated ? 0xe3 : 0x5b;
      uint8_t lb = de->outdated ? 0xa1 : 0x70;
      ov_draw_text(px,
                   stride,
                   px0 + pw - PAD - 64,
                   ry + g_ov_asc + 2,
                   stride,
                   py0 + ph,
                   de->latest,
                   lr,
                   lg,
                   lb,
                   0xff);
    } else if(g_deps.checking) {
      ov_draw_text(px,
                   stride,
                   px0 + pw - PAD - 64,
                   ry + g_ov_asc + 2,
                   stride,
                   py0 + ph,
                   "...",
                   0x58,
                   0x5b,
                   0x70,
                   0xff);
    }
  }
}


void overlay_update(TrixieOverlay *o,
                    TwmState      *twm,
                    const Config  *cfg,
                    BarWorkerPool *pool) {
  if(!o || !o->visible) return;
  (void)pool;
  int w = o->w, h = o->h;
  if(w <= 0 || h <= 0) return;

  /* Process deferred workspace switch (queued by key handler, executed here
   * where we have TwmState access).                                          */
  if(o->pending_ws_switch >= 0 && twm) {
    int ws = o->pending_ws_switch;
    o->pending_ws_switch = -1;
    if(ws < twm->ws_count) twm_switch_ws(twm, ws);
  }

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

  /* ── Backdrop: solid fill + vignette edges ────────────────────────────── */
  ov_fill_rect(px, w, 0, 0, w, h, bg.r, bg.g, bg.b, 0xff, w, h);
  /* Vignette: 32px dark bands on all four edges */
  {
    int v = 32;
    ov_fill_rect(px, w, 0,     0,     w, v, 0, 0, 0, 0x60, w, h);
    ov_fill_rect(px, w, 0,     h - v, w, v, 0, 0, 0, 0x60, w, h);
    ov_fill_rect(px, w, 0,     0,     v, h, 0, 0, 0, 0x60, w, h);
    ov_fill_rect(px, w, w - v, 0,     v, h, 0, 0, 0, 0x60, w, h);
  }

  /* ── Content panel: 96% of screen, snapped to pixel grid ─────────────── */
  int pw  = w * 24 / 25;
  int ph  = h * 24 / 25;
  int px0 = (w - pw) / 2;
  int py0 = (h - ph) / 2;

  /* Panel background — slightly lighter than backdrop */
  uint8_t sr = (uint8_t)((int)bg.r + 0x0c < 0xff ? bg.r + 0x0c : 0xff);
  uint8_t sg = (uint8_t)((int)bg.g + 0x0c < 0xff ? bg.g + 0x0c : 0xff);
  uint8_t sb = (uint8_t)((int)bg.b + 0x0c < 0xff ? bg.b + 0x0c : 0xff);
  ov_fill_rect(px, w, px0, py0, pw, ph, sr, sg, sb, 0xff, w, h);

  /* Outer glow — always 2px feathered halo regardless of border_width */
  ov_fill_border(px, w, px0 - 2, py0 - 2, pw + 4, ph + 4,
                 ac.r, ac.g, ac.b, 0x28, w, h);
  ov_fill_border(px, w, px0 - 1, py0 - 1, pw + 2, ph + 2,
                 ac.r, ac.g, ac.b, 0x50, w, h);
  /* Solid border — respects cfg->border_width */
  {
    int bw = cfg->border_width > 0 ? cfg->border_width : 1;
    for(int t = 0; t < bw; t++) {
      ov_fill_rect(px, w, px0+t,        py0+t,        pw-t*2, 1,      ac.r, ac.g, ac.b, 0xff, w, h);
      ov_fill_rect(px, w, px0+t,        py0+ph-1-t,   pw-t*2, 1,      ac.r, ac.g, ac.b, 0xff, w, h);
      ov_fill_rect(px, w, px0+t,        py0+t,        1,      ph-t*2, ac.r, ac.g, ac.b, 0xff, w, h);
      ov_fill_rect(px, w, px0+pw-1-t,   py0+t,        1,      ph-t*2, ac.r, ac.g, ac.b, 0xff, w, h);
    }
  }

  /* ── Tab bar ──────────────────────────────────────────────────────────── */
  int tab_bar_h = HEADER_H - 2;
  int tab_y     = py0 + 1;
  int tab_text  = tab_y + g_ov_asc + ((tab_bar_h - g_ov_th) / 2);

  /* "trixie" logo — dim, left of separator */
  ov_draw_text(px, w, px0 + PAD, tab_text, w, h,
               "trixie", ac.r, ac.g, ac.b, 0x70);
  int sep_x = px0 + PAD + ov_measure("trixie") + 6;
  ov_fill_rect(px, w, sep_x, tab_y + 3, 1, tab_bar_h - 6,
               ac.r, ac.g, ac.b, 0x35, w, h);
  sep_x += 8;

  /* Tab pills */
  static const char *panel_labels[PANEL_COUNT] = {
    "Workspaces", "Commands", "Processes", "Log",   "Git",
    "Build",      "Notes",    "Search",    "Run",   "Deps",  "Files",
    "Nvim",       "LSP"
  };
  static const char *panel_keys[PANEL_COUNT] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "F", "N", "L"
  };

  int tab_x = sep_x;
  for(int i = 0; i < PANEL_COUNT; i++) {
    bool        sel   = (i == (int)o->panel);
    const char *label = panel_labels[i];
    const char *key   = panel_keys[i];
    /* For the LSP tab, append live badge count */
    char label_buf[48];
    if(i == PANEL_LSP) {
      snprintf(label_buf, sizeof(label_buf), "%s%s", label, lsp_tab_badge());
      label = label_buf;
    }
    int         kw    = ov_measure(key);
    int         lw    = ov_measure(label);
    int         pill  = kw + 6 + lw + PAD;

    if(sel) {
      /* Filled pill for selected tab */
      ov_fill_rect(px, w, tab_x, tab_y, pill, tab_bar_h,
                   ac.r, ac.g, ac.b, 0xff, w, h);
      /* Key number — inverted, dimmed */
      ov_draw_text(px, w, tab_x + 4, tab_text, w, h,
                   key, bg.r, bg.g, bg.b, 0xa0);
      /* Label — inverted, full alpha */
      ov_draw_text(px, w, tab_x + 4 + kw + 6, tab_text, w, h,
                   label, bg.r, bg.g, bg.b, 0xff);
      /* 2px accent underline at bottom of pill */
      ov_fill_rect(px, w, tab_x, tab_y + tab_bar_h - 2, pill, 2,
                   bg.r, bg.g, bg.b, 0x40, w, h);
    } else {
      /* Key number — accent dim */
      ov_draw_text(px, w, tab_x + 4, tab_text, w, h,
                   key, ac.r, ac.g, ac.b, 0x55);
      /* Label — text dim */
      ov_draw_text(px, w, tab_x + 4 + kw + 6, tab_text, w, h,
                   label, 0x6e, 0x73, 0x87, 0xff);
    }
    tab_x += pill + 4;
  }

  /* Rule under tab bar */
  ov_fill_rect(px, w, px0, py0 + HEADER_H - 1, pw, 1,
               ac.r, ac.g, ac.b, 0xff, w, h);

  /* Active panel */
  switch(o->panel) {
    case PANEL_WORKSPACES:
      draw_panel_workspaces(px, w, px0, py0, pw, ph, twm, o->ws_cursor, cfg);
      break;
    case PANEL_COMMANDS: draw_panel_commands(px, w, px0, py0, pw, ph, o, cfg); break;
    case PANEL_PROCESSES:
      draw_panel_processes(px, w, px0, py0, pw, ph, o, cfg);
      break;
    case PANEL_LOG: draw_panel_log(px, w, px0, py0, pw, ph, o, cfg); break;
    case PANEL_GIT: draw_panel_git(px, w, px0, py0, pw, ph, o, cfg); break;
    case PANEL_BUILD: draw_panel_build(px, w, px0, py0, pw, ph, o, cfg); break;
    case PANEL_NOTES: draw_panel_notes(px, w, px0, py0, pw, ph, o, cfg); break;
    case PANEL_SEARCH: draw_panel_search(px, w, px0, py0, pw, ph, o, cfg); break;
    case PANEL_RUN: draw_panel_run(px, w, px0, py0, pw, ph, &o->cursor, cfg); break;
    case PANEL_DEPS:  draw_panel_deps(px, w, px0, py0, pw, ph, o, cfg); break;
    case PANEL_FILES:
      draw_panel_files(px, w, px0, py0, pw, ph,
                       &o->cursor,
                       o->fb_cwd, sizeof(o->fb_cwd),
                       o->fb_filter, &o->fb_filter_len, &o->fb_filter_mode,
                       cfg);
      break;
    case PANEL_NVIM:
      nvim_panel_poll(&cfg->overlay);
      draw_panel_nvim(px, w, px0, py0, pw, ph,
                      &o->nvim_cursor, cfg, &cfg->overlay);
      break;
    case PANEL_LSP:
      lsp_panel_tick(&cfg->overlay);
      draw_panel_lsp(px, w, px0, py0, pw, ph,
                     &o->lsp_cursor, cfg, &cfg->overlay);
      break;
    default: break;
  }

  /* ── Mode line — DE status + key hints ───────────────────────────────── */
  {
    int ml_h = ROW_H;
    int ml_y = py0 + ph - ml_h;
    ov_fill_rect(px, w, px0, ml_y - 1, pw, 1, ac.r, ac.g, ac.b, 0x60, w, h);

    /* Slightly darker tint for mode line */
    uint8_t ml_r = (uint8_t)(bg.r > 8 ? bg.r - 8 : 0);
    uint8_t ml_g = (uint8_t)(bg.g > 8 ? bg.g - 8 : 0);
    uint8_t ml_b = (uint8_t)(bg.b > 8 ? bg.b - 8 : 0);
    ov_fill_rect(px, w, px0, ml_y, pw, ml_h, ml_r, ml_g, ml_b, 0xff, w, h);

    int ml_text = ml_y + g_ov_asc + ((ml_h - g_ov_th) / 2);

    /* ── Left: key hints for current panel ── */
    static const char *hints[PANEL_COUNT] = {
      "j/k scroll  Tab next panel",
      "/ filter  j/k navigate  Enter exec",
      "j/k select  Enter=SIGTERM  K=SIGKILL  s=sort",
      "/ filter  c clear  j/k scroll",
      "r refresh  d diff  s stage  u unstage",
      "b=cmd  Enter=run  e=errors  j/k errors",
      "e=edit  n=rename  N=new  d=del  [/]=note",
      "/ search  f=files/text  Enter=open  j/k navigate",
      "Enter=start/stop  Tab=output  a=add  d=del  c=clear",
      "u=check outdated  r=refresh  j/k navigate",
      "j/k=navigate  Enter/l=open  h=up  /=filter  .=hidden  o=xdg-open  Tab=preview",
      "j/k navigate  Enter=open  r=refresh",
      "j/k navigate  Enter=jump  e/w/i=filter-sev  f=sort-file  g=group",
    };
    const char *hint = (o->panel < PANEL_COUNT) ? hints[o->panel] : "";
    ov_draw_text(px, w, px0 + PAD, ml_text, w, h,
                 hint, fg_.r, fg_.g, fg_.b, 0x80);

    /* ── Right: live DE status — ws | app | layout ── */
    if(twm && twm->ws_count > 0) {
      Workspace *ws   = &twm->workspaces[twm->active_ws];
      Pane      *fp   = twm_focused(twm);

      /* "󰮯 N" — monitor icon + workspace number */
      char ws_buf[24];
      snprintf(ws_buf, sizeof(ws_buf), "󰮯 %d", twm->active_ws + 1);

      /* focused app name (title preferred, fall back to app_id) */
      const char *app = "";
      if(fp) app = fp->title[0] ? fp->title : fp->app_id;

      /* layout + pane count with  grid icon */
      char lay_buf[48];
      snprintf(lay_buf, sizeof(lay_buf), "󰕰  %s  %d",  /* nf-md-view_grid */
               layout_label(ws->layout),
               ws->pane_count);

      /* Measure right-to-left so we can right-align */
      int lay_w = ov_measure(lay_buf);
      int sep_w = ov_measure("  │  ");
      int app_w = ov_measure(app);
      int ws_w  = ov_measure(ws_buf);

      int rx = px0 + pw - PAD;

      /* Layout */
      rx -= lay_w;
      ov_draw_text(px, w, rx, ml_text, w, h,
                   lay_buf, 0x89, 0xdc, 0xeb, 0xa0);
      rx -= sep_w;
      ov_draw_text(px, w, rx, ml_text, w, h,
                   "  │  ", ac.r, ac.g, ac.b, 0x30);

      /* App name — truncate if too long */
      if(app_w > 0) {
        char app_trunc[256];
        strncpy(app_trunc, app, sizeof(app_trunc) - 1);
        int avail = rx - (px0 + PAD + ov_measure(hint) + PAD * 2);
        while(ov_measure(app_trunc) > avail && strlen(app_trunc) > 3)
          app_trunc[strlen(app_trunc) - 1] = '\0';
        rx -= ov_measure(app_trunc);
        ov_draw_text(px, w, rx, ml_text, w, h,
                     app_trunc, 0xcd, 0xd6, 0xf4, 0xd0);
        rx -= sep_w;
        ov_draw_text(px, w, rx, ml_text, w, h,
                     "  │  ", ac.r, ac.g, ac.b, 0x30);
      }

      /* Workspace */
      rx -= ws_w;
      /* Urgency tint */
      bool urgent = (twm->ws_urgent_mask >> twm->active_ws) & 1;
      uint8_t wr = urgent ? 0xf3 : ac.r;
      uint8_t wg = urgent ? 0x8b : ac.g;
      uint8_t wb = urgent ? 0xa8 : ac.b;
      ov_draw_text(px, w, rx, ml_text, w, h,
                   ws_buf, wr, wg, wb, 0xff);
    }
  }

  wlr_scene_buffer_set_buffer(o->scene_buf, &rb->base);
  wlr_buffer_drop(&rb->base);
}
