/* screenshot.c — screenshot via grim subprocess
 *
 * wlroots 0.18 has no compositor-side pixel readback API.
 * The wlr_screencopy_v1 protocol is for CLIENT apps only.
 * The correct approach is to spawn grim, which uses the screencopy
 * client protocol that our compositor already advertises.
 *
 * Dependencies: grim (full/window), slurp (region picker)
 *   pacman -S grim slurp   /   apt install grim slurp
 */
#define _POSIX_C_SOURCE 200809L
#include "screenshot.h"
#include "trixie.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <wlr/util/log.h>

/* ── opaque ctx — just a marker so the header type is satisfied ───────────── */
struct ScreenshotCtx {
  int unused;
};

/* ── output path ──────────────────────────────────────────────────────────── */

static void build_output_path(char *buf, int sz) {
  const char *pics = getenv("XDG_PICTURES_DIR");
  if(!pics || !pics[0]) {
    const char *home = getenv("HOME");
    if(!home) home = "/tmp";
    snprintf(buf, sz, "%s/Pictures", home);
  } else {
    snprintf(buf, sz, "%s", pics);
  }
  mkdir(buf, 0755);
  time_t     now = time(NULL);
  struct tm *tm  = localtime(&now);
  int        dl  = (int)strlen(buf);
  strftime(buf + dl, sz - dl, "/trixie_%Y-%m-%d_%H-%M-%S.png", tm);
}

/* ── clipboard copy ───────────────────────────────────────────────────────── */

static void copy_to_clipboard_async(const char *path) {
  pid_t pid = fork();
  if(pid < 0) return;
  if(pid == 0) {
    pid_t p2 = fork();
    if(p2 < 0) _exit(1);
    if(p2 == 0) {
      setsid();
      int fd = open(path, O_RDONLY);
      if(fd >= 0) {
        dup2(fd, STDIN_FILENO);
        close(fd);
      }
      execlp("wl-copy", "wl-copy", "--type", "image/png", (char *)NULL);
      _exit(127);
    }
    _exit(0);
  }
  waitpid(pid, NULL, 0);
}

/* ── spawn grim detached ──────────────────────────────────────────────────── */
/*
 * grim [-g "X,Y WxH"] <output_path>
 *
 * We double-fork so the compositor doesn't need to waitpid and the child
 * is fully detached. After grim writes the file we optionally copy to
 * clipboard in a grandchild.
 */
static void spawn_grim(const char *geometry, const char *path, bool copy_clipboard) {
  pid_t pid = fork();
  if(pid < 0) return;
  if(pid == 0) {
    pid_t p2 = fork();
    if(p2 < 0) _exit(1);
    if(p2 == 0) {
      setsid();
      /* Close compositor fds */
      int maxfd = (int)sysconf(_SC_OPEN_MAX);
      for(int fd = 3; fd < maxfd; fd++)
        close(fd);

      if(geometry) {
        execlp("grim", "grim", "-g", geometry, path, (char *)NULL);
      } else {
        execlp("grim", "grim", path, (char *)NULL);
      }
      _exit(127);
    }
    /* Middle child: wait for grim, then maybe copy */
    int status = 0;
    waitpid(p2, &status, 0);
    if(WIFEXITED(status) && WEXITSTATUS(status) == 0 && copy_clipboard) {
      copy_to_clipboard_async(path);
    }
    _exit(0);
  }
  waitpid(pid, NULL, 0); /* reap middle fork immediately */
}

/* ── public API ───────────────────────────────────────────────────────────── */

ScreenshotCtx *screenshot_capture(void            *server_ptr,
                                  ScreenshotMode   mode,
                                  ScreenshotRegion region,
                                  bool             copy_clipboard) {
  (void)server_ptr;
  char path[512];
  build_output_path(path, sizeof(path));

  if(mode == SCREENSHOT_FULL) {
    spawn_grim(NULL, path, copy_clipboard);

  } else if(mode == SCREENSHOT_WINDOW || mode == SCREENSHOT_REGION) {
    if(region.w > 0 && region.h > 0) {
      char geom[64];
      snprintf(
          geom, sizeof(geom), "%d,%d %dx%d", region.x, region.y, region.w, region.h);
      spawn_grim(geom, path, copy_clipboard);
    } else {
      /* Fallback to full if no region provided */
      spawn_grim(NULL, path, copy_clipboard);
    }
  }

  wlr_log(WLR_INFO, "screenshot queued: %s", path);
  return NULL; /* ctx unused — grim runs async */
}

void screenshot_full(void *server) {
  ScreenshotRegion r = { 0 };
  screenshot_capture(server, SCREENSHOT_FULL, r, true);
}

void screenshot_window(void *server) {
  TrixieServer *s  = server;
  PaneId        id = twm_focused_id(&s->twm);
  Pane         *p  = id ? twm_pane_by_id(&s->twm, id) : NULL;
  if(!p) {
    screenshot_full(server);
    return;
  }
  ScreenshotRegion r = { p->rect.x, p->rect.y, p->rect.w, p->rect.h };
  screenshot_capture(server, SCREENSHOT_WINDOW, r, true);
}

void screenshot_region(void *server, ScreenshotRegion r) {
  if(r.w > 0 && r.h > 0) {
    screenshot_capture(server, SCREENSHOT_REGION, r, true);
    return;
  }

  /* Launch slurp to pick a region, then grim to capture it.
   * Both run in a detached grandchild so the compositor never blocks. */
  TrixieServer *s = server;
  (void)s;

  pid_t pid = fork();
  if(pid < 0) return;
  if(pid == 0) {
    pid_t p2 = fork();
    if(p2 < 0) _exit(1);
    if(p2 == 0) {
      setsid();
      int maxfd = (int)sysconf(_SC_OPEN_MAX);
      for(int fd = 3; fd < maxfd; fd++)
        close(fd);

      /* Build output path in grandchild */
      char path[512];
      build_output_path(path, sizeof(path));

      /* Use shell to pipe slurp → grim */
      char cmd[700];
      snprintf(
          cmd, sizeof(cmd), "slurp -f '%%x,%%y %%wx%%h' | grim -g - '%s'", path);
      execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
      _exit(127);
    }
    _exit(0);
  }
  waitpid(pid, NULL, 0);
}
