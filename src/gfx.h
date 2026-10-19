#ifndef GFX_H
#define GFX_H

#include <vitaGL.h>

#define RGBA8(r, g, b, a) ((((a) & 0xFF) << 24) | (((b) & 0xFF) << 16) | (((g) & 0xFF) << 8) | ((r) & 0xFF))

typedef struct gfx_texture gfx_texture;
typedef struct gfx_font gfx_font;

void gfx_init(void);
void gfx_set_clear_color(unsigned int color);
void gfx_begin_frame(void);
void gfx_end_frame(void);
void gfx_wait_rendering_done(void);

gfx_texture *gfx_create_texture(unsigned int w, unsigned int h, GLenum format, GLenum type);
gfx_texture *gfx_load_png(const char *path);
void gfx_free_texture(gfx_texture *tex);
unsigned int gfx_texture_get_width(const gfx_texture *tex);
unsigned int gfx_texture_get_height(const gfx_texture *tex);
unsigned int gfx_texture_get_stride(const gfx_texture *tex);
void *gfx_texture_get_datap(const gfx_texture *tex);
void gfx_texture_set_bgr565(gfx_texture *tex);

void gfx_draw_rectangle(float x, float y, float w, float h, unsigned int color);
void gfx_draw_texture(const gfx_texture *tex, float x, float y);
void gfx_draw_texture_tint(const gfx_texture *tex, float x, float y, unsigned int color);
void gfx_draw_texture_scale(const gfx_texture *tex, float x, float y, float x_scale, float y_scale);
void gfx_draw_texture_tint_scale(const gfx_texture *tex, float x, float y, float x_scale, float y_scale, unsigned int color);
void gfx_draw_texture_part_scale(const gfx_texture *tex, float x, float y, float tex_x, float tex_y,
      float tex_w, float tex_h, float x_scale, float y_scale);
void gfx_draw_texture_tint_part_scale(const gfx_texture *tex, float x, float y, float tex_x, float tex_y,
      float tex_w, float tex_h, float x_scale, float y_scale, unsigned int color);

gfx_font *gfx_load_font(const char *path);
void gfx_free_font(gfx_font *font);
int gfx_font_draw_text(gfx_font *font, int x, int y, unsigned int color, unsigned int size, const char *text);
int gfx_font_text_width(gfx_font *font, unsigned int size, const char *text);

#endif
