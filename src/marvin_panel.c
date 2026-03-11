/* marvin_panel.c — Self-contained Marvin project-manager panel.
 *
 * Compiled as a separate translation unit; linked with overlay.c.
 * Include path: overlay_internal.h  →  trixie.h
 */
#define _POSIX_C_SOURCE 200809L

/* overlay_internal.h MUST come first: it pulls in trixie.h which fully
 * defines Config, Color, TrixieOverlay etc.  marvin_panel.h and nvim_panel.h
 * depend on those types — including them first caused the "typedef
 * redefinition with different types" errors.                                */
#include "marvin_panel.h"
#include "nvim_panel.h"       /* nvim_is_connected() */
#include "overlay_internal.h" /* Color, ROW_H, PAD, HDR_H, ov_*, tui_* */
#include "trixie.h"           /* full Config + TrixieOverlay definitions */

/* Socket support for ipc_send_raw() — must be after _POSIX_C_SOURCE */
#include <sys/socket.h>
#include <sys/un.h>

#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── Layout helpers (mirrors overlay.c macros) ───────────────────────────── */
/* BDR is defined here only; HDR_H comes from overlay_internal.h as an alias
   for HEADER_H.  Do not redefine either here.                               */
#define BDR        2
#define IX(bx)     ((bx) + BDR + PAD)
#define IY(by)     ((by) + BDR + HDR_H + 4)
#define IW(bw)     ((bw) - BDR * 2 - PAD * 2)
#define ROW_TY(ry) ((ry) + (ROW_H - g_ov_th) / 2 + g_ov_asc)

/* tui_box / tui_hsep / tui_scrollbar are now declared in overlay_internal.h */

/* tui_input_box is static in overlay.c — we need our own copy here.
 * It is small enough to duplicate without maintenance burden.             */
static int mp_input_box(uint32_t   *px,
                        int         stride,
                        int         x,
                        int         y,
                        int         w,
                        const char *value,
                        const char *placeholder,
                        bool        active,
                        Color       ac,
                        Color       bg,
                        int         cw,
                        int         ch) {
  int     box_h = ROW_H + 6;
  uint8_t ib_r  = (uint8_t)(bg.r > 0x0c ? bg.r - 0x0c : 0);
  uint8_t ib_g  = (uint8_t)(bg.g > 0x0c ? bg.g - 0x0c : 0);
  uint8_t ib_b  = (uint8_t)(bg.b > 0x0c ? bg.b - 0x0c : 0);
  ov_fill_rect(px, stride, x, y, w, box_h, ib_r, ib_g, ib_b, 0xff, cw, ch);
  uint8_t bdr_a = active ? 0xff : 0x40;
  ov_fill_rect(px, stride, x, y, w, 1, ac.r, ac.g, ac.b, bdr_a, cw, ch);
  ov_fill_rect(px, stride, x, y + box_h - 1, w, 1, ac.r, ac.g, ac.b, bdr_a, cw, ch);
  ov_fill_rect(px, stride, x, y, 1, box_h, ac.r, ac.g, ac.b, bdr_a, cw, ch);
  ov_fill_rect(px, stride, x + w - 1, y, 1, box_h, ac.r, ac.g, ac.b, bdr_a, cw, ch);
  if(active)
    ov_fill_rect(
        px, stride, x + 1, y + box_h - 2, w - 2, 2, ac.r, ac.g, ac.b, 0xff, cw, ch);
  int ty = y + (box_h - g_ov_th) / 2 + g_ov_asc;
  if(value && value[0]) {
    ov_draw_text(px, stride, x + PAD / 2, ty, cw, ch, value, 0xcd, 0xd6, 0xf4, 0xff);
    if(active) {
      int64_t ms = ov_now_ms();
      if((ms / 530) % 2 == 0) {
        int cx2 = x + PAD / 2 + ov_measure(value);
        ov_fill_rect(
            px, stride, cx2, y + 3, 2, box_h - 6, ac.r, ac.g, ac.b, 0xff, cw, ch);
      }
    }
  } else if(placeholder) {
    ov_draw_text(
        px, stride, x + PAD / 2, ty, cw, ch, placeholder, 0x45, 0x47, 0x5a, 0xff);
  }
  return y + box_h;
}

/* ── ipc_send_raw — best-effort fire-and-forget via the IPC socket ────────
 * ipc.c has no ipc_send_raw symbol; we implement a thin client here that
 * connects to the compositor's own socket and sends the message.
 * Returns true if the write succeeded.                                     */
static bool g_mv_tasks_output_expanded = false; /* 'o' toggles inline preview */
static bool ipc_send_raw(const char *msg) {
  const char *sock = getenv("TRIXIE_IPC_SOCK");
  if(!sock || !sock[0]) return false;

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if(fd < 0) return false;

  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  strncpy(addr.sun_path, sock, sizeof(addr.sun_path) - 1);
  if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return false;
  }
  size_t  len = strlen(msg);
  ssize_t n   = write(fd, msg, len);
  /* read and discard reply so the compositor doesn't block */
  char    rbuf[256];
  (void)read(fd, rbuf, sizeof(rbuf));
  close(fd);
  return n == (ssize_t)len;
}

/* ════════════════════════════════════════════════════════════════════════════
 * §A  Shared state (formerly at the top of overlay.c §4)
 * ════════════════════════════════════════════════════════════════════════════ */

#define MARVIN_CONSOLE_MAX 16384
#define MARVIN_NAME_MAX    128

typedef struct {
  char name[MARVIN_NAME_MAX];
  char type[32];
  char lang[32];
  char root[512];
  char last_action[64];
  char last_status[32];
  char last_cmd[256];
  bool valid;
} MarvinProject;

typedef struct {
  const char *label;
  const char *nvim_cmd;
  const char *hint;
  bool        available;
} MarvinAction;

static MarvinProject   g_marvin_proj                        = { 0 };
MarvinTab              g_marvin_tab                         = MARVIN_TAB_PROJECT;
int                    g_marvin_cursor                      = 0;
static char            g_marvin_console[MARVIN_CONSOLE_MAX] = { 0 };
static int             g_marvin_console_len                 = 0;
static int64_t         g_mv_detect_next                     = 0;
static pthread_mutex_t g_marvin_con_lock = PTHREAD_MUTEX_INITIALIZER;

/* ════════════════════════════════════════════════════════════════════════════
 * §B  Filesystem helpers
 * ════════════════════════════════════════════════════════════════════════════ */

static bool mv_file(const char *dir, const char *name) {
  char p[640];
  snprintf(p, sizeof(p), "%s/%s", dir, name);
  struct stat st;
  return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}
static bool mv_has_ext(const char *dir, const char *ext) {
  DIR *d = opendir(dir);
  if(!d) return false;
  struct dirent *e;
  bool           found = false;
  int            el    = (int)strlen(ext);
  while((e = readdir(d)) && !found) {
    int l = (int)strlen(e->d_name);
    if(l > el && strcmp(e->d_name + l - el, ext) == 0) found = true;
  }
  closedir(d);
  return found;
}
static void mv_basename(const char *path, char *out, int sz) {
  const char *sl = strrchr(path, '/');
  strncpy(out, sl ? sl + 1 : path, sz - 1);
  out[sz - 1] = '\0';
}
static bool mv_toml_field(const char *path, const char *key, char *out, int sz) {
  FILE *f = fopen(path, "r");
  if(!f) return false;
  char line[512];
  bool found = false;
  while(fgets(line, sizeof(line), f) && !found) {
    char *p = line;
    while(*p == ' ' || *p == '\t')
      p++;
    if(strncmp(p, key, strlen(key))) continue;
    p += strlen(key);
    while(*p == ' ' || *p == '\t')
      p++;
    if(*p != '=') continue;
    p++;
    while(*p == ' ' || *p == '\t' || *p == '"')
      p++;
    int len = (int)strlen(p);
    while(len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r' ||
                      p[len - 1] == '"' || p[len - 1] == ' '))
      p[--len] = '\0';
    strncpy(out, p, sz - 1);
    out[sz - 1] = '\0';
    found       = true;
  }
  fclose(f);
  return found;
}
static bool mv_go_module(const char *dir, char *out, int sz) {
  char path[640];
  snprintf(path, sizeof(path), "%s/go.mod", dir);
  FILE *f = fopen(path, "r");
  if(!f) return false;
  char line[256];
  bool found = false;
  while(fgets(line, sizeof(line), f) && !found) {
    if(strncmp(line, "module ", 7)) continue;
    char *p = line + 7;
    while(*p == ' ')
      p++;
    int len = (int)strlen(p);
    while(len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' '))
      p[--len] = '\0';
    char *sl = strrchr(p, '/');
    if(sl) p = sl + 1;
    strncpy(out, p, sz - 1);
    out[sz - 1] = '\0';
    found       = true;
  }
  fclose(f);
  return found;
}
static bool mv_npm_name(const char *dir, char *out, int sz) {
  char path[640];
  snprintf(path, sizeof(path), "%s/package.json", dir);
  FILE *f = fopen(path, "r");
  if(!f) return false;
  char line[512];
  bool found = false;
  while(fgets(line, sizeof(line), f) && !found) {
    char *p = strstr(line, "\"name\"");
    if(!p) continue;
    p = strchr(p + 6, ':');
    if(!p) continue;
    p++;
    while(*p == ' ' || *p == '"')
      p++;
    int len = (int)strlen(p);
    while(len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r' ||
                      p[len - 1] == '"' || p[len - 1] == ',' || p[len - 1] == ' '))
      p[--len] = '\0';
    strncpy(out, p, sz - 1);
    out[sz - 1] = '\0';
    found       = true;
  }
  fclose(f);
  return found;
}

/* ════════════════════════════════════════════════════════════════════════════
 * §C  Project detection
 * ════════════════════════════════════════════════════════════════════════════ */

typedef struct {
  const char *marker;
  const char *type;
  const char *lang;
} MvDef;
static const MvDef MV_DEFS[] = {
  { "Cargo.toml",       "cargo",  "rust"   },
  { "go.mod",           "go",     "go"     },
  { "meson.build",      "meson",  "cpp"    },
  { "CMakeLists.txt",   "cmake",  "cpp"    },
  { "pom.xml",          "maven",  "java"   },
  { "build.gradle",     "gradle", "java"   },
  { "build.gradle.kts", "gradle", "java"   },
  { "package.json",     "npm",    "js"     },
  { "build.zig",        "zig",    "zig"    },
  { "Gemfile",          "ruby",   "ruby"   },
  { "pyproject.toml",   "python", "python" },
  { "setup.py",         "python", "python" },
  { "Makefile",         "make",   "c"      },
  { "GNUmakefile",      "make",   "c"      },
  { NULL,               NULL,     NULL     },
};

static bool mv_detect(const char *start) {
  char dir[512];
  strncpy(dir, start, sizeof(dir) - 1);
  int dl = (int)strlen(dir);
  while(dl > 1 && dir[dl - 1] == '/')
    dir[--dl] = '\0';

  for(int lv = 0; lv < 10; lv++) {
    if(mv_has_ext(dir, ".sln") || mv_has_ext(dir, ".csproj")) {
      memset(&g_marvin_proj, 0, sizeof(g_marvin_proj));
      strncpy(g_marvin_proj.type, "dotnet", sizeof(g_marvin_proj.type) - 1);
      strncpy(g_marvin_proj.lang, "csharp", sizeof(g_marvin_proj.lang) - 1);
      strncpy(g_marvin_proj.root, dir, sizeof(g_marvin_proj.root) - 1);
      mv_basename(dir, g_marvin_proj.name, sizeof(g_marvin_proj.name));
      g_marvin_proj.valid = true;
      return true;
    }
    for(int i = 0; MV_DEFS[i].marker; i++) {
      if(!mv_file(dir, MV_DEFS[i].marker)) continue;
      memset(&g_marvin_proj, 0, sizeof(g_marvin_proj));
      strncpy(g_marvin_proj.type, MV_DEFS[i].type, sizeof(g_marvin_proj.type) - 1);
      strncpy(g_marvin_proj.lang, MV_DEFS[i].lang, sizeof(g_marvin_proj.lang) - 1);
      strncpy(g_marvin_proj.root, dir, sizeof(g_marvin_proj.root) - 1);
      char nm[MARVIN_NAME_MAX] = { 0 };
      bool got                 = false;
      if(!strcmp(MV_DEFS[i].type, "cargo")) {
        char tf[640];
        snprintf(tf, sizeof(tf), "%s/Cargo.toml", dir);
        got         = mv_toml_field(tf, "name", nm, sizeof(nm));
        char vr[64] = { 0 };
        if(mv_toml_field(tf, "version", vr, sizeof(vr)))
          strncpy(g_marvin_proj.last_cmd, vr, sizeof(g_marvin_proj.last_cmd) - 1);
      } else if(!strcmp(MV_DEFS[i].type, "go")) {
        got = mv_go_module(dir, nm, sizeof(nm));
      } else if(!strcmp(MV_DEFS[i].type, "npm")) {
        got = mv_npm_name(dir, nm, sizeof(nm));
        if(mv_file(dir, "tsconfig.json"))
          strncpy(g_marvin_proj.lang, "ts", sizeof(g_marvin_proj.lang) - 1);
      }
      if(!got) mv_basename(dir, nm, sizeof(nm));
      strncpy(g_marvin_proj.name, nm, sizeof(g_marvin_proj.name) - 1);
      g_marvin_proj.valid = true;
      return true;
    }
    char *sl = strrchr(dir, '/');
    if(!sl || sl == dir) break;
    *sl = '\0';
  }
  return false;
}

/* ════════════════════════════════════════════════════════════════════════════
 * §D  Console helpers
 * ════════════════════════════════════════════════════════════════════════════ */

static void mv_console_append(const char *line) {
  pthread_mutex_lock(&g_marvin_con_lock);
  int len   = (int)strlen(line);
  int spare = MARVIN_CONSOLE_MAX - g_marvin_console_len - 2;
  if(spare < len) {
    int drop = MARVIN_CONSOLE_MAX / 4;
    memmove(g_marvin_console, g_marvin_console + drop, g_marvin_console_len - drop);
    g_marvin_console_len -= drop;
    spare = MARVIN_CONSOLE_MAX - g_marvin_console_len - 2;
  }
  int copy = len < spare ? len : spare;
  memcpy(g_marvin_console + g_marvin_console_len, line, copy);
  g_marvin_console_len += copy;
  g_marvin_console[g_marvin_console_len++] = '\n';
  g_marvin_console[g_marvin_console_len]   = '\0';
  pthread_mutex_unlock(&g_marvin_con_lock);

  const char *run = getenv("XDG_RUNTIME_DIR");
  if(run && run[0]) {
    char path[512];
    snprintf(path, sizeof(path), "%s/marvin_console.log", run);
    FILE *f = fopen(path, "a");
    if(f) {
      fputs(line, f);
      fputc('\n', f);
      fclose(f);
    }
  }
}

/* ════════════════════════════════════════════════════════════════════════════
 * §E  Spawn runner
 * ════════════════════════════════════════════════════════════════════════════ */

typedef struct {
  int   fd;
  pid_t pid;
  char  title[128];
  char  action[64];
} MvRunCtx;

static void *mv_run_thread(void *arg) {
  MvRunCtx *c = (MvRunCtx *)arg;
  char      line[512];
  FILE     *f = fdopen(c->fd, "r");
  if(f) {
    while(fgets(line, sizeof(line), f)) {
      int l = (int)strlen(line);
      if(l > 0 && line[l - 1] == '\n') line[--l] = '\0';
      if(l > 0) mv_console_append(line);
    }
    fclose(f);
  } else
    close(c->fd);
  int status = 0;
  waitpid(c->pid, &status, 0);
  int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  strncpy(
      g_marvin_proj.last_action, c->action, sizeof(g_marvin_proj.last_action) - 1);
  if(code == 0) {
    strncpy(g_marvin_proj.last_status, "ok", sizeof(g_marvin_proj.last_status) - 1);
    char msg[160];
    snprintf(msg, sizeof(msg), "✓ %s finished", c->title);
    mv_console_append(msg);
  } else {
    strncpy(
        g_marvin_proj.last_status, "error", sizeof(g_marvin_proj.last_status) - 1);
    char msg[160];
    snprintf(msg, sizeof(msg), "✗ %s failed (exit %d)", c->title, code);
    mv_console_append(msg);
  }
  free(c);
  return NULL;
}

static void mv_run(const char *cmd, const char *title, const char *action_id) {
  if(!cmd || !cmd[0]) return;
  const char *run = getenv("XDG_RUNTIME_DIR");
  if(run && run[0]) {
    char path[512];
    snprintf(path, sizeof(path), "%s/marvin_console.log", run);
    FILE *f = fopen(path, "w");
    if(f) fclose(f);
  }
  pthread_mutex_lock(&g_marvin_con_lock);
  g_marvin_console_len = 0;
  g_marvin_console[0]  = '\0';
  pthread_mutex_unlock(&g_marvin_con_lock);

  char hdr[512];
  snprintf(hdr, sizeof(hdr), "$ %s", cmd);
  mv_console_append(hdr);

  int pfd[2];
  if(pipe(pfd) != 0) {
    mv_console_append("error: pipe failed");
    return;
  }
  pid_t pid = fork();
  if(pid < 0) {
    close(pfd[0]);
    close(pfd[1]);
    mv_console_append("error: fork failed");
    return;
  }
  if(pid == 0) {
    close(pfd[0]);
    dup2(pfd[1], STDOUT_FILENO);
    dup2(pfd[1], STDERR_FILENO);
    close(pfd[1]);
    if(g_marvin_proj.root[0]) chdir(g_marvin_proj.root);
    setsid();
    execl("/bin/sh", "sh", "-c", cmd, NULL);
    _exit(127);
  }
  close(pfd[1]);
  MvRunCtx *ctx = malloc(sizeof(MvRunCtx));
  if(!ctx) {
    close(pfd[0]);
    return;
  }
  ctx->fd  = pfd[0];
  ctx->pid = pid;
  strncpy(ctx->title, title, sizeof(ctx->title) - 1);
  strncpy(ctx->action, action_id, sizeof(ctx->action) - 1);
  strncpy(
      g_marvin_proj.last_status, "running", sizeof(g_marvin_proj.last_status) - 1);
  strncpy(
      g_marvin_proj.last_action, action_id, sizeof(g_marvin_proj.last_action) - 1);
  strncpy(g_marvin_proj.last_cmd, cmd, sizeof(g_marvin_proj.last_cmd) - 1);

  pthread_t      thr;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if(pthread_create(&thr, &attr, mv_run_thread, ctx) != 0) {
    close(pfd[0]);
    free(ctx);
  }
  pthread_attr_destroy(&attr);
}

/* ════════════════════════════════════════════════════════════════════════════
 * §F  Action tables
 * ════════════════════════════════════════════════════════════════════════════ */

typedef struct {
  const char *id;
  const char *label;
  const char *cmd;
  const char *hint;
} MvAction;

static const MvAction MV_CARGO[] = {
  { "build", "Build", "cargo build", "b" },
  { "run", "Run", "cargo run", "r" },
  { "brun", "Build & Run", "cargo build && cargo run", "R" },
  { "test", "Test", "cargo test", "t" },
  { "clean", "Clean", "cargo clean", "x" },
  { "fmt", "Format", "cargo fmt", "f" },
  { "lint", "Clippy", "cargo clippy", "l" },
  { "pkg", "Package", "cargo package", "p" },
  { "install", "Install", "cargo install --path .", "i" },
  { NULL }
};
static const MvAction MV_GO[] = {
  { "build", "Build", "go build ./...", "b" },
  { "run", "Run", "go run .", "r" },
  { "brun", "Build & Run", "go build ./... && go run .", "R" },
  { "test", "Test", "go test ./...", "t" },
  { "clean", "Clean", "go clean ./...", "x" },
  { "fmt", "Format", "gofmt -w .", "f" },
  { "lint", "Vet", "go vet ./...", "l" },
  { "install", "Install", "go install ./...", "i" },
  { NULL }
};
static const MvAction MV_CMAKE[] = {
  { "build", "Build", "cmake --build build", "b" },
  { "config", "Configure", "cmake -B build -DCMAKE_BUILD_TYPE=Debug", "r" },
  { "brun",
   "Config+Build", "cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build",
   "R" },
  { "test", "Test", "cd build && ctest --output-on-failure", "t" },
  { "clean", "Clean", "cmake --build build --target clean", "x" },
  { "install", "Install", "cmake --install build", "i" },
  { NULL }
};
static const MvAction MV_MESON[] = {
  { "build", "Build", "meson compile -C build", "b" },
  { "config", "Setup", "meson setup build", "r" },
  { "brun", "Setup+Build", "meson setup build && meson compile -C build", "R" },
  { "test", "Test", "meson test -C build", "t" },
  { "clean", "Clean", "meson compile -C build --clean", "x" },
  { "install", "Install", "meson install -C build", "i" },
  { NULL }
};
static const MvAction MV_MAKE[] = {
  { "build", "Build", "make", "b" },
  { "run", "Run", "make run", "r" },
  { "brun", "Build & Run", "make && make run", "R" },
  { "test", "Test", "make test", "t" },
  { "clean", "Clean", "make clean", "x" },
  { "fmt", "Format", "make fmt", "f" },
  { "lint", "Lint", "make lint", "l" },
  { "install", "Install", "make install", "i" },
  { NULL }
};
static const MvAction MV_NODE[] = {
  { "build", "Build", "npm run build", "b" },
  { "run", "Dev", "npm run dev", "r" },
  { "brun", "Build+Start", "npm run build && npm start", "R" },
  { "test", "Test", "npm test", "t" },
  { "clean", "Clean", "rm -rf dist node_modules/.cache", "x" },
  { "fmt", "Format", "npm run format 2>/dev/null || npx prettier --write .", "f" },
  { "lint", "Lint", "npm run lint 2>/dev/null || npx eslint .", "l" },
  { "install", "Install", "npm install", "i" },
  { NULL }
};
static const MvAction MV_PYTHON[] = {
  { "run", "Run", "python3 -m . 2>/dev/null || python3 main.py", "r" },
  { "test",
   "Test", "python3 -m pytest 2>/dev/null || python3 -m unittest discover",
   "t" },
  { "fmt", "Format", "black . 2>/dev/null || autopep8 -r --in-place .", "f" },
  { "lint", "Lint", "ruff check . 2>/dev/null || flake8 .", "l" },
  { "install",
   "Install", "pip install -e . 2>/dev/null || pip install -r requirements.txt",
   "i" },
  { NULL }
};
static const MvAction MV_MAVEN[] = {
  { "build", "Compile", "mvn compile", "b" },
  { "run", "Run", "mvn exec:java", "r" },
  { "brun", "Package+Run", "mvn package && java -jar target/*.jar", "R" },
  { "test", "Test", "mvn test", "t" },
  { "clean", "Clean", "mvn clean", "x" },
  { "pkg", "Package", "mvn package", "p" },
  { "install", "Install", "mvn install", "i" },
  { NULL }
};
static const MvAction MV_GRADLE[] = {
  { "build", "Build", "./gradlew build", "b" },
  { "run", "Run", "./gradlew run", "r" },
  { "brun", "Build & Run", "./gradlew build run", "R" },
  { "test", "Test", "./gradlew test", "t" },
  { "clean", "Clean", "./gradlew clean", "x" },
  { "pkg", "Jar", "./gradlew jar", "p" },
  { NULL }
};
static const MvAction MV_DOTNET[] = {
  { "build", "Build", "dotnet build", "b" },
  { "run", "Run", "dotnet run", "r" },
  { "brun", "Build & Run", "dotnet build && dotnet run", "R" },
  { "test", "Test", "dotnet test", "t" },
  { "clean", "Clean", "dotnet clean", "x" },
  { "fmt", "Format", "dotnet format", "f" },
  { "pkg", "Pack", "dotnet pack", "p" },
  { NULL }
};
static const MvAction MV_ZIG[] = {
  { "build", "Build", "zig build", "b" },
  { "run", "Run", "zig build run", "r" },
  { "brun", "Build & Run", "zig build run", "R" },
  { "test", "Test", "zig build test", "t" },
  { "clean", "Clean", "rm -rf zig-out zig-cache", "x" },
  { "fmt", "Format", "zig fmt .", "f" },
  { NULL }
};
static const MvAction MV_RUBY[] = {
  { "run", "Run", "bundle exec ruby main.rb 2>/dev/null || ruby main.rb", "r" },
  { "test",
   "Test", "bundle exec rspec 2>/dev/null || ruby -Itest test/**/*_test.rb",
   "t" },
  { "fmt", "Format", "bundle exec rubocop -a 2>/dev/null || rubocop -a", "f" },
  { "lint", "Lint", "bundle exec rubocop 2>/dev/null || rubocop", "l" },
  { "install", "Bundle", "bundle install", "i" },
  { NULL }
};

static const MvAction *mv_actions_for_type(void) {
  const char *t = g_marvin_proj.type;
  if(!strcmp(t, "cargo")) return MV_CARGO;
  if(!strcmp(t, "go")) return MV_GO;
  if(!strcmp(t, "cmake")) return MV_CMAKE;
  if(!strcmp(t, "meson")) return MV_MESON;
  if(!strcmp(t, "make")) return MV_MAKE;
  if(!strcmp(t, "npm")) return MV_NODE;
  if(!strcmp(t, "python")) return MV_PYTHON;
  if(!strcmp(t, "maven")) return MV_MAVEN;
  if(!strcmp(t, "gradle")) return MV_GRADLE;
  if(!strcmp(t, "dotnet")) return MV_DOTNET;
  if(!strcmp(t, "zig")) return MV_ZIG;
  if(!strcmp(t, "ruby")) return MV_RUBY;
  return MV_MAKE;
}

void mv_run_by_id(const char *id) {
  if(!g_marvin_proj.valid) return;
  const MvAction *a = mv_actions_for_type();
  for(; a->id; a++)
    if(!strcmp(a->id, id)) {
      mv_run(a->cmd, a->label, a->id);
      return;
    }
}

static const MarvinAction *marvin_actions(void) {
  static MarvinAction acts[24];
  memset(acts, 0, sizeof(acts));
  int n = 0;
  if(!g_marvin_proj.valid) {
    acts[n++] = (MarvinAction){ "No project detected", "", "", false };
    acts[n]   = (MarvinAction){ NULL };
    return acts;
  }
  const MvAction *src = mv_actions_for_type();
  for(; src->id && n < 23; src++)
    acts[n++] = (MarvinAction){ src->label, src->id, src->hint, true };
  acts[n] = (MarvinAction){ NULL };
  return acts;
}
static int marvin_total_items(void) {
  const MarvinAction *a = marvin_actions();
  int                 n = 0;
  while(a->label) {
    n++;
    a++;
  }
  return n;
}

/* ════════════════════════════════════════════════════════════════════════════
 * §G  marvin_poll
 * ════════════════════════════════════════════════════════════════════════════ */

void marvin_poll(const char *cwd_hint) {
  int64_t now = ov_now_ms();
  if(now < g_mv_detect_next) return;
  g_mv_detect_next = now + 2000;

  char cwd[512] = { 0 };
  if(cwd_hint && cwd_hint[0]) strncpy(cwd, cwd_hint, sizeof(cwd) - 1);
  if(!cwd[0]) getcwd(cwd, sizeof(cwd));
  if(!cwd[0]) return;

  char la[64], ls[32], lc[256];
  strncpy(la, g_marvin_proj.last_action, sizeof(la) - 1);
  strncpy(ls, g_marvin_proj.last_status, sizeof(ls) - 1);
  strncpy(lc, g_marvin_proj.last_cmd, sizeof(lc) - 1);
  char prev_root[512];
  strncpy(prev_root, g_marvin_proj.root, sizeof(prev_root) - 1);

  if(mv_detect(cwd)) {
    if(strcmp(g_marvin_proj.root, prev_root) == 0 && la[0]) {
      strncpy(g_marvin_proj.last_action, la, sizeof(g_marvin_proj.last_action) - 1);
      strncpy(g_marvin_proj.last_status, ls, sizeof(g_marvin_proj.last_status) - 1);
      strncpy(g_marvin_proj.last_cmd, lc, sizeof(g_marvin_proj.last_cmd) - 1);
    }
    char msg[640];
    snprintf(msg,
             sizeof(msg),
             "marvin: %s (%s/%s) @ %s",
             g_marvin_proj.name,
             g_marvin_proj.type,
             g_marvin_proj.lang,
             g_marvin_proj.root);
    log_ring_push(msg);
  }
}

/* ════════════════════════════════════════════════════════════════════════════
 * §H  Wizard state
 * ════════════════════════════════════════════════════════════════════════════ */

#define MV_MAX_ACTIONS      32
#define MV_ARGS_MAX         256
#define MV_WIZARD_STEPS_MAX 16
#define MV_OPT_MAX          12
#define MV_OPT_LABEL        64
#define MV_OPT_DESC         128
#define MV_STEP_LABEL       128
#define MV_BREADCRUMB_MAX   256

typedef enum {
  WIZ_NONE = 0,
  WIZ_MAKEFILE,
  WIZ_MESON,
  WIZ_COMPILEDB,
  WIZ_SCAFFOLD
} WizardType;
typedef enum { WSTEP_SELECT = 0, WSTEP_INPUT } WStepKind;

typedef struct {
  char label[MV_OPT_LABEL];
  char desc[MV_OPT_DESC];
  char value[64];
} WizOpt;
typedef struct {
  WStepKind kind;
  char      prompt[MV_STEP_LABEL];
  WizOpt    opts[MV_OPT_MAX];
  int       opt_count;
  int       sel;
  char      input_val[MV_ARGS_MAX];
  char      input_placeholder[MV_ARGS_MAX];
} WizStep;
typedef struct {
  char lang[32];
  char std[16];
  char compiler[32];
  char name[128];
  char src_dir[128];
  char inc_dir[128];
  char sanitizer[16];
  char profile[16];
  char module[256];
  char compiledb_method[32];
  char custom_cmd[256];
  char scaffold_type[32];
  char scaffold_name[128];
  char scaffold_parent[512];
  bool compile_commands;
  bool add_install;
  bool add_tests;
  char test_framework[16];
} WizAnswers;
typedef struct {
  bool       active;
  WizardType type;
  WizStep    steps[MV_WIZARD_STEPS_MAX];
  int        step_count;
  int        cur;
  WizAnswers ans;
  char       breadcrumb[MV_BREADCRUMB_MAX];
} WizState;

static WizState g_wiz                                         = { 0 };
static char     g_mv_action_args[MV_MAX_ACTIONS][MV_ARGS_MAX] = { 0 };
static bool     g_mv_args_editing                             = false;

/* ── Public API: context string + input-mode query ───────────────────────── */
void marvin_panel_ctx(char *buf, size_t sz) {
  if(!buf || sz == 0) return;
  buf[0] = '\0';
  if(g_wiz.active && g_wiz.breadcrumb[0]) {
    snprintf(buf, sz, "wizard: %s", g_wiz.breadcrumb);
    return;
  }
  static const char *tab_names[MARVIN_TAB_COUNT] = {
    "project", "tasks", "wizard", "console"
  };
  const char *tab = (g_marvin_tab < MARVIN_TAB_COUNT) ? tab_names[g_marvin_tab] : "";
  if(g_marvin_proj.valid && g_marvin_proj.name[0])
    snprintf(buf, sz, "%s  %s/%s", tab, g_marvin_proj.name, g_marvin_proj.type);
  else
    snprintf(buf, sz, "%s", tab);
}

bool marvin_panel_in_input(void) {
  if(!g_wiz.active) return false;
  if(g_wiz.cur < 0 || g_wiz.cur >= g_wiz.step_count) return false;
  return g_wiz.steps[g_wiz.cur].kind == WSTEP_INPUT;
}

/* ── wizard helpers ──────────────────────────────────────────────────────── */
static void wiz_push_step(WStepKind kind, const char *prompt) {
  if(g_wiz.step_count >= MV_WIZARD_STEPS_MAX) return;
  WizStep *s = &g_wiz.steps[g_wiz.step_count++];
  memset(s, 0, sizeof(*s));
  s->kind = kind;
  strncpy(s->prompt, prompt, MV_STEP_LABEL - 1);
}
static void wiz_add_opt(const char *label, const char *desc, const char *value) {
  if(!g_wiz.step_count) return;
  WizStep *s = &g_wiz.steps[g_wiz.step_count - 1];
  if(s->opt_count >= MV_OPT_MAX) return;
  WizOpt *o = &s->opts[s->opt_count++];
  strncpy(o->label, label, MV_OPT_LABEL - 1);
  strncpy(o->desc, desc, MV_OPT_DESC - 1);
  strncpy(o->value, value, 63);
}
static void wiz_set_input_placeholder(const char *ph) {
  if(!g_wiz.step_count) return;
  strncpy(g_wiz.steps[g_wiz.step_count - 1].input_placeholder, ph, MV_ARGS_MAX - 1);
}

/* ── system-app handoff ──────────────────────────────────────────────────── */
static void
mv_spawn_system_app(const char *wiz_type, const char *root, const char *extra_arg) {
  pid_t pid = fork();
  if(pid != 0) return;
  setsid();
  int maxfd = (int)sysconf(_SC_OPEN_MAX);
  for(int fd = 3; fd < maxfd; fd++)
    close(fd);
  if(extra_arg && extra_arg[0])
    execl("/usr/bin/marvin-ui",
          "marvin-ui",
          "--wizard",
          wiz_type,
          "--root",
          root,
          extra_arg,
          NULL);
  else
    execl("/usr/bin/marvin-ui",
          "marvin-ui",
          "--wizard",
          wiz_type,
          "--root",
          root,
          NULL);
  _exit(127);
}
static void
mv_handoff_wizard(const char *wiz_type, const char *root, const char *opts_json) {
  char msg[2048];
  snprintf(msg,
           sizeof(msg),
           "marvin_wizard %s %s %s",
           wiz_type,
           root,
           opts_json ? opts_json : "{}");
  if(!ipc_send_raw(msg)) mv_spawn_system_app(wiz_type, root, opts_json);
}

/* ── pkg-config helper ───────────────────────────────────────────────────── */
static void mv_detect_pkg_deps(const char *root,
                               char       *out,
                               int         outsz,
                               bool       *needs_posix,
                               bool       *needs_wlr_unstable) {
  out[0] = '\0';
  if(needs_posix) *needs_posix = false;
  if(needs_wlr_unstable) *needs_wlr_unstable = false;
  /* POSIX detection */
  {
    char cmd[1024];
    snprintf(cmd,
             sizeof(cmd),
             "grep -rql --include='*.c' --include='*.cpp' --include='*.h' "
             "-E '_POSIX_C_SOURCE|pthread\\.h|unistd\\.h|fork\\s*\\(|execv|"
             "getaddrinfo|opendir|mmap\\s*\\(' '%s' --exclude-dir=build "
             "--exclude-dir=builddir 2>/dev/null",
             root);
    FILE *f = popen(cmd, "r");
    if(f) {
      char b[8];
      if(needs_posix) *needs_posix = (fgets(b, sizeof(b), f) != NULL);
      pclose(f);
    }
  }
  /* wlroots */
  {
    char cmd[1024];
    snprintf(cmd,
             sizeof(cmd),
             "grep -rql --include='*.c' --include='*.cpp' --include='*.h' "
             "-E '#\\s*include\\s*[<\"]wlr/' '%s' --exclude-dir=build "
             "--exclude-dir=builddir 2>/dev/null",
             root);
    FILE *f = popen(cmd, "r");
    if(f) {
      char b[8];
      if(needs_wlr_unstable) *needs_wlr_unstable = (fgets(b, sizeof(b), f) != NULL);
      pclose(f);
    }
  }
  /* pkg-config scan */
  char inc_cmd[1024];
  snprintf(
      inc_cmd,
      sizeof(inc_cmd),
      "grep -rh --include='*.c' --include='*.cpp' --include='*.h' --include='*.hpp' "
      "-E '^\\s*#\\s*include\\s*[<\"][^>\"]+[>\"]' '%s' --exclude-dir=build "
      "2>/dev/null "
      "| sed -E 's/^[^<\"]*[<\"]([^>\"]+)[>\"]/\\1/' | sort -u",
      root);
  FILE *f = popen(inc_cmd, "r");
  if(!f) return;
  static const char *skip_list[] = {
    "stdio",  "stdlib", "string", "stdint", "stdbool", "stddef",  "stdarg",
    "assert", "errno",  "limits", "math",   "float",   "complex", "inttypes",
    "signal", "setjmp", "locale", "time",   "ctype",   "wchar",   "wctype",
    "unistd", "sys",    "bits",   NULL
  };
  char found[2048] = { 0 };
  int  fp          = 0;
  char inc_line[256];
  while(fgets(inc_line, sizeof(inc_line), f)) {
    int l = (int)strlen(inc_line);
    while(l > 0 && (inc_line[l - 1] == '\n' || inc_line[l - 1] == '\r'))
      inc_line[--l] = '\0';
    if(!l) continue;
    char  cand[128] = { 0 };
    char *slash     = strchr(inc_line, '/');
    if(slash) {
      int n = (int)(slash - inc_line);
      if(n > 0 && n < 64) {
        strncpy(cand, inc_line, n);
        cand[n] = '\0';
      }
    } else {
      strncpy(cand, inc_line, sizeof(cand) - 1);
      char *dot = strrchr(cand, '.');
      if(dot) *dot = '\0';
    }
    if(!cand[0]) continue;
    bool skip = false;
    for(int i = 0; skip_list[i]; i++)
      if(!strcmp(cand, skip_list[i])) {
        skip = true;
        break;
      }
    if(skip || strstr(found, cand)) continue;
    char chk[256];
    snprintf(chk, sizeof(chk), "pkg-config --exists '%s' 2>/dev/null", cand);
    if(system(chk) == 0) {
      if(fp + (int)strlen(cand) + 2 < (int)sizeof(found)) {
        if(fp > 0) found[fp++] = ' ';
        strcpy(found + fp, cand);
        fp += (int)strlen(cand);
      }
    }
  }
  pclose(f);
  strncpy(out, found, outsz - 1);
}

/* ── file writer ─────────────────────────────────────────────────────────── */
static void mv_write_file(const char *path, const char *content) {
  FILE *f = fopen(path, "w");
  if(!f) {
    mv_console_append("error: cannot write file");
    return;
  }
  fputs(content, f);
  fclose(f);
  char msg[512];
  snprintf(msg, sizeof(msg), "✓ wrote %s", path);
  mv_console_append(msg);
}

/* ── Makefile generator ──────────────────────────────────────────────────── */
static void mv_gen_makefile(const WizAnswers *a, const char *root) {
  char path[640];
  snprintf(path, sizeof(path), "%s/Makefile", root);
  bool is_cpp = !strcmp(a->lang, "cpp"), is_go = !strcmp(a->lang, "go"),
       is_rust = !strcmp(a->lang, "rust"), is_gen = !strcmp(a->lang, "generic");
  if(is_go) {
    char c[4096];
    snprintf(
        c,
        sizeof(c),
        "# %s — generated by Trixie Marvin\nBINARY := %s\nMODULE := %s\nBUILD_DIR "
        ":= build\n\n"
        ".PHONY: all build test lint fmt clean install\nall: build\n"
        "build:\n\t@mkdir -p $(BUILD_DIR)\n\tgo build -o $(BUILD_DIR)/$(BINARY) .\n"
        "test:\n\tgo test ./...\nfmt:\n\tgofmt -w .\nclean:\n\t@rm -rf "
        "$(BUILD_DIR)\n"
        "install:\n\tgo install .\n",
        a->name,
        a->name,
        a->module[0] ? a->module : ".");
    mv_write_file(path, c);
    return;
  }
  if(is_rust) {
    const char *pf = !strcmp(a->profile, "release") ? "--release" : "";
    const char *pd = !strcmp(a->profile, "release") ? "release" : "debug";
    char        c[4096];
    snprintf(c,
             sizeof(c),
             "# %s — generated by Trixie Marvin\nCARGO := cargo\nPFLAG := %s\nBIN "
             ":= target/%s/%s\n\n"
             ".PHONY: all build run test clippy fmt clean release\nall: build\n"
             "build:\n\t$(CARGO) build $(PFLAG)\nrun: build\n\t./$(BIN)\n"
             "test:\n\t$(CARGO) test\nclippy:\n\t$(CARGO) clippy -- -D warnings\n"
             "fmt:\n\t$(CARGO) fmt\nclean:\n\t$(CARGO) clean\nrelease:\n\t$(CARGO) "
             "build --release\n",
             a->name,
             pf,
             pd,
             a->name);
    mv_write_file(path, c);
    return;
  }
  if(is_gen) {
    char c[2048];
    snprintf(
        c,
        sizeof(c),
        "# %s — generated by Trixie Marvin\n.PHONY: all build test clean install\n"
        "all: build\nbuild:\n\t@echo 'Add build command'\ntest:\n\t@echo 'Add test "
        "command'\n"
        "clean:\n\t@echo 'Add clean command'\ninstall:\n\t@echo 'Add install "
        "command'\n",
        a->name);
    mv_write_file(path, c);
    return;
  }
  char pkg[512] = { 0 };
  bool np = false, nw = false;
  mv_detect_pkg_deps(root, pkg, sizeof(pkg), &np, &nw);
  const char *cc  = is_cpp ? (a->compiler[0] ? a->compiler : "g++")
                           : (a->compiler[0] ? a->compiler : "gcc");
  const char *var = is_cpp ? "CXXFLAGS" : "CFLAGS";
  const char *ext = is_cpp ? "cpp" : "c";
  const char *std = a->std[0] ? a->std : (is_cpp ? "c++17" : "c11");
  char        cf[1024];
  snprintf(cf,
           sizeof(cf),
           "-std=%s -Wall -Wextra -pedantic%s%s",
           std,
           np ? " -D_POSIX_C_SOURCE=200809L" : "",
           nw ? " -DWLR_USE_UNSTABLE" : "");
  if(a->sanitizer[0] && strcmp(a->sanitizer, "none")) {
    char sf[64] = { 0 };
    if(!strcmp(a->sanitizer, "asan"))
      strcpy(sf, "-fsanitize=address");
    else if(!strcmp(a->sanitizer, "tsan"))
      strcpy(sf, "-fsanitize=thread");
    else
      strcpy(sf, "-fsanitize=undefined");
    strncat(cf, " ", sizeof(cf) - strlen(cf) - 1);
    strncat(cf, sf, sizeof(cf) - strlen(cf) - 1);
  }
  char c[8192];
  if(pkg[0])
    snprintf(
        c,
        sizeof(c),
        "# %s — generated by Trixie Marvin\nCC := %s\nPKG_DEPS := %s\n"
        "PKG_CF := $(shell pkg-config --cflags $(PKG_DEPS))\n"
        "PKG_LF := $(shell pkg-config --libs $(PKG_DEPS))\n"
        "%s := %s $(PKG_CF)\nLDFLAGS := $(PKG_LF)\n"
        "SRC_DIR := %s\nINC_DIR := %s\nOBJ_DIR := build/obj\nBIN_DIR := build/bin\n"
        "TARGET := $(BIN_DIR)/%s\nSRCS := $(wildcard $(SRC_DIR)/*.%s)\n"
        "OBJS := $(patsubst $(SRC_DIR)/%%.%s,$(OBJ_DIR)/%%.o,$(SRCS))\nDEPS := "
        "$(OBJS:.o=.d)\n\n"
        ".PHONY: all clean test install\nall: $(TARGET)\n"
        "$(TARGET): $(OBJS) | $(BIN_DIR)\n\t$(CC) $^ -o $@ $(LDFLAGS)\n"
        "$(OBJ_DIR)/%%.o: $(SRC_DIR)/%%.%s | $(OBJ_DIR)\n\t$(CC) $(%s) -I$(INC_DIR) "
        "-MMD -MP -c $< -o $@\n"
        "-include $(DEPS)\n$(OBJ_DIR) $(BIN_DIR):\n\t@mkdir -p $@\n"
        "clean:\n\t@rm -rf build/\ntest:\n\t@echo 'No tests'\n"
        "install: $(TARGET)\n\t@install -m755 $(TARGET) /usr/local/bin/%s\n",
        a->name,
        cc,
        pkg,
        var,
        cf,
        a->src_dir[0] ? a->src_dir : "src",
        a->inc_dir[0] ? a->inc_dir : "include",
        a->name,
        ext,
        ext,
        ext,
        var,
        a->name);
  else
    snprintf(
        c,
        sizeof(c),
        "# %s — generated by Trixie Marvin\nCC := %s\n%s := %s\nLDFLAGS :=\n"
        "SRC_DIR := %s\nINC_DIR := %s\nOBJ_DIR := build/obj\nBIN_DIR := build/bin\n"
        "TARGET := $(BIN_DIR)/%s\nSRCS := $(wildcard $(SRC_DIR)/*.%s)\n"
        "OBJS := $(patsubst $(SRC_DIR)/%%.%s,$(OBJ_DIR)/%%.o,$(SRCS))\nDEPS := "
        "$(OBJS:.o=.d)\n\n"
        ".PHONY: all clean test install\nall: $(TARGET)\n"
        "$(TARGET): $(OBJS) | $(BIN_DIR)\n\t$(CC) $^ -o $@ $(LDFLAGS)\n"
        "$(OBJ_DIR)/%%.o: $(SRC_DIR)/%%.%s | $(OBJ_DIR)\n\t$(CC) $(%s) -I$(INC_DIR) "
        "-MMD -MP -c $< -o $@\n"
        "-include $(DEPS)\n$(OBJ_DIR) $(BIN_DIR):\n\t@mkdir -p $@\n"
        "clean:\n\t@rm -rf build/\ntest:\n\t@echo 'No tests'\n"
        "install: $(TARGET)\n\t@install -m755 $(TARGET) /usr/local/bin/%s\n",
        a->name,
        cc,
        var,
        cf,
        a->src_dir[0] ? a->src_dir : "src",
        a->inc_dir[0] ? a->inc_dir : "include",
        a->name,
        ext,
        ext,
        ext,
        var,
        a->name);
  mv_write_file(path, c);
  if(a->compile_commands)
    mv_console_append("tip: run  bear -- make  to generate compile_commands.json");
}

/* ── meson.build generator ───────────────────────────────────────────────── */
static void mv_gen_meson(const WizAnswers *a, const char *root) {
  char path[640];
  snprintf(path, sizeof(path), "%s/meson.build", root);
  bool        is_cpp = !strcmp(a->lang, "cpp");
  const char *ls = is_cpp ? "cpp" : "c", *sk = is_cpp ? "cpp_std" : "c_std",
             *sv = a->std[0] ? a->std : (is_cpp ? "c++17" : "c11");
  char pkg[512]  = { 0 };
  bool np = false, nw = false;
  mv_detect_pkg_deps(root, pkg, sizeof(pkg), &np, &nw);
  char dep_decls[2048] = { 0 }, dep_list[1024] = { 0 };
  if(pkg[0]) {
    char tmp[512];
    strncpy(tmp, pkg, sizeof(tmp) - 1);
    char *save = NULL, *tok = strtok_r(tmp, " ", &save);
    while(tok) {
      char vn[64];
      strncpy(vn, tok, sizeof(vn) - 1);
      for(char *p = vn; *p; p++)
        if(*p == '-' || *p == '.') *p = '_';
      char decl[256];
      snprintf(decl,
               sizeof(decl),
               "%s_dep = dependency('%s', required : true)\n",
               vn,
               tok);
      strncat(dep_decls, decl, sizeof(dep_decls) - strlen(dep_decls) - 1);
      if(dep_list[0])
        strncat(dep_list, ", ", sizeof(dep_list) - strlen(dep_list) - 1);
      char ref[80];
      snprintf(ref, sizeof(ref), "%s_dep", vn);
      strncat(dep_list, ref, sizeof(dep_list) - strlen(dep_list) - 1);
      tok = strtok_r(NULL, " ", &save);
    }
  }
  char ca[512] = { 0 };
  if(np || nw || (a->sanitizer[0] && strcmp(a->sanitizer, "none"))) {
    strcpy(ca, "c_args = [");
    if(np) strcat(ca, "'-D_POSIX_C_SOURCE=200809L', ");
    if(nw) strcat(ca, "'-DWLR_USE_UNSTABLE', ");
    if(!strcmp(a->sanitizer, "asan"))
      strcat(ca, "'-fsanitize=address', '-fno-omit-frame-pointer', ");
    else if(!strcmp(a->sanitizer, "tsan"))
      strcat(ca, "'-fsanitize=thread', ");
    else if(!strcmp(a->sanitizer, "ubsan"))
      strcat(ca, "'-fsanitize=undefined', ");
    size_t cl = strlen(ca);
    if(cl >= 2 && ca[cl - 2] == ',') {
      ca[cl - 2] = ']';
      ca[cl - 1] = '\0';
    } else
      strcat(ca, "]");
  }
  char c[8192];
  if(dep_list[0])
    snprintf(c,
             sizeof(c),
             "# %s — generated by Trixie Marvin\nproject('%s', '%s',\n"
             "  version : '0.1.0',\n  default_options : "
             "['warning_level=3','%s=%s','buildtype=debugoptimized'])\n\n"
             "src = files(\n  # add source files, e.g.: 'src/main.%s',\n)\n"
             "inc = include_directories('%s')\n\n%s\n%s\n"
             "executable('%s', src,\n  include_directories : inc,\n  dependencies : "
             "[%s],\n%s"
             "  link_args : ['-Wl,--as-needed'],\n  install : %s,\n)\n",
             a->name,
             a->name,
             ls,
             sk,
             sv,
             is_cpp ? "cpp" : "c",
             a->inc_dir[0] ? a->inc_dir : "include",
             dep_decls,
             ca[0] ? ca : "",
             a->name,
             dep_list,
             ca[0] ? "  c_args : c_args,\n" : "",
             a->add_install ? "true" : "false");
  else
    snprintf(c,
             sizeof(c),
             "# %s — generated by Trixie Marvin\nproject('%s', '%s',\n"
             "  version : '0.1.0',\n  default_options : "
             "['warning_level=3','%s=%s','buildtype=debugoptimized'])\n\n"
             "src = files(\n  # add source files, e.g.: 'src/main.%s',\n)\n"
             "inc = include_directories('%s')\n\n%s\n"
             "executable('%s', src,\n  include_directories : inc,\n%s"
             "  link_args : ['-Wl,--as-needed'],\n  install : %s,\n)\n",
             a->name,
             a->name,
             ls,
             sk,
             sv,
             is_cpp ? "cpp" : "c",
             a->inc_dir[0] ? a->inc_dir : "include",
             ca[0] ? ca : "",
             a->name,
             ca[0] ? "  c_args : c_args,\n" : "",
             a->add_install ? "true" : "false");
  mv_write_file(path, c);
}

/* ── compile_commands runner ─────────────────────────────────────────────── */
static void mv_gen_compiledb(const WizAnswers *a, const char *root) {
  char cmd[1024] = { 0 }, title[64] = "compile_commands.json";
  if(!strcmp(a->compiledb_method, "cmake"))
    snprintf(cmd,
             sizeof(cmd),
             "cd '%s' && cmake -B build -S . -DCMAKE_EXPORT_COMPILE_COMMANDS=ON && "
             "ln -sf build/compile_commands.json .",
             root);
  else if(!strcmp(a->compiledb_method, "meson"))
    snprintf(
        cmd,
        sizeof(cmd),
        "cd '%s' && meson setup builddir && ln -sf builddir/compile_commands.json .",
        root);
  else if(!strcmp(a->compiledb_method, "bear_make"))
    snprintf(cmd, sizeof(cmd), "cd '%s' && bear -- make", root);
  else if(!strcmp(a->compiledb_method, "bear_custom"))
    snprintf(cmd,
             sizeof(cmd),
             "cd '%s' && bear -- %s",
             root,
             a->custom_cmd[0] ? a->custom_cmd : "make");
  else if(!strcmp(a->compiledb_method, "compiledb"))
    snprintf(cmd, sizeof(cmd), "cd '%s' && compiledb make", root);
  if(cmd[0]) mv_run(cmd, title, "compiledb");
}

/* ── scaffold runner ─────────────────────────────────────────────────────── */
static void mv_gen_scaffold(const WizAnswers *a) {
  char        cmd[1024] = { 0 }, title[128] = { 0 };
  const char *parent =
      a->scaffold_parent[0] ? a->scaffold_parent : g_marvin_proj.root;
  if(parent[0]) {
    char mk[640];
    snprintf(mk, sizeof(mk), "mkdir -p '%s'", parent);
    system(mk);
  }
  if(!strcmp(a->scaffold_type, "cargo_bin")) {
    snprintf(
        cmd, sizeof(cmd), "cd '%s' && cargo new '%s'", parent, a->scaffold_name);
    snprintf(title, sizeof(title), "cargo new %s", a->scaffold_name);
  } else if(!strcmp(a->scaffold_type, "cargo_lib")) {
    snprintf(cmd,
             sizeof(cmd),
             "cd '%s' && cargo new --lib '%s'",
             parent,
             a->scaffold_name);
    snprintf(title, sizeof(title), "cargo new --lib %s", a->scaffold_name);
  } else if(!strcmp(a->scaffold_type, "go_mod")) {
    snprintf(cmd,
             sizeof(cmd),
             "mkdir -p '%s/%s' && cd '%s/%s' && go mod init '%s'",
             parent,
             a->scaffold_name,
             parent,
             a->scaffold_name,
             a->module[0] ? a->module : a->scaffold_name);
    snprintf(title, sizeof(title), "go mod init %s", a->scaffold_name);
  }
  if(cmd[0]) {
    mv_run(cmd, title, "scaffold");
    char opts[512];
    snprintf(opts,
             sizeof(opts),
             "{\"type\":\"%s\",\"name\":\"%s\",\"parent\":\"%s\"}",
             a->scaffold_type,
             a->scaffold_name,
             parent);
    mv_handoff_wizard("scaffold", parent, opts);
  }
}

/* ════════════════════════════════════════════════════════════════════════════
 * §I  Wizard — step builders & advance
 * ════════════════════════════════════════════════════════════════════════════ */

static void wiz_finish(void); /* forward */

static void wiz_begin_makefile(void) {
  memset(&g_wiz, 0, sizeof(g_wiz));
  g_wiz.active = true;
  g_wiz.type   = WIZ_MAKEFILE;
  strncpy(g_wiz.breadcrumb, "New Makefile", MV_BREADCRUMB_MAX - 1);
  wiz_push_step(WSTEP_SELECT, "Language");
  wiz_add_opt("C", "gcc/clang, *.c sources", "c");
  wiz_add_opt("C++", "g++/clang++, *.cpp sources", "cpp");
  wiz_add_opt("Go", "go build wrapper", "go");
  wiz_add_opt("Rust", "cargo wrapper", "rust");
  wiz_add_opt("Generic", "Minimal skeleton", "generic");
}
static void wiz_begin_meson(void) {
  memset(&g_wiz, 0, sizeof(g_wiz));
  g_wiz.active = true;
  g_wiz.type   = WIZ_MESON;
  strncpy(g_wiz.breadcrumb, "New meson.build", MV_BREADCRUMB_MAX - 1);
  wiz_push_step(WSTEP_SELECT, "Language");
  wiz_add_opt("C", "c, *.c sources", "c");
  wiz_add_opt("C++", "cpp, *.cpp sources", "cpp");
}
static void wiz_begin_compiledb(void) {
  memset(&g_wiz, 0, sizeof(g_wiz));
  g_wiz.active = true;
  g_wiz.type   = WIZ_COMPILEDB;
  strncpy(g_wiz.breadcrumb, "Generate compile_commands.json", MV_BREADCRUMB_MAX - 1);
  bool hcm = mv_file(g_marvin_proj.root, "CMakeLists.txt"),
       hms = mv_file(g_marvin_proj.root, "meson.build"),
       hmk = mv_file(g_marvin_proj.root, "Makefile") ||
             mv_file(g_marvin_proj.root, "GNUmakefile"),
       brok = (access("/usr/bin/bear", X_OK) == 0 ||
               access("/usr/local/bin/bear", X_OK) == 0),
       cdbk = (access("/usr/bin/compiledb", X_OK) == 0 ||
               access("/usr/local/bin/compiledb", X_OK) == 0);
  wiz_push_step(WSTEP_SELECT, "Method");
  if(hcm)
    wiz_add_opt("CMake (recommended)",
                "cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                "cmake");
  if(hms) wiz_add_opt("Meson (recommended)", "meson setup builddir", "meson");
  if(brok && hmk) wiz_add_opt("bear + make", "bear -- make", "bear_make");
  if(brok) wiz_add_opt("bear + custom…", "bear -- <cmd>", "bear_custom");
  if(cdbk) wiz_add_opt("compiledb", "compiledb make", "compiledb");
  if(!g_wiz.steps[0].opt_count)
    wiz_add_opt("(No tool detected)", "Install cmake, meson, or bear first", "none");
}
static void wiz_begin_scaffold(void) {
  memset(&g_wiz, 0, sizeof(g_wiz));
  g_wiz.active = true;
  g_wiz.type   = WIZ_SCAFFOLD;
  strncpy(g_wiz.breadcrumb, "New Project Scaffold", MV_BREADCRUMB_MAX - 1);
  wiz_push_step(WSTEP_SELECT, "Project type");
  wiz_add_opt("Cargo binary", "cargo new <name>", "cargo_bin");
  wiz_add_opt("Cargo library", "cargo new --lib <name>", "cargo_lib");
  wiz_add_opt("Go module", "go mod init <module path>", "go_mod");
}

static void wiz_advance(void) {
  if(!g_wiz.active) return;
  WizStep    *s   = &g_wiz.steps[g_wiz.cur];
  const char *val = (s->kind == WSTEP_SELECT) ? s->opts[s->sel].value : s->input_val;

  if(g_wiz.type == WIZ_MAKEFILE) {
    switch(g_wiz.cur) {
      case 0:
        strncpy(g_wiz.ans.lang, val, sizeof(g_wiz.ans.lang) - 1);
        if(!strcmp(val, "generic") || !strcmp(val, "go") || !strcmp(val, "rust")) {
          wiz_push_step(WSTEP_INPUT, "Project name");
          wiz_set_input_placeholder(g_marvin_proj.name[0] ? g_marvin_proj.name
                                                          : "myapp");
        } else {
          wiz_push_step(WSTEP_SELECT, "Compiler");
          if(!strcmp(val, "cpp")) {
            wiz_add_opt("g++", "GNU C++", "g++");
            wiz_add_opt("clang++", "LLVM", "clang++");
          } else {
            wiz_add_opt("gcc", "GNU C", "gcc");
            wiz_add_opt("clang", "LLVM", "clang");
          }
        }
        break;
      case 1:
        if(!strcmp(g_wiz.ans.lang, "generic")) {
          strncpy(g_wiz.ans.name, val, sizeof(g_wiz.ans.name) - 1);
          wiz_finish();
          return;
        }
        if(!strcmp(g_wiz.ans.lang, "go")) {
          strncpy(g_wiz.ans.name, val, sizeof(g_wiz.ans.name) - 1);
          wiz_push_step(WSTEP_INPUT, "Go module path");
          wiz_set_input_placeholder("github.com/you/myapp");
          break;
        }
        if(!strcmp(g_wiz.ans.lang, "rust")) {
          strncpy(g_wiz.ans.name, val, sizeof(g_wiz.ans.name) - 1);
          wiz_push_step(WSTEP_SELECT, "Build profile");
          wiz_add_opt("dev", "Fast compile, debug symbols", "dev");
          wiz_add_opt("release", "Optimised binary", "release");
          break;
        }
        strncpy(g_wiz.ans.compiler, val, sizeof(g_wiz.ans.compiler) - 1);
        {
          bool ic = !strcmp(g_wiz.ans.lang, "cpp");
          wiz_push_step(WSTEP_SELECT, "Standard");
          if(ic) {
            wiz_add_opt("C++17", "Recommended", "c++17");
            wiz_add_opt("C++20", "Concepts", "c++20");
            wiz_add_opt("C++23", "Latest", "c++23");
            wiz_add_opt("C++14", "Lambdas", "c++14");
          } else {
            wiz_add_opt("C11", "Recommended", "c11");
            wiz_add_opt("C17", "Latest stable", "c17");
            wiz_add_opt("C99", "Wide compat", "c99");
          }
        }
        break;
      case 2:
        if(!strcmp(g_wiz.ans.lang, "go")) {
          strncpy(g_wiz.ans.module, val, sizeof(g_wiz.ans.module) - 1);
          wiz_finish();
          return;
        }
        if(!strcmp(g_wiz.ans.lang, "rust")) {
          strncpy(g_wiz.ans.profile, val, sizeof(g_wiz.ans.profile) - 1);
          wiz_finish();
          return;
        }
        strncpy(g_wiz.ans.std, val, sizeof(g_wiz.ans.std) - 1);
        wiz_push_step(WSTEP_SELECT, "Sanitizer");
        wiz_add_opt("None", "No sanitizer", "none");
        wiz_add_opt("AddressSanitizer", "-fsanitize=address", "asan");
        wiz_add_opt("ThreadSanitizer", "-fsanitize=thread", "tsan");
        wiz_add_opt("UBSanitizer", "-fsanitize=undefined", "ubsan");
        break;
      case 3:
        strncpy(g_wiz.ans.sanitizer, val, sizeof(g_wiz.ans.sanitizer) - 1);
        wiz_push_step(WSTEP_INPUT, "Project name");
        wiz_set_input_placeholder(g_marvin_proj.name[0] ? g_marvin_proj.name
                                                        : "myproject");
        break;
      case 4:
        strncpy(g_wiz.ans.name, val, sizeof(g_wiz.ans.name) - 1);
        wiz_push_step(WSTEP_INPUT, "Source directory");
        wiz_set_input_placeholder("src");
        break;
      case 5:
        strncpy(
            g_wiz.ans.src_dir, val[0] ? val : "src", sizeof(g_wiz.ans.src_dir) - 1);
        wiz_push_step(WSTEP_INPUT, "Include directory");
        wiz_set_input_placeholder("include");
        break;
      case 6:
        strncpy(g_wiz.ans.inc_dir,
                val[0] ? val : "include",
                sizeof(g_wiz.ans.inc_dir) - 1);
        wiz_push_step(WSTEP_SELECT, "compile_commands.json hint?");
        wiz_add_opt("Yes", "", "yes");
        wiz_add_opt("No", "", "no");
        break;
      case 7:
        g_wiz.ans.compile_commands = (!strcmp(val, "yes"));
        wiz_finish();
        return;
    }
  } else if(g_wiz.type == WIZ_MESON) {
    switch(g_wiz.cur) {
      case 0:
        strncpy(g_wiz.ans.lang, val, sizeof(g_wiz.ans.lang) - 1);
        {
          bool ic = !strcmp(val, "cpp");
          wiz_push_step(WSTEP_SELECT, "Standard");
          if(ic) {
            wiz_add_opt("C++17", "Recommended", "c++17");
            wiz_add_opt("C++20", "", "c++20");
            wiz_add_opt("C++23", "", "c++23");
            wiz_add_opt("C++14", "", "c++14");
          } else {
            wiz_add_opt("C11", "Recommended", "c11");
            wiz_add_opt("C17", "", "c17");
            wiz_add_opt("C99", "", "c99");
          }
        }
        break;
      case 1:
        strncpy(g_wiz.ans.std, val, sizeof(g_wiz.ans.std) - 1);
        wiz_push_step(WSTEP_SELECT, "Sanitizer");
        wiz_add_opt("None", "", "none");
        wiz_add_opt("AddressSanitizer", "", "asan");
        wiz_add_opt("ThreadSanitizer", "", "tsan");
        wiz_add_opt("UBSanitizer", "", "ubsan");
        break;
      case 2:
        strncpy(g_wiz.ans.sanitizer, val, sizeof(g_wiz.ans.sanitizer) - 1);
        wiz_push_step(WSTEP_INPUT, "Project name");
        wiz_set_input_placeholder(g_marvin_proj.name[0] ? g_marvin_proj.name
                                                        : "myproject");
        break;
      case 3:
        strncpy(g_wiz.ans.name, val, sizeof(g_wiz.ans.name) - 1);
        wiz_push_step(WSTEP_INPUT, "Include directory");
        wiz_set_input_placeholder("include");
        break;
      case 4:
        strncpy(g_wiz.ans.inc_dir,
                val[0] ? val : "include",
                sizeof(g_wiz.ans.inc_dir) - 1);
        wiz_push_step(WSTEP_SELECT, "Add install rules?");
        wiz_add_opt("No", "", "no");
        wiz_add_opt("Yes", "", "yes");
        break;
      case 5:
        g_wiz.ans.add_install = (!strcmp(val, "yes"));
        wiz_finish();
        return;
    }
  } else if(g_wiz.type == WIZ_COMPILEDB) {
    switch(g_wiz.cur) {
      case 0:
        strncpy(
            g_wiz.ans.compiledb_method, val, sizeof(g_wiz.ans.compiledb_method) - 1);
        if(!strcmp(val, "none")) {
          g_wiz.active = false;
          return;
        }
        if(!strcmp(val, "bear_custom")) {
          wiz_push_step(WSTEP_INPUT, "Build command for bear");
          wiz_set_input_placeholder("make");
        } else {
          wiz_finish();
          return;
        }
        break;
      case 1:
        strncpy(g_wiz.ans.custom_cmd, val, sizeof(g_wiz.ans.custom_cmd) - 1);
        wiz_finish();
        return;
    }
  } else if(g_wiz.type == WIZ_SCAFFOLD) {
    switch(g_wiz.cur) {
      case 0:
        strncpy(g_wiz.ans.scaffold_type, val, sizeof(g_wiz.ans.scaffold_type) - 1);
        wiz_push_step(WSTEP_INPUT, "Project name");
        wiz_set_input_placeholder("myapp");
        break;
      case 1:
        strncpy(g_wiz.ans.scaffold_name, val, sizeof(g_wiz.ans.scaffold_name) - 1);
        if(!strcmp(g_wiz.ans.scaffold_type, "go_mod")) {
          wiz_push_step(WSTEP_INPUT, "Go module path");
          char ph[256];
          snprintf(ph, sizeof(ph), "github.com/you/%s", val);
          wiz_set_input_placeholder(ph);
        } else {
          wiz_push_step(WSTEP_INPUT, "Parent directory");
          const char *h = getenv("HOME");
          char        ph[512];
          snprintf(ph, sizeof(ph), "%s/Code", h ? h : "/home");
          wiz_set_input_placeholder(ph);
        }
        break;
      case 2:
        if(!strcmp(g_wiz.ans.scaffold_type, "go_mod")) {
          strncpy(g_wiz.ans.module, val, sizeof(g_wiz.ans.module) - 1);
          wiz_push_step(WSTEP_INPUT, "Parent directory");
          const char *h = getenv("HOME");
          char        ph[512];
          snprintf(ph, sizeof(ph), "%s/Code", h ? h : "/home");
          wiz_set_input_placeholder(ph);
        } else {
          strncpy(
              g_wiz.ans.scaffold_parent, val, sizeof(g_wiz.ans.scaffold_parent) - 1);
          wiz_finish();
          return;
        }
        break;
      case 3:
        strncpy(
            g_wiz.ans.scaffold_parent, val, sizeof(g_wiz.ans.scaffold_parent) - 1);
        wiz_finish();
        return;
    }
  }
  g_wiz.cur = g_wiz.step_count - 1;
}

static void wiz_finish(void) {
  g_wiz.active     = false;
  const char *root = g_marvin_proj.root[0] ? g_marvin_proj.root : ".";
  switch(g_wiz.type) {
    case WIZ_MAKEFILE:
      mv_gen_makefile(&g_wiz.ans, root);
      mv_handoff_wizard("makefile", root, NULL);
      g_mv_detect_next = 0;
      marvin_poll(root);
      g_marvin_tab = MARVIN_TAB_CONSOLE;
      break;
    case WIZ_MESON:
      mv_gen_meson(&g_wiz.ans, root);
      mv_handoff_wizard("meson", root, NULL);
      g_mv_detect_next = 0;
      marvin_poll(root);
      g_marvin_tab = MARVIN_TAB_CONSOLE;
      break;
    case WIZ_COMPILEDB:
      mv_gen_compiledb(&g_wiz.ans, root);
      g_marvin_tab = MARVIN_TAB_CONSOLE;
      break;
    case WIZ_SCAFFOLD:
      mv_gen_scaffold(&g_wiz.ans);
      g_marvin_tab = MARVIN_TAB_CONSOLE;
      break;
    default: break;
  }
}

/* ════════════════════════════════════════════════════════════════════════════
 * §J  marvin_panel_key
 * ════════════════════════════════════════════════════════════════════════════ */

extern int  *overlay_scroll_ptr(TrixieOverlay *o);
extern char *overlay_fb_cwd_ptr(TrixieOverlay *o);

bool marvin_panel_key(TrixieOverlay *o, xkb_keysym_t sym, uint32_t mods) {
  (void)mods;
  int  *scroll = overlay_scroll_ptr(o);
  char *fb_cwd = overlay_fb_cwd_ptr(o);

  /* ── Active wizard intercept ─────────────────────────────────────────── */
  if(g_wiz.active) {
    WizStep *s = &g_wiz.steps[g_wiz.cur];
    if(sym == XKB_KEY_Escape) {
      if(g_wiz.cur > 0) {
        g_wiz.step_count = g_wiz.cur;
        g_wiz.cur--;
      } else
        g_wiz.active = false;
      return true;
    }
    if(s->kind == WSTEP_SELECT) {
      if(sym == XKB_KEY_j || sym == XKB_KEY_Down) {
        if(s->sel < s->opt_count - 1) s->sel++;
        return true;
      }
      if(sym == XKB_KEY_k || sym == XKB_KEY_Up) {
        if(s->sel > 0) s->sel--;
        return true;
      }
      if(sym == XKB_KEY_Return) {
        wiz_advance();
        return true;
      }
    } else {
      int l = (int)strlen(s->input_val);
      if(sym == XKB_KEY_BackSpace) {
        if(l > 0) s->input_val[l - 1] = '\0';
        return true;
      }
      if(sym == XKB_KEY_Return) {
        if(!s->input_val[0] && s->input_placeholder[0])
          strncpy(s->input_val, s->input_placeholder, MV_ARGS_MAX - 1);
        wiz_advance();
        return true;
      }
      if(sym >= 0x20 && sym < 0x7f && l < MV_ARGS_MAX - 1) {
        s->input_val[l]     = (char)sym;
        s->input_val[l + 1] = '\0';
      }
      return true;
    }
    return true;
  }

  /* ── Args-editing intercept ──────────────────────────────────────────── */
  if(g_mv_args_editing) {
    int   ai   = g_marvin_cursor;
    char *args = g_mv_action_args[ai < MV_MAX_ACTIONS ? ai : 0];
    int   l    = (int)strlen(args);
    if(sym == XKB_KEY_Escape) {
      g_mv_args_editing = false;
      return true;
    }
    if(sym == XKB_KEY_Return) {
      g_mv_args_editing        = false;
      const MarvinAction *acts = marvin_actions();
      if(ai < marvin_total_items()) {
        const char *id = acts[ai].nvim_cmd;
        if(id && id[0]) {
          const MvAction *src = mv_actions_for_type();
          for(; src->id; src++)
            if(!strcmp(src->id, id)) {
              char full[1024];
              if(args[0])
                snprintf(full, sizeof(full), "%s %s", src->cmd, args);
              else
                strncpy(full, src->cmd, sizeof(full) - 1);
              mv_run(full, src->label, src->id);
              g_marvin_tab = MARVIN_TAB_CONSOLE;
              break;
            }
        }
      }
      return true;
    }
    if(sym == XKB_KEY_BackSpace) {
      if(l > 0) args[l - 1] = '\0';
      return true;
    }
    if(sym >= 0x20 && sym < 0x7f && l < MV_ARGS_MAX - 1) {
      args[l]     = (char)sym;
      args[l + 1] = '\0';
    }
    return true;
  }

  /* ── Sub-tab cycling ─────────────────────────────────────────────────── */
  if(sym == XKB_KEY_bracketright) {
    g_marvin_tab    = (MarvinTab)((g_marvin_tab + 1) % MARVIN_TAB_COUNT);
    g_marvin_cursor = 0;
    *scroll         = 0;
    return true;
  }
  if(sym == XKB_KEY_bracketleft) {
    g_marvin_tab =
        (MarvinTab)((g_marvin_tab + MARVIN_TAB_COUNT - 1) % MARVIN_TAB_COUNT);
    g_marvin_cursor = 0;
    *scroll         = 0;
    return true;
  }
  if(sym == XKB_KEY_Tab) {
    bool rev = !!(mods & (1u << 0));
    g_marvin_tab =
        (MarvinTab)(rev ? (g_marvin_tab + MARVIN_TAB_COUNT - 1) % MARVIN_TAB_COUNT
                        : (g_marvin_tab + 1) % MARVIN_TAB_COUNT);
    g_marvin_cursor = 0;
    *scroll         = 0;
    return true;
  }

  /* ── TASKS tab ───────────────────────────────────────────────────────── */
  if(g_marvin_tab == MARVIN_TAB_TASKS) {
    int                 total = marvin_total_items();
    const MarvinAction *acts  = marvin_actions();
    if(sym == XKB_KEY_j || sym == XKB_KEY_Down) {
      if(g_marvin_cursor < total - 1) g_marvin_cursor++;
      while(acts[g_marvin_cursor].label &&
            !strcmp(acts[g_marvin_cursor].label, "---") &&
            g_marvin_cursor < total - 1)
        g_marvin_cursor++;
      return true;
    }
    if(sym == XKB_KEY_k || sym == XKB_KEY_Up) {
      if(g_marvin_cursor > 0) g_marvin_cursor--;
      while(acts[g_marvin_cursor].label &&
            !strcmp(acts[g_marvin_cursor].label, "---") && g_marvin_cursor > 0)
        g_marvin_cursor--;
      return true;
    }
    if(sym == XKB_KEY_g) {
      g_marvin_cursor = 0;
      return true;
    }
    if(sym == XKB_KEY_G) {
      g_marvin_cursor = total - 1;
      return true;
    }
    if(sym == XKB_KEY_Return || sym == XKB_KEY_space) {
      const MarvinAction *a = &acts[g_marvin_cursor];
      if(a->label && strcmp(a->label, "---") && a->nvim_cmd) {
        const char     *saved = g_mv_action_args[g_marvin_cursor];
        const MvAction *src   = mv_actions_for_type();
        for(; src->id; src++)
          if(!strcmp(src->id, a->nvim_cmd)) {
            char full[1024];
            if(saved[0])
              snprintf(full, sizeof(full), "%s %s", src->cmd, saved);
            else
              strncpy(full, src->cmd, sizeof(full) - 1);
            mv_run(full, src->label, src->id);
            g_marvin_tab = MARVIN_TAB_CONSOLE;
            break;
          }
      }
      return true;
    }
    if(sym == XKB_KEY_A) {
      const MarvinAction *a = &acts[g_marvin_cursor];
      if(a->label && strcmp(a->label, "---") && a->nvim_cmd)
        g_mv_args_editing = true;
      return true;
    }
    if(sym == XKB_KEY_o) {
      g_mv_tasks_output_expanded = !g_mv_tasks_output_expanded;
      return true;
    }
  }

  /* ── WIZARD tab ──────────────────────────────────────────────────────── */
  if(g_marvin_tab == MARVIN_TAB_WIZARD) {
    static int wiz_sel = 0;
    if(sym == XKB_KEY_j || sym == XKB_KEY_Down) {
      if(wiz_sel < 3) wiz_sel++;
      return true;
    }
    if(sym == XKB_KEY_k || sym == XKB_KEY_Up) {
      if(wiz_sel > 0) wiz_sel--;
      return true;
    }
    if(sym == XKB_KEY_Return) {
      switch(wiz_sel) {
        case 0: wiz_begin_makefile(); break;
        case 1: wiz_begin_meson(); break;
        case 2: wiz_begin_compiledb(); break;
        case 3: wiz_begin_scaffold(); break;
      }
      return true;
    }
  }

  /* ── PROJECT / CONSOLE scroll ────────────────────────────────────────── */
  if(g_marvin_tab == MARVIN_TAB_PROJECT || g_marvin_tab == MARVIN_TAB_CONSOLE) {
    if(sym == XKB_KEY_j || sym == XKB_KEY_Down) {
      (*scroll)++;
      return true;
    }
    if(sym == XKB_KEY_k || sym == XKB_KEY_Up) {
      if(*scroll > 0) (*scroll)--;
      return true;
    }
    if(sym == XKB_KEY_g) {
      *scroll = 0;
      return true;
    }
    if(sym == XKB_KEY_G) {
      *scroll = 9999;
      return true;
    }
    if(sym == XKB_KEY_c && g_marvin_tab == MARVIN_TAB_CONSOLE) {
      pthread_mutex_lock(&g_marvin_con_lock);
      g_marvin_console_len = 0;
      g_marvin_console[0]  = '\0';
      pthread_mutex_unlock(&g_marvin_con_lock);
      return true;
    }
  }

  /* ── Quick-run keys ──────────────────────────────────────────────────── */
  if(sym == XKB_KEY_b) {
    mv_run_by_id("build");
    g_marvin_tab = MARVIN_TAB_CONSOLE;
    return true;
  }
  if(sym == XKB_KEY_r) {
    mv_run_by_id("run");
    g_marvin_tab = MARVIN_TAB_CONSOLE;
    return true;
  }
  if(sym == XKB_KEY_R) {
    mv_run_by_id("brun");
    g_marvin_tab = MARVIN_TAB_CONSOLE;
    return true;
  }
  if(sym == XKB_KEY_t) {
    mv_run_by_id("test");
    g_marvin_tab = MARVIN_TAB_CONSOLE;
    return true;
  }
  if(sym == XKB_KEY_x) {
    mv_run_by_id("clean");
    g_marvin_tab = MARVIN_TAB_CONSOLE;
    return true;
  }
  if(sym == XKB_KEY_f) {
    mv_run_by_id("fmt");
    g_marvin_tab = MARVIN_TAB_CONSOLE;
    return true;
  }
  if(sym == XKB_KEY_l) {
    mv_run_by_id("lint");
    g_marvin_tab = MARVIN_TAB_CONSOLE;
    return true;
  }
  if(sym == XKB_KEY_p) {
    mv_run_by_id("pkg");
    g_marvin_tab = MARVIN_TAB_CONSOLE;
    return true;
  }
  if(sym == XKB_KEY_i) {
    mv_run_by_id("install");
    g_marvin_tab = MARVIN_TAB_CONSOLE;
    return true;
  }
  if(sym == XKB_KEY_comma) {
    g_mv_detect_next = 0;
    marvin_poll(fb_cwd);
    return true;
  }
  return true;
}

/* ════════════════════════════════════════════════════════════════════════════
 * §K  Wizard modal renderer
 * ════════════════════════════════════════════════════════════════════════════ */

static void draw_wizard_modal(
    uint32_t *px, int stride, int px0, int py0, int pw, int ph, const Config *cfg) {
  Color ac = cfg->colors.active_border, bg = cfg->colors.pane_bg;
  int   cw = stride, ch = py0 + ph;
  ov_fill_rect(px, stride, px0, py0, pw, ph, 0, 0, 0, 0x90, cw, ch);
  int mw = pw * 6 / 10, mh = ph / 2;
  if(mw < 400) mw = 400;
  if(mh < 200) mh = 200;
  int mx = px0 + (pw - mw) / 2, my = py0 + (ph - mh) / 2;
  ov_fill_rect(px, stride, mx, my, mw, mh, bg.r, bg.g, bg.b, 0xff, cw, ch);
  ov_fill_border(px, stride, mx, my, mw, mh, ac.r, ac.g, ac.b, 0xff, cw, ch);
  int     hh = ROW_H + 4;
  uint8_t hr = (uint8_t)(bg.r > 0x10 ? bg.r - 0x10 : 0),
          hg = (uint8_t)(bg.g > 0x10 ? bg.g - 0x10 : 0),
          hb = (uint8_t)(bg.b > 0x10 ? bg.b - 0x10 : 0);
  ov_fill_rect(
      px, stride, mx + BDR, my + BDR, mw - BDR * 2, hh, hr, hg, hb, 0xff, cw, ch);
  int hty = my + BDR + (hh - g_ov_th) / 2 + g_ov_asc;
  ov_draw_text(px,
               stride,
               mx + BDR + PAD,
               hty,
               cw,
               ch,
               g_wiz.breadcrumb,
               ac.r,
               ac.g,
               ac.b,
               0xff);
  char ss[32];
  snprintf(ss, sizeof(ss), "%d/%d", g_wiz.cur + 1, g_wiz.step_count);
  ov_draw_text(px,
               stride,
               mx + mw - BDR - PAD - ov_measure(ss),
               hty,
               cw,
               ch,
               ss,
               0x58,
               0x5b,
               0x70,
               0xff);
  ov_fill_rect(px,
               stride,
               mx + BDR,
               my + BDR + hh,
               mw - BDR * 2,
               1,
               ac.r,
               ac.g,
               ac.b,
               0x40,
               cw,
               ch);
  int      iy = my + BDR + hh + 4, ix = mx + BDR + PAD, iw = mw - BDR * 2 - PAD * 2;
  WizStep *s = &g_wiz.steps[g_wiz.cur];
  ov_draw_text(
      px, stride, ix, ROW_TY(iy), cw, ch, s->prompt, 0xe8, 0xea, 0xed, 0xff);
  iy += ROW_H + 4;
  if(s->kind == WSTEP_SELECT) {
    int vis = (mh - (iy - my) - BDR - 4) / ROW_H, scroll = s->sel - vis + 1;
    if(scroll < 0) scroll = 0;
    for(int i = 0; i < s->opt_count && i < vis; i++) {
      int di = i + scroll;
      if(di >= s->opt_count) break;
      bool sel = (di == s->sel);
      int  ry  = iy + i * ROW_H;
      if(sel) {
        ov_fill_rect(px,
                     stride,
                     mx + BDR,
                     ry,
                     mw - BDR * 2,
                     ROW_H,
                     ac.r,
                     ac.g,
                     ac.b,
                     0x18,
                     cw,
                     ch);
        ov_fill_rect(
            px, stride, mx + BDR, ry, 2, ROW_H, ac.r, ac.g, ac.b, 0xff, cw, ch);
      }
      ov_draw_text(px,
                   stride,
                   ix + 4,
                   ROW_TY(ry),
                   cw,
                   ch,
                   s->opts[di].label,
                   sel ? ac.r : 0xe8,
                   sel ? ac.g : 0xea,
                   sel ? ac.b : 0xed,
                   0xff);
      if(s->opts[di].desc[0]) {
        int lw = ov_measure(s->opts[di].label) + PAD;
        if(ix + 4 + lw + ov_measure(s->opts[di].desc) < mx + mw - BDR - PAD)
          ov_draw_text(px,
                       stride,
                       ix + 4 + lw,
                       ROW_TY(ry),
                       cw,
                       ch,
                       s->opts[di].desc,
                       0x45,
                       0x47,
                       0x5a,
                       0xff);
      }
    }
    int hy = my + mh - BDR - 4 - ROW_H;
    ov_fill_rect(px,
                 stride,
                 mx + BDR,
                 hy - 1,
                 mw - BDR * 2,
                 1,
                 ac.r,
                 ac.g,
                 ac.b,
                 0x20,
                 cw,
                 ch);
    ov_draw_text(px,
                 stride,
                 ix,
                 ROW_TY(hy),
                 cw,
                 ch,
                 "j/k select   Enter confirm   Esc back",
                 0x45,
                 0x47,
                 0x5a,
                 0xff);
  } else {
    mp_input_box(px,
                 stride,
                 ix,
                 iy,
                 iw,
                 s->input_val[0] ? s->input_val : NULL,
                 s->input_placeholder[0] ? s->input_placeholder : "…",
                 true,
                 ac,
                 bg,
                 cw,
                 ch);
    int hy = my + mh - BDR - 4 - ROW_H;
    ov_fill_rect(px,
                 stride,
                 mx + BDR,
                 hy - 1,
                 mw - BDR * 2,
                 1,
                 ac.r,
                 ac.g,
                 ac.b,
                 0x20,
                 cw,
                 ch);
    ov_draw_text(px,
                 stride,
                 ix,
                 ROW_TY(hy),
                 cw,
                 ch,
                 "Enter confirm (blank = default)   Esc back",
                 0x45,
                 0x47,
                 0x5a,
                 0xff);
  }
}

/* ════════════════════════════════════════════════════════════════════════════
 * §L  draw_panel_marvin
 * ════════════════════════════════════════════════════════════════════════════ */

void draw_panel_marvin(uint32_t      *px,
                       int            stride,
                       int            bx,
                       int            by,
                       int            bw,
                       int            bh,
                       TrixieOverlay *o,
                       const Config  *cfg) {
  int *scroll = overlay_scroll_ptr(o);
  marvin_poll(overlay_fb_cwd_ptr(o));
  Color ac = cfg->colors.active_border, bg = cfg->colors.pane_bg;
  int   ix = IX(bx), iy = IY(by), iw = IW(bw);

  /* ── Sub-tab strip ────────────────────────────────────────────────────── */
  static const char *tab_names[MARVIN_TAB_COUNT] = {
    "project", "tasks", "wizard", "console"
  };
  int tx = ix;
  for(int i = 0; i < MARVIN_TAB_COUNT; i++) {
    bool        sel = (i == (int)g_marvin_tab);
    const char *nm  = tab_names[i];
    int         nw  = ov_measure(nm) + PAD;
    if(sel) {
      ov_fill_rect(
          px, stride, tx, iy, nw, ROW_H, ac.r, ac.g, ac.b, 0xff, stride, by + bh);
      ov_draw_text(px,
                   stride,
                   tx + PAD / 2,
                   ROW_TY(iy),
                   stride,
                   by + bh,
                   nm,
                   bg.r,
                   bg.g,
                   bg.b,
                   0xff);
    } else {
      uint8_t tbr  = (uint8_t)(bg.r > 0x10 ? bg.r - 0x10 : 0),
              tbg_ = (uint8_t)(bg.g > 0x10 ? bg.g - 0x10 : 0),
              tbb  = (uint8_t)(bg.b > 0x10 ? bg.b - 0x10 : 0);
      ov_fill_rect(
          px, stride, tx, iy, nw, ROW_H, tbr, tbg_, tbb, 0xff, stride, by + bh);
      ov_draw_text(px,
                   stride,
                   tx + PAD / 2,
                   ROW_TY(iy),
                   stride,
                   by + bh,
                   nm,
                   0x58,
                   0x5b,
                   0x70,
                   0xff);
    }
    tx += nw + 2;
  }
  ov_fill_rect(px,
               stride,
               bx + BDR,
               iy + ROW_H,
               bw - BDR * 2,
               1,
               ac.r,
               ac.g,
               ac.b,
               0x40,
               stride,
               by + bh);
  int y = iy + ROW_H + 4;

  /* ── Project status header ────────────────────────────────────────────── */
  {
    uint8_t sr, sg, sb;
    if(g_marvin_proj.valid) {
      if(!strcmp(g_marvin_proj.last_status, "running")) {
        sr = 0xf9;
        sg = 0xe2;
        sb = 0xaf;
      } else if(!strcmp(g_marvin_proj.last_status, "error")) {
        sr = 0xff;
        sg = 0x6b;
        sb = 0x6b;
      } else {
        sr = 0xa6;
        sg = 0xe3;
        sb = 0xa1;
      }
    } else {
      sr = 0x58;
      sg = 0x5b;
      sb = 0x70;
    }
    char hdr[256];
    if(g_marvin_proj.valid)
      snprintf(hdr,
               sizeof(hdr),
               "%s  [%s/%s]",
               g_marvin_proj.name[0] ? g_marvin_proj.name : "unnamed",
               g_marvin_proj.type,
               g_marvin_proj.lang);
    else
      snprintf(hdr, sizeof(hdr), "no project detected");
    int pip_w = 6;
    ov_fill_rect(
        px, stride, ix, y + 4, pip_w, ROW_H - 8, sr, sg, sb, 0xff, stride, by + bh);
    ov_draw_text(px,
                 stride,
                 ix + pip_w + 6,
                 ROW_TY(y),
                 stride,
                 by + bh,
                 hdr,
                 g_marvin_proj.valid ? 0xe8 : 0x58,
                 g_marvin_proj.valid ? 0xea : 0x5b,
                 g_marvin_proj.valid ? 0xed : 0x70,
                 0xff);
    bool        nv = nvim_is_connected();
    const char *sl = nv ? "nvim connected" : "nvim offline";
    uint8_t skr = nv ? 0xa6 : 0x58, skg = nv ? 0xe3 : 0x5b, skb = nv ? 0xa1 : 0x70;
    int     skx = bx + bw - BDR - PAD - ov_measure(sl);
    if(skx > ix + ov_measure(hdr) + pip_w + 16)
      ov_draw_text(
          px, stride, skx, ROW_TY(y), stride, by + bh, sl, skr, skg, skb, 0xcc);
    y += ROW_H + 2;
  }

  /* ── Last action status ───────────────────────────────────────────────── */
  if(g_marvin_proj.valid && g_marvin_proj.last_action[0]) {
    char sb[512];
    snprintf(sb,
             sizeof(sb),
             "last: %s  →  %s",
             g_marvin_proj.last_action,
             g_marvin_proj.last_status[0] ? g_marvin_proj.last_status : "?");
    uint8_t lr = 0x58, lg = 0x5b, lb = 0x70;
    if(!strcmp(g_marvin_proj.last_status, "ok")) {
      lr = 0xa6;
      lg = 0xe3;
      lb = 0xa1;
    } else if(!strcmp(g_marvin_proj.last_status, "error")) {
      lr = 0xff;
      lg = 0x6b;
      lb = 0x6b;
    } else if(!strcmp(g_marvin_proj.last_status, "running")) {
      lr = 0xf9;
      lg = 0xe2;
      lb = 0xaf;
    }
    ov_draw_text(px, stride, ix, ROW_TY(y), stride, by + bh, sb, lr, lg, lb, 0xff);
    y += ROW_H + 2;
  }
  tui_hsep(px, stride, bx, y, bw, NULL, ac, bg, stride, by + bh);
  y += ROW_H;
  int content_y = y, visible_rows = (bh - (y - by) - BDR - 4) / ROW_H;
  if(visible_rows < 1) goto draw_wizard_overlay;

  /* ── [0] PROJECT ──────────────────────────────────────────────────────── */
  if(g_marvin_tab == MARVIN_TAB_PROJECT) {
    if(!g_marvin_proj.valid) {
      ov_draw_text(px,
                   stride,
                   ix,
                   ROW_TY(y),
                   stride,
                   by + bh,
                   "No project detected.",
                   0x58,
                   0x5b,
                   0x70,
                   0xff);
      y += ROW_H;
      ov_draw_text(px,
                   stride,
                   ix,
                   ROW_TY(y),
                   stride,
                   by + bh,
                   "Navigate to a project directory,",
                   0x58,
                   0x5b,
                   0x70,
                   0xff);
      y += ROW_H;
      ov_draw_text(px,
                   stride,
                   ix,
                   ROW_TY(y),
                   stride,
                   by + bh,
                   "or use [wizard] tab to scaffold one.",
                   0x45,
                   0x47,
                   0x5a,
                   0xff);
      goto draw_wizard_overlay;
    }
    struct {
      const char *key;
      const char *val;
    } fields[] = {
      { "Name", g_marvin_proj.name     },
      { "Type", g_marvin_proj.type     },
      { "Lang", g_marvin_proj.lang     },
      { "Root", g_marvin_proj.root     },
      { "Cmd",  g_marvin_proj.last_cmd },
      { NULL,   NULL                   }
    };
    int kw = ov_measure("Root") + PAD;
    for(int i = 0; fields[i].key && y + ROW_H <= by + bh - BDR - 4; i++) {
      if(!fields[i].val || !fields[i].val[0]) continue;
      ov_draw_text(px,
                   stride,
                   ix,
                   ROW_TY(y),
                   stride,
                   by + bh,
                   fields[i].key,
                   0x74,
                   0x78,
                   0x92,
                   0xff);
      char vb[256];
      strncpy(vb, fields[i].val, sizeof(vb) - 1);
      while(ov_measure(vb) > iw - kw - 4 && strlen(vb) > 4)
        vb[strlen(vb) - 1] = '\0';
      ov_draw_text(px,
                   stride,
                   ix + kw,
                   ROW_TY(y),
                   stride,
                   by + bh,
                   vb,
                   0xe8,
                   0xea,
                   0xed,
                   0xff);
      y += ROW_H;
    }
    const char *qa   = "b build  r run  R brun  t test  x clean  f fmt  l lint  i "
                       "install  , re-detect";
    int         qa_y = by + bh - BDR - 4 - ROW_H;
    ov_fill_rect(px,
                 stride,
                 bx + BDR,
                 qa_y - 1,
                 bw - BDR * 2,
                 1,
                 ac.r,
                 ac.g,
                 ac.b,
                 0x20,
                 stride,
                 by + bh);
    ov_draw_text(
        px, stride, ix, ROW_TY(qa_y), stride, by + bh, qa, 0x45, 0x48, 0x5a, 0xff);
    goto draw_wizard_overlay;
  }

  /* ── [1] TASKS ────────────────────────────────────────────────────────── */
  if(g_marvin_tab == MARVIN_TAB_TASKS) {
    const MarvinAction *acts  = marvin_actions();
    int                 total = marvin_total_items();

    /* Reserve bottom rows for output preview */
    int preview_lines = 0;
    if(g_marvin_console_len > 0) {
      preview_lines = g_mv_tasks_output_expanded ? 8 : 3;
    }
    int preview_h    = preview_lines > 0 ? (preview_lines * ROW_H + ROW_H + 4) : 0;
    int list_bot     = by + bh - BDR - 4 - preview_h;
    int list_visible = (list_bot - content_y) / ROW_H;
    if(list_visible < 1) list_visible = 1;

    int skip = *scroll;
    if(g_marvin_cursor < skip) skip = g_marvin_cursor;
    if(g_marvin_cursor >= skip + list_visible)
      skip = g_marvin_cursor - list_visible + 1;
    if(skip < 0) skip = 0;
    if(skip > total - list_visible) {
      skip = total - list_visible;
      if(skip < 0) skip = 0;
    }
    *scroll = skip;

    int row = 0;
    for(int i = 0; acts[i].label && row < list_visible; i++) {
      if(i < skip) continue;
      int  ry     = content_y + row * ROW_H;
      bool is_sep = !strcmp(acts[i].label, "---");
      bool sel    = (i == g_marvin_cursor && !is_sep);

      if(is_sep) {
        ov_fill_rect(px,
                     stride,
                     ix,
                     ry + ROW_H / 2,
                     iw,
                     1,
                     ac.r,
                     ac.g,
                     ac.b,
                     0x18,
                     stride,
                     by + bh);
      } else {
        if(sel) {
          ov_fill_rect(px,
                       stride,
                       ix - BDR,
                       ry,
                       iw + BDR * 2,
                       ROW_H,
                       ac.r,
                       ac.g,
                       ac.b,
                       0x18,
                       stride,
                       by + bh);
          ov_fill_rect(px,
                       stride,
                       ix - BDR,
                       ry,
                       2,
                       ROW_H,
                       ac.r,
                       ac.g,
                       ac.b,
                       0xff,
                       stride,
                       by + bh);
        }

        /* Running indicator for the selected action */
        bool is_running = sel && g_marvin_proj.valid &&
                          !strcmp(g_marvin_proj.last_status, "running") &&
                          acts[i].nvim_cmd &&
                          !strcmp(g_marvin_proj.last_action, acts[i].nvim_cmd);
        if(is_running) {
          static const char *spin[] = { "󰪞", "󰪟", "󰪠", "󰪡",
                                        "󰪢", "󰪣", "󰪤", "󰪥" };
          const char        *sp     = spin[(ov_now_ms() / 100) % 8];
          ov_draw_text(px,
                       stride,
                       ix + 4,
                       ROW_TY(ry),
                       stride,
                       by + bh,
                       sp,
                       0xa6,
                       0xe3,
                       0xa1,
                       0xff);
        }
        int label_x = ix + (is_running ? ov_measure("󰪞 ") : 4);
        ov_draw_text(px,
                     stride,
                     label_x,
                     ROW_TY(ry),
                     stride,
                     by + bh,
                     acts[i].label,
                     sel ? ac.r : 0xe8,
                     sel ? ac.g : 0xea,
                     sel ? ac.b : 0xed,
                     0xff);

        /* Last status badge on selected row */
        if(sel && g_marvin_proj.last_action[0] && acts[i].nvim_cmd &&
           !strcmp(g_marvin_proj.last_action, acts[i].nvim_cmd)) {
          const char *ls = g_marvin_proj.last_status;
          uint8_t     sr = 0x58, sg = 0x5b, sb = 0x70;
          if(!strcmp(ls, "ok")) {
            sr = 0xa6;
            sg = 0xe3;
            sb = 0xa1;
          } else if(!strcmp(ls, "error")) {
            sr = 0xf3;
            sg = 0x8b;
            sb = 0xa8;
          } else if(!strcmp(ls, "running")) {
            sr = 0xf9;
            sg = 0xe2;
            sb = 0xaf;
          }
          int sw = ov_measure(ls);
          int sx = bx + bw - BDR - PAD - sw;
          if(sx > label_x + ov_measure(acts[i].label) + PAD)
            ov_draw_text(
                px, stride, sx, ROW_TY(ry), stride, by + bh, ls, sr, sg, sb, 0xff);
        }

        if(acts[i].hint) {
          int  hx           = bx + bw - BDR - PAD - ov_measure(acts[i].hint);
          bool status_shown = sel && g_marvin_proj.last_action[0] &&
                              acts[i].nvim_cmd &&
                              !strcmp(g_marvin_proj.last_action, acts[i].nvim_cmd);
          if(!status_shown && hx > ix + ov_measure(acts[i].label) + 16)
            ov_draw_text(px,
                         stride,
                         hx,
                         ROW_TY(ry),
                         stride,
                         by + bh,
                         acts[i].hint,
                         0x45,
                         0x48,
                         0x5a,
                         0xff);
        }

        if(g_mv_args_editing && i == g_marvin_cursor) {
          int ax = ix + ov_measure(acts[i].label) + PAD * 2;
          int aw = bx + bw - BDR - PAD - ax;
          if(aw > 80)
            mp_input_box(px,
                         stride,
                         ax,
                         ry,
                         aw,
                         g_mv_action_args[i],
                         "extra args…",
                         true,
                         ac,
                         bg,
                         stride,
                         by + bh);
        }
      }
      row++;
    }

    tui_scrollbar(
        px, stride, bx, by, bw, bh, skip, total, list_visible, ac, stride, by + bh);

    /* ── Inline output preview ── */
    if(preview_h > 0) {
      int oy = list_bot;

      /* Separator + header */
      ov_fill_rect(px,
                   stride,
                   bx + BDR,
                   oy,
                   bw - BDR * 2,
                   1,
                   ac.r,
                   ac.g,
                   ac.b,
                   0x30,
                   stride,
                   by + bh);
      oy += 2;

      char        phdr[128];
      const char *toggle_hint =
          g_mv_tasks_output_expanded ? "o=collapse" : "o=expand";
      snprintf(phdr,
               sizeof(phdr),
               "output › %s  %s",
               g_marvin_proj.last_action[0] ? g_marvin_proj.last_action : "last run",
               toggle_hint);
      uint8_t hr = 0x58, hg = 0x5b, hb = 0x70;
      if(!strcmp(g_marvin_proj.last_status, "ok")) {
        hr = 0xa6;
        hg = 0xe3;
        hb = 0xa1;
      } else if(!strcmp(g_marvin_proj.last_status, "error")) {
        hr = 0xf3;
        hg = 0x8b;
        hb = 0xa8;
      } else if(!strcmp(g_marvin_proj.last_status, "running")) {
        hr = 0xf9;
        hg = 0xe2;
        hb = 0xaf;
      }
      ov_draw_text(
          px, stride, ix, ROW_TY(oy), stride, by + bh, phdr, hr, hg, hb, 0xff);
      oy += ROW_H + 2;

      /* Dark background for the output lines */
      ov_fill_rect(px,
                   stride,
                   bx + BDR,
                   oy,
                   bw - BDR * 2,
                   by + bh - oy - BDR,
                   0x10,
                   0x10,
                   0x1c,
                   0x80,
                   stride,
                   by + bh);

      pthread_mutex_lock(&g_marvin_con_lock);

      /* Count total lines */
      int total_lines = 1;
      for(int ci = 0; ci < g_marvin_console_len; ci++)
        if(g_marvin_console[ci] == '\n') total_lines++;

      int start_line = total_lines - preview_lines;
      if(start_line < 0) start_line = 0;

      int  line_idx = 0, prow = 0;
      char lbuf[512];
      int  bi = 0;
      for(int ci = 0; ci <= g_marvin_console_len && prow < preview_lines; ci++) {
        char c = (ci < g_marvin_console_len) ? g_marvin_console[ci] : '\0';
        if(c == '\n' || c == '\0') {
          lbuf[bi] = '\0';
          if(line_idx >= start_line && bi > 0) {
            int     ry2 = oy + prow * ROW_H;
            uint8_t lr = 0xa6, lg_ = 0xad, lb = 0xc8;
            if(strstr(lbuf, "error:") || strstr(lbuf, "ERROR")) {
              lr  = 0xf3;
              lg_ = 0x8b;
              lb  = 0xa8;
            } else if(strstr(lbuf, "warning:")) {
              lr  = 0xf9;
              lg_ = 0xe2;
              lb  = 0xaf;
            } else if(lbuf[0] == '$' || lbuf[0] == '>') {
              lr  = 0x74;
              lg_ = 0x78;
              lb  = 0x92;
            } else if(strstr(lbuf, "Finished") || strstr(lbuf, "ok")) {
              lr  = 0xa6;
              lg_ = 0xe3;
              lb  = 0xa1;
            }
            char trunc[512];
            strncpy(trunc, lbuf, sizeof(trunc) - 1);
            int avail = iw - 8;
            while(ov_measure(trunc) > avail && strlen(trunc) > 3)
              trunc[strlen(trunc) - 1] = '\0';
            ov_draw_text(px,
                         stride,
                         ix + 4,
                         ROW_TY(ry2),
                         stride,
                         by + bh,
                         trunc,
                         lr,
                         lg_,
                         lb,
                         0xff);
            prow++;
          }
          line_idx++;
          bi = 0;
        } else if(bi < 511) {
          lbuf[bi++] = c;
        }
      }
      pthread_mutex_unlock(&g_marvin_con_lock);
    }

    goto draw_wizard_overlay;
  }

  /* ── [2] WIZARD menu ──────────────────────────────────────────────────── */
  if(g_marvin_tab == MARVIN_TAB_WIZARD) {
    static int         wiz_sel     = 0;
    static const char *wiz_items[] = { "New Makefile",
                                       "New meson.build",
                                       "compile_commands.json",
                                       "New project scaffold" };
    static const char *wiz_desc[]  = {
      "Generate a Makefile for your project type",
      "Generate a meson.build with auto-detected deps",
      "Generate compile_commands.json for clangd/LSP",
      "Scaffold a new Cargo/Go project"
    };
    for(int i = 0; i < 4; i++) {
      bool sel = (i == wiz_sel);
      int  ry  = content_y + i * ROW_H;
      if(sel) {
        ov_fill_rect(px,
                     stride,
                     bx + BDR,
                     ry,
                     bw - BDR * 2,
                     ROW_H,
                     ac.r,
                     ac.g,
                     ac.b,
                     0x18,
                     stride,
                     by + bh);
        ov_fill_rect(px,
                     stride,
                     bx + BDR,
                     ry,
                     2,
                     ROW_H,
                     ac.r,
                     ac.g,
                     ac.b,
                     0xff,
                     stride,
                     by + bh);
      }
      ov_draw_text(px,
                   stride,
                   ix + 4,
                   ROW_TY(ry),
                   stride,
                   by + bh,
                   wiz_items[i],
                   sel ? ac.r : 0xe8,
                   sel ? ac.g : 0xea,
                   sel ? ac.b : 0xed,
                   0xff);
      int lw = ov_measure(wiz_items[i]) + PAD * 2;
      if(ix + 4 + lw + ov_measure(wiz_desc[i]) < bx + bw - BDR - PAD)
        ov_draw_text(px,
                     stride,
                     ix + 4 + lw,
                     ROW_TY(ry),
                     stride,
                     by + bh,
                     wiz_desc[i],
                     0x45,
                     0x47,
                     0x5a,
                     0xff);
    }
    int hy = by + bh - BDR - 4 - ROW_H;
    ov_fill_rect(px,
                 stride,
                 bx + BDR,
                 hy - 1,
                 bw - BDR * 2,
                 1,
                 ac.r,
                 ac.g,
                 ac.b,
                 0x20,
                 stride,
                 by + bh);
    ov_draw_text(px,
                 stride,
                 ix,
                 ROW_TY(hy),
                 stride,
                 by + bh,
                 "j/k navigate   Enter launch wizard",
                 0x45,
                 0x47,
                 0x5a,
                 0xff);
    goto draw_wizard_overlay;
  }

  /* ── [3] CONSOLE ──────────────────────────────────────────────────────── */
  if(g_marvin_tab == MARVIN_TAB_CONSOLE) {
    if(g_marvin_console_len == 0) {
      ov_draw_text(px,
                   stride,
                   ix,
                   ROW_TY(y),
                   stride,
                   by + bh,
                   "No console output yet.",
                   0x58,
                   0x5b,
                   0x70,
                   0xff);
      ov_draw_text(px,
                   stride,
                   ix,
                   ROW_TY(y + ROW_H),
                   stride,
                   by + bh,
                   "b=build  r=run  R=brun  t=test  x=clean  f=fmt  l=lint",
                   0x45,
                   0x48,
                   0x5a,
                   0xff);
      goto draw_wizard_overlay;
    }
    int total_lines = 1;
    for(int ci = 0; ci < g_marvin_console_len; ci++)
      if(g_marvin_console[ci] == '\n') total_lines++;
    int skip = *scroll, max_skip = total_lines - visible_rows;
    if(max_skip < 0) max_skip = 0;
    if(skip > max_skip) skip = max_skip;
    if(skip < 0) skip = 0;
    *scroll       = skip;
    int  line_idx = 0, row = 0;
    char lbuf[512];
    int  bi = 0;
    for(int ci = 0; ci <= g_marvin_console_len; ci++) {
      char c = (ci < g_marvin_console_len) ? g_marvin_console[ci] : '\0';
      if(c == '\n' || c == '\0') {
        lbuf[bi] = '\0';
        if(line_idx >= skip && row < visible_rows) {
          int     ry = content_y + row * ROW_H;
          uint8_t lr = 0xe8, lg = 0xea, lb = 0xed;
          if(strstr(lbuf, "ERROR") || strstr(lbuf, "error:")) {
            lr = 0xff;
            lg = 0x6b;
            lb = 0x6b;
          } else if(strstr(lbuf, "WARN") || strstr(lbuf, "warning:")) {
            lr = 0xf9;
            lg = 0xe2;
            lb = 0xaf;
          } else if(strstr(lbuf, "BUILD SUCCESS") || strstr(lbuf, "Finished")) {
            lr = 0xa6;
            lg = 0xe3;
            lb = 0xa1;
          } else if(lbuf[0] == '$' || lbuf[0] == '>') {
            lr = 0x74;
            lg = 0x78;
            lb = 0x92;
          }
          ov_draw_text(
              px, stride, ix, ROW_TY(ry), stride, by + bh, lbuf, lr, lg, lb, 0xff);
          row++;
        }
        line_idx++;
        bi = 0;
      } else if(bi < 511) {
        lbuf[bi++] = c;
      }
    }
    tui_scrollbar(px,
                  stride,
                  bx,
                  by,
                  bw,
                  bh,
                  skip,
                  total_lines,
                  visible_rows,
                  ac,
                  stride,
                  by + bh);
  }

draw_wizard_overlay:
  if(g_wiz.active) draw_wizard_modal(px, stride, bx, by, bw, bh, cfg);
}
