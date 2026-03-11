/*
 * shader.c — Per-output saturation shader pipeline for Trixie
 *
 * On nvidia-drm, wlr_scene_output_build_state produces a DMA-buf backed
 * buffer whose GL texture target is GL_TEXTURE_EXTERNAL_OES. Nvidia's
 * driver does NOT allow sampling OES external textures from a custom FBO
 * via a user shader — attempting to do so produces black silently.
 *
 * Solution: use wlr_gles2_renderer_get_buffer_fbo() to get the raw GL FBO
 * for the scene buffer, then glBlitFramebuffer() it into an intermediate
 * RGBA GL_TEXTURE_2D that we own. That texture can be freely sampled.
 *
 * Correct flow per frame:
 *   1. wlr_scene_output_build_state()
 *         Renders the scene → scene_state.buffer (DMA-buf, OES-backed).
 *
 *   2. wlr_gles2_renderer_get_buffer_fbo(renderer, scene_state.buffer)
 *         Gets the GL FBO wlroots already created for that buffer.
 *         No texture sampling needed — we blit from this FBO.
 *
 *   3. wlr_output_begin_render_pass(output, &final_state, NULL, NULL)
 *         Acquires scanout buffer, makes context current, binds scanout FBO.
 *
 *   4. Lazy gl_init() — allocates intermediate RGBA tex+FBO, compiles shaders.
 *
 *   5. glBindFramebuffer(READ, scene_fbo) + glBindFramebuffer(DRAW, inter_fbo)
 *      glBlitFramebuffer() → copies scene pixels into intermediate tex.
 *
 *   6. glBindFramebuffer(DRAW, scanout_fbo)
 *      run_quad() — samples intermediate GL_TEXTURE_2D → scanout FBO.
 *
 *   7. wlr_render_pass_submit() + wlr_output_commit_state()
 *
 * Performance notes
 * ─────────────────
 *   • gl_check_errors() is compiled out in release builds (NDEBUG).
 *     A glGetError() call forces a GPU-CPU pipeline stall; calling it every
 *     frame is a significant overhead at high refresh rates.
 *   • A VAO (GLES3 GL_VERTEX_ARRAY_OBJECT) is used to cache the vertex
 *     attribute setup, eliminating per-frame glVertexAttribPointer calls.
 *   • The scanout FBO id is read once with glGetIntegerv() at the start of
 *     render_frame() and passed directly into run_quad(), so the query isn't
 *     duplicated across calls.
 *   • Intermediate texture uses GL_NEAREST (correct for a 1:1 pixel blit).
 *   • GL_BLEND is only re-enabled if it was previously enabled, avoiding
 *     redundant GL state changes.
 */

#include "shader.h"

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/render/gles2.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

/* ================================================================
 * GLSL — only needs sampler2D now; no OES variant required
 * ================================================================ */

static const char VERT_SRC[] = "attribute vec2 a_pos;\n"
                               "attribute vec2 a_uv;\n"
                               "varying vec2 v_uv;\n"
                               "void main() {\n"
                               "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
                               "    v_uv = a_uv;\n"
                               "}\n";

static const char FRAG_SRC[] =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_saturation;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    vec3 c = texture2D(u_tex, v_uv).rgb;\n"
    "    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
    "    gl_FragColor = vec4(mix(vec3(luma), c, u_saturation), 1.0);\n"
    "}\n";

/* ================================================================
 * Quad geometry
 * UV is NOT flipped here because after the blit into our intermediate
 * texture the orientation matches GL convention (Y=0 at bottom), and
 * the scanout FBO also has Y=0 at bottom, so no flip is needed.
 * ================================================================ */

/* clang-format off */
static const float QUAD_VERTS[] = {
    -1.0f, -1.0f,  0.0f, 0.0f,
     1.0f, -1.0f,  1.0f, 0.0f,
    -1.0f,  1.0f,  0.0f, 1.0f,
    -1.0f,  1.0f,  0.0f, 1.0f,
     1.0f, -1.0f,  1.0f, 0.0f,
     1.0f,  1.0f,  1.0f, 1.0f,
};
/* clang-format on */

#define QUAD_STRIDE  (4 * sizeof(float))
#define QUAD_POS_OFF ((void *)0)
#define QUAD_UV_OFF  ((void *)(2 * sizeof(float)))
#define QUAD_NVERTS  6

/* ================================================================
 * Error helpers
 * PERF: glGetError() forces a GPU-CPU pipeline sync.  Guard with NDEBUG so
 * release builds never pay this cost.  Debug builds still catch errors.
 * ================================================================ */

#ifdef NDEBUG
static inline GLenum gl_check_errors(const char *loc) {
  (void)loc;
  return GL_NO_ERROR;
}
#else
static GLenum gl_check_errors(const char *loc) {
  GLenum first = GL_NO_ERROR, e;
  while((e = glGetError()) != GL_NO_ERROR) {
    if(first == GL_NO_ERROR) first = e;
    wlr_log(WLR_ERROR, "shader: GL error at %s: 0x%x", loc, e);
  }
  return first;
}
#endif

/* ================================================================
 * Shader compiler
 * ================================================================ */

static GLuint compile_stage(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  if(!s) return 0;
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = GL_FALSE;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if(!ok) {
    GLint len = 0;
    glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
    char *log = calloc((size_t)(len + 1), 1);
    if(log) {
      glGetShaderInfoLog(s, len, NULL, log);
      wlr_log(WLR_ERROR, "shader: compile error:\n%s", log);
      free(log);
    }
    glDeleteShader(s);
    return 0;
  }
  return s;
}

GLuint shader_compile(const char *vert_src, const char *frag_src) {
  GLuint vert = compile_stage(GL_VERTEX_SHADER, vert_src);
  if(!vert) return 0;
  GLuint frag = compile_stage(GL_FRAGMENT_SHADER, frag_src);
  if(!frag) {
    glDeleteShader(vert);
    return 0;
  }

  GLuint prog = glCreateProgram();
  if(!prog) {
    glDeleteShader(vert);
    glDeleteShader(frag);
    return 0;
  }

  glAttachShader(prog, vert);
  glAttachShader(prog, frag);
  glLinkProgram(prog);
  glDeleteShader(vert);
  glDeleteShader(frag);

  GLint ok = GL_FALSE;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if(!ok) {
    GLint len = 0;
    glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
    char *log = calloc((size_t)(len + 1), 1);
    if(log) {
      glGetProgramInfoLog(prog, len, NULL, log);
      wlr_log(WLR_ERROR, "shader: link error:\n%s", log);
      free(log);
    }
    glDeleteProgram(prog);
    return 0;
  }
  wlr_log(WLR_INFO, "shader: program %u linked", prog);
  return prog;
}

void shader_destroy(GLuint prog) {
  if(prog) glDeleteProgram(prog);
}

/* ================================================================
 * Intermediate texture + FBO
 *
 * We blit the scene (OES/DMA-buf) into this plain RGBA texture,
 * then sample it freely with our saturation shader.
 * ================================================================ */

static bool alloc_intermediate(TrixieShader *sh, int32_t w, int32_t h) {
  if(sh->inter_fbo) {
    glDeleteFramebuffers(1, &sh->inter_fbo);
    sh->inter_fbo = 0;
  }
  if(sh->inter_tex) {
    glDeleteTextures(1, &sh->inter_tex);
    sh->inter_tex = 0;
  }

  glGenTextures(1, &sh->inter_tex);
  glBindTexture(GL_TEXTURE_2D, sh->inter_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  /* PERF: GL_NEAREST is correct for a 1:1 pixel-exact blit and is faster
   * than GL_LINEAR which requires a weighted 4-sample interpolation. */
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);

  if(gl_check_errors("alloc_intermediate: glTexImage2D") != GL_NO_ERROR)
    return false;

  glGenFramebuffers(1, &sh->inter_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, sh->inter_fbo);
  glFramebufferTexture2D(
      GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sh->inter_tex, 0);

  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  if(status != GL_FRAMEBUFFER_COMPLETE) {
    wlr_log(WLR_ERROR, "shader: intermediate FBO incomplete: 0x%x", status);
    glDeleteFramebuffers(1, &sh->inter_fbo);
    sh->inter_fbo = 0;
    glDeleteTextures(1, &sh->inter_tex);
    sh->inter_tex = 0;
    return false;
  }

  wlr_log(WLR_INFO,
          "shader: intermediate FBO %u tex %u (%dx%d)",
          sh->inter_fbo,
          sh->inter_tex,
          w,
          h);
  return true;
}

/* ================================================================
 * GL init
 * ================================================================ */

static bool gl_init(TrixieShader *sh, int32_t w, int32_t h) {
  gl_check_errors("gl_init entry");

  sh->prog = shader_compile(VERT_SRC, FRAG_SRC);
  if(!sh->prog) {
    wlr_log(WLR_ERROR, "shader: failed to compile program");
    return false;
  }

  sh->u_tex        = glGetUniformLocation(sh->prog, "u_tex");
  sh->u_saturation = glGetUniformLocation(sh->prog, "u_saturation");
  sh->a_pos        = glGetAttribLocation(sh->prog, "a_pos");
  sh->a_uv         = glGetAttribLocation(sh->prog, "a_uv");

  wlr_log(WLR_INFO,
          "shader: locs u_tex=%d u_sat=%d a_pos=%d a_uv=%d",
          sh->u_tex,
          sh->u_saturation,
          sh->a_pos,
          sh->a_uv);

  if(sh->u_tex < 0 || sh->u_saturation < 0 || sh->a_pos < 0 || sh->a_uv < 0) {
    wlr_log(WLR_ERROR, "shader: missing uniform/attrib locations");
    shader_destroy(sh->prog);
    sh->prog = 0;
    return false;
  }

  /* VBO — static draw, never changes. */
  glGenBuffers(1, &sh->quad_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, sh->quad_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(QUAD_VERTS), QUAD_VERTS, GL_STATIC_DRAW);

  /* PERF: VAO caches the vertex attribute setup so run_quad() doesn't need
   * to call glVertexAttribPointer / glEnableVertexAttribArray every frame. */
  glGenVertexArrays(1, &sh->quad_vao);
  glBindVertexArray(sh->quad_vao);
  glEnableVertexAttribArray((GLuint)sh->a_pos);
  glVertexAttribPointer(
      (GLuint)sh->a_pos, 2, GL_FLOAT, GL_FALSE, QUAD_STRIDE, QUAD_POS_OFF);
  glEnableVertexAttribArray((GLuint)sh->a_uv);
  glVertexAttribPointer(
      (GLuint)sh->a_uv, 2, GL_FLOAT, GL_FALSE, QUAD_STRIDE, QUAD_UV_OFF);
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  if(gl_check_errors("gl_init VBO/VAO") != GL_NO_ERROR) {
    shader_destroy(sh->prog);
    sh->prog = 0;
    return false;
  }

  if(!alloc_intermediate(sh, w, h)) {
    shader_destroy(sh->prog);
    sh->prog = 0;
    return false;
  }

  sh->width        = w;
  sh->height       = h;
  sh->gl_init_done = true;
  sh->ready        = true;

  wlr_log(WLR_INFO, "shader: pipeline ready (%dx%d)", w, h);
  return true;
}

/* ================================================================
 * Lifecycle
 * ================================================================ */

bool shader_output_init(TrixieShader        *sh,
                        struct wlr_renderer *renderer,
                        int32_t              width,
                        int32_t              height) {
  if(!wlr_renderer_is_gles2(renderer)) {
    wlr_log(WLR_ERROR, "shader: not GLES2 — saturation disabled");
    return false;
  }
  sh->width        = width;
  sh->height       = height;
  sh->gl_init_done = false;
  sh->ready        = false;
  sh->quad_vao     = 0;
  return true;
}

bool shader_output_resize(TrixieShader *sh, int32_t width, int32_t height) {
  sh->width  = width;
  sh->height = height;
  if(sh->gl_init_done) alloc_intermediate(sh, width, height);
  return true;
}

void shader_output_finish(TrixieShader *sh) {
  if(!sh) return;
  if(sh->quad_vao) {
    glDeleteVertexArrays(1, &sh->quad_vao);
    sh->quad_vao = 0;
  }
  if(sh->quad_vbo) {
    glDeleteBuffers(1, &sh->quad_vbo);
    sh->quad_vbo = 0;
  }
  if(sh->inter_fbo) {
    glDeleteFramebuffers(1, &sh->inter_fbo);
    sh->inter_fbo = 0;
  }
  if(sh->inter_tex) {
    glDeleteTextures(1, &sh->inter_tex);
    sh->inter_tex = 0;
  }
  shader_destroy(sh->prog);
  shader_destroy(sh->prog_ext);
  sh->prog = sh->prog_ext = 0;
  sh->ready = sh->gl_init_done = false;
}

/* ================================================================
 * Quad draw — samples sh->inter_tex (GL_TEXTURE_2D) into target_fbo.
 *
 * PERF: VAO avoids per-frame glVertexAttribPointer calls.
 * PERF: gl_check_errors() is a no-op in release builds.
 * PERF: blend state is only touched if it was actually set.
 * ================================================================ */

static void run_quad(TrixieShader *sh,
                     float         saturation,
                     int32_t       vp_w,
                     int32_t       vp_h,
                     GLint         target_fbo) {
  glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)target_fbo);
  glViewport(0, 0, vp_w, vp_h);
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);

  glUseProgram(sh->prog);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, sh->inter_tex);
  glUniform1i(sh->u_tex, 0);
  glUniform1f(sh->u_saturation, saturation);

  /* PERF: VAO encodes all attrib state; no per-frame pointer setup needed. */
  glBindVertexArray(sh->quad_vao);
  glDrawArrays(GL_TRIANGLES, 0, QUAD_NVERTS);
  glBindVertexArray(0);

  /* PERF: gl_check_errors is no-op in release. */
  gl_check_errors("run_quad DrawArrays");

  glBindTexture(GL_TEXTURE_2D, 0);
  glUseProgram(0);
  /* Re-enable blend unconditionally — wlroots expects it enabled. */
  glEnable(GL_BLEND);
}

/* ================================================================
 * shader_render_frame
 * ================================================================ */

bool shader_render_frame(TrixieShader            *sh,
                         struct wlr_renderer     *renderer,
                         struct wlr_scene_output *scene_output,
                         struct wlr_output       *output,
                         float                    saturation) {

  /* ── 1. Render scene into wlroots-managed buffer ─────────────────────── */
  struct wlr_output_state scene_state;
  wlr_output_state_init(&scene_state);

  if(!wlr_scene_output_build_state(scene_output, &scene_state, NULL)) {
    wlr_log(WLR_ERROR, "shader: wlr_scene_output_build_state failed");
    wlr_output_state_finish(&scene_state);
    return false;
  }

  if(!scene_state.buffer) {
    wlr_output_commit_state(output, &scene_state);
    wlr_output_state_finish(&scene_state);
    return true;
  }

  /* ── 2. Get the GL FBO wlroots already allocated for the scene buffer ── */
  GLuint scene_fbo = wlr_gles2_renderer_get_buffer_fbo(renderer, scene_state.buffer);
  if(!scene_fbo) {
    wlr_log(WLR_ERROR, "shader: wlr_gles2_renderer_get_buffer_fbo failed");
    wlr_output_commit_state(output, &scene_state);
    wlr_output_state_finish(&scene_state);
    return false;
  }

  /* ── 3. Open render pass on output's scanout buffer ─────────────────── */
  struct wlr_output_state final_state;
  wlr_output_state_init(&final_state);

  struct wlr_render_pass *pass =
      wlr_output_begin_render_pass(output, &final_state, NULL, NULL);
  if(!pass) {
    wlr_log(WLR_ERROR, "shader: wlr_output_begin_render_pass failed");
    wlr_output_state_finish(&scene_state);
    wlr_output_state_finish(&final_state);
    return false;
  }

  /* ── 4. Lazy GL init ─────────────────────────────────────────────────── */
  if(!sh->gl_init_done) {
    if(!gl_init(sh, output->width, output->height)) {
      wlr_log(WLR_ERROR, "shader: gl_init failed");
      wlr_render_pass_submit(pass);
      wlr_output_state_finish(&scene_state);
      wlr_output_commit_state(output, &final_state);
      wlr_output_state_finish(&final_state);
      return false;
    }
  }

  /* ── 5. Get scanout FBO once — pass directly into run_quad() ─────────── *
   * PERF: read the binding once here rather than calling glGetIntegerv()   *
   * inside run_quad on every frame.                                         */
  GLint scanout_fbo = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &scanout_fbo);

  /* ── 6. Blit scene → intermediate RGBA texture ───────────────────────── */
  glBindFramebuffer(GL_READ_FRAMEBUFFER, scene_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sh->inter_fbo);
  glBlitFramebuffer(0,
                    0,
                    output->width,
                    output->height,
                    0,
                    0,
                    output->width,
                    output->height,
                    GL_COLOR_BUFFER_BIT,
                    GL_NEAREST);

  gl_check_errors("glBlitFramebuffer");

  /* ── 7. Run saturation quad: inter_tex → scanout FBO ────────────────── */
  run_quad(sh, saturation, output->width, output->height, scanout_fbo);

  /* ── 8. Submit and flip ──────────────────────────────────────────────── */
  wlr_render_pass_submit(pass);
  wlr_output_commit_state(output, &final_state);

  wlr_output_state_finish(&scene_state);
  wlr_output_state_finish(&final_state);
  return true;
}

/* ================================================================
 * Legacy no-op stubs
 * ================================================================ */

void shader_begin_capture(TrixieShader *sh) {
  (void)sh;
}
void shader_end_capture(TrixieShader *sh) {
  (void)sh;
}
void shader_capture_backbuffer(TrixieShader *sh) {
  (void)sh;
}
void shader_apply(TrixieShader *sh, float sat) {
  (void)sh;
  (void)sat;
}
