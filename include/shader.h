#ifndef TRIXIE_SHADER_H
#define TRIXIE_SHADER_H

#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <stdbool.h>
#include <stdint.h>

struct wlr_output;
struct wlr_renderer;
struct wlr_scene_output;

GLuint shader_compile(const char *vert_src, const char *frag_src);
void shader_destroy(GLuint prog);

typedef struct TrixieShader {
  GLuint prog;
  GLint u_tex;
  GLint u_saturation;
  GLint a_pos;
  GLint a_uv;

  GLuint prog_ext;
  GLint u_tex_ext;
  GLint u_saturation_ext;
  GLint a_pos_ext;
  GLint a_uv_ext;

  GLuint quad_vbo;
  GLuint quad_vao;

  GLuint inter_tex;
  GLuint inter_fbo;

  GLuint fbo;
  GLuint fbo_tex;

  int32_t width;
  int32_t height;

  bool ready;
  bool gl_init_done;
} TrixieShader;

bool shader_output_init(TrixieShader *sh, struct wlr_renderer *renderer,
                        int32_t width, int32_t height);
bool shader_output_resize(TrixieShader *sh, int32_t width, int32_t height);
void shader_output_finish(TrixieShader *sh);

bool shader_render_frame(TrixieShader *sh, struct wlr_renderer *renderer,
                         struct wlr_scene_output *scene_output,
                         struct wlr_output *output, float saturation);

void shader_begin_capture(TrixieShader *sh);
void shader_end_capture(TrixieShader *sh);
void shader_capture_backbuffer(TrixieShader *sh);
void shader_apply(TrixieShader *sh, float saturation);

#endif /* TRIXIE_SHADER_H */
