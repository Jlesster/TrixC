/* bar.c — FreeType/HarfBuzz canvas primitives + Wibox surface management. */
#include "trixie.h"
#include <drm_fourcc.h>
#include <fontconfig/fontconfig.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include <hb-ft.h>
#include <hb.h>

/* ══════════════════════════════════════════════════════════════════════════
 * §1  Font state
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
  FT_Library ft_lib;
  FT_Face ft_face;
  FT_Face ft_face_bold;
  FT_Face ft_face_italic;
  hb_font_t *hb_font;
  hb_font_t *hb_font_bold;
  hb_font_t *hb_font_italic;
  int ascender;
  int descender;
  int height;
  bool ready;
} BarFont;

static BarFont g_font = {0};

/* ── Glyph cache ───────────────────────────────────────────────────────── */

#define GCACHE_SIZE 1024
#define GCACHE_MASK (GCACHE_SIZE - 1)

typedef struct {
  uint32_t glyph_index;
  int bearing_x, bearing_y;
  int advance_x;
  int bm_w, bm_h, bm_pitch;
  uint8_t *bm;
} GlyphCache;

static GlyphCache g_gcache[GCACHE_SIZE];

static void gcache_clear(void) {
  for (int i = 0; i < GCACHE_SIZE; i++) {
    free(g_gcache[i].bm);
    g_gcache[i] = (GlyphCache){0};
  }
}

static const GlyphCache *gcache_get(uint32_t gi) {
  if (!gi)
    return NULL;
  uint32_t slot = gi & GCACHE_MASK;
  for (int i = 0; i < GCACHE_SIZE; i++) {
    uint32_t s = (slot + (uint32_t)i) & GCACHE_MASK;
    if (g_gcache[s].glyph_index == gi)
      return &g_gcache[s];
    if (g_gcache[s].glyph_index == 0) {
      if (FT_Load_Glyph(g_font.ft_face, gi,
                        FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL))
        return NULL;
      FT_GlyphSlot sl = g_font.ft_face->glyph;
      GlyphCache *cg = &g_gcache[s];
      cg->glyph_index = gi;
      cg->bearing_x = sl->bitmap_left;
      cg->bearing_y = sl->bitmap_top;
      cg->advance_x = (int)(sl->advance.x >> 6);
      cg->bm_w = (int)sl->bitmap.width;
      cg->bm_h = (int)sl->bitmap.rows;
      cg->bm_pitch = sl->bitmap.pitch;
      int sz = cg->bm_h * cg->bm_pitch;
      cg->bm = sz > 0 ? malloc(sz) : NULL;
      if (cg->bm)
        memcpy(cg->bm, sl->bitmap.buffer, sz);
      return cg;
    }
  }
  if (FT_Load_Glyph(g_font.ft_face, gi, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL))
    return NULL;
  static GlyphCache s_tmp;
  FT_GlyphSlot sl = g_font.ft_face->glyph;
  s_tmp.glyph_index = gi;
  s_tmp.bearing_x = sl->bitmap_left;
  s_tmp.bearing_y = sl->bitmap_top;
  s_tmp.advance_x = (int)(sl->advance.x >> 6);
  s_tmp.bm_w = (int)sl->bitmap.width;
  s_tmp.bm_h = (int)sl->bitmap.rows;
  s_tmp.bm_pitch = sl->bitmap.pitch;
  s_tmp.bm = sl->bitmap.buffer;
  return &s_tmp;
}

/* ── Fontconfig resolver — resolve family name or absolute path to .ttf ─── */

static bool fc_resolve_path(const char *query, int weight, int slant, char *out,
                            int olen) {
  if (!query || !query[0])
    return false;

  /* Absolute path — use directly if readable */
  if (query[0] == '/') {
    FILE *f = fopen(query, "r");
    if (!f)
      return false;
    fclose(f);
    strncpy(out, query, olen - 1);
    out[olen - 1] = '\0';
    return true;
  }

  FcInit();
  FcConfig *fc = FcConfigGetCurrent();

  /* Filename match (e.g. "JetBrainsMono-Regular.ttf") */
  if (strchr(query, '.')) {
    FcFontSet *fs = FcConfigGetFonts(fc, FcSetSystem);
    if (fs) {
      for (int i = 0; i < fs->nfont; i++) {
        FcChar8 *file = NULL;
        if (FcPatternGetString(fs->fonts[i], FC_FILE, 0, &file) ==
            FcResultMatch) {
          const char *base = strrchr((char *)file, '/');
          base = base ? base + 1 : (char *)file;
          if (!strcasecmp(base, query)) {
            strncpy(out, (char *)file, olen - 1);
            out[olen - 1] = '\0';
            return true;
          }
        }
      }
    }
  }

  /* Family name — let fontconfig find the best match */
  FcPattern *pat = FcNameParse((FcChar8 *)query);
  if (!pat)
    return false;
  FcPatternAddInteger(pat, FC_WEIGHT, weight);
  FcPatternAddInteger(pat, FC_SLANT, slant);
  FcConfigSubstitute(fc, pat, FcMatchPattern);
  FcDefaultSubstitute(pat);
  FcResult res;
  FcPattern *match = FcFontMatch(fc, pat, &res);
  FcPatternDestroy(pat);
  if (!match)
    return false;
  FcChar8 *file = NULL;
  bool ok = false;
  if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
    strncpy(out, (char *)file, olen - 1);
    out[olen - 1] = '\0';
    ok = true;
  }
  FcPatternDestroy(match);
  return ok;
}

/* ── Face loader ───────────────────────────────────────────────────────── */

static bool load_face(FT_Library lib, const char *path, float size_pt,
                      FT_Face *face_out, hb_font_t **hb_out) {
  if (!path || !path[0])
    return false;
  if (FT_New_Face(lib, path, 0, face_out))
    return false;
  FT_Set_Char_Size(*face_out, 0, (FT_F26Dot6)(size_pt * 64.f), 96, 96);
  *hb_out = hb_ft_font_create_referenced(*face_out);
  hb_ft_font_set_funcs(*hb_out);
  return true;
}

static bool font_load(const char *query, const char *bold_query,
                      const char *italic_query, float size_pt) {
  if (size_pt <= 0.f)
    size_pt = 13.f;
  if (FT_Init_FreeType(&g_font.ft_lib))
    return false;

  /* Resolve the primary font via fontconfig */
  char path[512] = {0};
  bool resolved = fc_resolve_path(query, FC_WEIGHT_REGULAR, FC_SLANT_ROMAN,
                                  path, sizeof(path));
  if (!resolved) {
    /* fontconfig didn't find it — try common system fallbacks */
    const char *fallbacks[] = {
        "/run/current-system/sw/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/run/current-system/sw/share/fonts/truetype/liberation/"
        "LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/JetBrainsMono-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        NULL,
    };
    for (int i = 0; fallbacks[i]; i++) {
      FILE *f = fopen(fallbacks[i], "r");
      if (f) {
        fclose(f);
        strncpy(path, fallbacks[i], sizeof(path) - 1);
        resolved = true;
        wlr_log(WLR_INFO, "[font] '%s' not found, using fallback: %s",
                query ? query : "(null)", path);
        break;
      }
    }
  }

  if (!resolved || !path[0]) {
    wlr_log(WLR_ERROR, "[font] could not find font '%s'",
            query ? query : "(null)");
    FT_Done_FreeType(g_font.ft_lib);
    g_font.ft_lib = NULL;
    return false;
  }

  if (!load_face(g_font.ft_lib, path, size_pt, &g_font.ft_face,
                 &g_font.hb_font)) {
    wlr_log(WLR_ERROR, "[font] FreeType failed to load '%s'", path);
    FT_Done_FreeType(g_font.ft_lib);
    g_font.ft_lib = NULL;
    return false;
  }

  wlr_log(WLR_INFO, "[font] loaded: %s (%.0fpt)", path, size_pt);

  g_font.ascender =
      (int)ceilf((float)g_font.ft_face->size->metrics.ascender / 64.f);
  g_font.descender =
      (int)floorf((float)g_font.ft_face->size->metrics.descender / 64.f);
  g_font.height = g_font.ascender - g_font.descender;

  /* Bold — try to resolve separately, fall back to regular */
  char bold_path[512] = {0};
  if (bold_query && bold_query[0] &&
      fc_resolve_path(bold_query, FC_WEIGHT_BOLD, FC_SLANT_ROMAN, bold_path,
                      sizeof(bold_path)) &&
      load_face(g_font.ft_lib, bold_path, size_pt, &g_font.ft_face_bold,
                &g_font.hb_font_bold)) {
    /* ok */
  } else if (fc_resolve_path(path, FC_WEIGHT_BOLD, FC_SLANT_ROMAN, bold_path,
                             sizeof(bold_path)) &&
             strcmp(bold_path, path) != 0 &&
             load_face(g_font.ft_lib, bold_path, size_pt, &g_font.ft_face_bold,
                       &g_font.hb_font_bold)) {
    /* found bold variant of same family */
  } else {
    g_font.ft_face_bold = g_font.ft_face;
    g_font.hb_font_bold = g_font.hb_font;
  }

  /* Italic — same approach */
  char italic_path[512] = {0};
  if (italic_query && italic_query[0] &&
      fc_resolve_path(italic_query, FC_WEIGHT_REGULAR, FC_SLANT_ITALIC,
                      italic_path, sizeof(italic_path)) &&
      load_face(g_font.ft_lib, italic_path, size_pt, &g_font.ft_face_italic,
                &g_font.hb_font_italic)) {
    /* ok */
  } else if (fc_resolve_path(path, FC_WEIGHT_REGULAR, FC_SLANT_ITALIC,
                             italic_path, sizeof(italic_path)) &&
             strcmp(italic_path, path) != 0 &&
             load_face(g_font.ft_lib, italic_path, size_pt,
                       &g_font.ft_face_italic, &g_font.hb_font_italic)) {
    /* found italic variant */
  } else {
    g_font.ft_face_italic = g_font.ft_face;
    g_font.hb_font_italic = g_font.hb_font;
  }

  g_font.ready = true;
  return true;
}

/* ── Shared HarfBuzz buffer ────────────────────────────────────────────── */

static hb_buffer_t *s_hb_buf = NULL;
static hb_buffer_t *hb_buf_get(void) {
  if (!s_hb_buf)
    s_hb_buf = hb_buffer_create();
  hb_buffer_clear_contents(s_hb_buf);
  hb_buffer_set_direction(s_hb_buf, HB_DIRECTION_LTR);
  hb_buffer_set_script(s_hb_buf, HB_SCRIPT_LATIN);
  hb_buffer_set_language(s_hb_buf, hb_language_from_string("en", -1));
  return s_hb_buf;
}

/* ══════════════════════════════════════════════════════════════════════════
 * §2  canvas_* public API
 * ══════════════════════════════════════════════════════════════════════════ */

bool canvas_font_init(const char *path, const char *bold, const char *italic,
                      float size_pt) {
  return font_load(path, bold, italic, size_pt);
}

bool canvas_font_reload(const char *path, const char *bold, const char *italic,
                        float size_pt) {
  if (g_font.hb_font_italic && g_font.hb_font_italic != g_font.hb_font)
    hb_font_destroy(g_font.hb_font_italic);
  if (g_font.hb_font_bold && g_font.hb_font_bold != g_font.hb_font)
    hb_font_destroy(g_font.hb_font_bold);
  if (g_font.ft_face_italic && g_font.ft_face_italic != g_font.ft_face)
    FT_Done_Face(g_font.ft_face_italic);
  if (g_font.ft_face_bold && g_font.ft_face_bold != g_font.ft_face)
    FT_Done_Face(g_font.ft_face_bold);
  if (g_font.hb_font)
    hb_font_destroy(g_font.hb_font);
  if (g_font.ft_face)
    FT_Done_Face(g_font.ft_face);
  if (g_font.ft_lib)
    FT_Done_FreeType(g_font.ft_lib);
  memset(&g_font, 0, sizeof(g_font));
  gcache_clear();
  return font_load(path, bold, italic, size_pt);
}

int canvas_font_ascender(void) { return g_font.ascender; }
int canvas_font_height(void) { return g_font.height; }

/* ── Pixel blend helper ────────────────────────────────────────────────── */

static inline void blend_px(uint32_t *dst, uint8_t alpha, uint8_t r, uint8_t g,
                            uint8_t b) {
  if (!alpha)
    return;
  if (alpha == 255) {
    *dst = (0xffu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    return;
  }
  uint32_t d = *dst;
  uint32_t inv = 255 - alpha;
  uint8_t oa = (uint8_t)(alpha + (((d >> 24) & 0xff) * inv) / 255);
  uint8_t or_ = (uint8_t)((r * alpha + ((d >> 16) & 0xff) * inv) / 255);
  uint8_t og = (uint8_t)((g * alpha + ((d >> 8) & 0xff) * inv) / 255);
  uint8_t ob = (uint8_t)((b * alpha + (d & 0xff) * inv) / 255);
  *dst = ((uint32_t)oa << 24) | ((uint32_t)or_ << 16) | ((uint32_t)og << 8) |
         (uint32_t)ob;
}

/* ── canvas_draw_text ──────────────────────────────────────────────────── */

int canvas_draw_text(Canvas *c, int x, int y, const char *text, Color fg) {
  if (!g_font.ready || !text || !text[0])
    return 0;
  hb_buffer_t *buf = hb_buf_get();
  hb_buffer_add_utf8(buf, text, -1, 0, -1);
  hb_shape(g_font.hb_font, buf, NULL, 0);

  unsigned int gcount;
  hb_glyph_info_t *ginfo = hb_buffer_get_glyph_infos(buf, &gcount);
  hb_glyph_position_t *gpos = hb_buffer_get_glyph_positions(buf, &gcount);

  int pen_x = x;
  for (unsigned int i = 0; i < gcount; i++) {
    const GlyphCache *cg = gcache_get(ginfo[i].codepoint);
    if (!cg || !cg->bm)
      goto next;
    {
      int gx = pen_x + cg->bearing_x + (gpos[i].x_offset >> 6);
      int gy = y - cg->bearing_y + (gpos[i].y_offset >> 6);
      for (int row = 0; row < cg->bm_h; row++) {
        int py = gy + row;
        if (py < 0 || py >= c->h)
          continue;
        for (int col = 0; col < cg->bm_w; col++) {
          int px = gx + col;
          if (px < 0 || px >= c->w)
            continue;
          uint8_t a = cg->bm[row * cg->bm_pitch + col];
          if (a)
            blend_px(&c->px[py * c->stride + px], a, fg.r, fg.g, fg.b);
        }
      }
    }
  next:
    pen_x += gpos[i].x_advance >> 6;
  }
  return pen_x - x;
}

/* ── canvas_measure ────────────────────────────────────────────────────── */

int canvas_measure(const char *text) {
  if (!g_font.ready || !text || !text[0])
    return 0;
  hb_buffer_t *buf = hb_buf_get();
  hb_buffer_add_utf8(buf, text, -1, 0, -1);
  hb_shape(g_font.hb_font, buf, NULL, 0);
  unsigned int gcount;
  hb_glyph_position_t *gpos = hb_buffer_get_glyph_positions(buf, &gcount);
  int w = 0;
  for (unsigned int i = 0; i < gcount; i++)
    w += gpos[i].x_advance >> 6;
  return w;
}

/* ── canvas_fill_rect ──────────────────────────────────────────────────── */

void canvas_fill_rect(Canvas *c, int x, int y, int w, int h, Color col) {
  uint32_t pix;
  if (col.a == 255) {
    pix = (0xffu << 24) | ((uint32_t)col.r << 16) | ((uint32_t)col.g << 8) |
          col.b;
    for (int row = y; row < y + h && row < c->h; row++)
      for (int cx = x; cx < x + w && cx < c->w; cx++)
        if (row >= 0 && cx >= 0)
          c->px[row * c->stride + cx] = pix;
  } else {
    for (int row = y; row < y + h && row < c->h; row++)
      for (int cx = x; cx < x + w && cx < c->w; cx++)
        if (row >= 0 && cx >= 0)
          blend_px(&c->px[row * c->stride + cx], col.a, col.r, col.g, col.b);
  }
}

/* ── canvas_clear ──────────────────────────────────────────────────────── */

void canvas_clear(Canvas *c, Color col) {
  canvas_fill_rect(c, 0, 0, c->w, c->h, col);
}

/* ══════════════════════════════════════════════════════════════════════════
 * §3  wlr_buffer wrapper
 * ══════════════════════════════════════════════════════════════════════════ */

struct RawBuf {
  struct wlr_buffer base;
  uint32_t *data; /* owned copy — freed in rawbuf_destroy */
  int stride;
};

static void rawbuf_destroy(struct wlr_buffer *b) {
  struct RawBuf *rb = wl_container_of(b, rb, base);
  free(rb->data);
  free(rb);
}
static bool rawbuf_begin(struct wlr_buffer *b, uint32_t flags, void **data,
                         uint32_t *fmt, size_t *stride) {
  (void)flags;
  struct RawBuf *rb = wl_container_of(b, rb, base);
  *data = rb->data;
  *fmt = DRM_FORMAT_ARGB8888;
  *stride = (size_t)rb->stride;
  return true;
}
static void rawbuf_end(struct wlr_buffer *b) { (void)b; }

static const struct wlr_buffer_impl rawbuf_impl = {
    .destroy = rawbuf_destroy,
    .begin_data_ptr_access = rawbuf_begin,
    .end_data_ptr_access = rawbuf_end,
};

/* rawbuf_create: copies the pixel data — wlroots owns the copy */
static struct RawBuf *rawbuf_create(const uint32_t *px, int w, int h) {
  struct RawBuf *rb = calloc(1, sizeof(*rb));
  if (!rb)
    return NULL;
  rb->stride = w * 4;
  size_t sz = (size_t)(w * h) * 4;
  rb->data = malloc(sz);
  if (!rb->data) {
    free(rb);
    return NULL;
  }
  memcpy(rb->data, px, sz);
  wlr_buffer_init(&rb->base, &rawbuf_impl, w, h);
  return rb;
}

/* ══════════════════════════════════════════════════════════════════════════
 * §4  Wibox lifecycle
 * ══════════════════════════════════════════════════════════════════════════ */

TrixieWibox *wibox_create(TrixieServer *s, TrixieOutput *o, int x, int y, int w,
                          int h) {
  TrixieWibox *wb = calloc(1, sizeof(*wb));
  wb->x = x;
  wb->y = y;
  wb->w = w;
  wb->h = h;
  wb->visible = true;
  wb->lua_draw_ref = LUA_NOREF;
  wb->dirty = false; /* don't commit until Lua calls :redraw() */
  wb->L = s->L;
  wb->lua_ud_wb_ptr = NULL;

  wb->canvas.w = w;
  wb->canvas.h = h;
  wb->canvas.stride = w;
  wb->canvas.px = calloc((size_t)(w * h), 4);

  wb->scene_buf = wlr_scene_buffer_create(s->layer_overlay, NULL);
  wlr_scene_node_set_position(&wb->scene_buf->node, x, y);
  wlr_scene_node_set_enabled(&wb->scene_buf->node, true);

  if (!g_font.ready)
    canvas_font_init(o->server->cfg.font_path, o->server->cfg.font_path_bold,
                     o->server->cfg.font_path_italic, o->server->cfg.font_size);
  if (!g_font.ready)
    wlr_log(WLR_ERROR, "[wibox] font not ready — check font_path '%s'",
            o->server->cfg.font_path);
  else
    wlr_log(WLR_DEBUG, "[wibox] font ready: ascender=%d height=%d",
            g_font.ascender, g_font.height);
  return wb;
}

void wibox_mark_dirty(TrixieWibox *wb) {
  if (wb)
    wb->dirty = true;
}

void wibox_commit(TrixieWibox *wb) {
  if (!wb || !wb->dirty || !wb->canvas.px || !wb->scene_buf)
    return;
  struct RawBuf *rb = rawbuf_create(wb->canvas.px, wb->w, wb->h);
  wlr_scene_buffer_set_buffer(wb->scene_buf, &rb->base);
  wlr_buffer_drop(&rb->base);
  wb->dirty = false;
}

/* ── wibox_reset_output ────────────────────────────────────────────────────
 * Called from lua_reload.  Clears Lua callbacks and nulls back-pointers
 * WITHOUT touching scene nodes — safe to call from anywhere in the loop.
 * The scene nodes remain in the scene graph and are reused after reload. */
void wibox_reset_output(TrixieOutput *o, lua_State *L) {
  if (!o || !L)
    return;
  for (int i = 0; i < o->wibox_count; i++) {
    TrixieWibox *wb = o->wiboxes[i];
    if (!wb)
      continue;

    /* Unref draw callback — safe here, we are NOT inside GC */
    if (wb->lua_draw_ref != LUA_NOREF) {
      luaL_unref(L, LUA_REGISTRYINDEX, wb->lua_draw_ref);
      wb->lua_draw_ref = LUA_NOREF;
    }

    /* Null the old userdata's wb pointer so its __gc is a no-op */
    if (wb->lua_ud_wb_ptr) {
      *wb->lua_ud_wb_ptr = NULL;
      wb->lua_ud_wb_ptr = NULL;
    }

    /* Update to current Lua state (same state, but pointer is refreshed) */
    wb->L = L;

    /* Clear canvas so stale content doesn't flash before first redraw */
    memset(wb->canvas.px, 0, (size_t)(wb->w * wb->h) * 4);
    wb->dirty = false;
  }
}

/* ── wibox_clear_output ────────────────────────────────────────────────────
 * Fully destroys all wiboxes on an output.
 * Only call from output_handle_destroy or compositor shutdown — NEVER from
 * anywhere inside the wlroots event dispatch stack. */
void wibox_clear_output(TrixieOutput *o) {
  for (int i = 0; i < o->wibox_count; i++) {
    wibox_destroy(o->wiboxes[i]);
    o->wiboxes[i] = NULL;
  }
  o->wibox_count = 0;
}

/* ── wibox_destroy ─────────────────────────────────────────────────────────
 * Destroys a single wibox.  lua_draw_ref must already be LUA_NOREF
 * (cleared by wibox_reset_output or lwibox_gc) before calling this —
 * never call luaL_unref from here. */
void wibox_destroy(TrixieWibox *wb) {
  if (!wb)
    return;

  if (wb->lua_ud_wb_ptr) {
    *wb->lua_ud_wb_ptr = NULL;
    wb->lua_ud_wb_ptr = NULL;
  }
  /* Do NOT call luaL_unref here — may be called from within event dispatch */
  wb->lua_draw_ref = LUA_NOREF;
  wb->L = NULL;

  if (wb->scene_buf)
    wlr_scene_node_destroy(&wb->scene_buf->node);
  free(wb->canvas.px);
  free(wb);
}

/* ══════════════════════════════════════════════════════════════════════════
 * §5  Wibox Lua draw dispatch
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
  Canvas *c;
} CanvasUD;

void wibox_lua_draw(TrixieWibox *wb, lua_State *L) {
  if (!wb || !L || wb->lua_draw_ref == LUA_NOREF || !wb->visible)
    return;
  lua_rawgeti(L, LUA_REGISTRYINDEX, wb->lua_draw_ref);
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 1);
    return;
  }

  CanvasUD *ud = (CanvasUD *)lua_newuserdata(L, sizeof(CanvasUD));
  ud->c = &wb->canvas;
  luaL_setmetatable(L, "TrixieCanvas");

  if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
    wlr_log(WLR_ERROR, "[wibox] draw: %s", lua_tostring(L, -1));
    lua_pop(L, 1);
    wb->dirty = false; /* don't leave dirty on error — avoids infinite retry */
    return;
  }
  /* wb->dirty is already true (set by wb:redraw() or set_draw).
   * wibox_commit uploads the pixels and clears dirty. */
  wibox_commit(wb);
}

/* ══════════════════════════════════════════════════════════════════════════
 * §6  Legacy TrixieBar stubs
 * ══════════════════════════════════════════════════════════════════════════ */

struct TrixieBar {
  int _unused;
};

TrixieBar *bar_create(struct wlr_scene_tree *layer, int w, int h,
                      const Config *cfg) {
  (void)layer;
  (void)w;
  (void)h;
  if (!g_font.ready)
    canvas_font_init(cfg->font_path, cfg->font_path_bold, cfg->font_path_italic,
                     cfg->font_size);
  return calloc(1, sizeof(TrixieBar));
}

bool bar_update(TrixieBar *b, TwmState *twm, const Config *cfg) {
  (void)b;
  (void)twm;
  (void)cfg;
  return false;
}
void bar_destroy(TrixieBar *b) { free(b); }
void bar_mark_dirty(TrixieBar *b) { (void)b; }
void bar_set_visible(TrixieBar *b, bool v) {
  (void)b;
  (void)v;
}
void bar_resize(TrixieBar *b, int w, int h) {
  (void)b;
  (void)w;
  (void)h;
}
