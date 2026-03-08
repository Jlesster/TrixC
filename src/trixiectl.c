/* trixie-ctl — command-line client for the Trixie compositor IPC socket.
 *
 * Usage
 * ─────
 *   trixie-ctl <command> [args...]
 *   trixie-ctl subscribe          # stream JSON events until Ctrl-C
 *
 * Examples
 *   trixie-ctl workspace 3
 *   trixie-ctl overlay toggle
 *   trixie-ctl overlay cd ~/projects/myapp
 *   trixie-ctl overlay notify build "cargo build finished"
 *   trixie-ctl overlay git-refresh
 *   trixie-ctl overlay panel git
 *   trixie-ctl status_json
 *   trixie-ctl spawn kitty
 *
 * Shell integration (add to ~/.zshrc or ~/.bashrc)
 * ─────────────────────────────────────────────────
 *   # Sync overlay cwd on every cd
 *   function cd() { builtin cd "$@" && trixie-ctl overlay cd "$PWD" 2>/dev/null; }
 *
 *   # zsh precmd hook (runs before every prompt)
 *   precmd() { trixie-ctl overlay cd "$PWD" 2>/dev/null; }
 *
 *   # Optional: alias trixie-run to pipe output into overlay log
 *   function trixie-run() {
 *     trixie-ctl overlay notify run "started: $*" 2>/dev/null
 *     "$@"
 *     trixie-ctl overlay notify run "exit $?: $*" 2>/dev/null
 *   }
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* ── Socket path (mirrors ipc.c logic) ───────────────────────────────────── */

static const char *socket_path(void) {
  static char buf[256];
  if(buf[0]) return buf;
  const char *xdg = getenv("XDG_RUNTIME_DIR");
  if(xdg)
    snprintf(buf, sizeof(buf), "%s/trixie.sock", xdg);
  else
    snprintf(buf, sizeof(buf), "/tmp/trixie-%d.sock", (int)getuid());
  return buf;
}

/* ── Connect ─────────────────────────────────────────────────────────────── */

static int connect_sock(void) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if(fd < 0) {
    perror("trixie-ctl: socket");
    return -1;
  }
  struct sockaddr_un addr = { 0 };
  addr.sun_family         = AF_UNIX;
  strncpy(addr.sun_path, socket_path(), sizeof(addr.sun_path) - 1);
  if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    fprintf(stderr,
            "trixie-ctl: cannot connect to %s: %s\n"
            "  Is Trixie running?\n",
            socket_path(),
            strerror(errno));
    close(fd);
    return -1;
  }
  return fd;
}

/* ── Write all ───────────────────────────────────────────────────────────── */

static int write_all(int fd, const char *buf, size_t len) {
  while(len > 0) {
    ssize_t n = write(fd, buf, len);
    if(n < 0) {
      if(errno == EINTR) continue;
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
  if(fd < 0) return 1;

  /* Send command + newline */
  char buf[2048];
  snprintf(buf, sizeof(buf), "%s\n", cmd);
  if(write_all(fd, buf, strlen(buf)) < 0) {
    perror("trixie-ctl: write");
    close(fd);
    return 1;
  }

  /* Read reply */
  char    reply[8192] = { 0 };
  ssize_t total       = 0;
  for(;;) {
    ssize_t n = read(fd, reply + total, sizeof(reply) - 1 - total);
    if(n <= 0) break;
    total += n;
    if(total >= (ssize_t)(sizeof(reply) - 1)) break;
  }
  close(fd);

  if(total > 0) {
    reply[total] = '\0';
    /* Trim trailing newline */
    while(total > 0 && (reply[total - 1] == '\n' || reply[total - 1] == '\r'))
      reply[--total] = '\0';
    puts(reply);
    /* Exit code: 0 for ok, 1 for err */
    return strncmp(reply, "err:", 4) == 0 ? 1 : 0;
  }
  return 0;
}

/* ── Subscribe mode: stream JSON events ─────────────────────────────────── */

static int subscribe_mode(void) {
  int fd = connect_sock();
  if(fd < 0) return 1;

  const char *cmd = "subscribe\n";
  if(write_all(fd, cmd, strlen(cmd)) < 0) {
    perror("trixie-ctl: write");
    close(fd);
    return 1;
  }

  /* Stream events line by line until EOF or signal */
  char buf[4096];
  for(;;) {
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if(n <= 0) break;
    buf[n] = '\0';
    fputs(buf, stdout);
    fflush(stdout);
  }
  close(fd);
  return 0;
}

/* ── Help ────────────────────────────────────────────────────────────────── */

static void usage(void) {
  puts("trixie-ctl — Trixie compositor IPC client\n"
       "\n"
       "Usage: trixie-ctl <command> [args...]\n"
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
       "  reload                     hot-reload config\n"
       "  quit                       exit compositor\n"
       "\n"
       "Introspection\n"
       "  status                     human-readable workspace summary\n"
       "  status_json                machine-readable JSON status\n"
       "  query_pane [id]            pane details (default: focused)\n"
       "  subscribe                  stream JSON events until Ctrl-C\n"
       "\n"
       "Overlay\n"
       "  overlay toggle             show/hide overlay\n"
       "  overlay show / hide        explicit show or hide\n"
       "  overlay panel <name>       switch to panel (run|build|git|lsp|nvim|...)\n"
       "  overlay cd <path>          set project cwd (git, run detection, files)\n"
       "  overlay notify <title> <msg>  push message to overlay log\n"
       "  overlay run <cmd>          run command, show log panel\n"
       "  overlay git-refresh        force git panel to re-query\n"
       "\n"
       "Build\n"
       "  build                      trigger build on build panel\n"
       "\n"
       "Shell integration (add to ~/.zshrc)\n"
       "  # Sync overlay cwd on every directory change\n"
       "  precmd() { trixie-ctl overlay cd \"$PWD\" 2>/dev/null; }\n"
       "  # bash: PROMPT_COMMAND\n"
       "  PROMPT_COMMAND='trixie-ctl overlay cd \"$PWD\" 2>/dev/null'\n");
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
  if(argc < 2) {
    usage();
    return 1;
  }

  if(!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
    usage();
    return 0;
  }

  if(!strcmp(argv[1], "subscribe")) {
    return subscribe_mode();
  }

  /* Reconstruct the command string from argv */
  char cmd[2048] = { 0 };
  for(int i = 1; i < argc; i++) {
    if(i > 1) strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
    strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
  }

  return send_command(cmd);
}
