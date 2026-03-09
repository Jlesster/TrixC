/* bar_worker.c — Async background polling for bar modules.
 *
 */

#define _POSIX_C_SOURCE 200809L
#include "trixie.h"

/* Slot constants for new built-in workers.
 * If trixie.h already defines these they will be no-ops. */
#ifndef BAR_SLOT_GPU
#define BAR_SLOT_GPU (BAR_SLOT_BUILTIN_COUNT)
#endif
#ifndef BAR_SLOT_TEMP
#define BAR_SLOT_TEMP (BAR_SLOT_BUILTIN_COUNT + 1)
#endif
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

/* Per-interface rx/tx byte counters for bandwidth calculation */
static char      g_net_iface[32] = { 0 };
static long long g_net_rx_prev   = 0;
static long long g_net_tx_prev   = 0;
static int64_t   g_net_last_ms   = 0;

static long long read_net_counter(const char *iface, const char *dir) {
  char path[128];
  snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/%s_bytes", iface, dir);
  FILE *f = fopen(path, "r");
  if(!f) return 0;
  long long v = 0;
  fscanf(f, "%lld", &v);
  fclose(f);
  return v;
}

static void fmt_rate(long long Bps, char *out, int sz) {
  if(Bps >= 1024 * 1024)
    snprintf(out, sz, "%.1fM", Bps / (1024.0 * 1024.0));
  else if(Bps >= 1024)
    snprintf(out, sz, "%.0fK", Bps / 1024.0);
  else
    snprintf(out, sz, "%lldB", Bps);
}

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
  char buf[96];
  if(!up || iface[0] == '-') {
    snprintf(buf, sizeof(buf), "--- %s", iface);
    slot_write(pool, BAR_SLOT_NETWORK, buf);
    return;
  }

  long long rx = read_net_counter(iface, "rx");
  long long tx = read_net_counter(iface, "tx");
  int64_t   ms = now_ms();

  if(g_net_last_ms > 0 && !strcmp(g_net_iface, iface)) {
    double    dt  = (ms - g_net_last_ms) / 1000.0;
    long long drx = (long long)((rx - g_net_rx_prev) / dt);
    long long dtx = (long long)((tx - g_net_tx_prev) / dt);
    if(drx < 0) drx = 0;
    if(dtx < 0) dtx = 0;
    char rs[16], ts[16];
    fmt_rate(drx, rs, sizeof(rs));
    fmt_rate(dtx, ts, sizeof(ts));
    snprintf(buf, sizeof(buf), "↓%s ↑%s", rs, ts);
  } else {
    snprintf(buf, sizeof(buf), "NET %s", iface);
  }

  strncpy(g_net_iface, iface, sizeof(g_net_iface) - 1);
  g_net_rx_prev = rx;
  g_net_tx_prev = tx;
  g_net_last_ms = ms;
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

  /* Prime with first sample so we can show a value immediately */
  read_cpu_stat(&prev_idle, &prev_total);
  slot_write(pool, BAR_SLOT_CPU, "CPU --%%");

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
 * §5b  GPU worker  (nvidia-smi or /sys/class/drm + hwmon)
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool poll_gpu_nvidia(BarWorkerPool *pool) {
  /* Try nvidia-smi query — fast and reliable on Nvidia systems */
  FILE *p = popen("nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total"
                  " --format=csv,noheader,nounits 2>/dev/null | head -1",
                  "r");
  if(!p) return false;
  char buf[128] = { 0 };
  bool got      = fgets(buf, sizeof(buf), p) != NULL;
  pclose(p);
  if(!got || !buf[0] || buf[0] == '\n') return false;
  int       util = 0;
  long long used = 0, total = 0;
  if(sscanf(buf, "%d, %lld, %lld", &util, &used, &total) < 1) return false;
  char out[64];
  if(total > 0)
    snprintf(out, sizeof(out), "GPU %d%% %lldM", util, used);
  else
    snprintf(out, sizeof(out), "GPU %d%%", util);
  slot_write(pool, BAR_SLOT_GPU, out);
  return true;
}

static bool poll_gpu_amd(BarWorkerPool *pool) {
  /* AMD: /sys/class/drm/card.../device/gpu_busy_percent */
  DIR *d = opendir("/sys/class/drm");
  if(!d) return false;
  struct dirent *e;
  bool           found = false;
  while((e = readdir(d)) && !found) {
    if(strncmp(e->d_name, "card", 4) != 0 || strchr(e->d_name + 4, '-')) continue;
    char path[256];
    snprintf(
        path, sizeof(path), "/sys/class/drm/%s/device/gpu_busy_percent", e->d_name);
    FILE *f = fopen(path, "r");
    if(!f) continue;
    int pct = 0;
    if(fscanf(f, "%d", &pct) == 1) {
      /* also try vram */
      char vpath[256], out[64];
      snprintf(vpath,
               sizeof(vpath),
               "/sys/class/drm/%s/device/mem_info_vram_used",
               e->d_name);
      FILE     *vf        = fopen(vpath, "r");
      long long vram_used = 0;
      if(vf) {
        fscanf(vf, "%lld", &vram_used);
        fclose(vf);
        vram_used /= (1024 * 1024);
      }
      if(vram_used > 0)
        snprintf(out, sizeof(out), "GPU %d%% %lldM", pct, vram_used);
      else
        snprintf(out, sizeof(out), "GPU %d%%", pct);
      slot_write(pool, BAR_SLOT_GPU, out);
      found = true;
    }
    fclose(f);
  }
  closedir(d);
  return found;
}

static void *worker_gpu(void *arg) {
  BarWorkerPool *pool      = arg;
  /* Detect which GPU type we have once */
  bool           is_nvidia = (system("nvidia-smi -L >/dev/null 2>&1") == 0);
  while(pool->running) {
    bool ok = is_nvidia ? poll_gpu_nvidia(pool) : poll_gpu_amd(pool);
    if(!ok) {
      sleep_ms(pool, 10000);
      continue;
    } /* back off if no GPU */
    sleep_ms(pool, 2000);
  }
  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5c  Temperature worker  (/sys/class/hwmon)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void poll_temperature(BarWorkerPool *pool) {
  /* Walk hwmon devices looking for CPU/package temperature.
   * Priority: coretemp (Intel) > k10temp (AMD) > any with "Tdie"/"Package" */
  DIR *d = opendir("/sys/class/hwmon");
  if(!d) return;

  struct dirent *e;
  int            best_temp  = -1;
  /* score: 3=package/tdie, 2=coretemp/k10temp, 1=anything */
  int            best_score = 0;

  while((e = readdir(d))) {
    if(e->d_name[0] == '.') continue;
    char base[128];
    snprintf(base, sizeof(base), "/sys/class/hwmon/%s", e->d_name);

    /* Read driver name */
    char npath[160], name[32] = { 0 };
    snprintf(npath, sizeof(npath), "%s/name", base);
    FILE *nf = fopen(npath, "r");
    if(nf) {
      fscanf(nf, "%31s", name);
      fclose(nf);
    }
    bool is_cpu = (!strcmp(name, "coretemp") || !strcmp(name, "k10temp") ||
                   !strcmp(name, "zenpower") || !strcmp(name, "nct6775"));

    /* Scan temp*_input files */
    for(int ti = 1; ti <= 12; ti++) {
      char tpath[180], lpath[180], label[64] = { 0 };
      snprintf(tpath, sizeof(tpath), "%s/temp%d_input", base, ti);
      snprintf(lpath, sizeof(lpath), "%s/temp%d_label", base, ti);
      FILE *tf = fopen(tpath, "r");
      if(!tf) continue;
      int millic = 0;
      fscanf(tf, "%d", &millic);
      fclose(tf);
      FILE *lf = fopen(lpath, "r");
      if(lf) {
        fscanf(lf, "%63[^\n]", label);
        fclose(lf);
      }
      int temp = millic / 1000;
      if(temp <= 0 || temp > 120) continue;

      bool is_pkg = (strstr(label, "Package") || strstr(label, "Tdie") ||
                     strstr(label, "Tccd") || strstr(label, "Tctl"));
      int  score  = is_pkg ? 3 : (is_cpu ? 2 : 1);
      if(score > best_score || (score == best_score && temp > best_temp)) {
        best_score = score;
        best_temp  = temp;
      }
    }
  }
  closedir(d);

  if(best_temp < 0) return;
  char buf[32];
  snprintf(buf, sizeof(buf), "CPU %d°C", best_temp);
  slot_write(pool, BAR_SLOT_TEMP, buf);
}

static void *worker_temperature(void *arg) {
  BarWorkerPool *pool = arg;
  while(pool->running) {
    poll_temperature(pool);
    sleep_ms(pool, 3000);
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
  /* GPU and temperature — detached, exit gracefully if hardware absent */
  {
    pthread_t t;
    pthread_create(&t, NULL, worker_gpu, pool);
    pthread_detach(t);
  }
  {
    pthread_t t;
    pthread_create(&t, NULL, worker_temperature, pool);
    pthread_detach(t);
  }

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
