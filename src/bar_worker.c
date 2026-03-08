/* bar_worker.c — Async background polling for bar modules.
 *
 * Each built-in module (battery, network, volume, cpu, memory) and each
 * user-defined exec module runs in its own pthread.  Workers sleep for their
 * configured interval, poll their data source, write the result into a
 * BarModuleSlot, and bump a shared generation counter.
 *
 * bar_update() reads the slots directly — zero blocking, zero popen() on the
 * render thread.
 *
 * Thread safety
 * ─────────────
 * Slot writes are protected by pool->mu.  Reads from bar_update happen on the
 * Wayland event-loop thread without holding the mutex; this is safe because:
 *   - slot->text is written atomically (single strncpy into a buffer whose
 *     address never changes)
 *   - slot->valid is written after slot->text (acquire/release via the mutex)
 *   - generation is _Atomic so bar_update can cheaply check if anything new
 *     arrived since the last frame
 *
 * Shutdown
 * ────────
 * bar_workers_stop() sets pool->running = false, then joins all threads.
 * Workers check pool->running each iteration and exit cleanly.
 */

#define _POSIX_C_SOURCE 200809L
#include "trixie.h"
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── Time helper ────────────────────────────────────────────────────────────── */

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(BarWorkerPool *pool, int64_t ms) {
  if(ms <= 0) return;
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += ms / 1000;
  deadline.tv_nsec += (ms % 1000) * 1000000L;
  if(deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
  }
  pthread_mutex_lock(&pool->mu);
  /* pthread_cond_timedwait releases mu while waiting; it returns early if
   * stop() broadcasts pool->wake before the deadline expires. */
  while(pool->running) {
    int rc = pthread_cond_timedwait(&pool->wake, &pool->mu, &deadline);
    if(rc == ETIMEDOUT) break;
    /* Spurious wakeup or broadcast — re-check running and deadline. */
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    if(now.tv_sec > deadline.tv_sec ||
       (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
      break;
  }
  pthread_mutex_unlock(&pool->mu);
}

/* ── Slot write helper ──────────────────────────────────────────────────────── */

static void slot_write(BarWorkerPool *pool, int idx, const char *text) {
  pthread_mutex_lock(&pool->mu);
  BarModuleSlot *s = &pool->slots[idx];
  strncpy(s->text, text, sizeof(s->text) - 1);
  s->text[sizeof(s->text) - 1] = '\0';
  s->valid                     = true;
  s->updated_ms                = now_ms();
  /* Bump generation so bar_update knows something changed */
  atomic_fetch_add(&pool->generation, 1);
  pthread_mutex_unlock(&pool->mu);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Battery worker
 * ═══════════════════════════════════════════════════════════════════════════ */

static void poll_battery(BarWorkerPool *pool) {
  int  pct      = -1;
  bool charging = false;

  const char *paths[] = {
    "/sys/class/power_supply/BAT0/capacity",
    "/sys/class/power_supply/BAT1/capacity",
    NULL,
  };
  for(int i = 0; paths[i]; i++) {
    FILE *f = fopen(paths[i], "r");
    if(!f) continue;
    fscanf(f, "%d", &pct);
    fclose(f);
    break;
  }

  const char *spaths[] = {
    "/sys/class/power_supply/BAT0/status",
    "/sys/class/power_supply/BAT1/status",
    NULL,
  };
  for(int i = 0; spaths[i]; i++) {
    FILE *f = fopen(spaths[i], "r");
    if(!f) continue;
    char st[32] = { 0 };
    fscanf(f, "%31s", st);
    fclose(f);
    charging = (!strcmp(st, "Charging") || !strcmp(st, "Full"));
    break;
  }

  if(pct < 0) return; /* no battery */

  const char *icon = charging ? "[+]" : pct > 70 ? "[=]" : pct > 30 ? "[-]" : "[!]";
  char        buf[64];
  snprintf(buf, sizeof(buf), "%s %d%%", icon, pct);
  slot_write(pool, BAR_SLOT_BATTERY, buf);
}

static void *worker_battery(void *arg) {
  BarWorkerPool *pool = arg;
  while(pool->running) {
    poll_battery(pool);
    sleep_ms(pool, 5000);
  }
  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Network worker
 * ═══════════════════════════════════════════════════════════════════════════ */

static void poll_network(BarWorkerPool *pool) {
  char iface[32] = "---";
  bool up        = false;

  FILE *f = fopen("/proc/net/route", "r");
  if(f) {
    char line[256];
    fgets(line, sizeof(line), f); /* header */
    while(fgets(line, sizeof(line), f)) {
      char     ifa[32];
      unsigned dest, flags;
      if(sscanf(line, "%31s %x %*s %x", ifa, &dest, &flags) == 3 && dest == 0 &&
         (flags & 0x2)) {
        strncpy(iface, ifa, sizeof(iface) - 1);
        fclose(f);
        char path[128];
        snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", ifa);
        FILE *sf = fopen(path, "r");
        if(sf) {
          char state[16] = { 0 };
          fscanf(sf, "%15s", state);
          fclose(sf);
          up = !strcmp(state, "up");
        }
        goto done;
      }
    }
    fclose(f);
  }

  /* Fallback: scan for any up interface */
  {
    DIR *d = opendir("/sys/class/net");
    if(d) {
      struct dirent *ent;
      while((ent = readdir(d)) != NULL) {
        if(ent->d_name[0] == '.' || !strcmp(ent->d_name, "lo")) continue;
        char path[128];
        snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", ent->d_name);
        FILE *sf = fopen(path, "r");
        if(!sf) continue;
        char state[16] = { 0 };
        bool ok        = fscanf(sf, "%15s", state) == 1;
        fclose(sf);
        if(ok && !strcmp(state, "up")) {
          strncpy(iface, ent->d_name, sizeof(iface) - 1);
          up = true;
          break;
        }
      }
      closedir(d);
    }
  }

done:;
  char buf[64];
  snprintf(buf, sizeof(buf), "%s %s", up ? "NET" : "---", iface);
  slot_write(pool, BAR_SLOT_NETWORK, buf);
}

static void *worker_network(void *arg) {
  BarWorkerPool *pool = arg;
  while(pool->running) {
    poll_network(pool);
    sleep_ms(pool, 2000);
  }
  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Volume worker
 * ═══════════════════════════════════════════════════════════════════════════ */

static void poll_volume(BarWorkerPool *pool) {
  int  vol   = -1;
  bool muted = false;

  /* Try wpctl first (PipeWire) */
  FILE *p = popen("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r");
  if(p) {
    char buf[128] = { 0 };
    bool got      = fgets(buf, sizeof(buf), p) != NULL;
    pclose(p);
    if(got) {
      float v = 0.f;
      if(sscanf(buf, "Volume: %f", &v) == 1) {
        muted = strstr(buf, "[MUTED]") != NULL;
        vol   = (int)(v * 100.f + 0.5f);
        goto write_vol;
      }
    }
  }

  /* Fallback: amixer */
  p = popen("amixer sget Master 2>/dev/null", "r");
  if(p) {
    char buf[128];
    while(fgets(buf, sizeof(buf), p)) {
      int pct;
      if(sscanf(buf, " Front Left: %*d [%d%%]", &pct) == 1 ||
         sscanf(buf, " Mono: %*d [%d%%]", &pct) == 1) {
        muted = strstr(buf, "[off]") != NULL;
        vol   = pct;
        break;
      }
    }
    pclose(p);
  }

write_vol:
  if(vol < 0) return;
  const char *icon = muted ? "[M]" : vol >= 70 ? "[+]" : vol >= 30 ? "[~]" : "[-]";
  char        buf[64];
  snprintf(buf, sizeof(buf), "%s %d%%", icon, vol);
  slot_write(pool, BAR_SLOT_VOLUME, buf);
}

static void *worker_volume(void *arg) {
  BarWorkerPool *pool = arg;
  while(pool->running) {
    poll_volume(pool);
    sleep_ms(pool, 1000);
  }
  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  CPU worker  (reads /proc/stat, computes usage % over 1-second window)
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool read_cpu_stat(long long *idle_out, long long *total_out) {
  FILE *f = fopen("/proc/stat", "r");
  if(!f) return false;
  char line[256];
  bool ok = false;
  while(fgets(line, sizeof(line), f)) {
    if(strncmp(line, "cpu ", 4) != 0) continue;
    long long u, n, s, i, w, irq, softirq, steal;
    if(sscanf(line,
              "cpu  %lld %lld %lld %lld %lld %lld %lld %lld",
              &u,
              &n,
              &s,
              &i,
              &w,
              &irq,
              &softirq,
              &steal) == 8) {
      *idle_out  = i + w;
      *total_out = u + n + s + i + w + irq + softirq + steal;
      ok         = true;
    }
    break;
  }
  fclose(f);
  return ok;
}

static void *worker_cpu(void *arg) {
  BarWorkerPool *pool      = arg;
  long long      prev_idle = 0, prev_total = 0;
  read_cpu_stat(&prev_idle, &prev_total);

  while(pool->running) {
    sleep_ms(pool, 1000);
    long long idle, total;
    if(!read_cpu_stat(&idle, &total)) continue;
    long long d_idle  = idle - prev_idle;
    long long d_total = total - prev_total;
    prev_idle         = idle;
    prev_total        = total;
    if(d_total <= 0) continue;
    int  pct = (int)(100LL * (d_total - d_idle) / d_total);
    char buf[32];
    snprintf(buf, sizeof(buf), "CPU %d%%", pct);
    slot_write(pool, BAR_SLOT_CPU, buf);
  }
  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  Memory worker  (reads /proc/meminfo)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void poll_memory(BarWorkerPool *pool) {
  FILE *f = fopen("/proc/meminfo", "r");
  if(!f) return;
  long long total = 0, avail = 0;
  char      line[256];
  while(fgets(line, sizeof(line), f)) {
    long long v;
    if(sscanf(line, "MemTotal: %lld kB", &v) == 1) total = v;
    if(sscanf(line, "MemAvailable: %lld kB", &v) == 1) avail = v;
    if(total && avail) break;
  }
  fclose(f);
  if(!total) return;
  long long used_mb  = (total - avail) / 1024;
  long long total_mb = total / 1024;
  char      buf[64];
  snprintf(buf, sizeof(buf), "MEM %lldM", used_mb);
  (void)total_mb; /* available for extended format if wanted */
  slot_write(pool, BAR_SLOT_MEMORY, buf);
}

static void *worker_memory(void *arg) {
  BarWorkerPool *pool = arg;
  while(pool->running) {
    poll_memory(pool);
    sleep_ms(pool, 2000);
  }
  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  Exec module workers
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
  BarWorkerPool *pool;
  int            slot_idx;
  char           cmd[256];
  char           icon[32];
  int            interval_ms;
} ExecWorkerArg;

static void *worker_exec(void *arg) {
  ExecWorkerArg *ea   = arg;
  BarWorkerPool *pool = ea->pool;

  while(pool->running) {
    FILE *p = popen(ea->cmd, "r");
    if(p) {
      char buf[256] = { 0 };
      bool got      = fgets(buf, sizeof(buf), p) != NULL;
      pclose(p);
      if(got) {
        /* strip trailing newline */
        size_t n = strlen(buf);
        while(n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
          buf[--n] = '\0';
        char out[256];
        if(ea->icon[0])
          snprintf(out, sizeof(out), "%s %s", ea->icon, buf);
        else
          strncpy(out, buf, sizeof(out) - 1);
        slot_write(pool, ea->slot_idx, out);
      }
    }
    sleep_ms(pool, ea->interval_ms);
  }

  free(ea);
  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

void bar_workers_init(BarWorkerPool *pool, const Config *cfg) {
  memset(pool, 0, sizeof(*pool));
  pthread_mutex_init(&pool->mu, NULL);
  pthread_cond_init(&pool->wake, NULL);
  atomic_store(&pool->generation, 0);
  pool->running = true;
  pool->cfg     = cfg;

  /* Built-in workers */
  pthread_create(&pool->threads[BAR_SLOT_BATTERY], NULL, worker_battery, pool);
  pthread_create(&pool->threads[BAR_SLOT_NETWORK], NULL, worker_network, pool);
  pthread_create(&pool->threads[BAR_SLOT_VOLUME], NULL, worker_volume, pool);
  pthread_create(&pool->threads[BAR_SLOT_CPU], NULL, worker_cpu, pool);
  pthread_create(&pool->threads[BAR_SLOT_MEMORY], NULL, worker_memory, pool);

  /* Exec module workers */
  bar_workers_sync_exec(pool, cfg);
}

/* (Re-)spawn exec workers after a config reload.
 * Existing exec workers will exit naturally when pool->running stays true
 * but their slot is reused — for a full reload the caller should stop and
 * reinit the pool, or we simply let old workers finish their current sleep
 * and exit (they check pool->running each loop). */
void bar_workers_sync_exec(BarWorkerPool *pool, const Config *cfg) {
  for(int i = 0; i < cfg->bar.module_cfg_count && i < MAX_BAR_MODULE_CFGS; i++) {
    const BarModuleCfg *mc = &cfg->bar.module_cfgs[i];
    if(!mc->exec[0]) continue;

    int            slot = BAR_SLOT_EXEC_BASE + i;
    ExecWorkerArg *ea   = calloc(1, sizeof(*ea));
    ea->pool            = pool;
    ea->slot_idx        = slot;
    strncpy(ea->cmd, mc->exec, sizeof(ea->cmd) - 1);
    strncpy(ea->icon, mc->icon, sizeof(ea->icon) - 1);
    ea->interval_ms = (mc->interval > 0 ? mc->interval : 5) * 1000;

    pthread_t t;
    pthread_create(&t, NULL, worker_exec, ea);
    pthread_detach(t); /* fire-and-forget; exits when pool->running = false */
  }
}

void bar_workers_stop(BarWorkerPool *pool) {
  /* Signal all sleeping workers to wake up and check pool->running. */
  pthread_mutex_lock(&pool->mu);
  pool->running = false;
  pthread_cond_broadcast(&pool->wake);
  pthread_mutex_unlock(&pool->mu);

  /* Now join — threads will have exited almost immediately. */
  for(int i = 0; i < BAR_SLOT_BUILTIN_COUNT; i++) {
    if(pool->threads[i]) pthread_join(pool->threads[i], NULL);
  }
  pthread_cond_destroy(&pool->wake);
  pthread_mutex_destroy(&pool->mu);
}
