/* run_panel.c — Run-configurations panel for the Trixie overlay.
 *
 * Also contains:
 *   - run_configs_init(const OverlayCfg *)  — config-aware preset detection
 *   - nvim_get_diag_snapshot()              — bridge for lsp_panel
 *
 * Owns everything related to the [9] Run tab:
 *   - RunConfig struct + global state
 *   - stdout/stderr capture via pipe + reader thread (the main fix)
 *   - Per-config output ring buffer so each process has its own tail log
 *   - Auto-detect presets: Cargo, Go, Maven, Gradle, Python, Make
 *   - draw_panel_run()  — two-pane layout: config list (top) + output (bottom)
 *   - run_panel_key()   — key handler, called from overlay_key()
 *   - run_configs_init(), run_configs_poll() — lifecycle helpers
 *
 * Layout (two-pane split inside the panel content area):
 *
 *   ┌─────────────────────────────────────────────────────┐
 *   │  ● cargo run          cargo run 2>&1       12.3s    │  ← config list
 *   │  ○ cargo test         cargo test 2>&1               │    (upper ~40%)
 *   │  ○ go run .           go run . 2>&1                  │
 *   ├─────────────────────────────────────────────────────┤
 *   │  ── stdout: cargo run ──────────────────────────────│  ← output pane
 *   │  Compiling trixie v0.1.0                            │    (lower ~60%)
 *   │     Finished dev [unoptimized] target(s) in 4.21s  │
 *   │  Running `target/debug/trixie`                      │
 *   └─────────────────────────────────────────────────────┘
 *
 * Keys
 * ────
 *   j / k        navigate config list
 *   Enter        start / stop selected config
 *   Tab          focus output pane / back
 *   a            add new config (name → Tab → cmd → Enter)
 *   d            delete selected config
 *   c            clear output of selected config
 *   Esc          cancel add-form
 */

#define _POSIX_C_SOURCE 200809L
#include "nvim_panel.h"
#include "overlay_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Per-process output ring
 * ═══════════════════════════════════════════════════════════════════════════ */

#define OUT_RING_SIZE 256
#define OUT_LINE_MAX  512

typedef struct {
  char lines[OUT_RING_SIZE][OUT_LINE_MAX];
  int  head;
  int  count;
  int  scroll; /* lines scrolled up from bottom; 0 = follow tail */
} OutRing;

static void out_ring_push(OutRing *r, const char *line) {
  strncpy(r->lines[r->head], line, OUT_LINE_MAX - 1);
  r->lines[r->head][OUT_LINE_MAX - 1] = '\0';
  r->head                             = (r->head + 1) % OUT_RING_SIZE;
  if(r->count < OUT_RING_SIZE) r->count++;
}

static const char *out_ring_get(const OutRing *r, int idx) {
  if(idx < 0 || idx >= r->count) return "";
  int real = (r->head - r->count + idx + OUT_RING_SIZE * 2) % OUT_RING_SIZE;
  return r->lines[real];
}

static void out_ring_clear(OutRing *r) {
  r->head = r->count = r->scroll = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  RunConfig
 * ═══════════════════════════════════════════════════════════════════════════ */

#define RUN_CONFIGS_MAX 16
#define RUN_CMD_MAX     256
#define RUN_NAME_MAX    48

typedef struct {
  char name[RUN_NAME_MAX];
  char cmd[RUN_CMD_MAX];

  pid_t   pid;
  bool    running;
  int64_t started_ms;
  int     exit_code;
  bool    exited;

  /* Stdout/stderr capture */
  int       pipe_read_fd; /* -1 when not running */
  pthread_t reader_thread;
  bool      reader_valid;

  pthread_mutex_t out_lock;
  OutRing         out;
} RunConfig;

RunConfig g_run_configs[RUN_CONFIGS_MAX];
int       g_run_count = 0;

/* ── Reader thread: drains pipe_read_fd into out ring ───────────────────── */
static void *reader_thread_fn(void *arg) {
  RunConfig *rc = (RunConfig *)arg;
  int        fd = rc->pipe_read_fd;
  char       buf[OUT_LINE_MAX];
  int        pos = 0;

  while(true) {
    char    c;
    ssize_t n = read(fd, &c, 1);
    if(n <= 0) break; /* EOF or error — process exited, pipe closed */

    if(c == '\n' || c == '\r') {
      if(pos > 0) {
        buf[pos] = '\0';
        pthread_mutex_lock(&rc->out_lock);
        out_ring_push(&rc->out, buf);
        log_ring_push(buf); /* mirror to global log */
        pthread_mutex_unlock(&rc->out_lock);
        pos = 0;
      }
    } else if(pos < OUT_LINE_MAX - 1) {
      buf[pos++] = c;
    } else {
      /* Line too long — flush what we have */
      buf[pos] = '\0';
      pthread_mutex_lock(&rc->out_lock);
      out_ring_push(&rc->out, buf);
      log_ring_push(buf);
      pthread_mutex_unlock(&rc->out_lock);
      pos        = 0;
      buf[pos++] = c;
    }
  }

  /* Flush any trailing partial line */
  if(pos > 0) {
    buf[pos] = '\0';
    pthread_mutex_lock(&rc->out_lock);
    out_ring_push(&rc->out, buf);
    log_ring_push(buf);
    pthread_mutex_unlock(&rc->out_lock);
  }

  close(fd);
  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

static void rc_init_one(RunConfig *rc) {
  memset(rc, 0, sizeof(*rc));
  rc->pipe_read_fd = -1;
  pthread_mutex_init(&rc->out_lock, NULL);
}

static void add_preset(const char *name, const char *cmd) {
  if(g_run_count >= RUN_CONFIGS_MAX) return;
  RunConfig *rc = &g_run_configs[g_run_count++];
  rc_init_one(rc);
  strncpy(rc->name, name, RUN_NAME_MAX - 1);
  strncpy(rc->cmd, cmd, RUN_CMD_MAX - 1);
}

/* ── Utility: find DevLangCfg by name ──────────────────────────────────── */
static const DevLangCfg *find_lang(const OverlayCfg *ov, const char *name) {
  if(!ov) return NULL;
  for(int i = 0; i < ov->lang_count; i++)
    if(!strcasecmp(ov->langs[i].name, name)) return &ov->langs[i];
  return NULL;
}

static void add_lang_preset(const char *label, const char *cmd) {
  if(!cmd || !cmd[0]) return;
  add_preset(label, cmd);
}

void run_configs_init(const OverlayCfg *ov_cfg) {
  if(g_run_count > 0) return;

  /* Optionally switch to project root so stat() finds the right files */
  char old_cwd[1024] = { 0 };
  if(ov_cfg && ov_cfg->project_root[0]) {
    if(getcwd(old_cwd, sizeof(old_cwd)) == NULL) old_cwd[0] = '\0';
    chdir(ov_cfg->project_root);
  }

  struct stat st;

  /* ── Rust / Cargo ── */
  if(stat("Cargo.toml", &st) == 0) {
    const DevLangCfg *lc = find_lang(ov_cfg, "rust");
    add_lang_preset("cargo build", lc ? lc->build : "cargo build 2>&1");
    add_lang_preset("cargo run", lc ? lc->run : "cargo run");
    add_lang_preset("cargo test", lc ? lc->test : "cargo test");
    if(lc && lc->fmt[0]) add_lang_preset("cargo fmt", lc->fmt);
    if(lc && lc->lint[0]) add_lang_preset("cargo clippy", lc->lint);
  }

  /* ── Go ── */
  if(stat("go.mod", &st) == 0) {
    const DevLangCfg *lc = find_lang(ov_cfg, "go");
    add_lang_preset("go build", lc ? lc->build : "go build ./... 2>&1");
    add_lang_preset("go run", lc ? lc->run : "go run .");
    add_lang_preset("go test", lc ? lc->test : "go test ./...");
    if(lc && lc->fmt[0]) add_lang_preset("gofmt", lc->fmt);
    if(lc && lc->lint[0]) add_lang_preset("golangci-lint", lc->lint);
  }

  /* ── Java (Maven) ── */
  if(stat("pom.xml", &st) == 0) {
    const DevLangCfg *lc = find_lang(ov_cfg, "java");
    add_lang_preset("mvn compile", lc ? lc->build : "mvn compile -q");
    add_lang_preset("mvn run", lc ? lc->run : "mvn exec:java -q");
    add_lang_preset("mvn test", lc ? lc->test : "mvn test -q");
    if(lc && lc->fmt[0]) add_lang_preset("java fmt", lc->fmt);
    if(lc && lc->lint[0]) add_lang_preset("checkstyle", lc->lint);
  }

  /* ── Java (Gradle) ── */
  if(stat("build.gradle", &st) == 0 || stat("build.gradle.kts", &st) == 0) {
    const DevLangCfg *lc = find_lang(ov_cfg, "java");
    add_lang_preset("gradle build", lc ? lc->build : "./gradlew build");
    add_lang_preset("gradle run", lc ? lc->run : "./gradlew run");
    add_lang_preset("gradle test", lc ? lc->test : "./gradlew test");
  }

  /* ── Python ── */
  if(stat("pyproject.toml", &st) == 0 || stat("setup.py", &st) == 0 ||
     stat("requirements.txt", &st) == 0) {
    const DevLangCfg *lc = find_lang(ov_cfg, "python");
    add_lang_preset("python run", lc ? lc->run : "python -m main");
    add_lang_preset("pytest", lc ? lc->test : "pytest -v");
    add_lang_preset("pip install", "pip install -e .");
    if(lc && lc->fmt[0]) add_lang_preset("black", lc->fmt);
    if(lc && lc->lint[0]) add_lang_preset("ruff", lc->lint);
  }

  /* ── C / C++ (Meson) ── */
  if(stat("meson.build", &st) == 0 || stat("build.ninja", &st) == 0) {
    const DevLangCfg *lc = find_lang(ov_cfg, "c");
    add_lang_preset("meson build",
                    lc ? lc->build : "meson compile -C builddir 2>&1");
    add_lang_preset("meson test", lc ? lc->test : "meson test -C builddir");
    if(lc && lc->run[0]) add_lang_preset("run binary", lc->run);
    if(lc && lc->fmt[0]) add_lang_preset("clang-format", lc->fmt);
    if(lc && lc->lint[0]) add_lang_preset("clang-tidy", lc->lint);
  }

  /* ── C / C++ (CMake) ── */
  if(stat("CMakeLists.txt", &st) == 0) {
    const DevLangCfg *lc = find_lang(ov_cfg, "cpp");
    if(!lc) lc = find_lang(ov_cfg, "c");
    add_lang_preset("cmake build", lc ? lc->build : "cmake --build build 2>&1");
    add_lang_preset("ctest", lc ? lc->test : "ctest --test-dir build");
    if(lc && lc->run[0]) add_lang_preset("run binary", lc->run);
    if(lc && lc->fmt[0]) add_lang_preset("clang-format", lc->fmt);
  }

  /* ── Makefile ── */
  if(stat("Makefile", &st) == 0 || stat("makefile", &st) == 0) {
    add_lang_preset("make", "make -j$(nproc)");
    add_lang_preset("make test", "make test");
    add_lang_preset("make clean", "make clean");
  }

  if(g_run_count == 0) add_lang_preset("run", "./run.sh");

  /* Restore cwd */
  if(old_cwd[0]) chdir(old_cwd);
}

void run_config_start(int idx) {
  if(idx < 0 || idx >= g_run_count) return;
  RunConfig *rc = &g_run_configs[idx];
  if(rc->running) return;

  /* Create pipe for stdout+stderr capture */
  int pipefd[2];
  if(pipe(pipefd) < 0) {
    log_ring_push("==> run: pipe() failed");
    return;
  }
  /* Make read end non-blocking for clean teardown, write end blocking */
  fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

  pid_t pid = fork();
  if(pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    log_ring_push("==> run: fork() failed");
    return;
  }

  if(pid == 0) {
    /* Child: redirect stdout + stderr → write end of pipe */
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);
    setsid();
    execl("/bin/sh", "sh", "-c", rc->cmd, NULL);
    _exit(127);
  }

  /* Parent */
  close(pipefd[1]); /* we only read */

  /* Make read end blocking again for the reader thread */
  int flags = fcntl(pipefd[0], F_GETFL);
  fcntl(pipefd[0], F_SETFL, flags & ~O_NONBLOCK);

  rc->pid          = pid;
  rc->running      = true;
  rc->exited       = false;
  rc->exit_code    = 0;
  rc->started_ms   = ov_now_ms();
  rc->pipe_read_fd = pipefd[0];

  pthread_mutex_lock(&rc->out_lock);
  out_ring_clear(&rc->out);
  pthread_mutex_unlock(&rc->out_lock);

  if(rc->reader_valid) {
    pthread_join(rc->reader_thread, NULL);
    rc->reader_valid = false;
  }
  if(pthread_create(&rc->reader_thread, NULL, reader_thread_fn, rc) == 0) {
    rc->reader_valid = true;
  } else {
    close(pipefd[0]);
    rc->pipe_read_fd = -1;
    log_ring_push("==> run: pthread_create failed for reader");
  }

  char msg[128];
  snprintf(msg, sizeof(msg), "==> run [%s]: started pid %d", rc->name, (int)pid);
  log_ring_push(msg);
}

void run_config_stop(int idx) {
  if(idx < 0 || idx >= g_run_count) return;
  RunConfig *rc = &g_run_configs[idx];
  if(!rc->running || rc->pid <= 0) return;
  kill(-rc->pid, SIGTERM); /* kill process group */
  char msg[128];
  snprintf(msg, sizeof(msg), "==> run [%s]: SIGTERM pid %d", rc->name, (int)rc->pid);
  log_ring_push(msg);
}

/* Non-blocking reap — call each frame from the render loop */
void run_configs_poll(void) {
  for(int i = 0; i < g_run_count; i++) {
    RunConfig *rc = &g_run_configs[i];
    if(!rc->running || rc->pid <= 0) continue;
    int   status;
    pid_t r = waitpid(rc->pid, &status, WNOHANG);
    if(r == rc->pid) {
      rc->running   = false;
      rc->exited    = true;
      rc->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      /* Reader thread will see EOF and exit naturally */
      char msg[128];
      snprintf(msg,
               sizeof(msg),
               "==> run [%s]: exit %d  (%.1fs)",
               rc->name,
               rc->exit_code,
               (double)(ov_now_ms() - rc->started_ms) / 1000.0);
      log_ring_push(msg);
    }
  }
}

void run_configs_destroy(void) {
  for(int i = 0; i < g_run_count; i++) {
    RunConfig *rc = &g_run_configs[i];
    if(rc->running && rc->pid > 0) kill(-rc->pid, SIGTERM);
    if(rc->reader_valid) pthread_join(rc->reader_thread, NULL);
    if(rc->pipe_read_fd >= 0) close(rc->pipe_read_fd);
    pthread_mutex_destroy(&rc->out_lock);
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Edit state
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool g_run_editing    = false;
static int  g_run_edit_field = 0;
static char g_run_edit_name[RUN_NAME_MAX];
static int  g_run_edit_name_len = 0;
static char g_run_edit_cmd[RUN_CMD_MAX];
static int  g_run_edit_cmd_len = 0;
static bool g_run_focus_output = false; /* Tab toggles focus to output pane */

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  Key handler
 * ═══════════════════════════════════════════════════════════════════════════ */

bool run_panel_key(int *cursor, xkb_keysym_t sym) {
  /* Edit form swallows everything */
  if(g_run_editing) {
    char *buf = (g_run_edit_field == 0) ? g_run_edit_name : g_run_edit_cmd;
    int  *buflen =
        (g_run_edit_field == 0) ? &g_run_edit_name_len : &g_run_edit_cmd_len;
    int bufsz = (g_run_edit_field == 0) ? RUN_NAME_MAX : RUN_CMD_MAX;

    if(sym == XKB_KEY_Escape) {
      g_run_editing = false;
      return true;
    }
    if(sym == XKB_KEY_Tab && g_run_edit_field == 0) {
      g_run_edit_field = 1;
      return true;
    }
    if(sym == XKB_KEY_Return) {
      if(g_run_edit_field == 0) {
        g_run_edit_field = 1;
        return true;
      }
      if(g_run_edit_name[0] && g_run_edit_cmd[0] && g_run_count < RUN_CONFIGS_MAX) {
        RunConfig *rc = &g_run_configs[g_run_count++];
        rc_init_one(rc);
        strncpy(rc->name, g_run_edit_name, RUN_NAME_MAX - 1);
        strncpy(rc->cmd, g_run_edit_cmd, RUN_CMD_MAX - 1);
      }
      g_run_editing = false;
      return true;
    }
    if(sym == XKB_KEY_BackSpace) {
      if(*buflen > 0) buf[--(*buflen)] = '\0';
      return true;
    }
    if(sym >= 0x20 && sym < 0x7f && *buflen < bufsz - 1) {
      buf[(*buflen)++] = (char)sym;
      buf[*buflen]     = '\0';
    }
    return true;
  }

  /* Output-pane focus: j/k scroll its ring */
  if(g_run_focus_output) {
    int idx = (*cursor >= 0 && *cursor < g_run_count) ? *cursor : 0;
    if(sym == XKB_KEY_Tab || sym == XKB_KEY_Escape) {
      g_run_focus_output = false;
      return true;
    }
    if(sym == XKB_KEY_j || sym == XKB_KEY_Down) {
      if(idx < g_run_count) {
        RunConfig *rc = &g_run_configs[idx];
        pthread_mutex_lock(&rc->out_lock);
        if(rc->out.scroll > 0) rc->out.scroll--;
        pthread_mutex_unlock(&rc->out_lock);
      }
      return true;
    }
    if(sym == XKB_KEY_k || sym == XKB_KEY_Up) {
      if(idx < g_run_count) {
        RunConfig *rc = &g_run_configs[idx];
        pthread_mutex_lock(&rc->out_lock);
        rc->out.scroll++;
        pthread_mutex_unlock(&rc->out_lock);
      }
      return true;
    }
    if(sym == XKB_KEY_c) {
      if(idx < g_run_count) {
        pthread_mutex_lock(&g_run_configs[idx].out_lock);
        out_ring_clear(&g_run_configs[idx].out);
        pthread_mutex_unlock(&g_run_configs[idx].out_lock);
      }
      return true;
    }
    return true;
  }

  /* Normal list navigation */
  if(sym == XKB_KEY_j || sym == XKB_KEY_Down) {
    (*cursor)++;
    return true;
  }
  if(sym == XKB_KEY_k || sym == XKB_KEY_Up) {
    if(*cursor > 0) (*cursor)--;
    return true;
  }

  if(sym == XKB_KEY_Tab) {
    g_run_focus_output = true;
    return true;
  }

  if(sym == XKB_KEY_Return) {
    if(*cursor >= 0 && *cursor < g_run_count) {
      if(g_run_configs[*cursor].running)
        run_config_stop(*cursor);
      else
        run_config_start(*cursor);
    }
    return true;
  }

  if(sym == XKB_KEY_a) {
    g_run_editing       = true;
    g_run_edit_field    = 0;
    g_run_edit_name[0]  = '\0';
    g_run_edit_name_len = 0;
    g_run_edit_cmd[0]   = '\0';
    g_run_edit_cmd_len  = 0;
    return true;
  }

  if(sym == XKB_KEY_d) {
    if(*cursor >= 0 && *cursor < g_run_count) {
      run_config_stop(*cursor);
      if(g_run_configs[*cursor].reader_valid) {
        pthread_join(g_run_configs[*cursor].reader_thread, NULL);
        g_run_configs[*cursor].reader_valid = false;
      }
      pthread_mutex_destroy(&g_run_configs[*cursor].out_lock);
      for(int i = *cursor; i < g_run_count - 1; i++)
        g_run_configs[i] = g_run_configs[i + 1];
      g_run_count--;
      if(*cursor >= g_run_count && *cursor > 0) (*cursor)--;
    }
    return true;
  }

  if(sym == XKB_KEY_c) {
    if(*cursor >= 0 && *cursor < g_run_count) {
      pthread_mutex_lock(&g_run_configs[*cursor].out_lock);
      out_ring_clear(&g_run_configs[*cursor].out);
      pthread_mutex_unlock(&g_run_configs[*cursor].out_lock);
    }
    return true;
  }

  return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  Draw
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Layout split:
 *   - Config list  gets the top LIST_ROWS rows (or half the panel, whichever
 *     is smaller) with a minimum of 3 rows always visible.
 *   - Output pane  fills the remainder below a separator.
 *
 * Column layout within the config list (fixed pixel offsets based on PAD):
 *
 *   COL_STATUS  px0 + PAD
 *   COL_NAME    px0 + PAD + 28
 *   COL_CMD     px0 + PAD + 28 + NAME_COL_W   (NAME_COL_W = 200px)
 *   COL_TIME    px0 + pw  - PAD - 64
 */
#define LIST_ROWS_MAX 6
#define NAME_COL_W    200
#define TIME_COL_W    72

void draw_panel_run(uint32_t     *px,
                    int           stride,
                    int           px0,
                    int           py0,
                    int           pw,
                    int           ph,
                    int          *cursor,
                    const Config *cfg) {
  Color ac  = cfg->colors.active_border;
  Color bg  = cfg->colors.pane_bg;
  int   y   = py0 + HEADER_H + PAD;
  int   bot = py0 + ph - PAD;

  /* ── Toolbar hint ── */
  {
    const char *hint = g_run_focus_output
                           ? "Tab=configs  j/k=scroll  c=clear output"
                           : "Enter=start/stop  a=add  d=del  Tab=output  c=clear";
    uint8_t     hr   = g_run_focus_output ? 0xf9 : ac.r;
    uint8_t     hg   = g_run_focus_output ? 0xe2 : ac.g;
    uint8_t     hb   = g_run_focus_output ? 0xaf : ac.b;
    ov_draw_text(
        px, stride, px0 + PAD, y + g_ov_asc, stride, bot, hint, hr, hg, hb, 0xff);
    y += ROW_H;
  }

  /* ── Thin separator ── */
  ov_fill_rect(px,
               stride,
               px0 + PAD,
               y,
               pw - PAD * 2,
               1,
               ac.r,
               ac.g,
               ac.b,
               0x35,
               stride,
               bot);
  y += SECTION_GAP;

  /* ── Column headers ── */
  {
    int col_cmd  = px0 + PAD + 28 + NAME_COL_W + COL_GAP;
    int col_time = px0 + pw - PAD - TIME_COL_W;
    ov_draw_text(px,
                 stride,
                 px0 + PAD + 28,
                 y + g_ov_asc,
                 stride,
                 bot,
                 "NAME",
                 ac.r,
                 ac.g,
                 ac.b,
                 0x70);
    ov_draw_text(px,
                 stride,
                 col_cmd,
                 y + g_ov_asc,
                 stride,
                 bot,
                 "COMMAND",
                 ac.r,
                 ac.g,
                 ac.b,
                 0x70);
    ov_draw_text(px,
                 stride,
                 col_time,
                 y + g_ov_asc,
                 stride,
                 bot,
                 "TIME",
                 ac.r,
                 ac.g,
                 ac.b,
                 0x70);
    y += ROW_H;
  }

  /* ── Clamp cursor ── */
  if(*cursor >= g_run_count && g_run_count > 0) *cursor = g_run_count - 1;

  /* ── Config list ── */
  int list_rows = g_run_count < LIST_ROWS_MAX ? g_run_count : LIST_ROWS_MAX;
  int list_h    = list_rows * ROW_H;

  /* Edit form — takes the place of the bottom of the list */
  if(g_run_editing) {
    char namedisp[RUN_NAME_MAX + 20];
    char cmddisp[RUN_CMD_MAX + 20];
    snprintf(namedisp,
             sizeof(namedisp),
             "name › %s%s",
             g_run_edit_name,
             g_run_edit_field == 0 ? "▌" : "");
    snprintf(cmddisp,
             sizeof(cmddisp),
             " cmd › %s%s",
             g_run_edit_cmd,
             g_run_edit_field == 1 ? "▌" : "");

    uint8_t an_r = g_run_edit_field == 0 ? 0xf9 : 0x58;
    uint8_t an_g = g_run_edit_field == 0 ? 0xe2 : 0x5b;
    uint8_t an_b = g_run_edit_field == 0 ? 0xaf : 0x70;
    uint8_t ac_r = g_run_edit_field == 1 ? 0xf9 : 0x58;
    uint8_t ac_g = g_run_edit_field == 1 ? 0xe2 : 0x5b;
    uint8_t ac_b = g_run_edit_field == 1 ? 0xaf : 0x70;

    /* Soft background for the form */
    ov_fill_rect(px,
                 stride,
                 px0 + PAD,
                 y - 2,
                 pw - PAD * 2,
                 ROW_H * 2 + 4,
                 0x18,
                 0x18,
                 0x28,
                 0xa0,
                 stride,
                 bot);
    ov_fill_border(px,
                   stride,
                   px0 + PAD,
                   y - 2,
                   pw - PAD * 2,
                   ROW_H * 2 + 4,
                   ac.r,
                   ac.g,
                   ac.b,
                   0x60,
                   stride,
                   bot);

    ov_draw_text(px,
                 stride,
                 px0 + PAD + 8,
                 y + g_ov_asc,
                 stride,
                 bot,
                 namedisp,
                 an_r,
                 an_g,
                 an_b,
                 0xff);
    ov_draw_text(px,
                 stride,
                 px0 + PAD + 8,
                 y + ROW_H + g_ov_asc,
                 stride,
                 bot,
                 cmddisp,
                 ac_r,
                 ac_g,
                 ac_b,
                 0xff);
    y += ROW_H * 2 + SECTION_GAP;
    ov_draw_text(px,
                 stride,
                 px0 + PAD + 8,
                 y + g_ov_asc,
                 stride,
                 bot,
                 "Tab=next field  Enter=confirm  Esc=cancel",
                 0x58,
                 0x5b,
                 0x70,
                 0xff);
    y += ROW_H + SECTION_GAP;
  }

  int scroll = *cursor - LIST_ROWS_MAX + 1;
  if(scroll < 0) scroll = 0;

  for(int i = 0; i < list_rows; i++) {
    int idx = i + scroll;
    if(idx >= g_run_count) break;
    RunConfig *rc  = &g_run_configs[idx];
    bool       sel = (idx == *cursor) && !g_run_focus_output;
    int        ry  = y + i * ROW_H;

    if(sel) draw_cursor_line(px, stride, px0, ry, pw, ac, bg, stride, bot);

    /* Status indicator */
    const char *dot;
    uint8_t     dr, dg, db;
    if(rc->running) {
      /* Animated spinner — nf-md-loading */
      static const char *spin[] = { "󰪞", "󰪟", "󰪠", "󰪡",
                                    "󰪢", "󰪣", "󰪤", "󰪥" };
      dot                       = spin[(ov_now_ms() / 100) % 8];
      dr                        = 0xa6;
      dg                        = 0xe3;
      db                        = 0xa1;
    } else if(rc->exited && rc->exit_code != 0) {
      dot = " ";
      dr  = 0xf3;
      dg  = 0x8b;
      db  = 0xa8; /* nf-fa-times_circle */
    } else if(rc->exited) {
      dot = " ";
      dr  = 0x89;
      dg  = 0xdc;
      db  = 0xeb; /* nf-fa-check_circle */
    } else {
      dot = " ";
      dr  = 0x45;
      dg  = 0x47;
      db  = 0x5a; /* nf-fa-circle_o — idle */
    }
    ov_draw_text(px,
                 stride,
                 px0 + PAD,
                 ry + g_ov_asc + 2,
                 stride,
                 bot,
                 dot,
                 dr,
                 dg,
                 db,
                 0xff);

    /* Name column */
    uint8_t nr = sel ? ac.r : 0xcd;
    uint8_t ng = sel ? ac.g : 0xd6;
    uint8_t nb = sel ? ac.b : 0xf4;
    ov_draw_text(px,
                 stride,
                 px0 + PAD + 28,
                 ry + g_ov_asc + 2,
                 stride,
                 bot,
                 rc->name,
                 nr,
                 ng,
                 nb,
                 0xff);

    /* Command column — truncate to available width */
    {
      int col_cmd   = px0 + PAD + 28 + NAME_COL_W + COL_GAP;
      int col_time  = px0 + pw - PAD - TIME_COL_W;
      int cmd_avail = col_time - col_cmd - COL_GAP;
      if(cmd_avail > 40) {
        char trunc[RUN_CMD_MAX];
        strncpy(trunc, rc->cmd, sizeof(trunc) - 1);
        while(ov_measure(trunc) > cmd_avail && strlen(trunc) > 3)
          trunc[strlen(trunc) - 1] = '\0';
        ov_draw_text(px,
                     stride,
                     col_cmd,
                     ry + g_ov_asc + 2,
                     stride,
                     bot,
                     trunc,
                     0x45,
                     0x47,
                     0x5a,
                     0xff);
      }
    }

    /* Time / exit status column */
    {
      int     col_time = px0 + pw - PAD - TIME_COL_W;
      char    ts[32]   = "";
      uint8_t tr, tg, tb;
      if(rc->running) {
        snprintf(ts,
                 sizeof(ts),
                 "%.1fs",
                 (double)(ov_now_ms() - rc->started_ms) / 1000.0);
        tr = 0xa6;
        tg = 0xe3;
        tb = 0xa1;
      } else if(rc->exited) {
        snprintf(ts, sizeof(ts), "exit %d", rc->exit_code);
        tr = rc->exit_code == 0 ? 0x58 : 0xf3;
        tg = rc->exit_code == 0 ? 0x5b : 0x8b;
        tb = rc->exit_code == 0 ? 0x70 : 0xa8;
      }
      if(ts[0])
        ov_draw_text(px,
                     stride,
                     col_time,
                     ry + g_ov_asc + 2,
                     stride,
                     bot,
                     ts,
                     tr,
                     tg,
                     tb,
                     0xff);
    }
  }
  y += list_h + SECTION_GAP;

  /* ── Separator before output pane ── */
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
               bot);
  y += SECTION_GAP;

  /* ── Output pane header ── */
  {
    int  sel_idx = (*cursor >= 0 && *cursor < g_run_count) ? *cursor : -1;
    char hdr[RUN_NAME_MAX + 32];
    if(sel_idx >= 0)
      snprintf(hdr, sizeof(hdr), "stdout › %s", g_run_configs[sel_idx].name);
    else
      strncpy(hdr, "stdout", sizeof(hdr) - 1);

    uint8_t hr = g_run_focus_output ? 0xf9 : ac.r;
    uint8_t hg = g_run_focus_output ? 0xe2 : ac.g;
    uint8_t hb = g_run_focus_output ? 0xaf : ac.b;
    ov_draw_text(
        px, stride, px0 + PAD, y + g_ov_asc, stride, bot, hdr, hr, hg, hb, 0xff);

    /* "LIVE" badge if running — nf-fa-circle pulse */
    if(sel_idx >= 0 && g_run_configs[sel_idx].running) {
      static const char *pulse[] = { " ", " " }; /* nf-fa-circle / nf-fa-circle_o */
      const char        *dot_    = pulse[(ov_now_ms() / 500) % 2];
      int px_ = px0 + pw - PAD - ov_measure(" ") - ov_measure("LIVE") - 4;
      ov_draw_text(
          px, stride, px_, y + g_ov_asc, stride, bot, dot_, 0xa6, 0xe3, 0xa1, 0xff);
      ov_draw_text(px,
                   stride,
                   px_ + ov_measure(" ") + 4,
                   y + g_ov_asc,
                   stride,
                   bot,
                   "LIVE",
                   0xa6,
                   0xe3,
                   0xa1,
                   0xc0);
    }
    y += ROW_H;
  }

  /* ── Output lines ── */
  {
    int out_h        = bot - y - PAD;
    int visible_rows = out_h / ROW_H;
    if(visible_rows < 1) return;

    int sel_idx = (*cursor >= 0 && *cursor < g_run_count) ? *cursor : -1;
    if(sel_idx < 0) return;

    RunConfig *rc = &g_run_configs[sel_idx];

    /* Soft tinted background for the output area */
    ov_fill_rect(px,
                 stride,
                 px0 + PAD,
                 y,
                 pw - PAD * 2,
                 out_h,
                 0x10,
                 0x10,
                 0x1c,
                 0x80,
                 stride,
                 py0 + ph);

    pthread_mutex_lock(&rc->out_lock);
    int count      = rc->out.count;
    int scroll     = rc->out.scroll;
    /* Clamp scroll so we never go past available lines */
    int max_scroll = count - visible_rows;
    if(max_scroll < 0) max_scroll = 0;
    if(scroll > max_scroll) {
      rc->out.scroll = scroll = max_scroll;
    }

    /* Draw from tail minus scroll */
    int start = count - visible_rows - scroll;
    if(start < 0) start = 0;

    for(int i = 0; i < visible_rows; i++) {
      int li = start + i;
      if(li >= count) break;
      const char *line = out_ring_get(&rc->out, li);
      int         ry   = y + i * ROW_H;

      /* Colour-code by content */
      uint8_t lr = 0xa6, lg = 0xad, lb = 0xc8;
      if(strstr(line, "error") || strstr(line, "ERROR") || strstr(line, "FAILED")) {
        lr = 0xf3;
        lg = 0x8b;
        lb = 0xa8;
      } else if(strstr(line, "warning") || strstr(line, "WARN")) {
        lr = 0xf9;
        lg = 0xe2;
        lb = 0xaf;
      } else if(strstr(line, "==>") || strstr(line, "Finished") ||
                strstr(line, "ok")) {
        lr = ac.r;
        lg = ac.g;
        lb = ac.b;
      }

      /* Truncate to panel width */
      char trunc[OUT_LINE_MAX];
      strncpy(trunc, line, sizeof(trunc) - 1);
      int avail = pw - PAD * 2 - 8;
      while(ov_measure(trunc) > avail && strlen(trunc) > 3)
        trunc[strlen(trunc) - 1] = '\0';

      ov_draw_text(px,
                   stride,
                   px0 + PAD + 4,
                   ry + g_ov_asc + 2,
                   stride,
                   bot,
                   trunc,
                   lr,
                   lg,
                   lb,
                   0xff);
    }
    pthread_mutex_unlock(&rc->out_lock);

    /* Scroll indicator */
    if(scroll > 0) {
      char si[24];
      snprintf(si, sizeof(si), "↑ %d", scroll);
      ov_draw_text(px,
                   stride,
                   px0 + pw - PAD - ov_measure(si),
                   y + g_ov_asc + 2,
                   stride,
                   bot,
                   si,
                   0x89,
                   0xdc,
                   0xeb,
                   0xc0);
    }
  }
}
