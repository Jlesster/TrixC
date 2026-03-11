/* overlay.c — Pixel-perfect ratatui-style TUI overlay for Trixie.sonConsole
 * :JasonBuildRun
 * :JasonBuildRun
 * :JasonBuildRun
 * 
 * Visual system — pure ov_fill_rect, zero box-drawing glyphs:
 *
 *   Panel box
 *   ─────────
 *   2px border (ac color) on all four sides
 *   Filled header bar: darkened bg + ac-colored title text left,
 *                      dim hint text right
 *   1px separator under header (ac, 40% alpha)
 *   Content rows with ROW_H = g_ov_th + 8 (4 px above + below)
 *   Cursor row: lighter bg + 2px left accent pip
 *   Right-edge 2px scrollbar thumb when overflowing
 *   Section dividers: 1px line + label sitting on top
 *
 *   Tab bar
 *   ───────
 *   Single row above every panel box.
 *   Active tab: solid ac fill, bg-coloured text.
 *   Inactive:   no fill, dimmed text.
 *   1px rule under entire strip.
 *
 *   Mode line
 *   ─────────
 *   1px rule above, then one text row: hints left, WS status right.
 */

#define _POSIX_C_SOURCE 200809L
#include "overlay_internal.h"
#include "run_panel.h"
#include "files_panel.h"
#include "nvim_panel.h"
#include "marvin_panel.h"
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
  int real = (g_log_ring.head - g_log_ring.count + idx + LOG_RING_SIZE * 2) % LOG_RING_SIZE;
  return g_log_ring.lines[real];
}
static void overlay_log_handler(enum wlr_log_importance imp, const char *fmt, va_list args) {
  (void)imp;
  char buf[LOG_LINE_MAX]; va_list a2; va_copy(a2, args);
  vsnprintf(buf, sizeof(buf), fmt, args); log_ring_push(buf);
  vfprintf(stderr, fmt, a2); va_end(a2); fputc('\n', stderr);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Process list
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PROC_MAX  48
#define PROC_HIST 16
typedef enum { PROC_SORT_CPU=0, PROC_SORT_RSS, PROC_SORT_PID } ProcSort;
static ProcSort g_proc_sort = PROC_SORT_CPU;
typedef struct {
  pid_t pid; char comm[32]; float cpu_pct; long rss_kb;
  float cpu_hist[PROC_HIST]; int hist_head, hist_count;
  long long prev_total_jiff, prev_proc_jiff;
} ProcEntry;
static ProcEntry g_procs[PROC_MAX];
static int       g_proc_count=0;
static int64_t   g_proc_next_ms=0;
static long      g_clk_tck=0;

int64_t ov_now_ms(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
  return ts.tv_sec*1000+ts.tv_nsec/1000000;
}
static void refresh_procs(void) {
  int64_t now=ov_now_ms(); if(now<g_proc_next_ms) return;
  g_proc_next_ms=now+2000;
  if(!g_clk_tck) g_clk_tck=sysconf(_SC_CLK_TCK);
  long long cpu_total=0;
  { FILE *f=fopen("/proc/stat","r");
    if(f){ long long u,n,s,i,w,r,si,st;
      if(fscanf(f,"cpu  %lld %lld %lld %lld %lld %lld %lld %lld",&u,&n,&s,&i,&w,&r,&si,&st)==8)
        cpu_total=u+n+s+i+w+r+si+st;
      fclose(f); } }
  static ProcEntry old[PROC_MAX]; int old_count=g_proc_count;
  memcpy(old,g_procs,sizeof(ProcEntry)*(size_t)old_count);
  g_proc_count=0;
  DIR *d=opendir("/proc"); if(!d) return;
  struct dirent *ent;
  while((ent=readdir(d))!=NULL && g_proc_count<PROC_MAX) {
    pid_t pid=(pid_t)atoi(ent->d_name); if(pid<=0) continue;
    char path[64]; snprintf(path,sizeof(path),"/proc/%d/stat",pid);
    FILE *f=fopen(path,"r"); if(!f) continue;
    ProcEntry pe={.pid=pid}; long long utime=0,stime=0; long rss=0; char comm_buf[64]={0};
    int matched=fscanf(f,"%*d (%63[^)]) %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u "
                         "%lld %lld %*d %*d %*d %*d %*d %*d %*u %*u %ld",
                         comm_buf,&utime,&stime,&rss);
    fclose(f); if(matched!=4) continue;
    strncpy(pe.comm,comm_buf,sizeof(pe.comm)-1);
    long long proc_jiff=utime+stime, prev_total=0, prev_proc=0;
    for(int oi=0;oi<old_count;oi++) if(old[oi].pid==pid){
      prev_total=old[oi].prev_total_jiff; prev_proc=old[oi].prev_proc_jiff;
      memcpy(pe.cpu_hist,old[oi].cpu_hist,sizeof(pe.cpu_hist));
      pe.hist_head=old[oi].hist_head; pe.hist_count=old[oi].hist_count; break;
    }
    pe.prev_total_jiff=cpu_total; pe.prev_proc_jiff=proc_jiff;
    long long dtotal=cpu_total-prev_total, dproc=proc_jiff-prev_proc;
    pe.cpu_pct=(dtotal>0)?(float)dproc*100.f/(float)dtotal:0.f;
    if(pe.cpu_pct<0.f) pe.cpu_pct=0.f;
    pe.cpu_hist[pe.hist_head]=pe.cpu_pct;
    pe.hist_head=(pe.hist_head+1)%PROC_HIST;
    if(pe.hist_count<PROC_HIST) pe.hist_count++;
    pe.rss_kb=rss*(long)(sysconf(_SC_PAGESIZE)/1024);
    g_procs[g_proc_count++]=pe;
  }
  closedir(d);
  for(int i=1;i<g_proc_count;i++){
    ProcEntry key=g_procs[i]; int j=i-1; bool before=false;
    while(j>=0){
      switch(g_proc_sort){
        case PROC_SORT_CPU: before=g_procs[j].cpu_pct<key.cpu_pct; break;
        case PROC_SORT_RSS: before=g_procs[j].rss_kb<key.rss_kb;   break;
        case PROC_SORT_PID: before=g_procs[j].pid>key.pid;         break;
      }
      if(!before) break; g_procs[j+1]=g_procs[j]; j--;
    }
    g_procs[j+1]=key;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Git state
 * ═══════════════════════════════════════════════════════════════════════════ */

#define GIT_LINE_MAX  128
#define GIT_LINES_MAX 64
#define GIT_DIFF_MAX  256
#define GIT_FILE_MAX  32
typedef struct { char xy[3]; char path[GIT_LINE_MAX]; bool staged; } GitFile;
typedef struct {
  char branch[GIT_LINE_MAX]; char lines[GIT_LINES_MAX][GIT_LINE_MAX]; int line_count;
  GitFile files[GIT_FILE_MAX]; int file_count;
  char diff[GIT_DIFF_MAX][GIT_LINE_MAX]; int diff_count; int diff_for; bool show_diff;
  int64_t fetched_ms; bool valid; char root[1024];
} GitState;
static GitState g_git;
static char     g_git_cwd[1024]={0};
static char  g_git_commit_msg[256] = { 0 };
static int   g_git_commit_len      = 0;
static bool  g_git_commit_editing  = false;

static int run_capture(const char *cmd, char out[][GIT_LINE_MAX], int max_lines) {
  FILE *f=popen(cmd,"r"); if(!f) return 0; int n=0;
  while(n<max_lines){ if(!fgets(out[n],GIT_LINE_MAX,f)) break;
    size_t l=strlen(out[n]);
    while(l>0&&(out[n][l-1]=='\n'||out[n][l-1]=='\r')) out[n][--l]='\0'; n++; }
  pclose(f); return n;
}
static void git_load_diff(int idx) {
  if(idx<0||idx>=g_git.file_count) return;
  if(g_git.diff_for==idx&&g_git.diff_count>0) return;
  GitFile *gf=&g_git.files[idx]; char cmd[512];
  if(gf->staged) snprintf(cmd,sizeof(cmd),"git diff --cached -- '%s' 2>/dev/null",gf->path);
  else           snprintf(cmd,sizeof(cmd),"git diff -- '%s' 2>/dev/null",gf->path);
  g_git.diff_count=run_capture(cmd,g_git.diff,GIT_DIFF_MAX); g_git.diff_for=idx;
}
static bool git_find_root(const char *dir,char *out,size_t outsz){
  char old[1024]={0}; getcwd(old,sizeof(old));
  if(chdir(dir)!=0) return false;
  FILE *f=popen("git rev-parse --show-toplevel 2>/dev/null","r"); bool ok=false;
  if(f){ if(fgets(out,(int)outsz,f)){ size_t l=strlen(out);
      while(l>0&&(out[l-1]=='\n'||out[l-1]=='\r')) out[--l]='\0'; ok=l>0; } pclose(f); }
  if(old[0]) chdir(old); return ok;
}
static void refresh_git(void) {
  int64_t now=ov_now_ms();
  char new_root[1024]={0};
  git_find_root(g_git_cwd[0]?g_git_cwd:".",new_root,sizeof(new_root));
  if(strcmp(new_root,g_git.root)!=0){ memset(&g_git,0,sizeof(g_git)); g_git.diff_for=-1;
    strncpy(g_git.root,new_root,sizeof(g_git.root)-1); }
  if(g_git.valid&&now-g_git.fetched_ms<5000) return;
  char old[1024]={0}; getcwd(old,sizeof(old));
  if(g_git.root[0]) chdir(g_git.root);
  memset(&g_git.branch,0,sizeof(g_git.branch));
  memset(&g_git.lines,0,sizeof(g_git.lines));
  memset(&g_git.files,0,sizeof(g_git.files));
  g_git.line_count=g_git.file_count=g_git.diff_count=0;
  g_git.diff_for=-1; g_git.show_diff=false;
  { FILE *f=popen("git rev-parse --abbrev-ref HEAD 2>/dev/null","r");
    if(!f){ strcpy(g_git.branch,"(not a git repo)"); if(old[0])chdir(old); return; }
    if(!fgets(g_git.branch,sizeof(g_git.branch),f)){
      pclose(f); strcpy(g_git.branch,"(not a git repo)"); if(old[0])chdir(old); return; }
    pclose(f); size_t bl=strlen(g_git.branch);
    while(bl>0&&(g_git.branch[bl-1]=='\n'||g_git.branch[bl-1]=='\r')) g_git.branch[--bl]='\0'; }
  { char st[GIT_FILE_MAX][GIT_LINE_MAX];
    int sc=run_capture("git status --porcelain 2>/dev/null",st,GIT_FILE_MAX);
    for(int i=0;i<sc&&g_git.file_count<GIT_FILE_MAX;i++){
      if(strlen(st[i])<4) continue;
      GitFile *gf=&g_git.files[g_git.file_count++];
      gf->xy[0]=st[i][0]; gf->xy[1]=st[i][1]; gf->xy[2]='\0';
      strncpy(gf->path,st[i]+3,sizeof(gf->path)-1);
      gf->staged=(st[i][0]!=' '&&st[i][0]!='?'); }
    if(g_git.file_count==0)
      strncpy(g_git.lines[g_git.line_count++],"  (clean)",GIT_LINE_MAX-1); }
  { char cl[GIT_LINES_MAX][GIT_LINE_MAX];
    int cc=run_capture("git log --oneline -10 2>/dev/null",cl,GIT_LINES_MAX);
    if(g_git.line_count<GIT_LINES_MAX)
      strncpy(g_git.lines[g_git.line_count++],"Recent commits:",GIT_LINE_MAX-1);
    for(int i=0;i<cc&&g_git.line_count<GIT_LINES_MAX;i++){
      char buf[GIT_LINE_MAX]; snprintf(buf,sizeof(buf),"  %s",cl[i]);
      strncpy(g_git.lines[g_git.line_count++],buf,GIT_LINE_MAX-1); } }
  g_git.fetched_ms=now; g_git.valid=true;
  if(old[0]) chdir(old);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Build runner
 * ═══════════════════════════════════════════════════════════════════════════ */

#define BUILD_CMD_MAX  256
#define BUILD_ERR_MAX  128
#define BUILD_ERR_LINE 256
typedef struct { char file[256]; int line,col; char msg[BUILD_ERR_LINE]; bool is_warning; } BuildError;
typedef struct {
  char cmd[BUILD_CMD_MAX]; atomic_bool running; bool done; int exit_code;
  int64_t started_ms,finished_ms;
  BuildError errors[BUILD_ERR_MAX]; int err_count;
  pthread_mutex_t err_lock; pthread_t thread; bool thread_valid;
} BuildState;
static BuildState g_build;

static bool parse_error_c(const char *ln,BuildError *out){
  const char *p=ln; char file[256]; int fi=0;
  while(*p&&fi<255){ if(*p==':'&&*(p+1)>='0'&&*(p+1)<='9'){file[fi]='\0';p++;break;} file[fi++]=*p++; }
  if(!fi||!*p) return false; int line=0,col=0;
  if(sscanf(p,"%d:%d:",&line,&col)<1) return false;
  while(*p&&*p!=':')p++; if(*p==':')p++;
  while(*p&&*p!=':')p++; if(*p==':')p++;
  while(*p==' ')p++;
  bool warn=(strncmp(p,"warning",7)==0), err=(strncmp(p,"error",5)==0)||(strncmp(p,"fatal",5)==0);
  if(!warn&&!err) return false;
  while(*p&&!(*p==':'&&*(p+1)==' '))p++; if(*p)p+=2;
  strncpy(out->file,file,sizeof(out->file)-1); out->line=line; out->col=col; out->is_warning=warn;
  strncpy(out->msg,p,sizeof(out->msg)-1); return true;
}
static bool parse_error_rust(const char *ln,BuildError *out){
  const char *p=ln; while(*p==' ')p++;
  if(strncmp(p,"--> ",4)==0){ p+=4; char file[256]; int fi=0;
    while(*p&&*p!=':'&&fi<255) file[fi++]=*p++; file[fi]='\0';
    int line=0,col=0; if(*p==':'){ p++; sscanf(p,"%d:%d",&line,&col); }
    strncpy(out->file,file,sizeof(out->file)-1); out->line=line; out->col=col; out->is_warning=false;
    strncpy(out->msg,"(see above)",sizeof(out->msg)-1); return fi>0&&line>0; }
  if(strncmp(ln,"error",5)==0||strncmp(ln,"warning",7)==0){
    bool warn=(ln[0]=='w'); const char *colon=strchr(ln,':'); if(!colon) return false;
    colon++; while(*colon==' ')colon++; if(!*colon) return false;
    out->file[0]='\0'; out->line=0; out->col=0; out->is_warning=warn;
    strncpy(out->msg,colon,sizeof(out->msg)-1); return true; }
  return false;
}
static bool parse_error_go(const char *ln,BuildError *out){
  const char *p=ln; if(*p=='.') p++; if(*p=='/') p++;
  char file[256]; int fi=0;
  while(*p&&*p!=':'&&fi<255) file[fi++]=*p++; file[fi]='\0';
  if(!fi||*p!=':') return false; p++;
  int line=0,col=0; if(sscanf(p,"%d:%d:",&line,&col)<1) return false;
  if(line<=0) return false;
  while(*p&&*p!=':')p++; if(*p)p++;
  while(*p&&*p!=':')p++; if(*p)p++;
  while(*p==' ')p++;
  if(!strstr(file,".go")) return false;
  strncpy(out->file,file,sizeof(out->file)-1); out->line=line; out->col=col; out->is_warning=false;
  strncpy(out->msg,p,sizeof(out->msg)-1); return true;
}
static bool parse_error_java(const char *ln,BuildError *out){
  const char *p=ln;
  if(strncmp(p,"[ERROR] ",8)==0) p+=8; else if(strncmp(p,"[WARNING] ",10)==0) p+=10;
  char file[256]; int fi=0;
  while(*p&&*p!=':'&&fi<255) file[fi++]=*p++; file[fi]='\0';
  if(!fi||*p!=':') return false; p++;
  int line=0; if(sscanf(p,"%d:",&line)<1) return false;
  if(line<=0||!strstr(file,".java")) return false;
  while(*p&&*p!=':')p++; if(*p)p++;
  while(*p==' ')p++;
  bool warn=(strncmp(p,"warning",7)==0), err=(strncmp(p,"error",5)==0);
  if(warn||err){ while(*p&&!(*p==':'&&*(p+1)==' '))p++; if(*p)p+=2; }
  strncpy(out->file,file,sizeof(out->file)-1); out->line=line; out->col=0; out->is_warning=warn;
  strncpy(out->msg,p,sizeof(out->msg)-1); return true;
}
static void build_try_parse_error(const char *ln){
  BuildError e={0};
  bool ok=parse_error_c(ln,&e)||parse_error_rust(ln,&e)||parse_error_go(ln,&e)||parse_error_java(ln,&e);
  if(!ok) return;
  pthread_mutex_lock(&g_build.err_lock);
  if(g_build.err_count<BUILD_ERR_MAX) g_build.errors[g_build.err_count++]=e;
  pthread_mutex_unlock(&g_build.err_lock);
}
static void build_autodetect(char *cmd_out,size_t sz){
  struct stat st;
  if(stat("build.ninja",&st)==0||stat("meson.build",&st)==0){snprintf(cmd_out,sz,"meson compile -C builddir 2>&1");return;}
  if(stat("Cargo.toml",&st)==0){snprintf(cmd_out,sz,"cargo build 2>&1");return;}
  if(stat("go.mod",&st)==0){snprintf(cmd_out,sz,"go build ./... 2>&1");return;}
  if(stat("pom.xml",&st)==0){snprintf(cmd_out,sz,"mvn compile -q 2>&1");return;}
  if(stat("build.gradle",&st)==0||stat("build.gradle.kts",&st)==0){snprintf(cmd_out,sz,"./gradlew build 2>&1");return;}
  snprintf(cmd_out,sz,"make -j$(nproc) 2>&1");
}
static void *build_thread(void *arg){ (void)arg;
  char hdr[LOG_LINE_MAX]; snprintf(hdr,sizeof(hdr),"==> build: %s",g_build.cmd); log_ring_push(hdr);
  FILE *f=popen(g_build.cmd,"r");
  if(!f){log_ring_push("==> build: popen failed");atomic_store(&g_build.running,false);g_build.done=true;return NULL;}
  char buf[LOG_LINE_MAX];
  while(fgets(buf,sizeof(buf),f)){ size_t l=strlen(buf);
    while(l>0&&(buf[l-1]=='\n'||buf[l-1]=='\r'))buf[--l]='\0';
    log_ring_push(buf); build_try_parse_error(buf); }
  int status=pclose(f); g_build.exit_code=WIFEXITED(status)?WEXITSTATUS(status):-1;
  g_build.finished_ms=ov_now_ms(); atomic_store(&g_build.running,false); g_build.done=true;
  char footer[LOG_LINE_MAX];
  snprintf(footer,sizeof(footer),"==> build finished: exit %d (%.1fs)  errors: %d",
           g_build.exit_code,(double)(g_build.finished_ms-g_build.started_ms)/1000.0,g_build.err_count);
  log_ring_push(footer); return NULL;
}
static void run_build(void){
  if(!g_build.cmd[0]) build_autodetect(g_build.cmd,sizeof(g_build.cmd));
  if(atomic_load(&g_build.running)) return;
  pthread_mutex_lock(&g_build.err_lock); g_build.err_count=0;
  memset(g_build.errors,0,sizeof(g_build.errors)); pthread_mutex_unlock(&g_build.err_lock);
  g_build.done=false; g_build.exit_code=-1;
  g_build.started_ms=ov_now_ms(); g_build.finished_ms=0;
  atomic_store(&g_build.running,true);
  if(g_build.thread_valid){pthread_join(g_build.thread,NULL);g_build.thread_valid=false;}
  if(pthread_create(&g_build.thread,NULL,build_thread,NULL)==0) g_build.thread_valid=true;
  else{atomic_store(&g_build.running,false);log_ring_push("==> build: pthread_create failed");}
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Marvin — delegated to marvin_panel.c
 *
 * All state, detection, action tables, poll, key handling, and rendering
 * live in marvin_panel.c.  This section only retains the path helpers that
 * other parts of overlay.c still reference (overlay_set_cwd, etc.) and the
 * Markdown style helpers used by the old notes renderer (now unused but kept
 * for zero diff on surrounding sections).
 * ═══════════════════════════════════════════════════════════════════════════ */

static void marvin_state_path(char *out, size_t sz){
  const char *run=getenv("XDG_RUNTIME_DIR");
  if(run&&run[0]) snprintf(out,sz,"%s/marvin_state",run);
  else { const char *h=getenv("HOME"); snprintf(out,sz,"%s/.config/trixie/marvin_state",h?h:"/root"); }
}
static void marvin_console_path(char *out, size_t sz){
  const char *run=getenv("XDG_RUNTIME_DIR");
  if(run&&run[0]) snprintf(out,sz,"%s/marvin_console.log",run);
  else { const char *h=getenv("HOME"); snprintf(out,sz,"%s/.config/trixie/marvin_console.log",h?h:"/root"); }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  Search
 * ═══════════════════════════════════════════════════════════════════════════ */

#define SEARCH_RESULTS_MAX 256
#define SEARCH_QUERY_MAX   128
#define SEARCH_LINE_MAX    256
typedef struct { char file[256]; int line; char text[SEARCH_LINE_MAX]; } SearchResult;
typedef struct {
  char query[SEARCH_QUERY_MAX]; int query_len;
  SearchResult results[SEARCH_RESULTS_MAX]; int result_count;
  bool running, file_only; int64_t last_run_ms;
  pthread_t thread; bool thread_valid; pthread_mutex_t lock;
  SearchResult pending[SEARCH_RESULTS_MAX]; int pending_count; bool pending_ready;
} SearchState;
static SearchState g_search;
static bool find_in_path(const char *prog){
  const char *path_env=getenv("PATH"); if(!path_env) return false;
  char path_copy[4096]; strncpy(path_copy,path_env,sizeof(path_copy)-1);
  char *save=NULL, *dir=strtok_r(path_copy,":",&save);
  while(dir){ char full[1024]; snprintf(full,sizeof(full),"%s/%s",dir,prog);
    if(access(full,X_OK)==0) return true; dir=strtok_r(NULL,":",&save); }
  return false;
}
static void *search_thread(void *arg){ (void)arg;
  char query[SEARCH_QUERY_MAX]; bool file_only;
  pthread_mutex_lock(&g_search.lock);
  strncpy(query,g_search.query,sizeof(query)-1); file_only=g_search.file_only;
  g_search.pending_count=0; pthread_mutex_unlock(&g_search.lock);
  if(!query[0]){ pthread_mutex_lock(&g_search.lock); g_search.pending_count=0;
    g_search.pending_ready=true; g_search.running=false; pthread_mutex_unlock(&g_search.lock); return NULL; }
  bool has_rg=find_in_path("rg"); char cmd[512];
  if(has_rg){ if(file_only) snprintf(cmd,sizeof(cmd),"rg -l --color=never -i '%s' 2>/dev/null | head -256",query);
    else snprintf(cmd,sizeof(cmd),"rg -n --color=never -i --no-heading '%s' 2>/dev/null | head -256",query); }
  else { if(file_only) snprintf(cmd,sizeof(cmd),"grep -r -l -i '%s' . 2>/dev/null | head -256",query);
    else snprintf(cmd,sizeof(cmd),"grep -r -n -i '%s' . 2>/dev/null | head -256",query); }
  FILE *f=popen(cmd,"r"); int n=0;
  if(f){ char line[512];
    while(fgets(line,sizeof(line),f)&&n<SEARCH_RESULTS_MAX){
      size_t l=strlen(line);
      while(l>0&&(line[l-1]=='\n'||line[l-1]=='\r'))line[--l]='\0';
      SearchResult *sr=&g_search.pending[n];
      if(file_only){ strncpy(sr->file,line,sizeof(sr->file)-1); sr->line=0; sr->text[0]='\0'; }
      else{ char *c1=strchr(line,':'); if(!c1) continue; *c1='\0';
        strncpy(sr->file,line,sizeof(sr->file)-1);
        char *c2=strchr(c1+1,':');
        if(!c2){ sr->line=0; strncpy(sr->text,c1+1,sizeof(sr->text)-1); }
        else { *c2='\0'; sr->line=atoi(c1+1); strncpy(sr->text,c2+1,sizeof(sr->text)-1); } }
      n++; }
    pclose(f); }
  pthread_mutex_lock(&g_search.lock); g_search.pending_count=n;
  g_search.pending_ready=true; g_search.running=false; pthread_mutex_unlock(&g_search.lock);
  return NULL;
}
static void search_run(void){
  if(g_search.running) return; g_search.running=true;
  if(g_search.thread_valid){pthread_join(g_search.thread,NULL);g_search.thread_valid=false;}
  if(pthread_create(&g_search.thread,NULL,search_thread,NULL)==0) g_search.thread_valid=true;
  else g_search.running=false;
}
static void search_poll(void){
  if(!g_search.pending_ready) return; pthread_mutex_lock(&g_search.lock);
  memcpy(g_search.results,g_search.pending,sizeof(SearchResult)*(size_t)g_search.pending_count);
  g_search.result_count=g_search.pending_count; g_search.pending_ready=false;
  pthread_mutex_unlock(&g_search.lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  Deps
 * ═══════════════════════════════════════════════════════════════════════════ */

#define DEPS_MAX      128
#define DEPS_LINE_MAX 128
typedef enum { DEPS_UNKNOWN,DEPS_RUST,DEPS_GO,DEPS_MAVEN,DEPS_GRADLE } DepsLang;
typedef struct { char name[64]; char version[32]; char latest[32]; bool outdated; } DepEntry;
typedef struct {
  DepsLang lang; DepEntry entries[DEPS_MAX]; int count;
  bool checking,valid; int64_t fetched_ms; pthread_t thread; bool thread_valid;
} DepsState;
static DepsState g_deps;
static DepsLang deps_detect(void){
  struct stat st;
  if(stat("Cargo.toml",&st)==0) return DEPS_RUST;
  if(stat("go.mod",&st)==0)     return DEPS_GO;
  if(stat("pom.xml",&st)==0)    return DEPS_MAVEN;
  if(stat("build.gradle",&st)==0||stat("build.gradle.kts",&st)==0) return DEPS_GRADLE;
  return DEPS_UNKNOWN;
}
static void deps_parse_cargo(void){
  FILE *f=fopen("Cargo.toml","r"); if(!f) return;
  char line[256]; bool in_deps=false;
  while(fgets(line,sizeof(line),f)&&g_deps.count<DEPS_MAX){
    if(strncmp(line,"[dependencies]",14)==0||strncmp(line,"[dev-dependencies]",18)==0||
       strncmp(line,"[build-dependencies]",20)==0){in_deps=true;continue;}
    if(line[0]=='['){in_deps=false;continue;} if(!in_deps) continue;
    char *eq=strchr(line,'='); if(!eq) continue;
    char name[64]={0},ver[32]={0}; int nlen=(int)(eq-line);
    while(nlen>0&&line[nlen-1]==' ')nlen--;
    strncpy(name,line,(size_t)(nlen<63?nlen:63));
    char *val=eq+1; while(*val==' ')val++;
    if(*val=='"'){ val++; char *end=strchr(val,'"');
      if(end) strncpy(ver,val,(size_t)((end-val)<31?(end-val):31)); }
    else if(strstr(val,"version")){ char *vs=strstr(val,"\"");
      if(vs){ vs++; char *ve=strchr(vs,'"'); if(ve) strncpy(ver,vs,(size_t)((ve-vs)<31?(ve-vs):31)); } }
    else continue;
    if(!name[0]) continue;
    strncpy(g_deps.entries[g_deps.count].name,name,63);
    strncpy(g_deps.entries[g_deps.count].version,ver,31); g_deps.count++;
  }
  fclose(f);
}
static void deps_parse_go(void){
  char lines[DEPS_MAX][DEPS_LINE_MAX];
  int n=run_capture("go list -m all 2>/dev/null",lines,DEPS_MAX);
  for(int i=1;i<n&&g_deps.count<DEPS_MAX;i++){
    char *sp=strchr(lines[i],' '); if(!sp) continue; *sp='\0';
    strncpy(g_deps.entries[g_deps.count].name,lines[i],63);
    strncpy(g_deps.entries[g_deps.count].version,sp+1,31); g_deps.count++; }
}
static void deps_parse_maven(void){
  char lines[DEPS_MAX][DEPS_LINE_MAX];
  int n=run_capture("mvn dependency:list -q 2>/dev/null | grep '\\[INFO\\]' | grep ':' | head -128",lines,DEPS_MAX);
  for(int i=0;i<n&&g_deps.count<DEPS_MAX;i++){
    char *p=strstr(lines[i],"   "); if(!p) continue; while(*p==' ')p++;
    char parts[5][64]={{0}}; int pi=0; char *tok=strtok(p,":");
    while(tok&&pi<5){strncpy(parts[pi++],tok,63);tok=strtok(NULL,":");}
    if(pi<4) continue;
    strncpy(g_deps.entries[g_deps.count].name,parts[1],63);
    strncpy(g_deps.entries[g_deps.count].version,parts[3],31); g_deps.count++; }
}
static void *deps_check_thread(void *arg){ (void)arg;
  if(g_deps.lang==DEPS_RUST){
    char lines[DEPS_MAX][DEPS_LINE_MAX];
    int n=run_capture("cargo outdated 2>/dev/null | tail -n +3",lines,DEPS_MAX);
    for(int i=0;i<n;i++){
      char name[64]={0},cur[32]={0},latest[32]={0};
      if(sscanf(lines[i],"%63s %31s %*s %31s",name,cur,latest)<3) continue;
      if(strcmp(latest,"---")==0) continue;
      for(int j=0;j<g_deps.count;j++) if(strcmp(g_deps.entries[j].name,name)==0){
        strncpy(g_deps.entries[j].latest,latest,31);
        g_deps.entries[j].outdated=(strcmp(cur,latest)!=0); break; } } }
  else if(g_deps.lang==DEPS_GO){
    char lines[DEPS_MAX][DEPS_LINE_MAX];
    int n=run_capture("go list -u -m all 2>/dev/null",lines,DEPS_MAX);
    for(int i=1;i<n;i++){
      char name[64]={0},ver[32]={0},latest[32]={0};
      sscanf(lines[i],"%63s %31s %31s",name,ver,latest);
      if(latest[0]=='['){ size_t ll=strlen(latest);
        if(ll>2){memmove(latest,latest+1,ll-2);latest[ll-2]='\0';}
        for(int j=0;j<g_deps.count;j++) if(strcmp(g_deps.entries[j].name,name)==0){
          strncpy(g_deps.entries[j].latest,latest,31); g_deps.entries[j].outdated=true; break; } } } }
  g_deps.checking=false; return NULL;
}
static void deps_load(void){
  int64_t now=ov_now_ms(); if(g_deps.valid&&now-g_deps.fetched_ms<30000) return;
  g_deps.count=0; g_deps.lang=deps_detect();
  switch(g_deps.lang){ case DEPS_RUST:deps_parse_cargo();break; case DEPS_GO:deps_parse_go();break;
    case DEPS_MAVEN:deps_parse_maven();break; default:break; }
  g_deps.valid=true; g_deps.fetched_ms=now;
}
static void deps_check_outdated(void){
  if(g_deps.checking) return; g_deps.checking=true;
  if(g_deps.thread_valid){pthread_join(g_deps.thread,NULL);g_deps.thread_valid=false;}
  if(pthread_create(&g_deps.thread,NULL,deps_check_thread,NULL)==0) g_deps.thread_valid=true;
  else g_deps.checking=false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  Log filter
 * ═══════════════════════════════════════════════════════════════════════════ */

#define LOG_FILTER_MAX 64
static char g_log_filter[LOG_FILTER_MAX];
static int  g_log_filter_len=0;
static bool g_log_filter_mode=false;

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  FreeType font
 * ═══════════════════════════════════════════════════════════════════════════ */

FT_Library g_ov_ft=NULL;
FT_Face    g_ov_face=NULL;
FT_Face    g_ov_icon_face=NULL;
int        g_ov_asc=0;
int        g_ov_th=0;

/* ── Overlay glyph bitmap cache ─────────────────────────────────────────────
 * ov_draw_text is called for every text row on every dirty overlay paint.
 * Caching the rendered bitmaps reduces FT_Load_Glyph(FT_LOAD_RENDER) from
 * O(chars × frames) to O(unique chars) — essentially a one-time cost.
 *
 * Key: codepoint (uint32_t).  Open-addressing, power-of-2 slots.
 * We store which FT_Face rendered it so we can detect face changes.
 * Cleared on ov_font_init() (font or size change).
 */
#define OV_GCACHE_SIZE 1024
#define OV_GCACHE_MASK (OV_GCACHE_SIZE - 1)

typedef struct {
  uint32_t   cp;         /* codepoint — 0 means empty */
  FT_Face    face;       /* which face rendered this (for invalidation) */
  int        bearing_x;
  int        bearing_y;
  int        advance_x;  /* pixels */
  int        bm_w, bm_h, bm_pitch;
  uint8_t   *bm;         /* heap-allocated copy of the bitmap */
} OvGlyphCache;

static OvGlyphCache g_ov_gcache[OV_GCACHE_SIZE];

static void ov_gcache_clear(void) {
  for(int i = 0; i < OV_GCACHE_SIZE; i++) {
    free(g_ov_gcache[i].bm);
    g_ov_gcache[i] = (OvGlyphCache){ 0 };
  }
}

/* Looks up cp in the cache, rendering it if necessary.
 * Returns NULL if the glyph cannot be loaded or has no bitmap. */
static const OvGlyphCache *ov_gcache_get(uint32_t cp) {
  if(!cp) return NULL;
  uint32_t slot = cp & OV_GCACHE_MASK;
  for(int i = 0; i < OV_GCACHE_SIZE; i++) {
    uint32_t s = (slot + (uint32_t)i) & OV_GCACHE_MASK;
    OvGlyphCache *cg = &g_ov_gcache[s];
    if(cg->cp == cp) return cg;
    if(cg->cp == 0) {
      /* Empty slot — load and fill. */
      bool is_icon = (cp >= 0xE000 && cp <= 0xF8FF) ||
                     (cp >= 0x1F300) || (cp >= 0xF0000);
      FT_Face face = NULL;
      if(!is_icon && g_ov_face) {
        FT_UInt idx = FT_Get_Char_Index(g_ov_face, cp);
        if(idx && FT_Load_Glyph(g_ov_face, idx, FT_LOAD_RENDER) == 0)
          face = g_ov_face;
      }
      if(!face && g_ov_icon_face) {
        FT_UInt idx = FT_Get_Char_Index(g_ov_icon_face, cp);
        if(idx && FT_Load_Glyph(g_ov_icon_face, idx, FT_LOAD_RENDER) == 0)
          face = g_ov_icon_face;
      }
      if(!face && g_ov_face) {
        if(FT_Load_Char(g_ov_face, cp, FT_LOAD_RENDER) == 0)
          face = g_ov_face;
      }
      if(!face) return NULL;
      FT_GlyphSlot sl = face->glyph;
      cg->cp        = cp;
      cg->face      = face;
      cg->bearing_x = sl->bitmap_left;
      cg->bearing_y = sl->bitmap_top;
      cg->advance_x = (int)(sl->advance.x >> 6);
      cg->bm_w      = (int)sl->bitmap.width;
      cg->bm_h      = (int)sl->bitmap.rows;
      cg->bm_pitch  = sl->bitmap.pitch;
      int sz = cg->bm_h * cg->bm_pitch;
      cg->bm = sz > 0 ? malloc(sz) : NULL;
      if(cg->bm) memcpy(cg->bm, sl->bitmap.buffer, sz);
      return cg;
    }
  }
  return NULL; /* cache full — glyph missed, skip drawing */
}

static uint32_t utf8_next(const char **pp){
  const unsigned char *p=(const unsigned char *)*pp;
  uint32_t cp; int extra;
  if(*p<0x80){cp=*p++;extra=0;} else if((*p&0xe0)==0xc0){cp=*p++&0x1f;extra=1;}
  else if((*p&0xf0)==0xe0){cp=*p++&0x0f;extra=2;} else if((*p&0xf8)==0xf0){cp=*p++&0x07;extra=3;}
  else{cp=*p++;extra=0;}
  while(extra-->0&&(*p&0xc0)==0x80) cp=(cp<<6)|(*p++&0x3f);
  *pp=(const char *)p; return cp;
}
static const char *ICON_FONT_CANDIDATES[]={
  "/usr/share/fonts/TTF/NerdFontsSymbolsOnly.ttf",
  "/usr/share/fonts/TTF/Symbols-2048-em Nerd Font Complete.ttf",
  "/usr/share/fonts/nerd-fonts-symbols/NerdFontsSymbolsOnly.ttf",
  "/usr/share/fonts/truetype/nerd-fonts/NerdFontsSymbolsOnly.ttf",
  "/usr/share/fonts/NerdFontsSymbolsOnly.ttf",
  NULL,NULL,
};
static void ov_load_icon_face(float size_pt){
  static char home_path[512]; const char *home=getenv("HOME");
  if(home){ snprintf(home_path,sizeof(home_path),"%s/.local/share/fonts/NerdFontsSymbolsOnly.ttf",home);
    ICON_FONT_CANDIDATES[5]=home_path; }
  if(g_ov_icon_face){FT_Done_Face(g_ov_icon_face);g_ov_icon_face=NULL;}
  for(int i=0;ICON_FONT_CANDIDATES[i];i++)
    if(FT_New_Face(g_ov_ft,ICON_FONT_CANDIDATES[i],0,&g_ov_icon_face)==0){
      FT_Set_Char_Size(g_ov_icon_face,0,(FT_F26Dot6)(size_pt*64.f),96,96);
      wlr_log(WLR_INFO,"overlay: icon font → %s",ICON_FONT_CANDIDATES[i]); return; }
  wlr_log(WLR_DEBUG,"overlay: no dedicated icon font found");
}
static void ov_font_init(const char *path,float size_pt){
  if(!g_ov_ft) FT_Init_FreeType(&g_ov_ft);
  if(g_ov_face){FT_Done_Face(g_ov_face);g_ov_face=NULL;}
  if(!path||!path[0]) return;
  if(FT_New_Face(g_ov_ft,path,0,&g_ov_face)){g_ov_face=NULL;return;}
  if(size_pt<=0.f) size_pt=13.f;
  FT_Set_Char_Size(g_ov_face,0,(FT_F26Dot6)(size_pt*64.f),96,96);
  g_ov_asc=(int)ceilf((float)g_ov_face->size->metrics.ascender/64.f);
  int desc=(int)floorf((float)g_ov_face->size->metrics.descender/64.f);
  g_ov_th=g_ov_asc-desc;
  ov_gcache_clear(); /* new face/size — all cached bitmaps are stale */
  ov_load_icon_face(size_pt);
}
static FT_Face ov_load_glyph(uint32_t cp,int load_flags){
  bool is_icon=(cp>=0xE000&&cp<=0xF8FF)||(cp>=0x1F300)||(cp>=0xF0000);
  if(!is_icon&&g_ov_face){ FT_UInt idx=FT_Get_Char_Index(g_ov_face,cp);
    if(idx&&FT_Load_Glyph(g_ov_face,idx,load_flags)==0) return g_ov_face; }
  if(g_ov_icon_face){ FT_UInt idx=FT_Get_Char_Index(g_ov_icon_face,cp);
    if(idx&&FT_Load_Glyph(g_ov_icon_face,idx,load_flags)==0) return g_ov_icon_face; }
  if(g_ov_face){ if(FT_Load_Char(g_ov_face,cp,load_flags)==0) return g_ov_face; }
  return NULL;
}
int ov_measure(const char *text){
  if(!g_ov_face||!text) return 0;
  int w=0; const char *p=text;
  while(*p){
    uint32_t cp=utf8_next(&p);
    const OvGlyphCache *cg=ov_gcache_get(cp);
    if(cg) w+=cg->advance_x;
  }
  return w;
}
void ov_draw_text(uint32_t *px,int stride,int x,int y,int clip_w,int clip_h,
                  const char *text,uint8_t r,uint8_t g,uint8_t b,uint8_t a){
  if(!g_ov_face||!text) return;
  int pen=x; const char *p=text;
  while(*p){
    uint32_t cp=utf8_next(&p);
    const OvGlyphCache *cg=ov_gcache_get(cp);
    if(!cg||!cg->bm){ if(cg) pen+=cg->advance_x; continue; }
    int gx=pen+cg->bearing_x, gy=y-cg->bearing_y;
    for(int row=0;row<cg->bm_h;row++){ int py=gy+row;
      if(py<0||py>=clip_h) continue;
      for(int col=0;col<cg->bm_w;col++){ int px_x=gx+col;
        if(px_x<0||px_x>=clip_w) continue;
        uint8_t ga=cg->bm[row*cg->bm_pitch+col]; if(!ga) continue;
        uint8_t ba=(uint8_t)((uint32_t)ga*a/255); uint32_t *d=&px[py*stride+px_x];
        uint32_t inv=255-ba;
        uint8_t or_=(uint8_t)((r*ba+((*d>>16)&0xff)*inv)/255);
        uint8_t og =(uint8_t)((g*ba+((*d>>8 )&0xff)*inv)/255);
        uint8_t ob =(uint8_t)((b*ba+(*d      &0xff)*inv)/255);
        *d=(0xffu<<24)|((uint32_t)or_<<16)|((uint32_t)og<<8)|ob; } }
    pen+=cg->advance_x;
  }
}
void ov_fill_rect(uint32_t *px,int stride,int x,int y,int w,int h,
                  uint8_t r,uint8_t g,uint8_t b,uint8_t a,int cw,int ch){
  /* Clamp to canvas bounds once — avoid per-pixel branch inside hot loops. */
  int x0 = x < 0 ? 0 : x;
  int y0 = y < 0 ? 0 : y;
  int x1 = (x + w) > cw ? cw : (x + w);
  int y1 = (y + h) > ch ? ch : (y + h);
  if(x0 >= x1 || y0 >= y1) return;

  uint32_t c=((uint32_t)a<<24)|((uint32_t)r<<16)|((uint32_t)g<<8)|b;
  if(a==255){
    /* Opaque: fill each row with a single loop — no blending needed. */
    for(int row=y0;row<y1;row++){
      uint32_t *dst=&px[row*stride+x0];
      int       len=x1-x0;
      for(int i=0;i<len;i++) dst[i]=c;
    }
  } else {
    uint32_t inv=255-a;
    for(int row=y0;row<y1;row++){
      uint32_t *dst=&px[row*stride+x0];
      int       len=x1-x0;
      for(int i=0;i<len;i++){
        uint32_t *d=&dst[i];
        uint8_t or_=(uint8_t)((r*a+((*d>>16)&0xff)*inv)/255);
        uint8_t og =(uint8_t)((g*a+((*d>>8 )&0xff)*inv)/255);
        uint8_t ob =(uint8_t)((b*a+(*d      &0xff)*inv)/255);
        *d=(0xffu<<24)|((uint32_t)or_<<16)|((uint32_t)og<<8)|ob;
      }
    }
  }
}

void ov_fill_border(uint32_t *px,int stride,int x,int y,int w,int h,
                    uint8_t r,uint8_t g,uint8_t b,uint8_t a,int cw,int ch){
  ov_fill_rect(px,stride,x,    y,    w,1,r,g,b,a,cw,ch);
  ov_fill_rect(px,stride,x,    y+h-1,w,1,r,g,b,a,cw,ch);
  ov_fill_rect(px,stride,x,    y,    1,h,r,g,b,a,cw,ch);
  ov_fill_rect(px,stride,x+w-1,y,    1,h,r,g,b,a,cw,ch);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §9  wlr_buffer wrapper
 * ═══════════════════════════════════════════════════════════════════════════ */

struct OvRawBuf { struct wlr_buffer base; uint32_t *data; int stride; };
static void ovb_destroy(struct wlr_buffer *b){
  struct OvRawBuf *rb=wl_container_of(b,rb,base); free(rb->data); free(rb); }
static bool ovb_begin(struct wlr_buffer *b,uint32_t flags,void **data,uint32_t *fmt,size_t *stride){
  (void)flags; struct OvRawBuf *rb=wl_container_of(b,rb,base);
  *data=rb->data; *fmt=DRM_FORMAT_ARGB8888; *stride=(size_t)rb->stride; return true; }
static void ovb_end(struct wlr_buffer *b){(void)b;}
static const struct wlr_buffer_impl ovb_impl={
  .destroy=ovb_destroy,.begin_data_ptr_access=ovb_begin,.end_data_ptr_access=ovb_end };

/* ── Persistent pixel scratch buffer + persistent header ──────────────────────
 * Both the pixel slab and the OvRawBuf header are kept alive for the lifetime
 * of the overlay.  ovb_create re-inits the wlr_buffer (resetting its refcount)
 * and zeroes the pixel data — no allocator calls on the hot paint path.       */
static uint32_t    *s_ov_pixels   = NULL;
static int          s_ov_px_w     = 0;
static int          s_ov_px_h     = 0;
static struct OvRawBuf *s_ov_rb   = NULL;  /* persistent header */

/* Destroy for the persistent header — frees the struct but NOT the pixel slab. */
static void ovb_destroy_shared(struct wlr_buffer *b) {
  struct OvRawBuf *rb = wl_container_of(b, rb, base);
  /* rb->data is the persistent slab — leave it alone. */
  free(rb);
}
static const struct wlr_buffer_impl ovb_impl_shared = {
  .destroy              = ovb_destroy_shared,
  .begin_data_ptr_access = ovb_begin,
  .end_data_ptr_access   = ovb_end,
};

static struct OvRawBuf *ovb_create(int w,int h){
  /* (Re)allocate the persistent pixel buffer only when dimensions change. */
  if(w != s_ov_px_w || h != s_ov_px_h || !s_ov_pixels) {
    free(s_ov_pixels);
    s_ov_pixels = malloc((size_t)(w * h) * 4);
    s_ov_px_w   = w;
    s_ov_px_h   = h;
    /* Pixel slab changed — the header's data pointer must be updated too. */
    free(s_ov_rb);
    s_ov_rb = NULL;
  }
  /* Allocate the header once; re-use it every subsequent frame. */
  if(!s_ov_rb) {
    s_ov_rb         = calloc(1, sizeof(*s_ov_rb));
    s_ov_rb->data   = s_ov_pixels;
    s_ov_rb->stride = w * 4;
  }
  /* Zero the pixel data without touching the allocator. */
  memset(s_ov_pixels, 0, (size_t)(w * h) * 4);
  /* Re-init the wlr_buffer header so its refcount is 1 for this frame. */
  wlr_buffer_init(&s_ov_rb->base, &ovb_impl_shared, w, h);
  return s_ov_rb;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §10  Layout constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define ROW_H   (g_ov_th + 8)
#define PAD     12
#define BDR     2
#define HDR_H   (ROW_H + 4)

/* ═══════════════════════════════════════════════════════════════════════════
 * §11  Pixel-perfect TUI primitives
 * ═══════════════════════════════════════════════════════════════════════════ */

void tui_box(uint32_t *px, int stride,
                    int bx, int by, int bw, int bh,
                    const char *title, const char *hint,
                    Color ac, Color bg,
                    int cw, int ch) {
  ov_fill_rect(px,stride, bx,by, bw,bh, bg.r,bg.g,bg.b,0xff, cw,ch);
  uint8_t hr=(uint8_t)(bg.r>0x10?bg.r-0x10:0);
  uint8_t hg=(uint8_t)(bg.g>0x10?bg.g-0x10:0);
  uint8_t hb=(uint8_t)(bg.b>0x10?bg.b-0x10:0);
  ov_fill_rect(px,stride, bx+BDR, by+BDR, bw-BDR*2, HDR_H, hr,hg,hb,0xff, cw,ch);
  ov_fill_rect(px,stride, bx,by,   bw,BDR, ac.r,ac.g,ac.b,0xff, cw,ch);
  ov_fill_rect(px,stride, bx,by+bh-BDR, bw,BDR, ac.r,ac.g,ac.b,0xff, cw,ch);
  ov_fill_rect(px,stride, bx,by,   BDR,bh, ac.r,ac.g,ac.b,0xff, cw,ch);
  ov_fill_rect(px,stride, bx+bw-BDR,by, BDR,bh, ac.r,ac.g,ac.b,0xff, cw,ch);
  ov_fill_rect(px,stride, bx+BDR, by+BDR+HDR_H, bw-BDR*2, 1, ac.r,ac.g,ac.b,0x50, cw,ch);
  int ty = by + BDR + (HDR_H - g_ov_th)/2 + g_ov_asc;
  if(title && title[0])
    ov_draw_text(px,stride, bx+BDR+PAD, ty, cw,ch, title, ac.r,ac.g,ac.b,0xff);
  if(hint && hint[0]) {
    int hw = ov_measure(hint);
    ov_draw_text(px,stride, bx+bw-BDR-PAD-hw, ty, cw,ch, hint, 0x58,0x5b,0x70,0xff);
  }
}

void tui_hsep(uint32_t *px, int stride,
                     int bx, int by_sep, int bw,
                     const char *label,
                     Color ac, Color bg,
                     int cw, int ch) {
  int line_y = by_sep + ROW_H/2;
  ov_fill_rect(px,stride, bx+BDR, line_y, bw-BDR*2, 1, ac.r,ac.g,ac.b,0x40, cw,ch);
  if(label && label[0]) {
    int lw  = ov_measure(label);
    int lx  = bx + BDR + PAD + 8;
    int gap = 4;
    ov_fill_rect(px,stride, lx-gap, line_y, lw+gap*2, 1, bg.r,bg.g,bg.b,0xff, cw,ch);
    int text_y = by_sep + (ROW_H-g_ov_th)/2 + g_ov_asc;
    ov_draw_text(px,stride, lx, text_y, cw,ch, label, 0x58,0x5b,0x70,0xff);
  }
}

void draw_cursor_line(uint32_t *px, int stride,
                      int px0, int ry, int pw,
                      Color ac, Color bg,
                      int cw, int ch) {
  uint8_t sr=(uint8_t)((int)bg.r+0x16<0xff?bg.r+0x16:0xff);
  uint8_t sg=(uint8_t)((int)bg.g+0x16<0xff?bg.g+0x16:0xff);
  uint8_t sb=(uint8_t)((int)bg.b+0x16<0xff?bg.b+0x16:0xff);
  ov_fill_rect(px,stride, px0+BDR, ry, pw-BDR*2, ROW_H, sr,sg,sb,0xff, cw,ch);
  ov_fill_rect(px,stride, px0+BDR, ry, 2, ROW_H, ac.r,ac.g,ac.b,0xff, cw,ch);
}

void tui_scrollbar(uint32_t *px, int stride,
                          int bx, int by, int bw, int bh,
                          int scroll, int total, int visible,
                          Color ac, int cw, int ch) {
  if(total <= visible) return;
  int track_top = by + BDR + HDR_H + 2;
  int track_h   = bh - BDR*2 - HDR_H - 4;
  if(track_h <= 0) return;
  int thumb_h = (visible * track_h) / total;
  if(thumb_h < 6) thumb_h = 6;
  int thumb_y = track_top + (scroll * (track_h-thumb_h)) / (total-visible);
  int sx = bx + bw - BDR - 3;
  ov_fill_rect(px,stride, sx, track_top, 2, track_h, ac.r,ac.g,ac.b,0x18, cw,ch);
  ov_fill_rect(px,stride, sx, thumb_y,   2, thumb_h,  ac.r,ac.g,ac.b,0x90, cw,ch);
}

static int tui_input_box(uint32_t *px, int stride,
                         int x, int y, int w,
                         const char *value, const char *placeholder,
                         bool active,
                         Color ac, Color bg,
                         int cw, int ch) {
  int box_h = ROW_H + 6;
  uint8_t ib_r=(uint8_t)(bg.r>0x0c?bg.r-0x0c:0);
  uint8_t ib_g=(uint8_t)(bg.g>0x0c?bg.g-0x0c:0);
  uint8_t ib_b=(uint8_t)(bg.b>0x0c?bg.b-0x0c:0);
  ov_fill_rect(px,stride, x,y, w,box_h, ib_r,ib_g,ib_b,0xff, cw,ch);
  uint8_t bdr_a = active ? 0xff : 0x40;
  ov_fill_rect(px,stride, x,    y,    w,1,  ac.r,ac.g,ac.b,bdr_a,cw,ch);
  ov_fill_rect(px,stride, x,    y+box_h-1,w,1,  ac.r,ac.g,ac.b,bdr_a,cw,ch);
  ov_fill_rect(px,stride, x,    y,    1,box_h,  ac.r,ac.g,ac.b,bdr_a,cw,ch);
  ov_fill_rect(px,stride, x+w-1,y,    1,box_h,  ac.r,ac.g,ac.b,bdr_a,cw,ch);
  if(active)
    ov_fill_rect(px,stride, x+1, y+box_h-2, w-2, 2, ac.r,ac.g,ac.b,0xff, cw,ch);
  int ty = y + (box_h-g_ov_th)/2 + g_ov_asc;
  if(value && value[0]) {
    ov_draw_text(px,stride, x+PAD/2, ty, cw,ch, value, 0xcd,0xd6,0xf4,0xff);
    if(active) {
      int64_t ms = ov_now_ms();
      if((ms/530)%2==0) {
        int cx2 = x + PAD/2 + ov_measure(value);
        ov_fill_rect(px,stride, cx2, y+3, 2, box_h-6, ac.r,ac.g,ac.b,0xff, cw,ch);
      }
    }
  } else if(placeholder) {
    ov_draw_text(px,stride, x+PAD/2, ty, cw,ch, placeholder, 0x45,0x47,0x5a,0xff);
  }
  return y + box_h;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §12  Panel / overlay structs
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
  PANEL_WORKSPACES=0,PANEL_COMMANDS,PANEL_PROCESSES,PANEL_LOG,
  PANEL_GIT,PANEL_BUILD,PANEL_MARVIN,PANEL_SEARCH,PANEL_RUN,
  PANEL_DEPS,PANEL_FILES,PANEL_NVIM,PANEL_LSP,PANEL_COUNT,
} PanelId;

typedef struct { const char *label; const char *ipc_cmd; } OvCommand;
static const OvCommand g_commands[]={
  {"Close focused window","close"},{"Toggle fullscreen","fullscreen"},{"Toggle float","float"},
  {"Next layout","layout next"},{"Previous layout","layout prev"},{"BSP layout","set_layout bsp"},
  {"Columns layout","set_layout columns"},{"Monocle layout","set_layout monocle"},
  {"Grow main pane","grow_main"},{"Shrink main pane","shrink_main"},{"Swap with master","swap_main"},
  {"Swap forward","swap forward"},{"Swap backward","swap back"},{"Next workspace","next_workspace"},
  {"Previous workspace","prev_workspace"},{"Move pane to workspace 1","move_to_workspace 1"},
  {"Move pane to workspace 2","move_to_workspace 2"},{"Move pane to workspace 3","move_to_workspace 3"},
  {"Reload config","reload"},{"DPMS off","dpms off"},{"DPMS on","dpms on"},{"Quit compositor","quit"},
};
#define G_COMMAND_COUNT (int)(sizeof(g_commands)/sizeof(g_commands[0]))

#define OV_FILTER_MAX 64
struct TrixieOverlay {
  struct wlr_scene_buffer *scene_buf;
  bool  dirty;
  int w,h; bool visible; PanelId panel;
  int cursor,scroll;
  char filter[OV_FILTER_MAX]; int filter_len; bool filter_mode;
  char last_font[256]; float last_size;
  int matches[G_COMMAND_COUNT]; int match_count;
  char build_cmd[BUILD_CMD_MAX]; int build_cmd_editing;
  int build_err_cursor; bool build_show_errors;
  bool marvin_loaded; bool search_active;
  char fb_cwd[1024]; char fb_filter[128]; int fb_filter_len; bool fb_filter_mode;
  int ws_cursor; int pending_ws_switch;
  int nvim_cursor; int lsp_cursor;
  const Config *cfg;
};

/* ── Accessor shims — marvin_panel.c calls back into TrixieOverlay fields ──
 * Placed here because TrixieOverlay is fully defined above.
 * marvin_panel.c declares them extern.                                     */
int *overlay_scroll_ptr(TrixieOverlay *o)  { return &o->scroll; }
char *overlay_fb_cwd_ptr(TrixieOverlay *o) { return o->fb_cwd;  }

/* ═══════════════════════════════════════════════════════════════════════════
 * §13  Dev helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *overlay_editor(const OverlayCfg *c){
  if(c&&c->editor[0]) return c->editor;
  const char *e=getenv("EDITOR"); return (e&&e[0])?e:"nvim";
}
static const char *overlay_terminal(const OverlayCfg *c){
  if(c&&c->terminal[0]) return c->terminal;
  const char *e=getenv("TERM_PROGRAM"); if(e&&e[0]) return e;
  static const char *cands[]={"/usr/bin/kitty","/usr/bin/foot","/usr/bin/alacritty",
                               "/usr/bin/wezterm","/usr/bin/xterm",NULL};
  for(int i=0;cands[i];i++) if(access(cands[i],X_OK)==0){
    const char *s=strrchr(cands[i],'/'); return s?s+1:cands[i]; }
  return "xterm";
}
void overlay_open_file(const char *path,int line,const OverlayCfg *ov_cfg){
  if(ov_cfg&&ov_cfg->lsp_diagnostics&&nvim_is_connected()){nvim_open_file(path,line);return;}
  const char *editor=overlay_editor(ov_cfg), *term=overlay_terminal(ov_cfg);
  char cmd[1280];
  if(line>0) snprintf(cmd,sizeof(cmd),"%s -e %s +%d \"%s\"",term,editor,line,path);
  else       snprintf(cmd,sizeof(cmd),"%s -e %s \"%s\"",term,editor,path);
  pid_t pid=fork();
  if(pid==0){ setsid(); int maxfd=(int)sysconf(_SC_OPEN_MAX);
    for(int fd=3;fd<maxfd;fd++) close(fd); execl("/bin/sh","sh","-c",cmd,NULL); _exit(1); }
  if(pid<0) log_ring_push("==> overlay: fork() failed opening file");
}
static int panel_name_to_id(const char *name){
  if(!name||!name[0]) return PANEL_RUN;
  if(!strcasecmp(name,"workspaces")||!strcasecmp(name,"ws"))   return PANEL_WORKSPACES;
  if(!strcasecmp(name,"commands")  ||!strcasecmp(name,"cmd"))  return PANEL_COMMANDS;
  if(!strcasecmp(name,"processes") ||!strcasecmp(name,"proc")) return PANEL_PROCESSES;
  if(!strcasecmp(name,"log")       ||!strcasecmp(name,"logs")) return PANEL_LOG;
  if(!strcasecmp(name,"git"))                                   return PANEL_GIT;
  if(!strcasecmp(name,"build"))                                 return PANEL_BUILD;
  if(!strcasecmp(name,"marvin")||!strcasecmp(name,"notes")) return PANEL_MARVIN;
  if(!strcasecmp(name,"search")    ||!strcasecmp(name,"find")) return PANEL_SEARCH;
  if(!strcasecmp(name,"run"))                                   return PANEL_RUN;
  if(!strcasecmp(name,"deps")      ||!strcasecmp(name,"dep"))  return PANEL_DEPS;
  if(!strcasecmp(name,"files"))                                 return PANEL_FILES;
  if(!strcasecmp(name,"nvim")      ||!strcasecmp(name,"neovim")) return PANEL_NVIM;
  if(!strcasecmp(name,"lsp")       ||!strcasecmp(name,"diag")) return PANEL_LSP;
  return PANEL_RUN;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §14  Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

TrixieOverlay *overlay_create(struct wlr_scene_tree *layer,int w,int h,const Config *cfg){
  TrixieOverlay *o=calloc(1,sizeof(*o));
  o->w=w;o->h=h;o->cfg=cfg;o->panel=PANEL_WORKSPACES;o->pending_ws_switch=-1;
  o->scene_buf=wlr_scene_buffer_create(layer,NULL);
  wlr_scene_node_set_enabled(&o->scene_buf->node,false);
  ov_font_init(cfg->font_path,cfg->bar.font_size>0.f?cfg->bar.font_size:cfg->font_size);
  strncpy(o->last_font,cfg->font_path,sizeof(o->last_font)-1);
  o->last_size=cfg->bar.font_size>0.f?cfg->bar.font_size:cfg->font_size;
  static bool log_installed=false;
  if(!log_installed){wlr_log_init(WLR_INFO,overlay_log_handler);log_installed=true;}
  pthread_mutex_init(&g_build.err_lock,NULL); atomic_init(&g_build.running,false);
  pthread_mutex_init(&g_search.lock,NULL);
  build_autodetect(g_build.cmd,sizeof(g_build.cmd));
  strncpy(o->build_cmd,g_build.cmd,sizeof(o->build_cmd)-1);
  if(cfg->overlay.project_root[0]){
    if(chdir(cfg->overlay.project_root)==0){
      wlr_log(WLR_INFO,"overlay: project_root → %s",cfg->overlay.project_root);
      strncpy(o->fb_cwd,cfg->overlay.project_root,sizeof(o->fb_cwd)-1);
      strncpy(g_git_cwd,cfg->overlay.project_root,sizeof(g_git_cwd)-1);
    } else wlr_log(WLR_ERROR,"overlay: chdir('%s') failed",cfg->overlay.project_root);
  }
  run_configs_init(cfg->overlay); deps_load();
  if(cfg->overlay.default_panel[0])
    o->panel=(PanelId)panel_name_to_id(cfg->overlay.default_panel);
  return o;
}
int overlay_build_err_count(void){return g_build.err_count;}
void overlay_build_err_get(int idx,char *file,int fsz,int *line,int *col,char *msg,int msz,bool *is_warning){
  if(idx<0||idx>=g_build.err_count){
    if(file)file[0]='\0';if(line)*line=0;if(col)*col=0;if(msg)msg[0]='\0';if(is_warning)*is_warning=false;return;}
  BuildError *e=&g_build.errors[idx];
  if(file){strncpy(file,e->file,fsz-1);file[fsz-1]='\0';}
  if(line)*line=e->line; if(col)*col=e->col;
  if(msg){strncpy(msg,e->msg,msz-1);msg[msz-1]='\0';} if(is_warning)*is_warning=e->is_warning;
}
void overlay_destroy(TrixieOverlay *o){
  if(!o) return;
  if(g_build.thread_valid){pthread_join(g_build.thread,NULL);g_build.thread_valid=false;}
  pthread_mutex_destroy(&g_build.err_lock);
  if(g_search.thread_valid){pthread_join(g_search.thread,NULL);g_search.thread_valid=false;}
  pthread_mutex_destroy(&g_search.lock);
  if(g_deps.thread_valid){pthread_join(g_deps.thread,NULL);g_deps.thread_valid=false;}
  run_configs_destroy(); nvim_disconnect();
  wlr_scene_node_destroy(&o->scene_buf->node);
  if(g_ov_face){FT_Done_Face(g_ov_face);g_ov_face=NULL;}
  if(g_ov_icon_face){FT_Done_Face(g_ov_icon_face);g_ov_icon_face=NULL;}
  if(g_ov_ft){FT_Done_FreeType(g_ov_ft);g_ov_ft=NULL;}
  free(o);
}
void overlay_toggle(TrixieOverlay *o){
  if(!o) return; o->visible=!o->visible;
  wlr_scene_node_set_enabled(&o->scene_buf->node,o->visible);
  if(o->visible){ marvin_poll(o->fb_cwd); o->marvin_loaded=true; o->dirty=true; }
}
bool overlay_visible(TrixieOverlay *o){return o&&o->visible;}
void overlay_resize(TrixieOverlay *o,int w,int h){if(o){o->w=w;o->h=h;}}
void overlay_set_cwd(TrixieOverlay *o,const char *path){
  if(!path||!path[0]) return;
  char expanded[1024]={0};
  if(!strncmp(path,"~/",2)){const char *home=getenv("HOME");if(!home)home="/root";
    snprintf(expanded,sizeof(expanded),"%s/%s",home,path+2);}
  else strncpy(expanded,path,sizeof(expanded)-1);
  if(o) strncpy(o->fb_cwd,expanded,sizeof(o->fb_cwd)-1);
  strncpy(g_git_cwd,expanded,sizeof(g_git_cwd)-1);
  g_git.valid=false;
  if(o&&o->cfg) run_configs_init(o->cfg->overlay);
  if(o) o->dirty=true; marvin_poll(expanded);
  log_ring_push("  cwd changed"); wlr_log(WLR_DEBUG,"overlay: cwd → %s",expanded);
}
void overlay_notify(TrixieOverlay *o,const char *title,const char *msg){
  if(o) o->dirty=true; char line[LOG_LINE_MAX];
  if(title&&title[0]&&msg&&msg[0]) snprintf(line,sizeof(line),"  %s: %s",title,msg);
  else if(msg&&msg[0])   snprintf(line,sizeof(line),"  %s",msg);
  else if(title&&title[0]) snprintf(line,sizeof(line),"  %s",title);
  else return; log_ring_push(line);
}
void overlay_show_panel(TrixieOverlay *o,const char *name){
  if(!o||!name||!name[0]) return;
  o->panel=(PanelId)panel_name_to_id(name);
  if(!o->visible){o->visible=true;wlr_scene_node_set_enabled(&o->scene_buf->node,true);
    { marvin_poll(o->fb_cwd); o->marvin_loaded=true; o->dirty=true; }}
}
void overlay_git_invalidate(TrixieOverlay *o){(void)o;g_git.valid=false;}

/* ═══════════════════════════════════════════════════════════════════════════
 * §15  Filter / match
 * ═══════════════════════════════════════════════════════════════════════════ */

static void rebuild_matches(TrixieOverlay *o){
  o->match_count=0;
  for(int i=0;i<G_COMMAND_COUNT;i++){
    if(!o->filter[0]){o->matches[o->match_count++]=i;continue;}
    const char *h=g_commands[i].label,*n=o->filter; bool found=false;
    for(int hi=0;h[hi]&&!found;hi++){
      bool match=true;
      for(int ni=0;n[ni];ni++){
        if(!h[hi+ni]){match=false;break;}
        char hc=h[hi+ni],nc=n[ni];
        if(hc>='A'&&hc<='Z')hc+=32; if(nc>='A'&&nc<='Z')nc+=32;
        if(hc!=nc){match=false;break;}
      }
      if(match)found=true;
    }
    if(found)o->matches[o->match_count++]=i;
  }
  if(o->cursor>=o->match_count) o->cursor=o->match_count>0?o->match_count-1:0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §16  Key handling
 * ═══════════════════════════════════════════════════════════════════════════ */

static void git_run_async(const char *cmd){
  pid_t pid=fork(); if(pid==0){setsid();int maxfd=(int)sysconf(_SC_OPEN_MAX);
    for(int fd=3;fd<maxfd;fd++)close(fd);execl("/bin/sh","sh","-c",cmd,NULL);_exit(1);}
  if(pid<0)log_ring_push("==> git: fork() failed");
}

static void panel_reset_state(TrixieOverlay *o, PanelId p){
  switch(p){
    case PANEL_MARVIN:
      g_marvin_cursor=0;
      break;
    case PANEL_BUILD:
      o->build_cmd_editing=0;
      break;
    case PANEL_LOG:
      g_log_filter_mode=false;
      break;
    case PANEL_COMMANDS:
      o->filter_mode=false;
      break;
    case PANEL_SEARCH:
      o->search_active=false;
      break;
    case PANEL_FILES:
      o->fb_filter_mode=false;
      break;
    default: break;
  }
}

static void switch_panel(TrixieOverlay *o, PanelId p){
  if(o->panel==p) return;
  panel_reset_state(o, o->panel);
  o->panel=p;
  o->cursor=0; o->scroll=0;
}

bool overlay_key(TrixieOverlay *o,xkb_keysym_t sym,uint32_t mods){
  if(!o||!o->visible) return false; (void)mods;
  o->dirty=true;

  /* ══ §16a  TEXT-INPUT MODAL INTERCEPTS ══════════════════════════════════ */

  if(o->panel==PANEL_BUILD && o->build_cmd_editing){
    if(sym==XKB_KEY_Escape){ o->build_cmd_editing=0; return true;}
    if(sym==XKB_KEY_Return){
      o->build_cmd_editing=0;
      if(o->build_cmd[0]) strncpy(g_build.cmd,o->build_cmd,BUILD_CMD_MAX-1);
      run_build(); switch_panel(o,PANEL_LOG); return true;}
    int l=(int)strlen(o->build_cmd);
    if(sym==XKB_KEY_BackSpace){ if(l>0) o->build_cmd[l-1]='\0'; return true;}
    if(sym>=0x20&&sym<0x7f&&l<BUILD_CMD_MAX-1){
      o->build_cmd[l]=(char)sym; o->build_cmd[l+1]='\0';}
    return true;
  }
  if(o->panel == PANEL_GIT && g_git_commit_editing) {
    if(sym == XKB_KEY_Escape) {
      g_git_commit_editing = false;
      return true;
    }
    if(sym == XKB_KEY_Return) {
      g_git_commit_editing = false;
      if(g_git_commit_msg[0]) {
        /* Escape single-quotes in the message to avoid shell injection */
        char safe[512] = { 0 };
        int  si        = 0;
        for(int i = 0; g_git_commit_msg[i] && si < 500; i++) {
          if(g_git_commit_msg[i] == '\'') {
            if(si + 4 < 500) {
              safe[si++] = '\''; safe[si++] = '"';
              safe[si++] = '\''; safe[si++] = '"';
              safe[si++] = '\'';
            }
          } else {
            safe[si++] = g_git_commit_msg[i];
          }
        }
        safe[si] = '\0';
        char cmd[640];
        snprintf(cmd, sizeof(cmd),
                 "cd '%s' && git commit -m '%s' 2>&1 | head -20",
                 g_git.root[0] ? g_git.root : ".",
                 safe);
        git_run_async(cmd);
        g_git.valid              = false;
        g_git_commit_msg[0]      = '\0';
        g_git_commit_len         = 0;
        log_ring_push("==> git: committing…");
      }
      return true;
    }
    if(sym == XKB_KEY_BackSpace) {
      if(g_git_commit_len > 0) g_git_commit_msg[--g_git_commit_len] = '\0';
      return true;
    }
    if(sym >= 0x20 && sym < 0x7f && g_git_commit_len < 254) {
      g_git_commit_msg[g_git_commit_len++] = (char)sym;
      g_git_commit_msg[g_git_commit_len]   = '\0';
    }
    return true;
  }
  if(o->panel==PANEL_LOG && g_log_filter_mode){
    if(sym==XKB_KEY_Escape||sym==XKB_KEY_Return){ g_log_filter_mode=false; return true;}
    if(sym==XKB_KEY_BackSpace){
      if(g_log_filter_len>0) g_log_filter[--g_log_filter_len]='\0'; return true;}
    if(sym>=0x20&&sym<0x7f&&g_log_filter_len<LOG_FILTER_MAX-1){
      g_log_filter[g_log_filter_len++]=(char)sym; g_log_filter[g_log_filter_len]='\0';}
    return true;}

  if(o->panel==PANEL_COMMANDS && o->filter_mode){
    if(sym==XKB_KEY_Return||sym==XKB_KEY_Escape){ o->filter_mode=false; return true;}
    if(sym==XKB_KEY_BackSpace){
      if(o->filter_len>0){ o->filter[--o->filter_len]='\0'; rebuild_matches(o);} return true;}
    if(sym>=0x20&&sym<0x7f&&o->filter_len<OV_FILTER_MAX-1){
      o->filter[o->filter_len++]=(char)sym; o->filter[o->filter_len]='\0'; rebuild_matches(o);}
    return true;}

  if(o->panel==PANEL_SEARCH && o->search_active){
    if(sym==XKB_KEY_Escape||sym==XKB_KEY_Return){ o->search_active=false; return true;}
    if(sym==XKB_KEY_BackSpace){
      if(g_search.query_len>0){ g_search.query[--g_search.query_len]='\0'; search_run();}
      return true;}
    if(sym>=0x20&&sym<0x7f&&g_search.query_len<SEARCH_QUERY_MAX-1){
      g_search.query[g_search.query_len++]=(char)sym;
      g_search.query[g_search.query_len]='\0'; search_run();}
    return true;}

  if(o->panel==PANEL_FILES && o->fb_filter_mode){
    bool handled=files_panel_key(&o->cursor,o->fb_cwd,sizeof(o->fb_cwd),
                                 o->fb_filter,&o->fb_filter_len,&o->fb_filter_mode,
                                 sym,&o->cfg->overlay);
    if(strcmp(g_git_cwd,o->fb_cwd)!=0){
      strncpy(g_git_cwd,o->fb_cwd,sizeof(g_git_cwd)-1); g_git.valid=false;}
    return handled;}

  /* ══ §16b  UNIVERSAL ════════════════════════════════════════════════════ */

  if(sym==XKB_KEY_Escape||sym==XKB_KEY_grave){
    panel_reset_state(o,o->panel);
    o->visible=false;
    wlr_scene_node_set_enabled(&o->scene_buf->node,false);
    return true;}

  if(sym>=XKB_KEY_1&&sym<=XKB_KEY_9){
    o->filter[0]='\0'; o->filter_len=0; o->search_active=false;
    switch_panel(o,(PanelId)(sym-XKB_KEY_1));
    return true;}
  if(sym==XKB_KEY_0){
    o->search_active=false;
    switch_panel(o,PANEL_DEPS);
    return true;}

  if(sym==XKB_KEY_F){
    o->fb_filter[0]='\0'; o->fb_filter_len=0; o->fb_filter_mode=false;
    switch_panel(o,PANEL_FILES); return true;}
  if(sym==XKB_KEY_N){ switch_panel(o,PANEL_NVIM); return true;}
  if(sym==XKB_KEY_L){ switch_panel(o,PANEL_LSP);  return true;}

  /* Tab cycles sub-tabs only within Marvin; delegate entirely to marvin_panel_key */
  if(sym==XKB_KEY_Tab && o->panel==PANEL_MARVIN)
    return marvin_panel_key(o, sym, mods);

  if(sym==XKB_KEY_g){ o->cursor=0; o->scroll=0; o->ws_cursor=0; return true;}
  if(sym==XKB_KEY_G){ o->cursor=9999; o->scroll=9999; return true;}

  /* ══ §16c  PER-PANEL ACTIONS ════════════════════════════════════════════ */

  switch(o->panel){

    case PANEL_WORKSPACES:
      if(sym==XKB_KEY_j||sym==XKB_KEY_Down){ o->ws_cursor++; return true;}
      if(sym==XKB_KEY_k||sym==XKB_KEY_Up){ if(o->ws_cursor>0) o->ws_cursor--; return true;}
      if(sym==XKB_KEY_Return){ o->pending_ws_switch=o->ws_cursor; return true;}
      return true;

    case PANEL_LOG:
      if(sym==XKB_KEY_j||sym==XKB_KEY_Down){ o->cursor++; return true;}
      if(sym==XKB_KEY_k||sym==XKB_KEY_Up){ if(o->cursor>0) o->cursor--; return true;}
      if(sym==XKB_KEY_slash){ g_log_filter_mode=true; return true;}
      if(sym==XKB_KEY_c){ g_log_ring.head=g_log_ring.count=0; return true;}
      return true;

    case PANEL_GIT:
      if(sym==XKB_KEY_j||sym==XKB_KEY_Down){ o->cursor++; return true;}
      if(sym==XKB_KEY_k||sym==XKB_KEY_Up){ if(o->cursor>0) o->cursor--; return true;}
      if(sym==XKB_KEY_r){ g_git.valid=false; refresh_git(); return true;}
      if(sym==XKB_KEY_d){
        if(o->cursor>=0&&o->cursor<g_git.file_count){
          if(g_git.show_diff&&g_git.diff_for==o->cursor) g_git.show_diff=false;
          else{ git_load_diff(o->cursor); g_git.show_diff=true; }}
        return true;}
      if(sym==XKB_KEY_s){
        if(o->cursor>=0&&o->cursor<g_git.file_count){
          char cmd[512]; snprintf(cmd,sizeof(cmd),"git add -- '%s' 2>/dev/null",g_git.files[o->cursor].path);
          git_run_async(cmd); g_git.valid=false;}
        return true;}
      if(sym==XKB_KEY_u){
        if(o->cursor>=0&&o->cursor<g_git.file_count){
          char cmd[512]; snprintf(cmd,sizeof(cmd),"git restore --staged -- '%s' 2>/dev/null",g_git.files[o->cursor].path);
          system(cmd); g_git.valid=false;}
        return true;}
      if(sym == XKB_KEY_c) {
        g_git_commit_editing = true;
        return true;
      }
      if(sym == XKB_KEY_A) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "cd '%s' && git add -A 2>/dev/null",
                 g_git.root[0] ? g_git.root : ".");
        git_run_async(cmd);
        g_git.valid = false;
        log_ring_push("==> git: staged all");
        return true;
      }
      if(sym == XKB_KEY_p) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "cd '%s' && git push 2>&1 | tail -5",
                 g_git.root[0] ? g_git.root : ".");
        git_run_async(cmd);
        log_ring_push("==> git: pushing…");
        return true;
      }
      return true;

    case PANEL_COMMANDS:
      if(sym==XKB_KEY_j||sym==XKB_KEY_Down){ o->cursor++; return true;}
      if(sym==XKB_KEY_k||sym==XKB_KEY_Up){ if(o->cursor>0) o->cursor--; return true;}
      if(sym==XKB_KEY_slash){ o->filter_mode=true; return true;}
      if(sym==XKB_KEY_Return){
        rebuild_matches(o);
        if(o->cursor>=0&&o->cursor<o->match_count){
          char msg[256]; snprintf(msg,sizeof(msg),"==> cmd: %s",g_commands[o->matches[o->cursor]].ipc_cmd);
          log_ring_push(msg);}
        return true;}
      return true;

    case PANEL_PROCESSES:
      if(sym==XKB_KEY_j||sym==XKB_KEY_Down){ o->cursor++; return true;}
      if(sym==XKB_KEY_k||sym==XKB_KEY_Up){ if(o->cursor>0) o->cursor--; return true;}
      if(sym==XKB_KEY_o){ g_proc_sort=(ProcSort)((g_proc_sort+1)%3); return true;}
      if(sym==XKB_KEY_Return){
        if(o->cursor>=0&&o->cursor<g_proc_count) kill(g_procs[o->cursor].pid,SIGTERM);
        return true;}
      if(sym==XKB_KEY_K){
        if(o->cursor>=0&&o->cursor<g_proc_count) kill(g_procs[o->cursor].pid,SIGKILL);
        return true;}
      return true;

    case PANEL_SEARCH:
      if(sym==XKB_KEY_j||sym==XKB_KEY_Down){ o->cursor++; return true;}
      if(sym==XKB_KEY_k||sym==XKB_KEY_Up){ if(o->cursor>0) o->cursor--; return true;}
      if(sym==XKB_KEY_slash){ o->search_active=true; return true;}
      if(sym==XKB_KEY_t){
        g_search.file_only=!g_search.file_only;
        if(g_search.query_len>0) search_run();
        return true;}
      if(sym==XKB_KEY_Return){
        search_poll();
        if(o->cursor>=0&&o->cursor<g_search.result_count){
          SearchResult *sr=&g_search.results[o->cursor];
          overlay_open_file(sr->file,sr->line,o->cfg?&o->cfg->overlay:NULL);}
        return true;}
      if(sym>=0x20&&sym<0x7f){
        o->search_active=true;
        if(g_search.query_len<SEARCH_QUERY_MAX-1){
          g_search.query[g_search.query_len++]=(char)sym;
          g_search.query[g_search.query_len]='\0'; search_run();}
        return true;}
      return true;

    case PANEL_BUILD:
      if(o->build_show_errors){
        if(sym==XKB_KEY_j||sym==XKB_KEY_Down){
          pthread_mutex_lock(&g_build.err_lock); int mx=g_build.err_count-1; pthread_mutex_unlock(&g_build.err_lock);
          if(o->build_err_cursor<mx) o->build_err_cursor++; return true;}
        if(sym==XKB_KEY_k||sym==XKB_KEY_Up){
          if(o->build_err_cursor>0) o->build_err_cursor--; return true;}
      } else {
        if(sym==XKB_KEY_j||sym==XKB_KEY_Down){ o->cursor++; return true;}
        if(sym==XKB_KEY_k||sym==XKB_KEY_Up){ if(o->cursor>0) o->cursor--; return true;}
      }
      if(sym==XKB_KEY_x){ o->build_cmd[0]='\0'; o->build_cmd_editing=1; return true;}
      if(sym==XKB_KEY_e){ o->build_show_errors=!o->build_show_errors; o->build_err_cursor=0; return true;}
      if(sym==XKB_KEY_Return){
        if(o->build_show_errors&&g_build.err_count>0){
          pthread_mutex_lock(&g_build.err_lock); int ei=o->build_err_cursor;
          if(ei<g_build.err_count){BuildError *be=&g_build.errors[ei];
            if(be->file[0]&&be->line>0)
              overlay_open_file(be->file,be->line,o->cfg?&o->cfg->overlay:NULL);}
          pthread_mutex_unlock(&g_build.err_lock);
        } else {
          if(o->build_cmd[0]) strncpy(g_build.cmd,o->build_cmd,BUILD_CMD_MAX-1);
          run_build(); o->build_show_errors=false; switch_panel(o,PANEL_LOG);}
        return true;}
      return true;

    /* Marvin: fully delegated to marvin_panel.c */
    case PANEL_MARVIN:
      return marvin_panel_key(o, sym, mods);

    case PANEL_DEPS:
      if(sym==XKB_KEY_j||sym==XKB_KEY_Down){ o->cursor++; return true;}
      if(sym==XKB_KEY_k||sym==XKB_KEY_Up){ if(o->cursor>0) o->cursor--; return true;}
      if(sym==XKB_KEY_u){ deps_check_outdated(); return true;}
      if(sym==XKB_KEY_r){ g_deps.valid=false; deps_load(); return true;}
      return true;

    case PANEL_FILES:{
      bool handled=files_panel_key(&o->cursor,o->fb_cwd,sizeof(o->fb_cwd),
                                   o->fb_filter,&o->fb_filter_len,&o->fb_filter_mode,
                                   sym,&o->cfg->overlay);
      if(strcmp(g_git_cwd,o->fb_cwd)!=0){
        strncpy(g_git_cwd,o->fb_cwd,sizeof(g_git_cwd)-1); g_git.valid=false;}
      return handled;}

    case PANEL_RUN:  return run_panel_key(&o->cursor,sym);
    case PANEL_NVIM: return nvim_panel_key(&o->nvim_cursor,sym,&o->cfg->overlay);
    case PANEL_LSP:  return lsp_panel_key(&o->lsp_cursor,sym,&o->cfg->overlay);

    default: break;
  }
  return true;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * §17  Panel renderers
 * ═══════════════════════════════════════════════════════════════════════════ */

#define IX(bx)      ((bx)+BDR+PAD)
#define IY(by)      ((by)+BDR+HDR_H+4)
#define IW(bw)      ((bw)-BDR*2-PAD*2)
#define ROW_TY(ry)  ((ry)+(ROW_H-g_ov_th)/2+g_ov_asc)

/* ── [1] Workspaces ───────────────────────────────────────────────────── */
static void draw_panel_workspaces(uint32_t *px,int stride,
                                  int bx,int by,int bw,int bh,
                                  TwmState *twm,int ws_cursor,const Config *cfg){
  Color ac=cfg->colors.active_border, im=cfg->colors.inactive_border, bg=cfg->colors.pane_bg;
  int ix=IX(bx), iy=IY(by), iw=IW(bw);
  int cols=twm->ws_count>5?5:(twm->ws_count>0?twm->ws_count:1);
  int rows=(twm->ws_count+cols-1)/cols;
  if(ws_cursor>=twm->ws_count)ws_cursor=twm->ws_count-1;
  if(ws_cursor<0)ws_cursor=0;
  ov_draw_text(px,stride,ix,ROW_TY(iy),stride,by+bh,
               "j/k navigate   Enter switch",0x50,0x54,0x68,0xff);
  int content_top=iy+ROW_H+4;
  int content_bot=by+bh-BDR-PAD/2;
  int avail_w=iw, avail_h=content_bot-content_top;
  if(avail_h<1) return;
  int gap=6;
  int cell_w=(avail_w-gap*(cols-1))/cols;
  int cell_h=(avail_h-gap*(rows-1))/rows;
  if(cell_w<4)cell_w=4; if(cell_h<4)cell_h=4;
  for(int i=0;i<twm->ws_count;i++){
    int col=i%cols,row=i/cols;
    int cx=ix+col*(cell_w+gap), cy=content_top+row*(cell_h+gap);
    if(cy+cell_h>content_bot) break;
    bool active=(i==twm->active_ws), selected=(i==ws_cursor), urgent=(twm->ws_urgent_mask>>i)&1;
    Color bc=active?ac:im;
    uint8_t cbr=(uint8_t)(bg.r>0x10?bg.r-0x10:0);
    uint8_t cbg_=(uint8_t)(bg.g>0x10?bg.g-0x10:0);
    uint8_t cbb=(uint8_t)(bg.b>0x10?bg.b-0x10:0);
    ov_fill_rect(px,stride,cx,cy,cell_w,cell_h,cbr,cbg_,cbb,0xff,stride,by+bh);
    if(active)   ov_fill_rect(px,stride,cx,cy,cell_w,cell_h,ac.r,ac.g,ac.b,0x14,stride,by+bh);
    if(selected&&!active) ov_fill_rect(px,stride,cx,cy,cell_w,cell_h,0x89,0xdc,0xeb,0x18,stride,by+bh);
    if(urgent)   ov_fill_rect(px,stride,cx,cy,cell_w,cell_h,0xf3,0x8b,0xa8,0x18,stride,by+bh);
    uint8_t bdr=urgent?0xf3:(selected?0x89:bc.r);
    uint8_t bdg=urgent?0x8b:(selected?0xdc:bc.g);
    uint8_t bdb=urgent?0xa8:(selected?0xeb:bc.b);
    ov_fill_rect(px,stride,cx,    cy,    cell_w,BDR,bdr,bdg,bdb,0xff,stride,by+bh);
    ov_fill_rect(px,stride,cx,    cy+cell_h-BDR,cell_w,BDR,bdr,bdg,bdb,0xff,stride,by+bh);
    ov_fill_rect(px,stride,cx,    cy,    BDR,cell_h,bdr,bdg,bdb,0xff,stride,by+bh);
    ov_fill_rect(px,stride,cx+cell_w-BDR,cy,BDR,cell_h,bdr,bdg,bdb,0xff,stride,by+bh);
    uint8_t hhr=(uint8_t)(cbr>0x08?cbr-0x08:0),hhg=(uint8_t)(cbg_>0x08?cbg_-0x08:0),hhb=(uint8_t)(cbb>0x08?cbb-0x08:0);
    ov_fill_rect(px,stride,cx+BDR,cy+BDR,cell_w-BDR*2,ROW_H,hhr,hhg,hhb,0xff,stride,by+bh);
    ov_fill_rect(px,stride,cx+BDR,cy+BDR+ROW_H,cell_w-BDR*2,1,bdr,bdg,bdb,0x50,stride,by+bh);
    char wnum[24]; const char *ws_icon=active?"󰮯 ":"󰖰 ";
    snprintf(wnum,sizeof(wnum),"%s%d",ws_icon,i+1);
    ov_draw_text(px,stride,cx+BDR+4,ROW_TY(cy+BDR),stride,by+bh,wnum,ac.r,ac.g,ac.b,active?0xff:0x70);
    Workspace *ws=&twm->workspaces[i]; char lay_buf[32];
    snprintf(lay_buf,sizeof(lay_buf),"%s %.0f%%",layout_label(ws->layout),ws->main_ratio*100.f);
    int lw2=ov_measure(lay_buf);
    ov_draw_text(px,stride,cx+cell_w-BDR-4-lw2,ROW_TY(cy+BDR),stride,by+bh,lay_buf,0x58,0x5b,0x70,0xff);
    int list_top=cy+BDR+ROW_H+4, list_bot=cy+cell_h-BDR-2;
    int line_h=g_ov_th+3, max_lines=(list_bot-list_top)/line_h;
    for(int j=0;j<ws->pane_count&&j<max_lines;j++){
      Pane *p=twm_pane_by_id(twm,ws->panes[j]); if(!p) continue;
      bool focused=ws->has_focused&&ws->focused==p->id;
      if(focused) ov_fill_rect(px,stride,cx+BDR,list_top+j*line_h,2,line_h-1,ac.r,ac.g,ac.b,0xff,stride,by+bh);
      const char *win_icon=focused?"󰖯 ":"󰖰 "; int icon_w=ov_measure(win_icon);
      char title[64]; strncpy(title,p->title[0]?p->title:p->app_id,sizeof(title)-1);
      int max_w=cell_w-BDR*2-4-icon_w;
      while(ov_measure(title)>max_w&&strlen(title)>3) title[strlen(title)-1]='\0';
      int pty=list_top+j*line_h+g_ov_asc;
      uint8_t fr=focused?ac.r:0xa6,fg=focused?ac.g:0xad,fb=focused?ac.b:0xc8;
      ov_draw_text(px,stride,cx+BDR+6,pty,stride,by+bh,win_icon,fr,fg,fb,focused?0xff:0x60);
      ov_draw_text(px,stride,cx+BDR+6+icon_w,pty,stride,by+bh,title,fr,fg,fb,focused?0xff:0xcc);
    }
    int overflow=ws->pane_count-max_lines;
    if(overflow>0&&max_lines>=0){
      char more[16];snprintf(more,sizeof(more),"+%d more",overflow);
      ov_draw_text(px,stride,cx+BDR+6,list_top+max_lines*line_h+g_ov_asc,stride,by+bh,more,0x45,0x47,0x5a,0xff);}
  }
}

/* ── [2] Commands ─────────────────────────────────────────────────────── */
static void draw_panel_commands(uint32_t *px,int stride,
                                int bx,int by,int bw,int bh,
                                TrixieOverlay *o,const Config *cfg){
  rebuild_matches(o);
  Color ac=cfg->colors.active_border, bg=cfg->colors.pane_bg;
  int ix=IX(bx), iy=IY(by), iw=IW(bw);
  char val[OV_FILTER_MAX+2]={0};
  strncpy(val,o->filter,sizeof(val)-1);
  int after_input=tui_input_box(px,stride,ix,iy,iw,val[0]?val:NULL,"type to filter…",
                                o->filter_mode,ac,bg,stride,by+bh);
  int y=after_input+4;
  if(o->cursor>=o->match_count&&o->match_count>0) o->cursor=o->match_count-1;
  int visible_rows=(bh-(y-by)-BDR-4)/ROW_H;
  int scroll=o->cursor-visible_rows+1; if(scroll<0)scroll=0;
  for(int i=0;i<o->match_count&&i<visible_rows;i++){
    int mi=o->matches[i+scroll], ry=y+i*ROW_H;
    bool sel=(i+scroll==o->cursor);
    if(sel) draw_cursor_line(px,stride,bx,ry,bw,ac,bg,stride,by+bh);
    ov_draw_text(px,stride,ix+6,ROW_TY(ry),stride,by+bh,g_commands[mi].label,
                 sel?ac.r:0xa6,sel?ac.g:0xad,sel?ac.b:0xc8,0xff);
    int hw=ov_measure(g_commands[mi].ipc_cmd);
    ov_draw_text(px,stride,bx+bw-BDR-PAD-hw,ROW_TY(ry),stride,by+bh,
                 g_commands[mi].ipc_cmd,0x45,0x47,0x5a,0xff);
  }
  tui_scrollbar(px,stride,bx,by,bw,bh,scroll,o->match_count,visible_rows,ac,stride,by+bh);
}

/* ── [3] Processes ────────────────────────────────────────────────────── */
static void draw_panel_processes(uint32_t *px,int stride,
                                 int bx,int by,int bw,int bh,
                                 TrixieOverlay *o,const Config *cfg){
  refresh_procs(); run_configs_poll();
  Color ac=cfg->colors.active_border, bg=cfg->colors.pane_bg;
  int ix=IX(bx), iy=IY(by), iw=IW(bw);
  int C0=ix, C1=C0+56, C2=C1+(iw-56-180)*55/100, C3=C2+68, C4=C3+68;
  int spark_w=ix+iw-C4; if(spark_w<32)spark_w=32;

  static const char *sort_labels[]={"CPU%","RSS","PID"};
  char sort_ind[32]; snprintf(sort_ind,sizeof(sort_ind),"o=cycle-sort  [%s]",sort_labels[g_proc_sort]);
  ov_draw_text(px,stride,C0,ROW_TY(iy),stride,by+bh,"PID",    ac.r,ac.g,ac.b,0x70);
  ov_draw_text(px,stride,C1,ROW_TY(iy),stride,by+bh,"COMMAND",ac.r,ac.g,ac.b,0x70);
  ov_draw_text(px,stride,C2,ROW_TY(iy),stride,by+bh,"CPU%",   ac.r,ac.g,ac.b,0x70);
  ov_draw_text(px,stride,C3,ROW_TY(iy),stride,by+bh,"RSS",    ac.r,ac.g,ac.b,0x70);
  ov_draw_text(px,stride,C4,ROW_TY(iy),stride,by+bh,"GRAPH",  ac.r,ac.g,ac.b,0x70);
  int si_w=ov_measure(sort_ind);
  ov_draw_text(px,stride,ix+iw-si_w,ROW_TY(iy),stride,by+bh,sort_ind,0x58,0x5b,0x70,0xff);
  int sep_y=iy+ROW_H;
  tui_hsep(px,stride,bx,sep_y,bw,NULL,ac,cfg->colors.pane_bg,stride,by+bh);
  int y=sep_y+ROW_H;
  int visible_rows=(bh-(y-by)-BDR-4)/ROW_H;
  if(o->cursor>=g_proc_count&&g_proc_count>0)o->cursor=g_proc_count-1;
  int scroll=o->cursor-visible_rows+1; if(scroll<0)scroll=0;
  for(int i=0;i<g_proc_count&&i<visible_rows;i++){
    ProcEntry *pe=&g_procs[i+scroll]; bool sel=(i+scroll==o->cursor);
    int ry=y+i*ROW_H;
    if(sel)draw_cursor_line(px,stride,bx,ry,bw,ac,bg,stride,by+bh);
    char pid_s[16],cpu_s[12],rss_s[12];
    snprintf(pid_s,sizeof(pid_s),"%d",pe->pid);
    snprintf(cpu_s,sizeof(cpu_s),"%.1f",pe->cpu_pct);
    if(pe->rss_kb>=1024)snprintf(rss_s,sizeof(rss_s),"%ldM",pe->rss_kb/1024);
    else snprintf(rss_s,sizeof(rss_s),"%ldK",pe->rss_kb);
    uint8_t nr=sel?ac.r:0xa6,ng=sel?ac.g:0xad,nb=sel?ac.b:0xc8;
    ov_draw_text(px,stride,C0,ROW_TY(ry),stride,by+bh,pid_s,0x58,0x5b,0x70,0xff);
    {char comm[64];strncpy(comm,pe->comm,sizeof(comm)-1);int avail=C2-C1-COL_GAP;
     while(ov_measure(comm)>avail&&strlen(comm)>3)comm[strlen(comm)-1]='\0';
     ov_draw_text(px,stride,C1,ROW_TY(ry),stride,by+bh,comm,nr,ng,nb,0xff);}
    uint8_t cr=pe->cpu_pct>50.f?0xf3:(pe->cpu_pct>20.f?0xf9:0xa6);
    uint8_t cg=pe->cpu_pct>50.f?0x8b:(pe->cpu_pct>20.f?0xe2:0xe3);
    uint8_t cb=pe->cpu_pct>50.f?0xa8:(pe->cpu_pct>20.f?0xaf:0xa1);
    ov_draw_text(px,stride,C2,ROW_TY(ry),stride,by+bh,cpu_s,cr,cg,cb,0xff);
    ov_draw_text(px,stride,C3,ROW_TY(ry),stride,by+bh,rss_s,0xa6,0xe3,0xa1,0xff);
    {int bar_count=pe->hist_count<PROC_HIST?pe->hist_count:PROC_HIST;
     int bar_w=spark_w/(PROC_HIST+1); if(bar_w<2)bar_w=2;
     int bar_max=ROW_H-4;
     int hist_start=(pe->hist_head-bar_count+PROC_HIST*2)%PROC_HIST;
     float hmax=0.1f;
     for(int hi=0;hi<bar_count;hi++){float v=pe->cpu_hist[(hist_start+hi)%PROC_HIST];if(v>hmax)hmax=v;}
     for(int hi=0;hi<bar_count;hi++){
       float v=pe->cpu_hist[(hist_start+hi)%PROC_HIST]/hmax;
       int bh2=(int)(v*bar_max); if(bh2<1)bh2=1;
       int bx2=C4+hi*(bar_w+1), by2=ry+ROW_H-2-bh2;
       float pct=pe->cpu_hist[(hist_start+hi)%PROC_HIST];
       uint8_t br2=pct>50.f?0xf3:(pct>20.f?0xf9:0xa6);
       uint8_t bg2_=pct>50.f?0x8b:(pct>20.f?0xe2:0xe3);
       uint8_t bb2=pct>50.f?0xa8:(pct>20.f?0xaf:0xa1);
       ov_fill_rect(px,stride,bx2,by2,bar_w,bh2,br2,bg2_,bb2,sel?0xff:0xcc,stride,by+bh);}}
  }
  tui_scrollbar(px,stride,bx,by,bw,bh,scroll,g_proc_count,visible_rows,ac,stride,by+bh);
}

/* ── [4] Log ──────────────────────────────────────────────────────────── */
static void draw_panel_log(uint32_t *px,int stride,
                           int bx,int by,int bw,int bh,
                           TrixieOverlay *o,const Config *cfg){
  Color ac=cfg->colors.active_border;
  int ix=IX(bx), iy=IY(by);
  char val[LOG_FILTER_MAX+2]={0}; strncpy(val,g_log_filter,sizeof(val)-1);
  int after_input=tui_input_box(px,stride,ix,iy,IW(bx+(bw-BDR*2-PAD*2)+BDR+PAD-(ix-bx)),
                                val[0]?val:NULL,"/ filter…",g_log_filter_mode,
                                ac,cfg->colors.pane_bg,stride,by+bh);
  int y=after_input+4;
  int visible_rows=(bh-(y-by)-BDR-4)/ROW_H;
  int indices[LOG_RING_SIZE]; int idx_count=0;
  for(int i=0;i<g_log_ring.count&&idx_count<LOG_RING_SIZE;i++){
    const char *line=log_ring_get(i);
    if(g_log_filter[0]&&!strstr(line,g_log_filter)) continue;
    indices[idx_count++]=i;}
  int scroll_base=idx_count-visible_rows-o->cursor; if(scroll_base<0)scroll_base=0;
  for(int i=0;i<visible_rows;i++){
    int fi=scroll_base+i; if(fi>=idx_count) break;
    const char *line=log_ring_get(indices[fi]); int ry=y+i*ROW_H;
    uint8_t lr=0xa6,lg=0xad,lb=0xc8;
    if(strstr(line,"ERROR")||strstr(line,"error"))    lr=0xf3,lg=0x8b,lb=0xa8;
    else if(strstr(line,"WARN")||strstr(line,"warn")) lr=0xf9,lg=0xe2,lb=0xaf;
    else if(strstr(line,"==>"))                       lr=ac.r,lg=ac.g,lb=ac.b;
    ov_draw_text(px,stride,ix,ROW_TY(ry),stride,by+bh,line,lr,lg,lb,0xff);}
  tui_scrollbar(px,stride,bx,by,bw,bh,scroll_base,idx_count,visible_rows,ac,stride,by+bh);
  (void)bw;
}

/* ── [5] Git ──────────────────────────────────────────────────────────── */
static void draw_panel_git(uint32_t *px,int stride,
                           int bx,int by,int bw,int bh,
                           TrixieOverlay *o,const Config *cfg){
  refresh_git();
  Color ac=cfg->colors.active_border, bg=cfg->colors.pane_bg;
  int ix=IX(bx), iy=IY(by), iw=IW(bw);
  char blabel[GIT_LINE_MAX+8]; snprintf(blabel,sizeof(blabel),"  %s",g_git.branch);
  ov_draw_text(px,stride,ix,ROW_TY(iy),stride,by+bh,blabel,ac.r,ac.g,ac.b,0xff);

  const char *hint =
      g_git_commit_editing
          ? "Enter=confirm  Esc=cancel"
          : "r=refresh  s=stage  u=unstage  A=stage-all  d=diff  c=commit  p=push";
  ov_draw_text(px,stride,bx+bw-BDR-PAD-ov_measure(hint),ROW_TY(iy),stride,by+bh,hint,0x58,0x5b,0x70,0xff);

  int sep_y=iy+ROW_H;
  tui_hsep(px,stride,bx,sep_y,bw,NULL,ac,bg,stride,by+bh);

  /* ── Commit input box (shown when editing or when a staged message exists) ── */
  int y=sep_y+ROW_H;
  if(g_git_commit_editing || g_git_commit_msg[0]) {
    int ciy = sep_y + ROW_H;
    ov_fill_rect(px, stride, bx+BDR, ciy, bw-BDR*2, ROW_H+8,
                 0x18, 0x18, 0x28, 0xa0, stride, by+bh);
    char disp[280];
    snprintf(disp, sizeof(disp), "commit › %s%s",
             g_git_commit_msg,
             g_git_commit_editing ? "▌" : "");
    uint8_t cr = g_git_commit_editing ? ac.r : 0x74;
    uint8_t cg = g_git_commit_editing ? ac.g : 0x78;
    uint8_t cb = g_git_commit_editing ? ac.b : 0x92;
    ov_draw_text(px, stride, ix, ROW_TY(ciy+4), stride, by+bh,
                 disp, cr, cg, cb, 0xff);
    y = sep_y + ROW_H + ROW_H + 8 + 2;
  }

  int avail_h=bh-(y-by)-BDR-4;
  int visible_rows=avail_h/ROW_H;
  if(g_git.show_diff&&g_git.diff_count>0){
    int left_w=bw*2/5, right_x=bx+left_w+2;
    ov_fill_rect(px,stride,bx+left_w,y,1,avail_h,ac.r,ac.g,ac.b,0x40,stride,by+bh);
    int scroll=o->cursor-visible_rows+1; if(scroll<0)scroll=0;
    for(int i=0;i<visible_rows&&(i+scroll)<g_git.file_count;i++){
      GitFile *gf=&g_git.files[i+scroll]; bool sel=(i+scroll==o->cursor); int ry=y+i*ROW_H;
      if(sel)draw_cursor_line(px,stride,bx,ry,left_w,ac,bg,stride,by+bh);
      uint8_t xr,xg,xb; const char *git_icon;
      char xy0=gf->xy[0],xy1=gf->xy[1];
      if(xy0=='A'){git_icon=" ";xr=0xa6;xg=0xe3;xb=0xa1;}
      else if(xy0=='M'){git_icon=" ";xr=0xf9;xg=0xe2;xb=0xaf;}
      else if(xy0=='D'){git_icon=" ";xr=0xf3;xg=0x8b;xb=0xa8;}
      else if(xy0=='R'){git_icon=" ";xr=0x89;xg=0xdc;xb=0xeb;}
      else if(xy0=='?'&&xy1=='?'){git_icon=" ";xr=0x58;xg=0x5b;xb=0x70;}
      else{git_icon=" ";xr=0x58;xg=0x5b;xb=0x70;}
      if(gf->staged) ov_fill_rect(px,stride,bx+BDR,ry+2,2,ROW_H-4,ac.r,ac.g,ac.b,0xff,stride,by+bh);
      ov_draw_text(px,stride,ix,ROW_TY(ry),stride,by+bh,git_icon,sel?ac.r:xr,sel?ac.g:xg,sel?ac.b:xb,0xff);
      int iw2=ov_measure(git_icon); char pathbuf[GIT_LINE_MAX]; strncpy(pathbuf,gf->path,sizeof(pathbuf)-1);
      int avail=left_w-BDR-PAD-iw2-4;
      while(ov_measure(pathbuf)>avail&&strlen(pathbuf)>3)pathbuf[strlen(pathbuf)-1]='\0';
      ov_draw_text(px,stride,ix+iw2+4,ROW_TY(ry),stride,by+bh,pathbuf,sel?ac.r:0xa6,sel?ac.g:0xad,sel?ac.b:0xc8,0xff);}
    int dix=right_x+PAD;
    for(int i=0;i<visible_rows&&i<g_git.diff_count;i++){
      const char *dl=g_git.diff[i]; int ry=y+i*ROW_H;
      uint8_t lr=0xa6,lg=0xad,lb=0xc8;
      if(dl[0]=='+'&&dl[1]!='+')      lr=0xa6,lg=0xe3,lb=0xa1;
      else if(dl[0]=='-'&&dl[1]!='-') lr=0xf3,lg=0x8b,lb=0xa8;
      else if(dl[0]=='@')             lr=0x89,lg=0xdc,lb=0xeb;
      else if(strncmp(dl,"diff ",5)==0||strncmp(dl,"index ",6)==0||
              strncmp(dl,"--- ",4)==0||strncmp(dl,"+++ ",4)==0) lr=ac.r,lg=ac.g,lb=ac.b;
      char dtrunc[GIT_LINE_MAX]; strncpy(dtrunc,dl,sizeof(dtrunc)-1);
      int avail=bx+bw-BDR-PAD-dix;
      while(ov_measure(dtrunc)>avail&&strlen(dtrunc)>3)dtrunc[strlen(dtrunc)-1]='\0';
      ov_draw_text(px,stride,dix,ROW_TY(ry),stride,by+bh,dtrunc,lr,lg,lb,0xff);}
  } else {
    int scroll=o->cursor-visible_rows+1; if(scroll<0)scroll=0;
    int fi_rows=g_git.file_count<visible_rows?g_git.file_count:visible_rows/2;
    for(int i=0;i<fi_rows&&(i+scroll)<g_git.file_count;i++){
      GitFile *gf=&g_git.files[i+scroll]; bool sel=(i+scroll==o->cursor); int ry=y+i*ROW_H;
      if(sel)draw_cursor_line(px,stride,bx,ry,bw,ac,bg,stride,by+bh);
      uint8_t xr,xg,xb; const char *git_icon2;
      char xy0=gf->xy[0],xy1=gf->xy[1];
      if(xy0=='A'||xy1=='A'){git_icon2=" ";xr=0xa6;xg=0xe3;xb=0xa1;}
      else if(xy0=='M'||xy1=='M'){git_icon2=" ";xr=0xf9;xg=0xe2;xb=0xaf;}
      else if(xy0=='D'||xy1=='D'){git_icon2=" ";xr=0xf3;xg=0x8b;xb=0xa8;}
      else if(xy0=='R'||xy1=='R'){git_icon2=" ";xr=0x89;xg=0xdc;xb=0xeb;}
      else if(xy0=='?'&&xy1=='?'){git_icon2=" ";xr=0x58;xg=0x5b;xb=0x70;}
      else{git_icon2=" ";xr=0x58;xg=0x5b;xb=0x70;}
      if(gf->staged)ov_fill_rect(px,stride,bx+BDR,ry+2,2,ROW_H-4,ac.r,ac.g,ac.b,0xff,stride,by+bh);
      ov_draw_text(px,stride,ix,ROW_TY(ry),stride,by+bh,git_icon2,sel?ac.r:xr,sel?ac.g:xg,sel?ac.b:xb,0xff);
      int iw3=ov_measure(git_icon2); char pathbuf2[GIT_LINE_MAX]; strncpy(pathbuf2,gf->path,sizeof(pathbuf2)-1);
      int avail2=iw-iw3-4;
      while(ov_measure(pathbuf2)>avail2&&strlen(pathbuf2)>3)pathbuf2[strlen(pathbuf2)-1]='\0';
      ov_draw_text(px,stride,ix+iw3+4,ROW_TY(ry),stride,by+bh,pathbuf2,sel?ac.r:0xa6,sel?ac.g:0xad,sel?ac.b:0xc8,0xff);}
    int log_y=y+fi_rows*ROW_H;
    tui_hsep(px,stride,bx,log_y,bw,"commits",ac,bg,stride,by+bh);
    int log_rows=(bh-(log_y+ROW_H-by)-BDR-4)/ROW_H;
    for(int i=0;i<log_rows&&i<g_git.line_count;i++){
      const char *line=g_git.lines[i]; int ry=log_y+ROW_H+i*ROW_H;
      uint8_t lr=0xa6,lg=0xad,lb=0xc8;
      if(strncmp(line,"Recent",6)==0)lr=ac.r,lg=ac.g,lb=ac.b;
      ov_draw_text(px,stride,ix,ROW_TY(ry),stride,by+bh,line,lr,lg,lb,0xff);}
  }
}

/* ── [6] Build ────────────────────────────────────────────────────────── */
static void draw_panel_build(uint32_t *px,int stride,
                             int bx,int by,int bw,int bh,
                             TrixieOverlay *o,const Config *cfg){
  Color ac=cfg->colors.active_border, bg=cfg->colors.pane_bg;
  int ix=IX(bx), iy=IY(by), iw=IW(bw);
  char val[BUILD_CMD_MAX+2]={0};
  strncpy(val,o->build_cmd[0]?o->build_cmd:g_build.cmd,sizeof(val)-1);
  int after_input=tui_input_box(px,stride,ix,iy,iw,val,"meson compile -C builddir 2>&1",
                                (bool)o->build_cmd_editing,ac,bg,stride,by+bh);
  int y=after_input+4;
  bool running=atomic_load(&g_build.running);
  if(running){
    static const char *spin[]={"󰪞","󰪟","󰪠","󰪡","󰪢","󰪣","󰪤","󰪥"};
    int64_t si=(ov_now_ms()/80)%8; char status[64];
    double elapsed=(double)(ov_now_ms()-g_build.started_ms)/1000.0;
    snprintf(status,sizeof(status),"%s  building…  %.1fs",spin[si],elapsed);
    ov_draw_text(px,stride,ix,ROW_TY(y),stride,by+bh,status,0xf9,0xe2,0xaf,0xff);
    y+=ROW_H;
  } else if(g_build.done){
    char status[80]; double elapsed=(double)(g_build.finished_ms-g_build.started_ms)/1000.0;
    pthread_mutex_lock(&g_build.err_lock); int ec=g_build.err_count; pthread_mutex_unlock(&g_build.err_lock);
    snprintf(status,sizeof(status),"exit %d  %.1fs  %d diagnostic%s",g_build.exit_code,elapsed,ec,ec==1?"":"s");
    uint8_t sr=g_build.exit_code==0?0xa6:0xf3,sg=g_build.exit_code==0?0xe3:0x8b,sb=g_build.exit_code==0?0xa1:0xa8;
    ov_draw_text(px,stride,ix,ROW_TY(y),stride,by+bh,status,sr,sg,sb,0xff); y+=ROW_H;}
  int visible_rows=(bh-(y-by)-BDR-4)/ROW_H;
  if(o->build_show_errors){
    pthread_mutex_lock(&g_build.err_lock); int ec=g_build.err_count;
    char hdr[64]; snprintf(hdr,sizeof(hdr),"%d diagnostic%s",ec,ec==1?"":"s");
    tui_hsep(px,stride,bx,y,bw,hdr,ac,bg,stride,by+bh); y+=ROW_H;
    visible_rows=(bh-(y-by)-BDR-4)/ROW_H;
    int scroll=o->build_err_cursor-visible_rows+1; if(scroll<0)scroll=0;
    for(int i=0;i<visible_rows&&(i+scroll)<ec;i++){
      BuildError *be=&g_build.errors[i+scroll]; bool sel=(i+scroll==o->build_err_cursor); int ry=y+i*ROW_H;
      if(sel)draw_cursor_line(px,stride,bx,ry,bw,ac,bg,stride,by+bh);
      uint8_t ir=be->is_warning?0xf9:0xf3,ig=be->is_warning?0xe2:0x8b,ib=be->is_warning?0xaf:0xa8;
      ov_draw_text(px,stride,ix,ROW_TY(ry),stride,by+bh,be->is_warning?"W":"E",ir,ig,ib,0xff);
      char loc[128];
      if(be->line>0)snprintf(loc,sizeof(loc),"%s:%d",be->file,be->line);
      else strncpy(loc,be->file[0]?be->file:"(unknown)",sizeof(loc)-1);
      ov_draw_text(px,stride,ix+20,ROW_TY(ry),stride,by+bh,loc,0x89,0xdc,0xeb,0xff);
      int loc_w=ov_measure(loc)+20+PAD; int msg_w=iw-loc_w;
      if(msg_w>40){char msg[BUILD_ERR_LINE];strncpy(msg,be->msg,sizeof(msg)-1);
        while(ov_measure(msg)>msg_w&&strlen(msg)>4)msg[strlen(msg)-1]='\0';
        ov_draw_text(px,stride,ix+loc_w,ROW_TY(ry),stride,by+bh,msg,sel?ac.r:0xa6,sel?ac.g:0xad,sel?ac.b:0xc8,0xff);}}
    pthread_mutex_unlock(&g_build.err_lock); return;}
  int start=g_log_ring.count-visible_rows; if(start<0)start=0;
  for(int i=0;i<visible_rows;i++){
    int li=start+i; if(li>=g_log_ring.count) break;
    const char *line=log_ring_get(li); int ry=y+i*ROW_H;
    uint8_t lr=0xa6,lg=0xad,lb=0xc8;
    if(strstr(line,"error:")||strstr(line,"ERROR"))lr=0xf3,lg=0x8b,lb=0xa8;
    else if(strstr(line,"warning:"))lr=0xf9,lg=0xe2,lb=0xaf;
    else if(strstr(line,"==>"))lr=ac.r,lg=ac.g,lb=ac.b;
    ov_draw_text(px,stride,ix,ROW_TY(ry),stride,by+bh,line,lr,lg,lb,0xff);}
}

/* ── [8] Search ───────────────────────────────────────────────────────── */
static void draw_panel_search(uint32_t *px,int stride,
                              int bx,int by,int bw,int bh,
                              TrixieOverlay *o,const Config *cfg){
  search_poll();
  Color ac=cfg->colors.active_border, bg=cfg->colors.pane_bg;
  int ix=IX(bx), iy=IY(by), iw=IW(bw);
  const char *mode_label=g_search.file_only?"FILES":"TEXT";
  int badge_w=ov_measure(mode_label)+8;
  ov_fill_rect(px,stride,ix,iy,badge_w,ROW_H,ac.r,ac.g,ac.b,0x25,stride,by+bh);
  ov_fill_rect(px,stride,ix,iy,2,ROW_H,ac.r,ac.g,ac.b,0xff,stride,by+bh);
  ov_draw_text(px,stride,ix+6,ROW_TY(iy),stride,by+bh,mode_label,ac.r,ac.g,ac.b,0xff);
  const char *toggle_hint="t toggle mode";
  ov_draw_text(px,stride,ix+badge_w+6,ROW_TY(iy),stride,by+bh,toggle_hint,0x58,0x5b,0x70,0xff);
  char val[SEARCH_QUERY_MAX+2]={0}; strncpy(val,g_search.query,sizeof(val)-1);
  int after_input=tui_input_box(px,stride,ix,iy+ROW_H+4,iw,val[0]?val:NULL,"search…",
                                o->search_active,ac,bg,stride,by+bh);
  int y=after_input+4;
  if(g_search.running){
    ov_draw_text(px,stride,ix,ROW_TY(y),stride,by+bh,"searching…",0xf9,0xe2,0xaf,0xff);y+=ROW_H;
  } else if(g_search.result_count>0){
    char cnt[48];snprintf(cnt,sizeof(cnt),"%d result%s",g_search.result_count,g_search.result_count==1?"":"s");
    ov_draw_text(px,stride,ix,ROW_TY(y),stride,by+bh,cnt,0x58,0x5b,0x70,0xff);y+=ROW_H;}
  tui_hsep(px,stride,bx,y,bw,NULL,ac,bg,stride,by+bh); y+=ROW_H;
  int visible_rows=(bh-(y-by)-BDR-4)/ROW_H;
  if(o->cursor>=g_search.result_count&&g_search.result_count>0)o->cursor=g_search.result_count-1;
  int scroll=o->cursor-visible_rows+1; if(scroll<0)scroll=0;
  for(int i=0;i<visible_rows&&(i+scroll)<g_search.result_count;i++){
    SearchResult *sr=&g_search.results[i+scroll]; bool sel=(i+scroll==o->cursor); int ry=y+i*ROW_H;
    if(sel)draw_cursor_line(px,stride,bx,ry,bw,ac,bg,stride,by+bh);
    char loc[300];
    if(sr->line>0)snprintf(loc,sizeof(loc),"%s:%d",sr->file,sr->line); else strncpy(loc,sr->file,sizeof(loc)-1);
    ov_draw_text(px,stride,ix+4,ROW_TY(ry),stride,by+bh,loc,0x89,0xdc,0xeb,0xff);
    if(sr->text[0]){int lw=ov_measure(loc)+PAD; char txt[SEARCH_LINE_MAX]; strncpy(txt,sr->text,sizeof(txt)-1);
      char *tp=txt; while(*tp==' '||*tp=='\t')tp++;
      int avail=iw-lw; while(ov_measure(tp)>avail&&strlen(tp)>3)tp[strlen(tp)-1]='\0';
      ov_draw_text(px,stride,ix+4+lw,ROW_TY(ry),stride,by+bh,tp,sel?ac.r:0xa6,sel?ac.g:0xad,sel?ac.b:0xc8,0xff);}
  }
  tui_scrollbar(px,stride,bx,by,bw,bh,scroll,g_search.result_count,visible_rows,ac,stride,by+bh);
}

/* ── [0] Deps ─────────────────────────────────────────────────────────── */
static void draw_panel_deps(uint32_t *px,int stride,
                            int bx,int by,int bw,int bh,
                            TrixieOverlay *o,const Config *cfg){
  deps_load();
  Color ac=cfg->colors.active_border, bg=cfg->colors.pane_bg;
  int ix=IX(bx), iy=IY(by), iw=IW(bw);
  static const char *lang_names[]={"?","Rust","Go","Maven","Gradle"};
  const char *ln=(g_deps.lang<5)?lang_names[g_deps.lang]:"?";
  char hdr[128];
  if(g_deps.checking)snprintf(hdr,sizeof(hdr),"%s — %d deps  checking…",ln,g_deps.count);
  else snprintf(hdr,sizeof(hdr),"%s — %d deps  u=outdated  r=refresh",ln,g_deps.count);
  ov_draw_text(px,stride,ix,ROW_TY(iy),stride,by+bh,hdr,ac.r,ac.g,ac.b,0xff);
  tui_hsep(px,stride,bx,iy+ROW_H,bw,NULL,ac,bg,stride,by+bh);
  int y=iy+ROW_H+ROW_H;
  int C_NAME=ix, C_CUR=ix+iw-160, C_LAT=ix+iw-64;
  ov_draw_text(px,stride,C_NAME,ROW_TY(y),stride,by+bh,"PACKAGE",ac.r,ac.g,ac.b,0x70);
  ov_draw_text(px,stride,C_CUR, ROW_TY(y),stride,by+bh,"CURRENT",ac.r,ac.g,ac.b,0x70);
  ov_draw_text(px,stride,C_LAT, ROW_TY(y),stride,by+bh,"LATEST", ac.r,ac.g,ac.b,0x70);
  ov_fill_rect(px,stride,bx+BDR,y+ROW_H,bw-BDR*2,1,ac.r,ac.g,ac.b,0x30,stride,by+bh);
  y+=ROW_H+4;
  int visible_rows=(bh-(y-by)-BDR-4)/ROW_H;
  if(o->cursor>=g_deps.count&&g_deps.count>0)o->cursor=g_deps.count-1;
  int scroll=o->cursor-visible_rows+1; if(scroll<0)scroll=0;
  for(int i=0;i<visible_rows&&(i+scroll)<g_deps.count;i++){
    DepEntry *de=&g_deps.entries[i+scroll]; bool sel=(i+scroll==o->cursor); int ry=y+i*ROW_H;
    if(sel)draw_cursor_line(px,stride,bx,ry,bw,ac,bg,stride,by+bh);
    uint8_t nr=sel?ac.r:0xa6,ng=sel?ac.g:0xad,nb=sel?ac.b:0xc8;
    if(de->outdated){nr=0xf9;ng=0xe2;nb=0xaf;}
    ov_draw_text(px,stride,C_NAME+4,ROW_TY(ry),stride,by+bh,de->name,nr,ng,nb,0xff);
    ov_draw_text(px,stride,C_CUR,   ROW_TY(ry),stride,by+bh,de->version,0x58,0x5b,0x70,0xff);
    if(de->latest[0]){uint8_t lr=de->outdated?0xa6:0x58,lg=de->outdated?0xe3:0x5b,lb=de->outdated?0xa1:0x70;
      ov_draw_text(px,stride,C_LAT,ROW_TY(ry),stride,by+bh,de->latest,lr,lg,lb,0xff);}
    else if(g_deps.checking)
      ov_draw_text(px,stride,C_LAT,ROW_TY(ry),stride,by+bh,"…",0x58,0x5b,0x70,0xff);}
  tui_scrollbar(px,stride,bx,by,bw,bh,scroll,g_deps.count,visible_rows,ac,stride,by+bh);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §18  overlay_update
 * ═══════════════════════════════════════════════════════════════════════════ */

bool overlay_update(TrixieOverlay *o,TwmState *twm,const Config *cfg,BarWorkerPool *pool){
  if(!o||!overlay_visible(o)) return false; (void)pool;
  int w=o->w,h=o->h; if(w<=0||h<=0) return false;

  int64_t now_ms=ov_now_ms();

  /* Determine if this is a "live" panel that needs periodic auto-refresh. */
  bool live = (o->panel==PANEL_LOG || o->panel==PANEL_PROCESSES ||
               o->panel==PANEL_BUILD || o->panel==PANEL_GIT ||
               o->panel==PANEL_MARVIN || o->panel==PANEL_RUN);

  /* Mark dirty for the periodic live-panel tick (250ms). */
  if(!o->dirty && live) {
    static int64_t g_live_tick=0;
    if((now_ms-g_live_tick)>=250){ g_live_tick=now_ms; o->dirty=true; }
  }

  /* Nothing to paint and nothing pending — let the compositor sleep.
   * Return false so output_handle_frame does NOT call schedule_frame again.
   * The next surface commit or input event will wake us up naturally. */
  if(!o->dirty) return false;

  /* Throttle repaints to ~30fps max (33ms).  If we're within the throttle
   * window, return true to request ONE more frame (to paint when ready). */
  static int64_t g_paint_next=0;
  if(now_ms < g_paint_next) return true;
  g_paint_next = now_ms + 33;

  o->dirty=false;

  if(o->pending_ws_switch>=0&&twm){
    int ws=o->pending_ws_switch; o->pending_ws_switch=-1;
    if(ws<twm->ws_count)twm_switch_ws(twm,ws);}

  float want_size=cfg->bar.font_size>0.f?cfg->bar.font_size:cfg->font_size;
  if(strcmp(o->last_font,cfg->font_path)!=0||fabsf(o->last_size-want_size)>0.01f){
    ov_font_init(cfg->font_path,want_size);
    strncpy(o->last_font,cfg->font_path,sizeof(o->last_font)-1);
    o->last_size=want_size;}

  struct OvRawBuf *rb=ovb_create(w,h);
  uint32_t        *px=rb->data;
  Color ac=cfg->colors.active_border;
  Color bg=cfg->colors.pane_bg;
  Color fg_=cfg->colors.bar_fg;

  ov_fill_rect(px,w,0,0,w,h,0,0,0,0xb0,w,h);

  int pw=w*22/24, ph=h*22/24;
  int px0=(w-pw)/2, py0=(h-ph)/2;

  ov_fill_rect(px,w,px0,py0,pw,ph,bg.r,bg.g,bg.b,0xff,w,h);

  int sel_h   = ROW_H + 4;
  int status_h= ROW_H + 4;
  int sel_y   = py0 + BDR;
  int sel_ty  = sel_y + (sel_h - g_ov_th)/2 + g_ov_asc;
  int hrule_y = sel_y + sel_h;
  int content_y = hrule_y + 1;
  int status_y  = py0 + ph - BDR - status_h;
  int content_h = status_y - content_y;
  int mode_h    = status_h;

  ov_fill_rect(px,w, px0,       py0,       pw,    BDR,   ac.r,ac.g,ac.b,0xff,w,h);
  ov_fill_rect(px,w, px0,       py0+ph-BDR,pw,    BDR,   ac.r,ac.g,ac.b,0xff,w,h);
  ov_fill_rect(px,w, px0,       py0,       BDR,   ph,    ac.r,ac.g,ac.b,0xff,w,h);
  ov_fill_rect(px,w, px0+pw-BDR,py0,       BDR,   ph,    ac.r,ac.g,ac.b,0xff,w,h);

  static const char *panel_labels[PANEL_COUNT]={
    "Workspaces","Commands","Processes","Log","Git",
    "Build","Marvin","Search","Run","Deps","Files","Nvim","LSP"};
  static const char *panel_keys[PANEL_COUNT]={
    "1","2","3","4","5","6","7","8","9","0","F","N","L"};

  {
    int lx = px0 + BDR + PAD;
    ov_draw_text(px,w,lx,sel_ty,w,h,"trixie",ac.r,ac.g,ac.b,0x40);
    lx += ov_measure("trixie") + PAD;
    ov_fill_rect(px,w,lx,sel_y+3,1,sel_h-6,ac.r,ac.g,ac.b,0x28,w,h);
    lx += PAD;

    for(int i=0;i<PANEL_COUNT;i++){
      bool sel=(i==(int)o->panel);
      const char *key   = panel_keys[i];
      const char *label = panel_labels[i];
      char label_buf[48];
      if(i==PANEL_LSP){snprintf(label_buf,sizeof(label_buf),"%s%s",label,lsp_tab_badge());label=label_buf;}
      int kw=ov_measure(key), lw=ov_measure(label);
      int entry_w = kw + 4 + lw + PAD;
      if(sel){
        ov_draw_text(px,w,lx,         sel_ty,w,h,key,  ac.r,ac.g,ac.b,0xff);
        ov_draw_text(px,w,lx+kw+4,    sel_ty,w,h,label,0xe8,0xea,0xed,0xff);
        ov_fill_rect(px,w,lx,sel_y+sel_h-2,entry_w,2,ac.r,ac.g,ac.b,0xff,w,h);
      } else {
        ov_draw_text(px,w,lx,      sel_ty,w,h,key,  ac.r,ac.g,ac.b,0x35);
        ov_draw_text(px,w,lx+kw+4, sel_ty,w,h,label,0x48,0x4c,0x60,0xff);
      }
      lx += entry_w + 6;
    }
  }

  ov_fill_rect(px,w, px0+BDR,hrule_y, pw-BDR*2,1, ac.r,ac.g,ac.b,0x35,w,h);
  ov_fill_rect(px,w, px0+BDR,status_y-1, pw-BDR*2,1, ac.r,ac.g,ac.b,0x35,w,h);

  static const char *panel_titles[PANEL_COUNT]={
    "Workspaces","Commands","Processes","Log","Git",
    "Build","Marvin","Search","Run","Dependencies","Files","Neovim","LSP Diagnostics"};
  static const char *panel_hints[PANEL_COUNT]={
    "j/k\0navigate\0Enter\0switch\0\0",
    "/\0filter\0j/k\0select\0Enter\0exec\0\0",
    "j/k\0navigate\0o\0sort\0Enter\0sigterm\0K\0sigkill\0\0",
    "j/k\0scroll\0/\0filter\0c\0clear\0\0",
    "j/k\0navigate\0r\0refresh\0s\0stage\0u\0unstage\0d\0diff\0\0",
    "x\0edit cmd\0Enter\0run\0e\0errors\0j/k\0navigate\0\0",
    "Tab/[\0sub-tab\0j/k\0navigate\0Enter\0run\0b/r/t/x\0actions\0,\0re-detect\0\0",
    "/\0search\0t\0toggle\0j/k\0navigate\0Enter\0open\0\0",
    "Enter\0toggle\0a\0add\0d\0del\0c\0clear\0\0",
    "j/k\0navigate\0u\0outdated\0r\0refresh\0\0",
    "j/k\0navigate\0Enter\0open\0/\0filter\0.\0hidden\0\0",
    "j/k\0navigate\0Enter\0jump\0r\0refresh\0s\0spawn\0\0",
    "j/k\0navigate\0Enter\0jump\0f\0sort\0g\0group\0\0",
  };
  const char *ptitle=(o->panel<PANEL_COUNT)?panel_titles[o->panel]:"";
  const char *phint =(o->panel<PANEL_COUNT)?panel_hints[o->panel]:"";

  char panel_ctx[128]={0};
  switch(o->panel){
    case PANEL_GIT:
      if(g_git.branch[0]) snprintf(panel_ctx,sizeof(panel_ctx)," %s",g_git.branch);
      break;
    case PANEL_MARVIN:{
      /* delegate context string to marvin_panel */
      marvin_panel_ctx(panel_ctx, sizeof(panel_ctx));
      break;}
    case PANEL_PROCESSES:{
      static const char *sl[]={"cpu","rss","pid"};
      snprintf(panel_ctx,sizeof(panel_ctx),"↕ %s  %d procs",sl[g_proc_sort],g_proc_count);
      break;}
    case PANEL_SEARCH:
      snprintf(panel_ctx,sizeof(panel_ctx),"%s",g_search.file_only?"files":"text");
      break;
    case PANEL_BUILD:{
      bool br=atomic_load(&g_build.running);
      if(br){
        double el=(double)(ov_now_ms()-g_build.started_ms)/1000.0;
        snprintf(panel_ctx,sizeof(panel_ctx),"running %.1fs",el);
      } else if(g_build.done){
        pthread_mutex_lock(&g_build.err_lock); int ec=g_build.err_count; pthread_mutex_unlock(&g_build.err_lock);
        snprintf(panel_ctx,sizeof(panel_ctx),"exit %d  %d diag%s",g_build.exit_code,ec,ec==1?"":"s");
      } break;}
    case PANEL_LOG:
      if(g_log_filter[0]) snprintf(panel_ctx,sizeof(panel_ctx),"filter: %s",g_log_filter);
      else snprintf(panel_ctx,sizeof(panel_ctx),"%d lines",g_log_ring.count);
      break;
    default: break;
  }

  tui_box(px,w, px0,content_y,pw,content_h, ptitle,panel_ctx[0]?panel_ctx:NULL, ac,bg, w,h);

  switch(o->panel){
    case PANEL_WORKSPACES:draw_panel_workspaces(px,w,px0,content_y,pw,content_h,twm,o->ws_cursor,cfg);break;
    case PANEL_COMMANDS:  draw_panel_commands(  px,w,px0,content_y,pw,content_h,o,cfg);break;
    case PANEL_PROCESSES: draw_panel_processes( px,w,px0,content_y,pw,content_h,o,cfg);break;
    case PANEL_LOG:       draw_panel_log(       px,w,px0,content_y,pw,content_h,o,cfg);break;
    case PANEL_GIT:       draw_panel_git(       px,w,px0,content_y,pw,content_h,o,cfg);break;
    case PANEL_BUILD:     draw_panel_build(     px,w,px0,content_y,pw,content_h,o,cfg);break;
    case PANEL_MARVIN:    draw_panel_marvin(    px,w,px0,content_y,pw,content_h,o,cfg);break;
    case PANEL_SEARCH:    draw_panel_search(    px,w,px0,content_y,pw,content_h,o,cfg);break;
    case PANEL_RUN:       draw_panel_run(       px,w,px0,content_y,pw,content_h,&o->cursor,cfg);break;
    case PANEL_DEPS:      draw_panel_deps(      px,w,px0,content_y,pw,content_h,o,cfg);break;
    case PANEL_FILES:
      draw_panel_files(px,w,px0,content_y,pw,content_h,
                       &o->cursor,o->fb_cwd,sizeof(o->fb_cwd),
                       o->fb_filter,&o->fb_filter_len,&o->fb_filter_mode,cfg);
      break;
    case PANEL_NVIM:
      nvim_panel_poll(&cfg->overlay);
      draw_panel_nvim(px,w,px0,content_y,pw,content_h,&o->nvim_cursor,cfg,&cfg->overlay);
      break;
    case PANEL_LSP:
      lsp_panel_tick(&cfg->overlay);
      draw_panel_lsp(px,w,px0,content_y,pw,content_h,&o->lsp_cursor,cfg,&cfg->overlay);
      break;
    default:break;
  }

  /* ── Status bar ──────────────────────────────────────────────────────── */
  {
    int st_ty = status_y + (status_h - g_ov_th)/2 + g_ov_asc;
    {
      int hx = px0 + BDR + PAD;
      int hx_max = px0 + pw/2;
      const char *p = phint;
      while(p && *p && hx < hx_max){
        const char *key   = p;
        int klen = (int)strlen(key);
        const char *label = key + klen + 1;
        if(!*label) break;
        int llen = (int)strlen(label);
        int kw = ov_measure(key);
        int lw = ov_measure(label);
        int kb_pad = 3;
        int kb_w = kb_pad + kw + kb_pad;
        int kb_h = status_h - 6;
        int kb_y = status_y + 3;
        ov_fill_rect(px,w, hx,     kb_y,      kb_w,  1,      ac.r,ac.g,ac.b,0x50,w,h);
        ov_fill_rect(px,w, hx,     kb_y+kb_h-1,kb_w,1,      ac.r,ac.g,ac.b,0x50,w,h);
        ov_fill_rect(px,w, hx,     kb_y,      1,     kb_h,   ac.r,ac.g,ac.b,0x50,w,h);
        ov_fill_rect(px,w, hx+kb_w-1,kb_y,    1,     kb_h,   ac.r,ac.g,ac.b,0x50,w,h);
        ov_draw_text(px,w, hx+kb_pad, st_ty,   w,h,   key,    ac.r,ac.g,ac.b,0xcc);
        ov_draw_text(px,w, hx+kb_w+4, st_ty,   w,h,   label,  0x6e,0x73,0x87,0xff);
        hx += kb_w + 4 + lw + PAD;
        p = label + llen + 1;
      }
    }
    {
      bool in_filter = (o->panel==PANEL_LOG&&g_log_filter_mode)
                     ||(o->panel==PANEL_COMMANDS&&o->filter_mode)
                     ||(o->panel==PANEL_SEARCH&&o->search_active)
                     ||(o->panel==PANEL_FILES&&o->fb_filter_mode);
      bool in_input  = (o->panel==PANEL_BUILD&&o->build_cmd_editing)
                     || marvin_panel_in_input();
      const char *mode_str = in_input?"INPUT":in_filter?"FILTER":"NORMAL";
      uint8_t mr=in_input?0xf9:(in_filter?0x89:ac.r);
      uint8_t mg=in_input?0xe2:(in_filter?0xdc:ac.g);
      uint8_t mb=in_input?0xaf:(in_filter?0xeb:ac.b);

      char ws_str[96]={0};
      if(twm&&twm->ws_count>0){
        Workspace *ws=&twm->workspaces[twm->active_ws];
        Pane *fp=twm_focused(twm);
        const char *app=(fp&&fp->title[0])?fp->title:(fp?fp->app_id:"");
        char app_t[32]; strncpy(app_t,app,sizeof(app_t)-1);
        while(strlen(app_t)>20) app_t[strlen(app_t)-1]='\0';
        if(app_t[0]) snprintf(ws_str,sizeof(ws_str),"ws%d  %s  %s",
                               twm->active_ws+1,app_t,layout_label(ws->layout));
        else         snprintf(ws_str,sizeof(ws_str),"ws%d  %s",
                               twm->active_ws+1,layout_label(ws->layout));
      }

      int mw = ov_measure(mode_str);
      int ww = ws_str[0] ? ov_measure(ws_str) : 0;
      int rx = px0 + pw - BDR - PAD;
      rx -= mw;
      ov_draw_text(px,w, rx,st_ty, w,h, mode_str, mr,mg,mb, 0xd0);
      if(ws_str[0]){
        rx -= PAD + 1;
        ov_fill_rect(px,w,rx,status_y+4,1,status_h-8,ac.r,ac.g,ac.b,0x28,w,h);
        rx -= PAD + ww;
        bool urgent = twm && ((twm->ws_urgent_mask>>twm->active_ws)&1);
        uint8_t wr=urgent?0xf3:0x58, wg=urgent?0x8b:0x5b, wb=urgent?0xa8:0x70;
        ov_draw_text(px,w, rx,st_ty, w,h, ws_str, wr,wg,wb, 0xb0);
      }
    }
  }
  /* Upload the rendered frame.  ovb_create already set up ovb_impl_shared on
   * the header so wlr_buffer_drop will free only the struct, not the pixel slab. */
  wlr_scene_buffer_set_buffer(o->scene_buf,&s_ov_rb->base);
  wlr_buffer_drop(&s_ov_rb->base);
  /* Immediately re-create the header so s_ov_rb is valid for next frame. */
  s_ov_rb         = calloc(1, sizeof(*s_ov_rb));
  s_ov_rb->data   = s_ov_pixels;
  s_ov_rb->stride = s_ov_px_w * 4;
  return true;
}
