/* trixiectl.c — Command-line IPC client for the trixie compositor.
 *
 * Mirrors the Rust embedded_ipc::send_command / EmbedCommand pattern:
 * build a command string, connect to the compositor's Unix socket, write
 * the command, read the reply, and exit with 0 on "ok:" or 1 on "err:".
 *
 * Usage
 * ─────
 *   trixiectl <command> [args...]
 *
 * Commands
 * ────────
 *   workspace <n>              switch to workspace n (1-based)
 *   next_workspace             switch to next workspace
 *   prev_workspace             switch to previous workspace
 *   move_to_workspace <n>      move focused window to workspace n
 *   focus <left|right|up|down> directional focus
 *   swap [forward|back]        cycle pane order
 *   swap_main                  swap focused pane with master slot
 *   layout [next|prev]         cycle tiling layout
 *   grow_main                  widen main pane
 *   shrink_main                narrow main pane
 *   ratio <0.1..0.9>           set main pane ratio exactly
 *   float                      toggle float on focused pane
 *   float_move <dx> <dy>       move floating window by (dx, dy) pixels
 *   float_resize <dw> <dh>     resize floating window by (dw, dh) pixels
 *   scratchpad <name>          toggle named scratchpad
 *   close                      close focused window
 *   fullscreen                 toggle fullscreen on focused window
 *   spawn <cmd>                run a command
 *   reload                     hot-reload config file
 *   quit                       terminate the compositor
 *   status                     human-readable status
 *   status_json                machine-readable JSON status
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* ── Socket path ────────────────────────────────────────────────────────────── */

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

/* ── Reliable write — retries on EINTR/partial writes ──────────────────────── */

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

/* ── Usage ──────────────────────────────────────────────────────────────────── */

static void usage(void) {
  fprintf(stderr,
          "Usage: trixiectl <command> [args...]\n"
          "\n"
          "Window management:\n"
          "  close                      close focused window\n"
          "  fullscreen                 toggle fullscreen\n"
          "  float                      toggle float\n"
          "  float_move <dx> <dy>       move floating window\n"
          "  float_resize <dw> <dh>     resize floating window\n"
          "\n"
          "Focus & layout:\n"
          "  focus <left|right|up|down> directional focus\n"
          "  swap [forward|back]        cycle pane order\n"
          "  swap_main                  swap focused pane with master\n"
          "  layout [next|prev]         cycle tiling layout\n"
          "  grow_main / shrink_main    adjust main pane ratio\n"
          "  ratio <0.1..0.9>           set main pane ratio\n"
          "\n"
          "Workspaces:\n"
          "  workspace <n>              switch to workspace n\n"
          "  next_workspace             next workspace\n"
          "  prev_workspace             previous workspace\n"
          "  move_to_workspace <n>      move focused window to workspace n\n"
          "\n"
          "Scratchpads:\n"
          "  scratchpad <name>          toggle named scratchpad\n"
          "\n"
          "Compositor:\n"
          "  spawn <cmd>                run a command\n"
          "  reload                     hot-reload config\n"
          "  quit                       terminate compositor\n"
          "\n"
          "Overlay dev suite:\n"
          "  overlay [toggle|show|hide] show/hide the dev overlay\n"
          "  build                      trigger async build (opens overlay)\n"
          "\n"
          "Status:\n"
          "  status                     human-readable status\n"
          "  status_json                machine-readable JSON\n");
}

/* ── Entry point ────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
  if(argc < 2) {
    usage();
    return 1;
  }

  /* Build command string from argv. */
  char cmd[1024] = { 0 };
  for(int i = 1; i < argc; i++) {
    if(i > 1) strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
    strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
  }
  strncat(cmd, "\n", sizeof(cmd) - strlen(cmd) - 1);

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if(fd < 0) {
    perror("trixiectl: socket");
    return 1;
  }

  struct sockaddr_un addr = { 0 };
  addr.sun_family         = AF_UNIX;
  strncpy(addr.sun_path, socket_path(), sizeof(addr.sun_path) - 1);

  if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    fprintf(stderr,
            "trixiectl: could not connect to %s\n"
            "(is trixie running?)\n",
            socket_path());
    close(fd);
    return 1;
  }

  /* BUG FIX: retry write until all bytes are delivered. */
  if(write_all(fd, cmd, strlen(cmd)) < 0) {
    perror("trixiectl: write");
    close(fd);
    return 1;
  }

  char    reply[4096] = { 0 };
  ssize_t n           = read(fd, reply, sizeof(reply) - 1);
  close(fd);

  if(n > 0) {
    fputs(reply, stdout);
    if(reply[n - 1] != '\n') fputc('\n', stdout);
  }

  return (n > 0 && strncmp(reply, "ok", 2) == 0) ? 0 : 1;
}
