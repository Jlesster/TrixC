/* nvim_panel.c — Neovim msgpack-RPC integration for the Trixie overlay.
 *
 * Provides two things:
 *
 *   1. A live connection to a running nvim instance via its --listen Unix
 *      socket.  Trixie can ask nvim to open files at specific lines, get the
 *      currently open buffer, and receive LSP diagnostic counts.
 *
 *   2. draw_panel_nvim() — an overlay panel ([N] tab) that shows:
 *        • current buffer / cursor position reported by nvim
 *        • LSP diagnostic summary (errors / warnings / hints per file)
 *        • a quick-jump list: select an LSP error, press Enter → nvim jumps
 *        • 's' spawns nvim in a terminal if no socket is found
 *        • 'r' refreshes diagnostics + buffer info
 *
 * Two modes of operation:
 *   POLL mode  — overlay polls nvim via msgpack-RPC each lsp_poll_ms
 *   PUSH mode  — nvim plugin pushes state via the IPC socket (preferred)
 *                overlay_nvim_state/diag/buffers() are called by ipc.c
 *                and override the polled data.
 *
 * Keys (panel focus)
 * ──────────────────
 *   j / k        navigate diagnostic list
 *   Enter        jump to diagnostic location in nvim
 *   r            refresh diagnostics + buffer info
 *   s            spawn nvim in configured terminal (if not connected)
 *   q            disconnect
 */

#define _POSIX_C_SOURCE 200809L
#include "nvim_panel.h"
#include "overlay_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  msgpack helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MP_FIXARRAY(n) ((uint8_t)(0x90 | ((n) & 0x0f)))
#define MP_FIXMAP(n)   ((uint8_t)(0x80 | ((n) & 0x0f)))
#define MP_FIXSTR(n)   ((uint8_t)(0xa0 | ((n) & 0x1f)))
#define MP_FIXINT(n)   ((uint8_t)((n) & 0x7f))
#define MP_NIL         0xc0
#define MP_FALSE       0xc2
#define MP_TRUE        0xc3
#define MP_UINT8       0xcc
#define MP_INT32       0xd2
#define MP_STR8        0xd9

typedef struct {
  uint8_t *data;
  size_t   len, cap;
} MpBuf;

static void mpbuf_init(MpBuf *b) {
  b->cap  = 256;
  b->len  = 0;
  b->data = malloc(b->cap);
}
static void mpbuf_free(MpBuf *b) {
  free(b->data);
  b->data = NULL;
}
static void mpbuf_push(MpBuf *b, uint8_t byte) {
  if(b->len >= b->cap) {
    b->cap *= 2;
    b->data = realloc(b->data, b->cap);
  }
  b->data[b->len++] = byte;
}
static void mpbuf_write(MpBuf *b, const void *src, size_t n) {
  for(size_t i = 0; i < n; i++)
    mpbuf_push(b, ((const uint8_t *)src)[i]);
}
static void mp_encode_nil(MpBuf *b) {
  mpbuf_push(b, MP_NIL);
}
static void mp_encode_int(MpBuf *b, int32_t v) {
  if(v >= 0 && v <= 127) {
    mpbuf_push(b, MP_FIXINT(v));
    return;
  }
  uint8_t be[4] = {
    (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v
  };
  mpbuf_push(b, MP_INT32);
  mpbuf_write(b, be, 4);
}

#define MP_STR16 0xda
#define MP_STR32 0xdb

static void mp_encode_str(MpBuf *b, const char *s) {
  size_t len = strlen(s);
  if(len <= 31) {
    mpbuf_push(b, MP_FIXSTR(len));
  } else if(len <= 255) {
    mpbuf_push(b, MP_STR8);
    mpbuf_push(b, (uint8_t)len);
  } else if(len <= 65535) {
    mpbuf_push(b, MP_STR16);
    mpbuf_push(b, (uint8_t)(len >> 8));
    mpbuf_push(b, (uint8_t)(len & 0xff));
  } else {
    mpbuf_push(b, MP_STR32);
    mpbuf_push(b, (uint8_t)(len >> 24));
    mpbuf_push(b, (uint8_t)(len >> 16));
    mpbuf_push(b, (uint8_t)(len >> 8));
    mpbuf_push(b, (uint8_t)(len & 0xff));
  }
  mpbuf_write(b, s, len);
}

static void mp_encode_array_hdr(MpBuf *b, int n) {
  mpbuf_push(b, MP_FIXARRAY(n));
}

#define PARSE_MAX_TOKENS 32
typedef struct {
  int  kind;
  int  ival;
  char sval[512];
  bool bval;
} MpToken;

static int mp_parse_flat(const uint8_t *data, size_t len, MpToken *out, int maxout) {
  size_t pos = 0;
  int    n   = 0;
  while(pos < len && n < maxout) {
    uint8_t b = data[pos++];
    if(b == MP_NIL) {
      out[n++].kind = 2;
    } else if(b == MP_TRUE) {
      out[n].kind   = 3;
      out[n++].bval = true;
    } else if(b == MP_FALSE) {
      out[n].kind   = 3;
      out[n++].bval = false;
    } else if((b & 0x80) == 0) {
      out[n].kind   = 0;
      out[n++].ival = b;
    } else if((b & 0xe0) == 0xe0) {
      out[n].kind   = 0;
      out[n++].ival = (int8_t)b;
    } else if((b & 0xe0) == 0xa0) {
      size_t slen = b & 0x1f;
      if(pos + slen > len) break;
      out[n].kind = 1;
      size_t cp   = slen < 511 ? slen : 511;
      memcpy(out[n].sval, data + pos, cp);
      out[n].sval[cp] = '\0';
      pos += slen;
      n++;
    } else if(b == MP_STR8) {
      if(pos >= len) break;
      size_t slen = data[pos++];
      if(pos + slen > len) break;
      out[n].kind = 1;
      size_t cp   = slen < 511 ? slen : 511;
      memcpy(out[n].sval, data + pos, cp);
      out[n].sval[cp] = '\0';
      pos += slen;
      n++;
    } else if(b == MP_INT32) {
      if(pos + 4 > len) break;
      int32_t v = ((int32_t)data[pos] << 24) | ((int32_t)data[pos + 1] << 16) |
                  ((int32_t)data[pos + 2] << 8) | data[pos + 3];
      pos += 4;
      out[n].kind   = 0;
      out[n++].ival = v;
    } else if(b == MP_UINT8) {
      if(pos >= len) break;
      out[n].kind   = 0;
      out[n++].ival = data[pos++];
    }
    /* fixarray / fixmap — skip header, tokens follow inline */
  }
  return n;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  LSP diagnostic types
 * ═══════════════════════════════════════════════════════════════════════════ */

#define NV_DIAG_MAX      256
#define NV_DIAG_MSG_MAX  256
#define NV_DIAG_FILE_MAX 256

typedef enum {
  NV_DIAG_ERROR = 1,
  NV_DIAG_WARN  = 2,
  NV_DIAG_INFO  = 3,
  NV_DIAG_HINT  = 4
} NvDiagSev;

typedef struct {
  char      file[NV_DIAG_FILE_MAX];
  int       line, col;
  NvDiagSev sev;
  char      msg[NV_DIAG_MSG_MAX];
  char      source[64];
} NvDiag;

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Global nvim connection state
 * ═══════════════════════════════════════════════════════════════════════════ */

#define NV_BUF_PATH_MAX 512
#define NV_RECV_MAX     (64 * 1024)

/* push_source tracks where diagnostics came from so the two modes don't fight */
typedef enum { PUSH_SRC_POLL = 0, PUSH_SRC_BRIDGE } DiagSource;

typedef struct {
  int             sock_fd;
  bool            connected;
  char            socket_path[256];
  pthread_t       reader;
  bool            reader_valid;
  atomic_bool     stop_reader;
  pthread_mutex_t lock;
  char            buf_path[NV_BUF_PATH_MAX];
  int             cursor_line, cursor_col;
  /* diagnostics */
  NvDiag          diags[NV_DIAG_MAX];
  int             diag_count;
  int             errors, warnings;
  DiagSource      diag_src; /* which source last wrote diags */
  /* bridge-pushed state (from nvim plugin via IPC) */
  char            bridge_file[NV_BUF_PATH_MAX];
  int             bridge_line, bridge_col;
  char            bridge_ft[64];
  bool            bridge_connected; /* nvim plugin is running */
  uint32_t        next_msgid;
  int64_t         next_poll_ms;
  int             poll_interval_ms;
  atomic_bool     connecting;
} NvState;

/* g_nv.lock protects all fields except where noted.
 * Specifically it guards: connected, bridge_connected, sock_fd,
 * buf_path, cursor_line/col, bridge_*, diags, errors, warnings,
 * diag_count, diag_src, next_msgid, next_poll_ms, poll_interval_ms.
 *
 * The reader thread receives a dup'd fd at thread start and never
 * touches g_nv.sock_fd after that, eliminating the use-after-close
 * race between nvim_disconnect() and an in-progress read/poll.
 *
 * stop_reader is atomic so the reader can check it without the lock.
 */
static NvState g_nv = {
  .sock_fd = -1,
  .lock    = PTHREAD_MUTEX_INITIALIZER,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Socket helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static int nv_connect_fd(const char *path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if(fd < 0) return -1;
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
  if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  /* Keep the fd non-blocking for nv_send(); the reader thread uses poll()
   * so it does not need a blocking fd.                                    */
  int flags = fcntl(fd, F_GETFL);
  if(flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  return fd;
}

static void nv_send(const MpBuf *b) {
  pthread_mutex_lock(&g_nv.lock);
  int fd = g_nv.sock_fd;
  pthread_mutex_unlock(&g_nv.lock);
  if(fd < 0) return;

  char msg[64];
  snprintf(msg, sizeof(msg), "==> nvim: sending %zu bytes", b->len);
  log_ring_push(msg); /* add this */

  size_t sent = 0;
  while(sent < b->len) {
    ssize_t n = write(fd, b->data + sent, b->len - sent);
    if(n > 0) {
      sent += (size_t)n;
      continue;
    }
    break;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  Diagnostic parsing
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *NV_DIAG_LUA =
    "local t = {} "
    "for _, d in ipairs(vim.diagnostic.get(nil)) do "
    "  local f = '' "
    "  pcall(function() f = vim.api.nvim_buf_get_name(d.bufnr) end) "
    "  table.insert(t, string.format('%d|%s|%d|%d|%s|%s', "
    "    d.severity, f, (d.lnum or 0)+1, (d.col or 0)+1, "
    "    (d.source or ''), "
    "    (d.message or ''):gsub('[\\n\\r]', ' '))) "
    "end "
    "return table.concat(t, '\\n')";

static void nv_parse_diag_blob(const char *blob) {
  /* Called with g_nv.lock already held by nv_handle_response(). */
  static char copy[NV_RECV_MAX]; /* static: lives in BSS, not on the stack */
  strncpy(copy, blob, sizeof(copy) - 1);
  copy[sizeof(copy) - 1] = '\0';

  g_nv.diag_count = 0;
  g_nv.errors     = 0;
  g_nv.warnings   = 0;
  g_nv.diag_src   = PUSH_SRC_POLL;

  char *ln = strtok(copy, "\n");
  while(ln && g_nv.diag_count < NV_DIAG_MAX) {
    char *parts[6];
    int   np    = 0;
    char *p     = ln;
    parts[np++] = p;
    while(*p && np < 6) {
      if(*p == '|') {
        *p          = '\0';
        parts[np++] = p + 1;
      }
      p++;
    }
    if(np >= 6) {
      NvDiag *d = &g_nv.diags[g_nv.diag_count++];
      d->sev    = (NvDiagSev)atoi(parts[0]);
      strncpy(d->file, parts[1], NV_DIAG_FILE_MAX - 1);
      d->line = atoi(parts[2]);
      d->col  = atoi(parts[3]);
      strncpy(d->source, parts[4], 63);
      strncpy(d->msg, parts[5], NV_DIAG_MSG_MAX - 1);
      if(d->sev == NV_DIAG_ERROR)
        g_nv.errors++;
      else if(d->sev == NV_DIAG_WARN)
        g_nv.warnings++;
    }
    ln = strtok(NULL, "\n");
  }
  /* Caller (nv_handle_response) must NOT re-lock; we are already inside. */
}

/* Parse the JSON array pushed by the nvim plugin (nvim_diag IPC command).
 * Format: [{"file":"...","line":N,"col":N,"msg":"...","severity":"error"},...]
 * We do a minimal hand-parse — no external JSON lib needed for this shape.  */
static void nv_parse_diag_json(const char *json) {
  pthread_mutex_lock(&g_nv.lock);
  g_nv.diag_count = 0;
  g_nv.errors     = 0;
  g_nv.warnings   = 0;
  g_nv.diag_src   = PUSH_SRC_BRIDGE;

  const char *p = json;
  while(*p && g_nv.diag_count < NV_DIAG_MAX) {
    while(*p && *p != '{')
      p++;
    if(!*p) break;
    p++;

    NvDiag d = { 0 };
    while(*p && *p != '}') {
      while(*p == ' ' || *p == ',')
        p++;
      if(*p == '}') break;
      if(*p != '"') {
        p++;
        continue;
      }
      p++;
      char key[32] = { 0 };
      int  ki      = 0;
      while(*p && *p != '"' && ki < 31)
        key[ki++] = *p++;
      if(*p == '"') p++;
      while(*p == ' ' || *p == ':')
        p++;
      if(*p == '"') {
        p++;
        char val[NV_DIAG_FILE_MAX] = { 0 };
        int  vi                    = 0;
        while(*p && *p != '"' && vi < NV_DIAG_FILE_MAX - 1) {
          if(*p == '\\') p++;
          val[vi++] = *p++;
        }
        if(*p == '"') p++;
        if(!strcmp(key, "file"))
          strncpy(d.file, val, NV_DIAG_FILE_MAX - 1);
        else if(!strcmp(key, "msg"))
          strncpy(d.msg, val, NV_DIAG_MSG_MAX - 1);
        else if(!strcmp(key, "severity")) {
          if(!strcmp(val, "error"))
            d.sev = NV_DIAG_ERROR;
          else if(!strcmp(val, "warn"))
            d.sev = NV_DIAG_WARN;
          else if(!strcmp(val, "info"))
            d.sev = NV_DIAG_INFO;
          else
            d.sev = NV_DIAG_HINT;
        }
      } else {
        int v = atoi(p);
        while(*p && *p != ',' && *p != '}')
          p++;
        if(!strcmp(key, "line"))
          d.line = v;
        else if(!strcmp(key, "col"))
          d.col = v;
      }
    }
    if(*p == '}') p++;

    if(d.file[0] || d.msg[0]) {
      if(d.sev == 0) d.sev = NV_DIAG_ERROR;
      g_nv.diags[g_nv.diag_count++] = d;
      if(d.sev == NV_DIAG_ERROR)
        g_nv.errors++;
      else if(d.sev == NV_DIAG_WARN)
        g_nv.warnings++;
    }
  }
  pthread_mutex_unlock(&g_nv.lock);
}

static void nv_handle_response(const uint8_t *data, size_t len) {
  MpToken toks[PARSE_MAX_TOKENS];
  int     ntok = mp_parse_flat(data, len, toks, PARSE_MAX_TOKENS);

  /* ── diagnostic blob (pipe-separated lines)? ── */
  for(int i = 0; i < ntok; i++) {
    if(toks[i].kind == 1 && strchr(toks[i].sval, '|')) {
      pthread_mutex_lock(&g_nv.lock);
      bool bridge = (g_nv.diag_src == PUSH_SRC_BRIDGE);
      if(!bridge) nv_parse_diag_blob(toks[i].sval); /* called with lock held */
      pthread_mutex_unlock(&g_nv.lock);
      return;
    }
  }

  /* ── file path ── */
  for(int i = 0; i < ntok; i++) {
    if(toks[i].kind == 1 && toks[i].sval[0] == '/') {
      pthread_mutex_lock(&g_nv.lock);
      if(!g_nv.bridge_connected)
        strncpy(g_nv.buf_path, toks[i].sval, NV_BUF_PATH_MAX - 1);
      pthread_mutex_unlock(&g_nv.lock);
      return;
    }
  }

  /* ── cursor position ── */
  int ic = 0, ints[4];
  for(int i = 0; i < ntok && ic < 4; i++)
    if(toks[i].kind == 0) ints[ic++] = toks[i].ival;
  if(ic >= 2) {
    pthread_mutex_lock(&g_nv.lock);
    if(!g_nv.bridge_connected) {
      if(ints[0] > 0) g_nv.cursor_line = ints[0];
      if(ints[1] >= 0) g_nv.cursor_col = ints[1] + 1;
    }
    pthread_mutex_unlock(&g_nv.lock);
  }
}

/* ── Reader thread ───────────────────────────────────────────────────────
 * Receives a dup'd copy of sock_fd at thread creation time and owns that
 * fd for its entire lifetime.  It never reads g_nv.sock_fd, so there is
 * no race with nvim_disconnect() closing and zeroing that field.
 *
 * The dup'd fd is passed as the thread arg and closed before exit.
 * O_NONBLOCK on the dup'd fd is independent of the original because we
 * set it with F_SETFL after dup(); POSIX says O_NONBLOCK is per open-file-
 * description, and dup() shares the description, BUT we explicitly set the
 * flag on the new fd only — this is fine because Linux implements the flag
 * per file-description so BOTH fds will become blocking.  To avoid that we
 * use poll() instead of relying on blocking reads, keeping the original fd
 * non-blocking and not touching its flags at all from the reader thread.
 * ──────────────────────────────────────────────────────────────────────── */
typedef struct {
  int fd;
} ReaderArg;

static void *nv_reader_fn(void *arg) {
  ReaderArg *ra  = (ReaderArg *)arg;
  int        rfd = ra->fd;
  free(ra);

  uint8_t buf[NV_RECV_MAX];

  while(!atomic_load(&g_nv.stop_reader)) {
    struct pollfd pfd = { .fd = rfd, .events = POLLIN };
    int           r   = poll(&pfd, 1, 200);

    if(r < 0) {
      if(errno == EINTR) continue;
      break;
    }
    if(r == 0) continue; /* timeout — recheck stop_reader */

    if(pfd.revents & (POLLHUP | POLLERR)) {
      log_ring_push("==> nvim: reader got POLLHUP/POLLERR — socket closed by nvim");
      break;
    }

    if(pfd.revents & (POLLHUP | POLLERR)) break;

    if(pfd.revents & POLLIN) {
      ssize_t n = read(rfd, buf, sizeof(buf) - 1);
      if(n <= 0) {
        if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        break;
      }
      buf[n] = '\0';
      nv_handle_response(buf, (size_t)n);
    }
  }

  log_ring_push("==> nvim: reader thread exiting");
  bool unexpected = !atomic_load(&g_nv.stop_reader);
  if(unexpected) {
    pthread_mutex_lock(&g_nv.lock);
    g_nv.connected = false;
    pthread_mutex_unlock(&g_nv.lock);
    nvim_retry_reset();
    log_ring_push("==> nvim: connection dropped unexpectedly");
  } else {
    log_ring_push("==> nvim: reader thread stopped cleanly");
  }
  close(rfd);
  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  Public connection API
 * ═══════════════════════════════════════════════════════════════════════════ */

bool nvim_connect(const char *socket_path) {
  /* Prevent concurrent connect attempts */
  bool expected = false;
  if(!atomic_compare_exchange_strong(&g_nv.connecting, &expected, true))
    return false;

  pthread_mutex_lock(&g_nv.lock);
  bool already = g_nv.connected;
  pthread_mutex_unlock(&g_nv.lock);
  if(already) {
    atomic_store(&g_nv.connecting, false);
    return true;
  }

  if(!socket_path || !socket_path[0]) {
    atomic_store(&g_nv.connecting, false);
    return false;
  }
  struct stat st;
  if(stat(socket_path, &st) != 0) {
    atomic_store(&g_nv.connecting, false);
    return false;
  }
  int fd = nv_connect_fd(socket_path);
  if(fd < 0) {
    atomic_store(&g_nv.connecting, false);
    return false;
  }

  /* dup() a private fd for the reader thread before we store sock_fd,
   * so the reader never needs to touch g_nv.sock_fd.                  */
  int rfd = dup(fd);
  if(rfd < 0) {
    close(fd);
    atomic_store(&g_nv.connecting, false);
    return false;
  }
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  fcntl(rfd, F_SETFD, FD_CLOEXEC);

  pthread_mutex_lock(&g_nv.lock);
  g_nv.sock_fd   = fd;
  g_nv.connected = true;
  strncpy(g_nv.socket_path, socket_path, sizeof(g_nv.socket_path) - 1);
  pthread_mutex_unlock(&g_nv.lock);

  atomic_store(&g_nv.stop_reader, false);

  ReaderArg *ra = malloc(sizeof(*ra));
  ra->fd        = rfd;
  if(pthread_create(&g_nv.reader, NULL, nv_reader_fn, ra) == 0) {
    pthread_detach(g_nv.reader);
    g_nv.reader_valid = true;
  } else {
    free(ra);
    close(rfd);
  }

  atomic_store(&g_nv.connecting, false);
  log_ring_push("==> nvim: connected to socket");
  g_nv.next_poll_ms = ov_now_ms() + 5000;

  MpBuf b;
  mpbuf_init(&b);
  mp_encode_array_hdr(&b, 4);
  mp_encode_int(&b, 0);
  mp_encode_int(&b, (int32_t)g_nv.next_msgid++);
  mp_encode_str(&b, "nvim_get_api_info");
  mp_encode_array_hdr(&b, 0);
  nv_send(&b);
  mpbuf_free(&b);

  return true;
}

void nvim_disconnect(void) {
  pthread_mutex_lock(&g_nv.lock);
  bool was_connected    = g_nv.connected;
  int  fd               = g_nv.sock_fd;
  g_nv.sock_fd          = -1; /* reader thread uses its own dup'd fd, not this */
  g_nv.connected        = false;
  g_nv.bridge_connected = false;
  pthread_mutex_unlock(&g_nv.lock);

  if(!was_connected) return;

  /* Signal reader thread to stop, then close the main fd.
   * The reader owns its own dup'd fd and will close it on exit. */
  atomic_store(&g_nv.stop_reader, true);
  if(fd >= 0) {
    shutdown(fd, SHUT_RDWR);
    close(fd);
  }
}

void nvim_open_file(const char *path, int line) {
  if(!g_nv.connected) return;
  char cmd[NV_BUF_PATH_MAX + 32];
  if(line > 0)
    snprintf(cmd, sizeof(cmd), "edit +%d %s", line, path);
  else
    snprintf(cmd, sizeof(cmd), "edit %s", path);
  MpBuf b;
  mpbuf_init(&b);
  mp_encode_array_hdr(&b, 4);
  mp_encode_int(&b, 0);
  mp_encode_int(&b, (int32_t)g_nv.next_msgid++);
  mp_encode_str(&b, "nvim_command");
  mp_encode_array_hdr(&b, 1);
  mp_encode_str(&b, cmd);
  nv_send(&b);
  mpbuf_free(&b);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6b  Bridge receivers — called by ipc.c when the nvim plugin pushes state
 * ═══════════════════════════════════════════════════════════════════════════ */

void overlay_nvim_state(
    TrixieOverlay *o, const char *file, int line, int col, const char *filetype) {
  (void)o;
  pthread_mutex_lock(&g_nv.lock);
  strncpy(g_nv.bridge_file, file, sizeof(g_nv.bridge_file) - 1);
  strncpy(g_nv.bridge_ft, filetype, sizeof(g_nv.bridge_ft) - 1);
  g_nv.bridge_line      = line;
  g_nv.bridge_col       = col;
  g_nv.bridge_connected = (file && file[0] != '\0');
  if(g_nv.bridge_connected) {
    strncpy(g_nv.buf_path, file, NV_BUF_PATH_MAX - 1);
    g_nv.cursor_line = line;
    g_nv.cursor_col  = col;
  }
  pthread_mutex_unlock(&g_nv.lock);
}

void overlay_nvim_diag(TrixieOverlay *o, const char *json) {
  (void)o;
  if(!json || !json[0] || !strcmp(json, "[]")) return;
  nv_parse_diag_json(json);
}

void overlay_nvim_buffers(TrixieOverlay *o, const char *json) {
  (void)o;
  (void)json;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6c  overlay_quickfix_json
 * ═══════════════════════════════════════════════════════════════════════════ */

extern int  overlay_build_err_count(void);
extern void overlay_build_err_get(int   idx,
                                  char *file,
                                  int   fsz,
                                  int  *line,
                                  int  *col,
                                  char *msg,
                                  int   msz,
                                  bool *is_warning);

void overlay_quickfix_json(char *out, size_t sz) {
  int count = overlay_build_err_count();
  int len   = 0;
  len += snprintf(out + len, sz - len, "[");
  for(int i = 0; i < count && len < (int)sz - 256; i++) {
    char file[256] = { 0 }, msg[512] = { 0 };
    int  line = 0, col = 0;
    bool warn = false;
    overlay_build_err_get(
        i, file, sizeof(file), &line, &col, msg, sizeof(msg), &warn);
    char emsg[512] = { 0 };
    int  ei        = 0;
    for(int mi = 0; msg[mi] && ei < 510; mi++) {
      if(msg[mi] == '"' || msg[mi] == '\\') emsg[ei++] = '\\';
      emsg[ei++] = msg[mi];
    }
    len += snprintf(out + len,
                    sz - len,
                    "%s{\"filename\":\"%s\",\"lnum\":%d,\"col\":%d,"
                    "\"text\":\"%s\",\"type\":\"%s\"}",
                    i > 0 ? "," : "",
                    file,
                    line,
                    col,
                    emsg,
                    warn ? "W" : "E");
  }
  len += snprintf(out + len, sz - len, "]");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  RPC poll helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void nv_request_buf(void) {
  MpBuf b;
  mpbuf_init(&b);
  mp_encode_array_hdr(&b, 4);
  mp_encode_int(&b, 0);
  mp_encode_int(&b, (int32_t)g_nv.next_msgid++);
  mp_encode_str(&b, "nvim_buf_get_name");
  mp_encode_array_hdr(&b, 1);
  mp_encode_int(&b, 0);
  nv_send(&b);
  mpbuf_free(&b);
}

static void nv_request_cursor(void) {
  MpBuf b;
  mpbuf_init(&b);
  mp_encode_array_hdr(&b, 4);
  mp_encode_int(&b, 0);
  mp_encode_int(&b, (int32_t)g_nv.next_msgid++);
  mp_encode_str(&b, "nvim_win_get_cursor");
  mp_encode_array_hdr(&b, 1);
  mp_encode_int(&b, 0);
  nv_send(&b);
  mpbuf_free(&b);
}

static void nv_request_diags(void) {
  MpBuf b;
  mpbuf_init(&b);
  mp_encode_array_hdr(&b, 4);
  mp_encode_int(&b, 0);
  mp_encode_int(&b, (int32_t)g_nv.next_msgid++);
  mp_encode_str(&b, "nvim_exec_lua");
  mp_encode_array_hdr(&b, 2);
  mp_encode_str(&b, NV_DIAG_LUA);
  mp_encode_array_hdr(&b, 0);
  nv_send(&b);
  mpbuf_free(&b);
}

void nvim_panel_poll(const OverlayCfg *ov_cfg) {
  if(!ov_cfg) return;
  if(!g_nv.connected) return;

  pthread_mutex_lock(&g_nv.lock);
  bool bridge = g_nv.bridge_connected;
  pthread_mutex_unlock(&g_nv.lock);
  if(bridge) return;

  int64_t now = ov_now_ms();
  if(now < g_nv.next_poll_ms) return;
  g_nv.next_poll_ms = now + (ov_cfg->lsp_poll_ms > 0 ? ov_cfg->lsp_poll_ms : 2000);

  log_ring_push("==> nvim: sending poll requests"); /* add this */
  nv_request_buf();
  nv_request_cursor();
  if(ov_cfg->lsp_diagnostics) nv_request_diags();
}

/* ── nv_spawn ────────────────────────────────────────────────────────────
 * Launch nvim in a terminal using fork()+exec() so the compositor event
 * loop is never blocked.  setsid() detaches the child from the compositor's
 * process group so compositor signals don't propagate to nvim.
 * ──────────────────────────────────────────────────────────────────────── */
static void nv_spawn(const OverlayCfg *ov_cfg, const char *file) {
  const char *term   = ov_cfg->terminal[0] ? ov_cfg->terminal : "kitty";
  const char *editor = ov_cfg->editor[0] ? ov_cfg->editor : "nvim";
  const char *listen = ov_cfg->nvim_listen_addr[0] ? ov_cfg->nvim_listen_addr
                                                   : "/tmp/trixie-nvim.sock";

  char cmd[1024];
  if(file && file[0])
    snprintf(
        cmd, sizeof(cmd), "%s -e %s --listen %s %s", term, editor, listen, file);
  else
    snprintf(cmd, sizeof(cmd), "%s -e %s --listen %s", term, editor, listen);

  pid_t pid = fork();
  if(pid == 0) {
    /* Child: detach from compositor's session and exec the shell command */
    setsid();
    /* Close all fds except stdin/stdout/stderr so compositor resources
     * aren't leaked into the child process.                              */
    int maxfd = (int)sysconf(_SC_OPEN_MAX);
    for(int fd = 3; fd < maxfd; fd++)
      close(fd);
    execl("/bin/sh", "sh", "-c", cmd, NULL);
    _exit(1);
  }
  /* Parent returns immediately — child is fully detached.
   * If fork failed we log it but don't crash.             */
  if(pid < 0)
    log_ring_push("==> nvim: fork() failed, could not spawn");
  else
    log_ring_push("==> nvim: spawned — waiting for socket...");

  strncpy(g_nv.socket_path, listen, sizeof(g_nv.socket_path) - 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  Draw
 * ═══════════════════════════════════════════════════════════════════════════ */

static void diag_color(NvDiagSev sev, uint8_t *r, uint8_t *g, uint8_t *b) {
  switch(sev) {
    case NV_DIAG_ERROR:
      *r = 0xf3;
      *g = 0x8b;
      *b = 0xa8;
      break;
    case NV_DIAG_WARN:
      *r = 0xf9;
      *g = 0xe2;
      *b = 0xaf;
      break;
    case NV_DIAG_INFO:
      *r = 0x89;
      *g = 0xdc;
      *b = 0xeb;
      break;
    case NV_DIAG_HINT:
      *r = 0xa6;
      *g = 0xe3;
      *b = 0xa1;
      break;
    default:
      *r = 0x6c;
      *g = 0x70;
      *b = 0x86;
      break;
  }
}
static const char *diag_icon(NvDiagSev sev) {
  switch(sev) {
    case NV_DIAG_ERROR: return " ";
    case NV_DIAG_WARN: return " ";
    case NV_DIAG_INFO: return " ";
    case NV_DIAG_HINT: return " ";
    default: return " ";
  }
}

void draw_panel_nvim(uint32_t         *px,
                     int               stride,
                     int               px0,
                     int               py0,
                     int               pw,
                     int               ph,
                     int              *cursor,
                     const Config     *cfg,
                     const OverlayCfg *ov_cfg) {
  Color ac  = cfg->colors.active_border;
  Color bg  = cfg->colors.pane_bg;
  int   y   = py0 + HEADER_H + PAD;
  int   bot = py0 + ph - PAD;

  /* ── Status bar ── */
  {
    char status[512];
    pthread_mutex_lock(&g_nv.lock);
    bool        bridge = g_nv.bridge_connected;
    bool        conn   = g_nv.connected || bridge;
    const char *path   = bridge ? g_nv.bridge_file : g_nv.buf_path;
    int         ln     = bridge ? g_nv.bridge_line : g_nv.cursor_line;
    int         col    = bridge ? g_nv.bridge_col : g_nv.cursor_col;
    char        ft[64];
    strncpy(ft, g_nv.bridge_ft, sizeof(ft) - 1);
    pthread_mutex_unlock(&g_nv.lock);

    const char *base = strrchr(path, '/');
    base             = base ? base + 1 : path;

    if(!conn) {
      snprintf(status,
               sizeof(status),
               "not connected — socket: %s",
               g_nv.socket_path[0] ? g_nv.socket_path : "(none configured)");
    } else {
      snprintf(status,
               sizeof(status),
               "%s %s  %s  ln %d  col %d",
               bridge ? "[bridge]" : "[poll]",
               conn ? "connected" : "disconnected",
               base[0] ? base : "(no buffer)",
               ln,
               col);
      if(ft[0]) {
        size_t sl = strlen(status);
        snprintf(status + sl, sizeof(status) - sl, "  %s", ft);
      }
    }
    uint8_t sr = conn ? ac.r : 0x6c;
    uint8_t sg = conn ? ac.g : 0x70;
    uint8_t sb = conn ? ac.b : 0x86;
    ov_draw_text(
        px, stride, px0 + PAD, y + g_ov_asc, stride, bot, status, sr, sg, sb, 0xff);
    y += ROW_H;
  }

  /* ── Hint ── */
  {
    bool        conn = g_nv.connected || g_nv.bridge_connected;
    const char *hint = conn ? "Enter=jump  r=refresh  q=disconnect"
                            : "s=spawn nvim  r=retry connect";
    ov_draw_text(px,
                 stride,
                 px0 + PAD,
                 y + g_ov_asc,
                 stride,
                 bot,
                 hint,
                 0x45,
                 0x47,
                 0x5a,
                 0xff);
    y += ROW_H;
  }

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

  /* ── Diagnostic summary ── */
  bool conn = g_nv.connected || g_nv.bridge_connected;
  if(conn) {
    pthread_mutex_lock(&g_nv.lock);
    int errors = g_nv.errors, warnings = g_nv.warnings, total = g_nv.diag_count;
    pthread_mutex_unlock(&g_nv.lock);

    char ec[32], wc[32], tc[32];
    snprintf(ec, sizeof(ec), " %d errors", errors);
    snprintf(wc, sizeof(wc), "  %d warnings", warnings);
    snprintf(tc, sizeof(tc), "  %d total", total);
    ov_draw_text(px,
                 stride,
                 px0 + PAD,
                 y + g_ov_asc,
                 stride,
                 bot,
                 ec,
                 0xf3,
                 0x8b,
                 0xa8,
                 errors > 0 ? 0xff : 0x50);
    int xoff = px0 + PAD + ov_measure(ec);
    ov_draw_text(px,
                 stride,
                 xoff,
                 y + g_ov_asc,
                 stride,
                 bot,
                 wc,
                 0xf9,
                 0xe2,
                 0xaf,
                 warnings > 0 ? 0xff : 0x50);
    xoff += ov_measure(wc);
    ov_draw_text(
        px, stride, xoff, y + g_ov_asc, stride, bot, tc, 0x6c, 0x70, 0x86, 0xff);
    y += ROW_H + SECTION_GAP / 2;

    int visible = (bot - y) / ROW_H;
    if(visible < 1) goto done_diags;

    if(*cursor < 0) *cursor = 0;
    pthread_mutex_lock(&g_nv.lock);
    int count = g_nv.diag_count;
    if(*cursor >= count && count > 0) *cursor = count - 1;
    int scroll = *cursor - visible + 1;
    if(scroll < 0) scroll = 0;

    for(int i = 0; i < visible; i++) {
      int idx = i + scroll;
      if(idx >= count) break;
      NvDiag *d   = &g_nv.diags[idx];
      bool    sel = (idx == *cursor);
      int     ry  = y + i * ROW_H;
      if(sel) draw_cursor_line(px, stride, px0, ry, pw, ac, bg, stride, bot);

      uint8_t dr, dg, db;
      diag_color(d->sev, &dr, &dg, &db);
      ov_draw_text(px,
                   stride,
                   px0 + PAD,
                   ry + g_ov_asc + 2,
                   stride,
                   bot,
                   diag_icon(d->sev),
                   dr,
                   dg,
                   db,
                   0xff);

      char        loc[NV_DIAG_FILE_MAX + 16];
      const char *base = strrchr(d->file, '/');
      base             = base ? base + 1 : d->file;
      snprintf(loc, sizeof(loc), "%s:%d", base, d->line);
      ov_draw_text(px,
                   stride,
                   px0 + PAD + 24,
                   ry + g_ov_asc + 2,
                   stride,
                   bot,
                   loc,
                   sel ? ac.r : 0x89,
                   sel ? ac.g : 0xdc,
                   sel ? ac.b : 0xeb,
                   0xff);

      int  msg_x = px0 + PAD + 24 + ov_measure(loc) + COL_GAP;
      char trunc[NV_DIAG_MSG_MAX];
      strncpy(trunc, d->msg, sizeof(trunc) - 1);
      int avail = pw - PAD * 2 - (msg_x - px0);
      while(ov_measure(trunc) > avail && strlen(trunc) > 3)
        trunc[strlen(trunc) - 1] = '\0';
      ov_draw_text(px,
                   stride,
                   msg_x,
                   ry + g_ov_asc + 2,
                   stride,
                   bot,
                   trunc,
                   dr,
                   dg,
                   db,
                   sel ? 0xff : 0xc0);
    }
    pthread_mutex_unlock(&g_nv.lock);
  }

done_diags:
  (void)ov_cfg;
  (void)bg;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §9  Key handler
 * ═══════════════════════════════════════════════════════════════════════════ */

bool nvim_panel_key(int *cursor, xkb_keysym_t sym, const OverlayCfg *ov_cfg) {
  if(sym == XKB_KEY_j || sym == XKB_KEY_Down) {
    (*cursor)++;
    return true;
  }
  if(sym == XKB_KEY_k || sym == XKB_KEY_Up) {
    if(*cursor > 0) (*cursor)--;
    return true;
  }

  if(sym == XKB_KEY_Return) {
    pthread_mutex_lock(&g_nv.lock);
    int count = g_nv.diag_count;
    if(*cursor >= 0 && *cursor < count) {
      NvDiag diag = g_nv.diags[*cursor];
      pthread_mutex_unlock(&g_nv.lock);
      if(g_nv.connected) nvim_open_file(diag.file, diag.line);
    } else {
      pthread_mutex_unlock(&g_nv.lock);
    }
    return true;
  }
  if(sym == XKB_KEY_r) {
    if(!g_nv.connected && ov_cfg->nvim_socket[0])
      nvim_connect(ov_cfg->nvim_socket);
    else if(g_nv.connected) {
      nv_request_buf();
      nv_request_cursor();
      if(ov_cfg->lsp_diagnostics) nv_request_diags();
    }
    g_nv.next_poll_ms = 0;
    return true;
  }
  if(sym == XKB_KEY_s) {
    if(!g_nv.connected) nv_spawn(ov_cfg, NULL);
    return true;
  }
  if(sym == XKB_KEY_q) {
    nvim_disconnect();
    return true;
  }
  return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §10  Public accessors
 * ═══════════════════════════════════════════════════════════════════════════ */

bool nvim_is_connected(void) {
  return g_nv.connected || g_nv.bridge_connected;
}
int nvim_error_count(void) {
  return g_nv.errors;
}
int nvim_warn_count(void) {
  return g_nv.warnings;
}
int nvim_diag_count(void) {
  return g_nv.diag_count;
}

void nvim_get_diag_snapshot(LspDiagSnapshot *out, int max, int *count) {
  pthread_mutex_lock(&g_nv.lock);
  int n = g_nv.diag_count < max ? g_nv.diag_count : max;
  for(int i = 0; i < n; i++) {
    NvDiag *d   = &g_nv.diags[i];
    out[i].sev  = (int)d->sev;
    out[i].line = d->line;
    out[i].col  = d->col;
    strncpy(out[i].file, d->file, sizeof(out[i].file) - 1);
    strncpy(out[i].source, d->source, sizeof(out[i].source) - 1);
    strncpy(out[i].msg, d->msg, sizeof(out[i].msg) - 1);
  }
  *count = n;
  pthread_mutex_unlock(&g_nv.lock);
}

void nvim_diag_get(int   idx,
                   int  *sev,
                   char *file,
                   int   fmax,
                   int  *line,
                   int  *col,
                   char *source,
                   int   smax,
                   char *msg,
                   int   mmax) {
  if(idx < 0 || idx >= g_nv.diag_count) {
    if(sev) *sev = 0;
    if(file) file[0] = '\0';
    if(line) *line = 0;
    if(col) *col = 0;
    if(source) source[0] = '\0';
    if(msg) msg[0] = '\0';
    return;
  }
  const NvDiag *d = &g_nv.diags[idx];
  if(sev) *sev = (int)d->sev;
  if(file) {
    strncpy(file, d->file, fmax - 1);
    file[fmax - 1] = '\0';
  }
  if(line) *line = d->line;
  if(col) *col = d->col;
  if(source) {
    strncpy(source, d->source, smax - 1);
    source[smax - 1] = '\0';
  }
  if(msg) {
    strncpy(msg, d->msg, mmax - 1);
    msg[mmax - 1] = '\0';
  }
}
