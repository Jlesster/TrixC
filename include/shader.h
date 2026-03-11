#ifndef TRIXIE_SHADER_H
#define TRIXIE_SHADER_H

#include <GLES2/gl2.h>
#include <stdbool.h>
#include <stdint.h>

struct wlr_output;
struct wlr_renderer;
struct wlr_scene_output;

/* ---------------------------------------------------------------
 * Shader program helpers
 * ------------------------------------------------------------- */

GLuint shader_compile(const char *vert_src, const char *frag_src);
void   shader_destroy(GLuint prog);

/* ---------------------------------------------------------------
 * Per-output pipeline state
 * ------------------------------------------------------------- */

typedef struct TrixieShader {
  /* Saturation program — sampler2D only, no OES needed */
  GLuint prog;
  GLint  u_tex;
  GLint  u_saturation;
  GLint  a_pos;
  GLint  a_uv;

  /* OES variant kept for ABI compat but unused */
  GLuint prog_ext;
  GLint  u_tex_ext;
  GLint  u_saturation_ext;
  GLint  a_pos_ext;
  GLint  a_uv_ext;

  GLuint quad_vbo;
  GLuint quad_vao; /* GLES3 VAO — caches vertex attrib state to avoid
                      per-frame glVertexAttribPointer calls in run_quad() */

  /* Intermediate RGBA texture + FBO.
   * The scene buffer on nvidia is DMA-buf backed and exposed as
   * GL_TEXTURE_EXTERNAL_OES, which nvidia refuses to sample from a
   * custom FBO shader. We blit the scene into this plain RGBA tex
   * via glBlitFramebuffer, then sample that instead. */
  GLuint inter_tex;
  GLuint inter_fbo;

  /* Kept for ABI compat with old call sites */
  GLuint fbo;
  GLuint fbo_tex;

  int32_t width;
  int32_t height;

  bool ready;
  bool gl_init_done;
} TrixieShader;

/* ---------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------- */

bool shader_output_init(TrixieShader        *sh,
                        struct wlr_renderer *renderer,
                        int32_t              width,
                        int32_t              height);
bool shader_output_resize(TrixieShader *sh, int32_t width, int32_t height);
void shader_output_finish(TrixieShader *sh);

/* ---------------------------------------------------------------
 * Main entry point.
 * ------------------------------------------------------------- */

bool shader_render_frame(TrixieShader            *sh,
                         struct wlr_renderer     *renderer,
                         struct wlr_scene_output *scene_output,
                         struct wlr_output       *output,
                         float                    saturation);

/* ---------------------------------------------------------------
 * Legacy no-op stubs
 * ------------------------------------------------------------- */

void shader_begin_capture(TrixieShader *sh);
void shader_end_capture(TrixieShader *sh);
void shader_capture_backbuffer(TrixieShader *sh);
void shader_apply(TrixieShader *sh, float saturation);

#endif /* TRIXIE_SHADER_H */
