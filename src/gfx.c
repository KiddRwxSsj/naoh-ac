#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <png.h>
#include <ft2build.h>
#include <psp2/gxm.h>
#include FT_FREETYPE_H

#include "gfx.h"

#define DISPLAY_W 960
#define DISPLAY_H 544

// vitaGL takes every byte of user ram above this for its own pools, so
// this is what's left over for thread stacks, audio ports and the shader
// compiler module. the cores live in newlib's heap, which is already
// carved out before main() runs
#define VGL_RAM_THRESHOLD (16 * 1024 * 1024)

#define ATLAS_W 1024
#define ATLAS_H 1024
#define MAX_FONT_SIZE 64
#define TEXT_BATCH 128

struct gfx_texture {
   GLuint id;
   unsigned int w, h;
   unsigned int stride;
   void *data;
};

typedef struct {
   unsigned char cached;
   unsigned short size;
   short x, y, w, h;
   short left, top;
   int advance;
} glyph_t;

// one glyph table per pixel size, each allocated the first time that
// size gets used
struct gfx_font {
   FT_Library lib;
   FT_Face face;
   gfx_texture *atlas;
   glyph_t *sizes[MAX_FONT_SIZE + 1];
   int shelf_x, shelf_y, shelf_h;
};

static int textured = 0;

static GLfloat batch_pos[TEXT_BATCH * 12];
static GLfloat batch_uv[TEXT_BATCH * 12];

static void set_color(unsigned int color)
{
   glColor4ub(color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF, color >> 24);
}

static void bind_texture(const gfx_texture *tex)
{
   if (tex)
   {
      if (!textured)
      {
         glEnable(GL_TEXTURE_2D);
         glEnableClientState(GL_TEXTURE_COORD_ARRAY);
         textured = 1;
      }
      glBindTexture(GL_TEXTURE_2D, tex->id);
   }
   else if (textured)
   {
      glDisable(GL_TEXTURE_2D);
      glDisableClientState(GL_TEXTURE_COORD_ARRAY);
      textured = 0;
   }
}

void gfx_init(void)
{
   vglInitExtended(0, DISPLAY_W, DISPLAY_H, VGL_RAM_THRESHOLD, SCE_GXM_MULTISAMPLE_NONE);

   glViewport(0, 0, DISPLAY_W, DISPLAY_H);
   glMatrixMode(GL_PROJECTION);
   glLoadIdentity();
   glOrtho(0, DISPLAY_W, DISPLAY_H, 0, -1, 1);
   glMatrixMode(GL_MODELVIEW);
   glLoadIdentity();

   glDisable(GL_DEPTH_TEST);
   glDisable(GL_CULL_FACE);
   glEnable(GL_BLEND);
   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
   glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
   glEnableClientState(GL_VERTEX_ARRAY);
}

void gfx_set_clear_color(unsigned int color)
{
   glClearColor((color & 0xFF) / 255.0f, ((color >> 8) & 0xFF) / 255.0f,
         ((color >> 16) & 0xFF) / 255.0f, (color >> 24) / 255.0f);
}

void gfx_begin_frame(void)
{
   glClear(GL_COLOR_BUFFER_BIT);
}

void gfx_end_frame(void)
{
   vglSwapBuffers(GL_FALSE);
}

void gfx_wait_rendering_done(void)
{
   glFinish();
}

gfx_texture *gfx_create_texture(unsigned int w, unsigned int h, GLenum format, GLenum type)
{
   unsigned int bpp;
   gfx_texture *tex;

   if (type == GL_UNSIGNED_SHORT_5_6_5)
      bpp = 2;
   else if (format == GL_ALPHA)
      bpp = 1;
   else
      bpp = 4;

   tex = calloc(1, sizeof(*tex));
   if (!tex)
      return NULL;

   glGenTextures(1, &tex->id);
   glBindTexture(GL_TEXTURE_2D, tex->id);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexImage2D(GL_TEXTURE_2D, 0, format, w, h, 0, format, type, NULL);

   // vitaGL keeps linear textures with rows padded out to 8 texels, same
   // as the gxm stride requirement, and hands us that buffer as-is
   tex->data = vglGetTexDataPointer(GL_TEXTURE_2D);
   if (!tex->data)
   {
      glDeleteTextures(1, &tex->id);
      free(tex);
      return NULL;
   }

   tex->w = w;
   tex->h = h;
   tex->stride = ((w + 7) & ~7) * bpp;

   if (textured)
      glBindTexture(GL_TEXTURE_2D, 0);

   return tex;
}

void gfx_free_texture(gfx_texture *tex)
{
   if (!tex)
      return;

   glDeleteTextures(1, &tex->id);
   free(tex);
}

unsigned int gfx_texture_get_width(const gfx_texture *tex)  { return tex->w; }
unsigned int gfx_texture_get_height(const gfx_texture *tex) { return tex->h; }
unsigned int gfx_texture_get_stride(const gfx_texture *tex) { return tex->stride; }
void *gfx_texture_get_datap(const gfx_texture *tex)         { return tex->data; }

// vita3k's opengl renderer reads U5U6U5_RGB with red and blue swapped, the
// BGR variant comes out right there (and wrong on real hardware)
void gfx_texture_set_bgr565(gfx_texture *tex)
{
   glBindTexture(GL_TEXTURE_2D, tex->id);
   sceGxmTextureSetFormat(vglGetGxmTexture(GL_TEXTURE_2D), SCE_GXM_TEXTURE_FORMAT_U5U6U5_BGR);
}

gfx_texture *gfx_load_png(const char *path)
{
   FILE *fp = fopen(path, "rb");
   png_structp png;
   png_infop info;
   gfx_texture *volatile tex = NULL;
   png_bytep *volatile rows = NULL;
   png_uint_32 w, h;
   int depth, type;

   if (!fp)
      return NULL;

   png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
   info = png ? png_create_info_struct(png) : NULL;
   if (!info)
      goto done;

   if (setjmp(png_jmpbuf(png)))
   {
      gfx_free_texture(tex);
      tex = NULL;
      goto done;
   }

   png_init_io(png, fp);
   png_read_info(png, info);
   png_get_IHDR(png, info, &w, &h, &depth, &type, NULL, NULL, NULL);

   if (depth == 16)
   {
      png_set_scale_16(png);
      depth = 8;
   }

   if (type == PNG_COLOR_TYPE_GRAY || type == PNG_COLOR_TYPE_GRAY_ALPHA)
   {
      png_set_gray_to_rgb(png);
      type = (type == PNG_COLOR_TYPE_GRAY) ? PNG_COLOR_TYPE_RGB : PNG_COLOR_TYPE_RGB_ALPHA;
      depth = 8;
   }

   if (png_get_valid(png, info, PNG_INFO_tRNS))
      png_set_tRNS_to_alpha(png);

   if (type == PNG_COLOR_TYPE_PALETTE)
   {
      png_set_palette_to_rgb(png);
      png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
   }

   if (depth < 8)
      png_set_packing(png);

   if (depth == 8 && type == PNG_COLOR_TYPE_RGB)
      png_set_filler(png, 0xFF, PNG_FILLER_AFTER);

   png_read_update_info(png, info);

   rows = malloc(sizeof(png_bytep) * h);
   if (!rows)
      goto done;

   tex = gfx_create_texture(w, h, GL_RGBA, GL_UNSIGNED_BYTE);
   if (!tex)
      goto done;

   for (png_uint_32 i = 0; i < h; i++)
      rows[i] = (png_bytep)tex->data + i * tex->stride;

   png_read_image(png, rows);

done:
   free(rows);
   png_destroy_read_struct(&png, &info, NULL);
   fclose(fp);
   return tex;
}

static void draw_quad(const gfx_texture *tex, float x, float y, float w, float h,
      float u0, float v0, float u1, float v1, unsigned int color)
{
   const GLfloat pos[8] = { x, y, x + w, y, x, y + h, x + w, y + h };
   const GLfloat uv[8]  = { u0, v0, u1, v0, u0, v1, u1, v1 };

   bind_texture(tex);
   set_color(color);

   glVertexPointer(2, GL_FLOAT, 0, pos);
   if (tex)
      glTexCoordPointer(2, GL_FLOAT, 0, uv);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void gfx_draw_rectangle(float x, float y, float w, float h, unsigned int color)
{
   draw_quad(NULL, x, y, w, h, 0, 0, 0, 0, color);
}

void gfx_draw_texture_tint_part_scale(const gfx_texture *tex, float x, float y, float tex_x, float tex_y,
      float tex_w, float tex_h, float x_scale, float y_scale, unsigned int color)
{
   draw_quad(tex, x, y, tex_w * x_scale, tex_h * y_scale,
         tex_x / tex->w, tex_y / tex->h, (tex_x + tex_w) / tex->w, (tex_y + tex_h) / tex->h, color);
}

void gfx_draw_texture_part_scale(const gfx_texture *tex, float x, float y, float tex_x, float tex_y,
      float tex_w, float tex_h, float x_scale, float y_scale)
{
   gfx_draw_texture_tint_part_scale(tex, x, y, tex_x, tex_y, tex_w, tex_h, x_scale, y_scale, 0xFFFFFFFF);
}

void gfx_draw_texture_tint_scale(const gfx_texture *tex, float x, float y, float x_scale, float y_scale, unsigned int color)
{
   draw_quad(tex, x, y, tex->w * x_scale, tex->h * y_scale, 0, 0, 1, 1, color);
}

void gfx_draw_texture_scale(const gfx_texture *tex, float x, float y, float x_scale, float y_scale)
{
   gfx_draw_texture_tint_scale(tex, x, y, x_scale, y_scale, 0xFFFFFFFF);
}

void gfx_draw_texture_tint(const gfx_texture *tex, float x, float y, unsigned int color)
{
   gfx_draw_texture_tint_scale(tex, x, y, 1.0f, 1.0f, color);
}

void gfx_draw_texture(const gfx_texture *tex, float x, float y)
{
   gfx_draw_texture_tint_scale(tex, x, y, 1.0f, 1.0f, 0xFFFFFFFF);
}

gfx_font *gfx_load_font(const char *path)
{
   gfx_font *font = calloc(1, sizeof(*font));
   if (!font)
      return NULL;

   if (FT_Init_FreeType(&font->lib))
      goto fail_lib;

   if (FT_New_Face(font->lib, path, 0, &font->face))
      goto fail_face;

   // gfx_create_texture leaves both filters on nearest, which is exactly
   // right here: every glyph is drawn 1:1 on whole pixels
   font->atlas = gfx_create_texture(ATLAS_W, ATLAS_H, GL_ALPHA, GL_UNSIGNED_BYTE);
   if (!font->atlas)
      goto fail_atlas;

   return font;

fail_atlas:
   FT_Done_Face(font->face);
fail_face:
   FT_Done_FreeType(font->lib);
fail_lib:
   free(font);
   return NULL;
}

void gfx_free_font(gfx_font *font)
{
   if (!font)
      return;

   gfx_free_texture(font->atlas);
   for (int i = 0; i <= MAX_FONT_SIZE; i++)
      free(font->sizes[i]);
   FT_Done_Face(font->face);
   FT_Done_FreeType(font->lib);
   free(font);
}

// the same glyph already cached at some other size, nearest first. only
// used if the atlas ever fills up, a scaled glyph beats a missing one
static glyph_t *nearest_cached(gfx_font *font, FT_UInt index, unsigned int size)
{
   for (unsigned int d = 1; d <= MAX_FONT_SIZE; d++)
   {
      if (size > d && font->sizes[size - d] && font->sizes[size - d][index].cached)
         return &font->sizes[size - d][index];
      if (size + d <= MAX_FONT_SIZE && font->sizes[size + d] && font->sizes[size + d][index].cached)
         return &font->sizes[size + d][index];
   }

   return NULL;
}

// every glyph is rasterized by freetype at the exact pixel size it's drawn
// at. sharing one bitmap across sizes (what vita2d did) meant stretching
// it, bilinear blur going up and dropped rows/columns going down
static glyph_t *get_glyph(gfx_font *font, FT_UInt index, unsigned int size)
{
   glyph_t *g;
   FT_GlyphSlot slot;
   FT_Bitmap *bmp;
   unsigned char *dst;
   int w, h;

   if (size == 0 || size > MAX_FONT_SIZE || index >= (FT_UInt)font->face->num_glyphs)
      return NULL;

   if (!font->sizes[size])
   {
      font->sizes[size] = calloc(font->face->num_glyphs, sizeof(glyph_t));
      if (!font->sizes[size])
         return NULL;
   }

   g = &font->sizes[size][index];
   if (g->cached)
      return g;

   FT_Set_Pixel_Sizes(font->face, size, size);
   if (FT_Load_Glyph(font->face, index, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL))
      return NULL;

   slot = font->face->glyph;
   bmp = &slot->bitmap;
   w = bmp->width;
   h = bmp->rows;

   // 1px gutter between glyphs so the fallback path above can never
   // sample its neighbour either
   if (font->shelf_x + w > ATLAS_W)
   {
      font->shelf_x = 0;
      font->shelf_y += font->shelf_h + 1;
      font->shelf_h = 0;
   }

   if (w > ATLAS_W || font->shelf_y + h > ATLAS_H)
      return nearest_cached(font, index, size);

   g->x = font->shelf_x;
   g->y = font->shelf_y;
   g->w = w;
   g->h = h;
   font->shelf_x += w + 1;
   if (h > font->shelf_h)
      font->shelf_h = h;

   dst = (unsigned char *)font->atlas->data + g->y * font->atlas->stride + g->x;

   for (int i = 0; i < h; i++, dst += font->atlas->stride)
   {
      const unsigned char *src = bmp->buffer + i * bmp->pitch;

      if (bmp->pixel_mode == FT_PIXEL_MODE_MONO)
      {
         for (int j = 0; j < w; j++)
            dst[j] = (src[j >> 3] & (0x80 >> (j & 7))) ? 0xFF : 0x00;
      }
      else if (bmp->pixel_mode == FT_PIXEL_MODE_GRAY)
         memcpy(dst, src, w);
   }

   g->left    = slot->bitmap_left;
   g->top     = slot->bitmap_top;
   g->advance = slot->advance.x >> 6;
   g->size    = size;
   g->cached  = 1;

   return g;
}

static int utf8_next(const unsigned char *s, unsigned int *c)
{
   if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80)
   {
      *c = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
      return 3;
   }

   if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80)
   {
      *c = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
      return 2;
   }

   *c = s[0];
   return 1;
}

static void flush_text(gfx_font *font, int count, unsigned int color)
{
   bind_texture(font->atlas);
   set_color(color);

   glVertexPointer(2, GL_FLOAT, 0, batch_pos);
   glTexCoordPointer(2, GL_FLOAT, 0, batch_uv);
   glDrawArrays(GL_TRIANGLES, 0, count * 6);
}

static int run_text(gfx_font *font, int draw, int x, int y, unsigned int color,
      unsigned int size, const char *text)
{
   const unsigned char *s = (const unsigned char *)text;
   int pen_x = x;
   int pen_y = y;
   int max_x = 0;
   int count = 0;

   while (*s)
   {
      unsigned int c;
      glyph_t *g;

      s += utf8_next(s, &c);

      if (c == '\n')
      {
         if (pen_x > max_x)
            max_x = pen_x;
         pen_x = x;
         pen_y += size;
         continue;
      }

      g = get_glyph(font, FT_Get_Char_Index(font->face, c), size);
      if (!g)
         continue;

      const float scale = size / (float)g->size;

      if (draw && g->w && g->h)
      {
         float x0 = pen_x + g->left * scale;
         float y0 = pen_y - g->top * scale;
         float x1 = x0 + g->w * scale;
         float y1 = y0 + g->h * scale;
         float u0 = g->x / (float)ATLAS_W;
         float v0 = g->y / (float)ATLAS_H;
         float u1 = (g->x + g->w) / (float)ATLAS_W;
         float v1 = (g->y + g->h) / (float)ATLAS_H;
         GLfloat *p = &batch_pos[count * 12];
         GLfloat *t = &batch_uv[count * 12];

         p[0] = x0; p[1]  = y0; p[2]  = x1; p[3]  = y0; p[4]  = x0; p[5]  = y1;
         p[6] = x1; p[7]  = y0; p[8]  = x1; p[9]  = y1; p[10] = x0; p[11] = y1;
         t[0] = u0; t[1]  = v0; t[2]  = u1; t[3]  = v0; t[4]  = u0; t[5]  = v1;
         t[6] = u1; t[7]  = v0; t[8]  = u1; t[9]  = v1; t[10] = u0; t[11] = v1;

         if (++count == TEXT_BATCH)
         {
            flush_text(font, count, color);
            count = 0;
         }
      }

      pen_x += g->advance * scale;
   }

   if (count)
      flush_text(font, count, color);

   if (pen_x > max_x)
      max_x = pen_x;

   return max_x - x;
}

int gfx_font_draw_text(gfx_font *font, int x, int y, unsigned int color, unsigned int size, const char *text)
{
   return run_text(font, 1, x, y, color, size, text);
}

int gfx_font_text_width(gfx_font *font, unsigned int size, const char *text)
{
   return run_text(font, 0, 0, 0, 0, size, text);
}
