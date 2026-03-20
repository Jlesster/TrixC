/* trixiectl — command-line client for the Trixie compositor IPC socket.
 *
 * Usage
 * ─────
 *   trixiectl <command> [args...]
 *   trixiectl subscribe          # stream JSON events until Ctrl-C
 *
 * Examples
 *   trixiectl workspace 3
 *   trixiectl overlay toggle
 *   trixiectl overlay cd ~/projects/myapp
 *   trixiectl overlay notify build "cargo build finished"
 *   trixiectl overlay git-refresh
 *   trixiectl overlay panel git
 *   trixiectl status_json
 *   trixiectl spawn kitty
 *   trixiectl clients
 *   trixiectl clients --plain
 *
 * Shell integration (add to ~/.zshrc or ~/.bashrc)
 * ─────────────────────────────────────────────────
 *   # Sync overlay cwd on every cd
 *   function cd() { builtin cd "$@" && trixiectl overlay cd "$PWD" 2>/dev/null;
 * }
 *
 *   # zsh precmd hook (runs before every prompt)
 *   precmd() { trixiectl overlay cd "$PWD" 2>/dev/null; }
 *
 *   # Optional: alias trixie-run to pipe output into overlay log
 *   function trixie-run() {
 *     trixiectl overlay notify run "started: $*" 2>/dev/null
 *     "$@"
 *     trixiectl overlay notify run "exit $?: $*" 2>/dev/null
 *   }
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* ── Socket path (mirrors ipc.c logic) ───────────────────────────────────── */

static const char *socket_path(void) {
  static char buf[256];
  if (buf[0])
    return buf;
  const char *xdg = getenv("XDG_RUNTIME_DIR");
  if (xdg)
    snprintf(buf, sizeof(buf), "%s/trixie.sock", xdg);
  else
    snprintf(buf, sizeof(buf), "/tmp/trixie-%d.sock", (int)getuid());
  return buf;
}

/* ── JSON unescape ───────────────────────────────────────────────────────── */
/* Handles the common escape sequences that appear in window titles/app IDs.
 * Not a full JSON parser — just enough to avoid garbled output from titles
 * containing backslash-escaped characters (e.g. \"foo\", newlines, etc.). */
static void json_unescape(char *s) {
  char *r = s, *w = s;
  while (*r) {
    if (r[0] == '\\') {
      switch (r[1]) {
      case '"':
        *w++ = '"';
        r += 2;
        continue;
      case '\\':
        *w++ = '\\';
        r += 2;
        continue;
      case 'n':
        *w++ = '\n';
        r += 2;
        continue;
      case 'r':
        *w++ = '\r';
        r += 2;
        continue;
      case 't':
        *w++ = '\t';
        r += 2;
        continue;
      default:
        break;
      }
    }
    *w++ = *r++;
  }
  *w = '\0';
}

/* ── Connect ─────────────────────────────────────────────────────────────── */

static int connect_sock(void) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("trixiectl: socket");
    return -1;
  }
  struct sockaddr_un addr = {0};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path(), sizeof(addr.sun_path) - 1);
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    fprintf(stderr,
            "trixiectl: cannot connect to %s: %s\n"
            "  Is Trixie running?\n",
            socket_path(), strerror(errno));
    close(fd);
    return -1;
  }
  return fd;
}

/* ── Write all ───────────────────────────────────────────────────────────── */

static int write_all(int fd, const char *buf, size_t len) {
  while (len > 0) {
    ssize_t n = write(fd, buf, len);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    buf += n;
    len -= (size_t)n;
  }
  return 0;
}

/* ── Send command, print reply ───────────────────────────────────────────── */

static int send_command(const char *cmd) {
  int fd = connect_sock();
  if (fd < 0)
    return 1;

  char buf[2048];
  snprintf(buf, sizeof(buf), "%s\n", cmd);
  if (write_all(fd, buf, strlen(buf)) < 0) {
    perror("trixiectl: write");
    close(fd);
    return 1;
  }

  char reply[8192] = {0};
  ssize_t total = 0;
  for (;;) {
    ssize_t n = read(fd, reply + total, sizeof(reply) - 1 - total);
    if (n <= 0)
      break;
    total += n;
    if (total >= (ssize_t)(sizeof(reply) - 1))
      break;
  }
  close(fd);

  if (total > 0) {
    reply[total] = '\0';
    while (total > 0 && (reply[total - 1] == '\n' || reply[total - 1] == '\r'))
      reply[--total] = '\0';
    puts(reply);
    return strncmp(reply, "err:", 4) == 0 ? 1 : 0;
  }
  return 0;
}

/* ── Subscribe mode: stream JSON events ─────────────────────────────────── */

static int subscribe_mode(void) {
  int fd = connect_sock();
  if (fd < 0)
    return 1;

  const char *cmd = "subscribe\n";
  if (write_all(fd, cmd, strlen(cmd)) < 0) {
    perror("trixiectl: write");
    close(fd);
    return 1;
  }

  char buf[4096];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0)
      break;
    buf[n] = '\0';
    fputs(buf, stdout);
    fflush(stdout);
  }
  close(fd);
  return 0;
}

/* ── Help ────────────────────────────────────────────────────────────────── */

static void usage(void) {
  puts("trixiectl — Trixie compositor IPC client\n"
       "\n"
       "Usage: trixiectl <command> [args...]\n"
       "\n"
       "Compositor control\n"
       "  workspace <n>              switch to workspace n\n"
       "  next_workspace             next workspace\n"
       "  prev_workspace             previous workspace\n"
       "  move_to_workspace <n>      move focused window to workspace n\n"
       "  focus <left|right|up|down> directional focus\n"
       "  layout [next|prev]         cycle layout\n"
       "  set_layout <name>          bsp|spiral|columns|rows|threecol|monocle\n"
       "  grow_main / shrink_main    adjust main ratio\n"
       "  inc_master / dec_master    add/remove a pane from the master column\n"
       "  ratio <0.1..0.9>           set exact main ratio\n"
       "  float                      toggle float on focused window\n"
       "  float_move <dx> <dy>       move floating window\n"
       "  float_resize <dw> <dh>     resize floating window\n"
       "  swap [forward|back]        cycle pane order\n"
       "  swap_main                  swap focused with master slot\n"
       "  scratchpad <name>          toggle named scratchpad\n"
       "  close                      close focused window\n"
       "  fullscreen                 toggle fullscreen\n"
       "  spawn <cmd>                execute command\n"
       "  dpms <on|off>              display power\n"
       "  idle_reset                 reset idle timer\n"
       "  reload                     hot-reload config only\n"
       "  binary_reload              rebuild binary + exec-replace (ninja)\n"
       "  quit                       exit compositor\n"
       "\n"
       "Introspection\n"
       "  clients                    list all open windows (JSON)\n"
       "  clients --json             same as above (explicit)\n"
       "  clients --plain            human-readable table\n"
       "  status                     human-readable workspace + scratchpad "
       "summary\n"
       "  status_json                machine-readable JSON (workspaces + "
       "scratchpads)\n"
       "  query_pane [id]            pane details (default: focused)\n"
       "  subscribe                  stream JSON events until Ctrl-C\n"
       "\n"
       "Overlay\n"
       "  overlay toggle             show/hide overlay\n"
       "  overlay show / hide        explicit show or hide\n"
       "  overlay panel <name>       switch to panel "
       "(run|build|git|lsp|nvim|...)\n"
       "  overlay cd <path>          set project cwd\n"
       "  overlay notify <title> <msg>  push message to overlay log\n"
       "  overlay run <cmd>          run command, show log panel\n"
       "  overlay git-refresh        force git panel to re-query\n"
       "\n"
       "Build\n"
       "  build                      trigger build on build panel\n"
       "\n"
       "Scratchpad pattern syntax (in trixie.conf)\n"
       "  app_id = kitty             exact app_id match (default)\n"
       "  app_id = title:~ncmpcpp    title contains 'ncmpcpp'\n"
       "  app_id = class:fire*       app_id glob\n"
       "  app_id = title:*Player*    title glob\n"
       "  title  = ~Music            shorthand for title:~Music\n"
       "\n"
       "Shell integration (add to ~/.zshrc)\n"
       "  precmd() { trixiectl overlay cd \"$PWD\" 2>/dev/null; }\n"
       "  PROMPT_COMMAND='trixiectl overlay cd \"$PWD\" 2>/dev/null'  # bash\n"
       "\n"
       "Examples\n"
       "  trixiectl clients                     # JSON list of all windows\n"
       "  trixiectl clients --plain             # human-readable table\n"
       "  trixiectl status_json | jq .scratchpads\n");
}

/* ── clients subcommand — fetch status_json, reformat ───────────────────── */

static int cmd_clients(int argc, char **argv) {
  bool plain =
      (argc >= 3 && (!strcmp(argv[2], "--plain") || !strcmp(argv[2], "-p")));

  int fd = connect_sock();
  if (fd < 0)
    return 1;
  const char *req = "status_json\n";
  if (write_all(fd, req, strlen(req)) < 0) {
    perror("trixiectl: write");
    close(fd);
    return 1;
  }
  char raw[65536] = {0};
  ssize_t total = 0;
  for (;;) {
    ssize_t n = read(fd, raw + total, sizeof(raw) - 1 - total);
    if (n <= 0)
      break;
    total += n;
    if (total >= (ssize_t)(sizeof(raw) - 1))
      break;
  }
  close(fd);
  if (total <= 0) {
    fputs("err: no response\n", stderr);
    return 1;
  }
  raw[total] = '\0';

  if (!plain) {
    /* JSON output — flatten pane_list from every workspace into one array */
    printf("[");
    bool first_out = true;
    char *ws_arr = strstr(raw, "\"workspaces\":[");
    if (ws_arr)
      ws_arr += strlen("\"workspaces\":[");
    while (ws_arr && *ws_arr && *ws_arr != ']') {
      char *pl = strstr(ws_arr, "\"pane_list\":[");
      char *idx_p = strstr(ws_arr, "\"index\":");
      int ws_idx = idx_p ? atoi(idx_p + 8) : -1;
      char *next_ws = strchr(ws_arr, '}');
      if (next_ws)
        ws_arr = next_ws + 1;
      else
        break;
      if (!pl || pl > ws_arr)
        continue;
      pl += strlen("\"pane_list\":[");
      while (*pl && *pl != ']') {
        if (*pl != '{') {
          pl++;
          continue;
        }
        char *end = strchr(pl, '}');
        if (!end)
          break;
        char *f = strstr(pl, "\"id\":");
        int id = f ? atoi(f + 5) : -1;
        char title[256] = "";
        f = strstr(pl, "\"title\":\"");
        if (f) {
          f += 9;
          int i = 0;
          while (*f && *f != '"' && i < 255)
            title[i++] = *f++;
        }
        char app_id[128] = "";
        f = strstr(pl, "\"app_id\":\"");
        if (f) {
          f += 10;
          int i = 0;
          while (*f && *f != '"' && i < 127)
            app_id[i++] = *f++;
        }
        json_unescape(title);
        json_unescape(app_id);
        bool focused = strstr(pl, "\"focused\":true") &&
                       strstr(pl, "\"focused\":true") < end;
        bool floating = strstr(pl, "\"floating\":true") &&
                        strstr(pl, "\"floating\":true") < end;
        bool fullscreen = strstr(pl, "\"fullscreen\":true") &&
                          strstr(pl, "\"fullscreen\":true") < end;
        printf("%s{\"id\":%d,\"app_id\":\"%s\",\"title\":\"%s\","
               "\"workspace\":%d,\"floating\":%s,\"fullscreen\":%s,\"focused\":"
               "%s}",
               first_out ? "" : ",", id, app_id, title, ws_idx,
               floating ? "true" : "false", fullscreen ? "true" : "false",
               focused ? "true" : "false");
        first_out = false;
        pl = end + 1;
      }
    }
    printf("]\n");
  } else {
    /* Plain table output */
    printf("%-6s %-3s %-20s %-8s %-8s %s\n", "ID", "WS", "APP_ID", "FLOAT",
           "FOCUSED", "TITLE");
    printf("%-6s %-3s %-20s %-8s %-8s %s\n", "------", "---",
           "--------------------", "--------", "--------", "-----");
    char *ws_arr = strstr(raw, "\"workspaces\":[");
    if (ws_arr)
      ws_arr += strlen("\"workspaces\":[");
    while (ws_arr && *ws_arr && *ws_arr != ']') {
      char *pl = strstr(ws_arr, "\"pane_list\":[");
      char *idx_p = strstr(ws_arr, "\"index\":");
      int ws_idx = idx_p ? atoi(idx_p + 8) : -1;
      char *next_ws = strchr(ws_arr, '}');
      if (next_ws)
        ws_arr = next_ws + 1;
      else
        break;
      if (!pl || pl > ws_arr)
        continue;
      pl += strlen("\"pane_list\":[");
      while (*pl && *pl != ']') {
        if (*pl != '{') {
          pl++;
          continue;
        }
        char *end = strchr(pl, '}');
        if (!end)
          break;
        char *f = strstr(pl, "\"id\":");
        int id = f ? atoi(f + 5) : -1;
        char title[256] = "";
        f = strstr(pl, "\"title\":\"");
        if (f) {
          f += 9;
          int i = 0;
          while (*f && *f != '"' && i < 255)
            title[i++] = *f++;
        }
        char app_id[128] = "";
        f = strstr(pl, "\"app_id\":\"");
        if (f) {
          f += 10;
          int i = 0;
          while (*f && *f != '"' && i < 127)
            app_id[i++] = *f++;
        }
        json_unescape(title);
        json_unescape(app_id);
        bool focused = strstr(pl, "\"focused\":true") &&
                       strstr(pl, "\"focused\":true") < end;
        bool floating = strstr(pl, "\"floating\":true") &&
                        strstr(pl, "\"floating\":true") < end;
        printf("%-6d %-3d %-20s %-8s %-8s %s\n", id, ws_idx,
               app_id[0] ? app_id : "(none)", floating ? "yes" : "no",
               focused ? "yes" : "no", title);
        pl = end + 1;
      }
    }
  }
  return 0;
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
  if (argc < 2) {
    usage();
    return 1;
  }

  if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
    usage();
    return 0;
  }

  if (!strcmp(argv[1], "subscribe")) {
    return subscribe_mode();
  }

  /* "reload" → config-only reload (fast).
   * "binary_reload" → ninja rebuild + exec-replace (passed through as-is). */
  if (!strcmp(argv[1], "reload")) {
    return send_command("reload");
  }

  if (!strcmp(argv[1], "clients")) {
    return cmd_clients(argc, argv);
  }

  /* Reconstruct the command string from argv */
  char cmd[2048] = {0};
  for (int i = 1; i < argc; i++) {
    if (i > 1)
      strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
    strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
  }

  return send_command(cmd);
}
