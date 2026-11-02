#ifndef GFX_H
#define GFX_H

#include <ppu-types.h>

/* Put these in text to draw the controller's button shapes. */
#define ICON_CROSS "\x01"
#define ICON_CIRCLE "\x02"
#define ICON_TRIANGLE "\x03"
#define ICON_SQUARE "\x04"

enum text_size { TEXT_SMALL, TEXT_BODY, TEXT_TITLE, TEXT_HEAD, TEXT_SIZES };

int gfx_init(void);
/* The steps of gfx_init, for tests that bring them up one at a time. */
int gfx_init_base(void);
int gfx_init_pool(void);
int gfx_init_font(void);
void gfx_begin_frame(u32 clear_color);
void gfx_end_frame(void);
/* Log the RSX's state, for working out a hang. */
void gfx_log_state(void);

/* Allocate RSX memory that lasts for the life of the app. */
void *gfx_alloc(u32 size);
u32 gfx_offset(void *ptr);

void gfx_rect(float x, float y, float w, float h, u32 color);
/* Colors run top to bottom. */
void gfx_vgradient(float x, float y, float w, float h, u32 top, u32 bottom);
/* Colors run left to right. */
void gfx_hgradient(float x, float y, float w, float h, u32 left, u32 right);
void gfx_outline(float x, float y, float w, float h, float thickness, u32 color);
/* Draw an opaque ARGB texture. Fade it by drawing a translucent rectangle over it. */
void gfx_image(u32 offset, u32 tex_w, u32 tex_h, float x, float y, float w, float h);

/* The same, for a texture whose rows are pitch_bytes apart. */
void gfx_image_pitch(u32 offset, u32 tex_w, u32 tex_h, u32 pitch_bytes, float x, float y, float w, float h);
/* A video picture in planar YUV 4:2:0, with the planes one after another, converted by a shader. */
void gfx_video(u32 offset, u32 width, u32 height, float x, float y, float w, float h);
/* The same, showing only the texture's rows from v0 to v1 (0 is the top, 1 the bottom), with alpha from
 * 0 to 255 to fade it. */
void gfx_image_rows(u32 offset, u32 tex_w, u32 tex_h, float x, float y, float w, float h, float v0, float v1,
                    u32 alpha);

float gfx_text_width(const char *text, enum text_size size);
/* y is the top of the line. */
void gfx_text(const char *text, float x, float y, enum text_size size, u32 color);
float gfx_line_height(enum text_size size);
/* Limit drawing to a rectangle until gfx_clip_off. */
void gfx_clip(float x, float y, float w, float h);
void gfx_clip_off(void);
/* Whether gfx_text_wrapped could show all of it. */
int gfx_text_fits(const char *text, float width_units, int max_lines, enum text_size size);
/* Word-wrap into at most max_lines, ending with "..." if it doesn't all fit. */
void gfx_text_wrapped(const char *text, float x, float y, float width_units, int max_lines,
                      enum text_size size, u32 color);

#endif
