/*
 * shader.c — Per-output saturation shader pipeline for Trixie
 *
 * Falls back to a passthrough (no saturation) if wlroots doesn't expose
 * wlr_gles2_renderer_get_buffer_fbo (added in wlroots 0.18.2).
 * The meson.build detects availability and sets -DHAVE_WLR_GLES2_FBO.
 */

#include "shader.h"
/* Map header's opaque aliases to real GL types now that we have GL headers.
 * TrixieGLuint == GLuint == unsigned int; TrixieGLint == GLint == int.    */
#define TrixieGLuint GLuint
#define TrixieGLint  GLint

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

/* ── GLSL ───────────────────────────────────────────────────────────────────
 */

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

/* ── Quad geometry ──────────────────────────────────────────────────────────
 */

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

#define QUAD_STRIDE (4 * sizeof(float))
#define QUAD_POS_OFF ((void *)0)
#define QUAD_UV_OFF ((void *)(2 * sizeof(float)))
#define QUAD_NVERTS 6

/* ── Error helpers ──────────────────────────────────────────────────────────
 */

#ifdef NDEBUG
static inline GLenum gl_check_errors(const char *loc) {
  (void)loc;
  return GL_NO_ERROR;
}
#else
static GLenum gl_check_errors(const char *loc) {
  GLenum first = GL_NO_ERROR, e;
  while ((e = glGetError()) != GL_NO_ERROR) {
    if (first == GL_NO_ERROR)
      first = e;
    wlr_log(WLR_ERROR, "shader: GL error at %s: 0x%x", loc, e);
  }
  return first;
}
#endif

/* ── Shader compiler ────────────────────────────────────────────────────────
 */

static GLuint compile_stage(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  if (!s)
    return 0;
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = GL_FALSE;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    GLint len = 0;
    glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
    char *log = calloc((size_t)(len + 1), 1);
    if (log) {
      glGetShaderInfoLog(s, len, NULL, log);
      wlr_log(WLR_ERROR, "shader compile:\n%s", log);
      free(log);
    }
    glDeleteShader(s);
    return 0;
  }
  return s;
}

GLuint shader_compile(const char *vert_src, const char *frag_src) {
  GLuint vert = compile_stage(GL_VERTEX_SHADER, vert_src);
  if (!vert)
    return 0;
  GLuint frag = compile_stage(GL_FRAGMENT_SHADER, frag_src);
  if (!frag) {
    glDeleteShader(vert);
    return 0;
  }
  GLuint prog = glCreateProgram();
  if (!prog) {
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
  if (!ok) {
    GLint len = 0;
    glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
    char *log = calloc((size_t)(len + 1), 1);
    if (log) {
      glGetProgramInfoLog(prog, len, NULL, log);
      wlr_log(WLR_ERROR, "shader link:\n%s", log);
      free(log);
    }
    glDeleteProgram(prog);
    return 0;
  }
  wlr_log(WLR_INFO, "shader: program %u linked", prog);
  return prog;
}

void shader_destroy(GLuint prog) {
  if (prog)
    glDeleteProgram(prog);
}

/* ── Intermediate FBO ───────────────────────────────────────────────────────
 */

static bool alloc_intermediate(TrixieShader *sh, int32_t w, int32_t h) {
  if (sh->inter_fbo) {
    glDeleteFramebuffers(1, &sh->inter_fbo);
    sh->inter_fbo = 0;
  }
  if (sh->inter_tex) {
    glDeleteTextures(1, &sh->inter_tex);
    sh->inter_tex = 0;
  }
  glGenTextures(1, &sh->inter_tex);
  glBindTexture(GL_TEXTURE_2D, sh->inter_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  if (gl_check_errors("alloc_intermediate texImage") != GL_NO_ERROR)
    return false;
  glGenFramebuffers(1, &sh->inter_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, sh->inter_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         sh->inter_tex, 0);
  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    wlr_log(WLR_ERROR, "shader: intermediate FBO incomplete: 0x%x", status);
    glDeleteFramebuffers(1, &sh->inter_fbo);
    sh->inter_fbo = 0;
    glDeleteTextures(1, &sh->inter_tex);
    sh->inter_tex = 0;
    return false;
  }
  wlr_log(WLR_INFO, "shader: intermediate FBO %u tex %u (%dx%d)", sh->inter_fbo,
          sh->inter_tex, w, h);
  return true;
}

/* ── GL init ────────────────────────────────────────────────────────────────
 */

static bool gl_init(TrixieShader *sh, int32_t w, int32_t h) {
  sh->prog = shader_compile(VERT_SRC, FRAG_SRC);
  if (!sh->prog)
    return false;
  sh->u_tex = glGetUniformLocation(sh->prog, "u_tex");
  sh->u_saturation = glGetUniformLocation(sh->prog, "u_saturation");
  sh->a_pos = glGetAttribLocation(sh->prog, "a_pos");
  sh->a_uv = glGetAttribLocation(sh->prog, "a_uv");
  if (sh->u_tex < 0 || sh->u_saturation < 0 || sh->a_pos < 0 || sh->a_uv < 0) {
    wlr_log(WLR_ERROR, "shader: missing uniform/attrib locations");
    shader_destroy(sh->prog);
    sh->prog = 0;
    return false;
  }
  glGenBuffers(1, &sh->quad_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, sh->quad_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(QUAD_VERTS), QUAD_VERTS, GL_STATIC_DRAW);
  glGenVertexArrays(1, &sh->quad_vao);
  glBindVertexArray(sh->quad_vao);
  glEnableVertexAttribArray((GLuint)sh->a_pos);
  glVertexAttribPointer((GLuint)sh->a_pos, 2, GL_FLOAT, GL_FALSE, QUAD_STRIDE,
                        QUAD_POS_OFF);
  glEnableVertexAttribArray((GLuint)sh->a_uv);
  glVertexAttribPointer((GLuint)sh->a_uv, 2, GL_FLOAT, GL_FALSE, QUAD_STRIDE,
                        QUAD_UV_OFF);
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  if (gl_check_errors("gl_init VBO/VAO") != GL_NO_ERROR) {
    shader_destroy(sh->prog);
    sh->prog = 0;
    return false;
  }
  if (!alloc_intermediate(sh, w, h)) {
    shader_destroy(sh->prog);
    sh->prog = 0;
    return false;
  }
  sh->width = w;
  sh->height = h;
  sh->gl_init_done = true;
  sh->ready = true;
  wlr_log(WLR_INFO, "shader: pipeline ready (%dx%d)", w, h);
  return true;
}

/* ── Lifecycle ──────────────────────────────────────────────────────────────
 */

bool shader_output_init(TrixieShader *sh, struct wlr_renderer *renderer,
                        int32_t width, int32_t height) {
  if (!wlr_renderer_is_gles2(renderer)) {
    wlr_log(WLR_ERROR, "shader: not GLES2 — saturation disabled");
    return false;
  }
  sh->width = width;
  sh->height = height;
  sh->gl_init_done = false;
  sh->ready = false;
  sh->quad_vao = 0;
  return true;
}

bool shader_output_resize(TrixieShader *sh, int32_t width, int32_t height) {
  sh->width = width;
  sh->height = height;
  if (sh->gl_init_done)
    alloc_intermediate(sh, width, height);
  return true;
}

void shader_output_finish(TrixieShader *sh) {
  if (!sh)
    return;
  if (sh->quad_vao) {
    glDeleteVertexArrays(1, &sh->quad_vao);
    sh->quad_vao = 0;
  }
  if (sh->quad_vbo) {
    glDeleteBuffers(1, &sh->quad_vbo);
    sh->quad_vbo = 0;
  }
  if (sh->inter_fbo) {
    glDeleteFramebuffers(1, &sh->inter_fbo);
    sh->inter_fbo = 0;
  }
  if (sh->inter_tex) {
    glDeleteTextures(1, &sh->inter_tex);
    sh->inter_tex = 0;
  }
  shader_destroy(sh->prog);
  shader_destroy(sh->prog_ext);
  sh->prog = sh->prog_ext = 0;
  sh->ready = sh->gl_init_done = false;
}

/* ── Quad draw ──────────────────────────────────────────────────────────────
 */

static void run_quad(TrixieShader *sh, float saturation, int32_t vp_w,
                     int32_t vp_h, GLint target_fbo) {
  glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)target_fbo);
  glViewport(0, 0, vp_w, vp_h);
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glUseProgram(sh->prog);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, sh->inter_tex);
  glUniform1i(sh->u_tex, 0);
  glUniform1f(sh->u_saturation, saturation);
  glBindVertexArray(sh->quad_vao);
  glDrawArrays(GL_TRIANGLES, 0, QUAD_NVERTS);
  glBindVertexArray(0);
  gl_check_errors("run_quad");
  glBindTexture(GL_TEXTURE_2D, 0);
  glUseProgram(0);
  glEnable(GL_BLEND);
}

/* ── Main entry ─────────────────────────────────────────────────────────────
 */

bool shader_render_frame(TrixieShader *sh, struct wlr_renderer *renderer,
                         struct wlr_scene_output *scene_output,
                         struct wlr_output *output, float saturation) {

#ifndef HAVE_WLR_GLES2_FBO
  /* wlr_gles2_renderer_get_buffer_fbo not available — fall back to plain
   * scene commit with no saturation shader.  Everything still works.      */
  (void)sh;
  (void)renderer;
  (void)saturation;
  wlr_scene_output_commit(scene_output, NULL);
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  wlr_scene_output_send_frame_done(scene_output, &now);
  return true;
#else
  /* ── 1. Render scene ─────────────────────────────────────────────────── */
  struct wlr_output_state scene_state;
  wlr_output_state_init(&scene_state);
  if (!wlr_scene_output_build_state(scene_output, &scene_state, NULL)) {
    wlr_log(WLR_ERROR, "shader: wlr_scene_output_build_state failed");
    wlr_output_state_finish(&scene_state);
    return false;
  }
  if (!scene_state.buffer) {
    wlr_output_commit_state(output, &scene_state);
    wlr_output_state_finish(&scene_state);
    return true;
  }

  /* ── 2. Get scene FBO ────────────────────────────────────────────────── */
  GLuint scene_fbo =
      wlr_gles2_renderer_get_buffer_fbo(renderer, scene_state.buffer);
  if (!scene_fbo) {
    wlr_log(WLR_ERROR, "shader: wlr_gles2_renderer_get_buffer_fbo failed");
    wlr_output_commit_state(output, &scene_state);
    wlr_output_state_finish(&scene_state);
    return false;
  }

  /* ── 3. Open render pass ─────────────────────────────────────────────── */
  struct wlr_output_state final_state;
  wlr_output_state_init(&final_state);
  struct wlr_render_pass *pass =
      wlr_output_begin_render_pass(output, &final_state, NULL, NULL);
  if (!pass) {
    wlr_log(WLR_ERROR, "shader: wlr_output_begin_render_pass failed");
    wlr_output_state_finish(&scene_state);
    wlr_output_state_finish(&final_state);
    return false;
  }

  /* ── 4. Lazy GL init ─────────────────────────────────────────────────── */
  if (!sh->gl_init_done && !gl_init(sh, output->width, output->height)) {
    wlr_log(WLR_ERROR, "shader: gl_init failed");
    wlr_render_pass_submit(pass);
    wlr_output_state_finish(&scene_state);
    wlr_output_commit_state(output, &final_state);
    wlr_output_state_finish(&final_state);
    return false;
  }

  GLint scanout_fbo = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &scanout_fbo);

  /* ── 5. Blit scene → intermediate ───────────────────────────────────── */
  glBindFramebuffer(GL_READ_FRAMEBUFFER, scene_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sh->inter_fbo);
  glBlitFramebuffer(0, 0, output->width, output->height, 0, 0, output->width,
                    output->height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  gl_check_errors("glBlitFramebuffer");

  /* ── 6. Run saturation quad ──────────────────────────────────────────── */
  run_quad(sh, saturation, output->width, output->height, scanout_fbo);

  /* ── 7. Submit ───────────────────────────────────────────────────────── */
  wlr_render_pass_submit(pass);
  wlr_output_commit_state(output, &final_state);
  wlr_output_state_finish(&scene_state);
  wlr_output_state_finish(&final_state);
  return true;
#endif
}

/* ── Legacy stubs ───────────────────────────────────────────────────────────
 */
void shader_begin_capture(TrixieShader *sh) { (void)sh; }
void shader_end_capture(TrixieShader *sh) { (void)sh; }
void shader_capture_backbuffer(TrixieShader *sh) { (void)sh; }
void shader_apply(TrixieShader *sh, float sat) {
  (void)sh;
  (void)sat;
}
