/* The UI is textured quads through the RSX 3D pipeline, with alpha blending and small shaders of
 * our own. Coordinates are virtual (SCREEN_W x SCREEN_H) and scaled to the display. */
#include "gfx.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <malloc.h>
#include <math.h>
#include <rsx/rsx.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sysutil/video.h>
#include <unistd.h>

#include "config.h"
#include "ui_fpo.h"
#include "video_fpo.h"
#include "ui_vpo.h"

#define CB_SIZE 0x10000 /* as Movian and Tiny3D; larger buffers hung when they wrapped */
#define HOST_SIZE (1024 * 1024)
#define HOST_ALIGN (1024 * 1024)
#define FLIP_WAIT_STEP_US 100
#define FLIP_WAIT_LIMIT 2000 /* 200 ms, many frames at any refresh rate */
#define LABEL_INDEX 255
/* Three, because the RSX starts the next frame as soon as a flip is queued: one buffer on screen, one
 * waiting to flip, and one to draw on. With two, it draws on the buffer still being shown, which flickers. */
#define DISPLAY_BUFFERS 3
#define FLIPS_PENDING_MAX 1
#define SURFACE_ALIGN 64
#define TEXTURE_POOL (96 * 1024 * 1024)
#define ALIGN 64
#define VERTEX_BYTES (4 * 1024 * 1024)
#define VERTICES_MAX (VERTEX_BYTES / sizeof(struct vertex))
#define VERTICES_PER_QUAD 4
#define WHITE_SIZE 2
#define DEPTH_BYTES 4
#define VIEWPORT_CLIPS 8

#define ATLAS_W 2048
#define ATLAS_H 1024
#define FIRST_CHAR 32
#define CHAR_COUNT 224
#define FONT_FILE APP_DIR "NotoSans-Regular.ttf"
#define GLYPH_PAD 1
#define ICON_PIXELS 64
#define ICON_SAMPLES 4
#define ICON_COUNT 4
#define ICON_UNITS_OF_LINE 0.82f
#define ICON_GAP_UNITS 2.0f
enum { ICON_INDEX_CROSS, ICON_INDEX_CIRCLE, ICON_INDEX_TRIANGLE, ICON_INDEX_SQUARE };
/* Glyphs are rendered for a 1080p screen, where 848 virtual units span 1920 pixels. */
#define PIXELS_PER_UNIT 2.264f

struct glyph {
	short atlas_x, atlas_y, w, h, left, top;
	float advance;
};

struct vertex {
	float x, y, z, w;
	float r, g, b, a;
	float u, v;
};

struct texture_ref {
	u32 offset, width, height, pitch; /* a pitch of 0 means width * 4 */
};

enum program_id { PROGRAM_UI, PROGRAM_VIDEO, PROGRAM_COUNT };

struct fragment_shader {
	rsxFragmentProgram *program;
	u32 offset;
};

static const float text_units[TEXT_SIZES] = {12.0f, 15.0f, 19.0f, 28.0f};
static struct glyph glyphs[TEXT_SIZES][CHAR_COUNT];
static float line_heights[TEXT_SIZES];
static float ascenders[TEXT_SIZES];
static struct texture_ref atlas_tex, white_tex;
static int icons_top;
static const u32 icon_colors[ICON_COUNT] = {0x7FB2FFFF, 0xFF7B7BFF, 0x63D39AFF, 0xF09AD2FF};

static gcmContextData *context;
static u32 width, height, pitch;
static float scale_x, scale_y;
static u32 *display[DISPLAY_BUFFERS];
static u32 display_offset[DISPLAY_BUFFERS];
static u32 depth_offset, depth_pitch;
static u32 back_buffer;
static u32 label_value = 1;
static volatile u32 flips_queued, flips_completed;
static u32 flip_timeouts;

static rsxVertexProgram *vertex_program;
static struct fragment_shader shaders[PROGRAM_COUNT];
static enum program_id loaded_program, batch_program;
static rsxProgramAttrib *attr_y, *attr_u, *attr_v;
static void *vertex_ucode;
static rsxProgramAttrib *attr_position, *attr_color, *attr_texcoord, *attr_texture;

static struct vertex *vertices[DISPLAY_BUFFERS];
static u32 vertices_offset[DISPLAY_BUFFERS];
static u32 vertex_next, batch_first;
static enum program_id next_program = PROGRAM_UI;
static u32 frame_start_cmd;
static u32 frame_batches, last_frame_batches, max_frame_cmd_bytes;
static struct texture_ref batch_texture;

static u8 *pool_next;
static u32 pool_left;

void *gfx_alloc(u32 size)
{
	size = (size + ALIGN - 1) & ~(u32)(ALIGN - 1);
	if (size > pool_left)
		return NULL;
	void *p = pool_next;
	pool_next += size;
	pool_left -= size;
	return p;
}

u32 gfx_offset(void *ptr)
{
	u32 offset = 0;
	rsxAddressToOffset(ptr, &offset);
	return offset;
}

/* Coverage of a button shape at a point, with coordinates from -1 to 1. */
static int icon_inside(int icon, float x, float y)
{
	const float ring_out = 0.92f, ring_in = 0.66f;
	float ax = fabsf(x), ay = fabsf(y);
	switch (icon) {
	case ICON_INDEX_CROSS: {
		float d1 = fabsf(x - y), d2 = fabsf(x + y);
		return (d1 < 0.34f || d2 < 0.34f) && ax < 0.8f && ay < 0.8f;
	}
	case ICON_INDEX_CIRCLE: {
		float r = sqrtf(x * x + y * y);
		return r < ring_out && r > ring_in;
	}
	case ICON_INDEX_SQUARE:
		return ax < 0.8f && ay < 0.8f && (ax > 0.52f || ay > 0.52f);
	default: {
		/* An outlined triangle: inside the outer shape, and not the inner one. */
		float outer = 0.9f, inner = 0.5f;
		float y1 = y + 0.2f;
		int in_outer = y < 0.72f && 1.732f * ax < y + outer + 0.14f;
		int in_inner = y1 < 0.5f && 1.732f * ax < y1 + inner - 0.02f;
		return in_outer && !in_inner;
	}
	}
}

/* The controller's shapes go in the atlas after the letters, tinted like text. */
static void build_icons(u32 *atlas, int top)
{
	for (int icon = 0; icon < ICON_COUNT; icon++) {
		int ox = icon * (ICON_PIXELS + GLYPH_PAD);
		for (int py = 0; py < ICON_PIXELS; py++) {
			u32 *dst = atlas + (top + py) * ATLAS_W + ox;
			for (int px = 0; px < ICON_PIXELS; px++) {
				int hits = 0;
				for (int sy = 0; sy < ICON_SAMPLES; sy++) {
					for (int sx = 0; sx < ICON_SAMPLES; sx++) {
						float x = ((float)px + ((float)sx + 0.5f) / ICON_SAMPLES) / ICON_PIXELS * 2 - 1;
						float y = ((float)py + ((float)sy + 0.5f) / ICON_SAMPLES) / ICON_PIXELS * 2 - 1;
						hits += icon_inside(icon, x, y);
					}
				}
				u32 alpha = (u32)hits * 255 / (ICON_SAMPLES * ICON_SAMPLES);
				dst[px] = (alpha << 24) | 0x00FFFFFF;
			}
		}
	}
	icons_top = top;
}

static void wait_finish(void)
{
	rsxSetWriteBackendLabel(context, LABEL_INDEX, label_value);
	rsxFlushBuffer(context);
	while (*(vu32 *)gcmGetLabelAddress(LABEL_INDEX) != label_value)
		usleep(30);
	label_value++;
}

static void on_flip(const u32 head)
{
	(void)head;
	flips_completed++;
}

static int init_display(void)
{
	static const u32 modes[] = {VIDEO_RESOLUTION_1080, VIDEO_RESOLUTION_720, VIDEO_RESOLUTION_480,
	                            VIDEO_RESOLUTION_576};
	videoResolution res;
	s32 mode = -1;
	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		if (videoGetResolutionAvailability(VIDEO_PRIMARY, modes[i], VIDEO_ASPECT_AUTO, 0) == 1 &&
		    videoGetResolution(modes[i], &res) == 0) {
			mode = modes[i];
			break;
		}
	}
	if (mode < 0)
		return -1;

	videoConfiguration config;
	memset(&config, 0, sizeof(config));
	config.resolution = (u8)mode;
	config.format = VIDEO_BUFFER_FORMAT_XRGB;
	config.aspect = VIDEO_ASPECT_AUTO;
	config.pitch = (u32)res.width * 4;
	if (videoConfigure(VIDEO_PRIMARY, &config, NULL, 0) != 0)
		return -1;

	width = res.width;
	height = res.height;
	pitch = width * 4;
	scale_x = width / SCREEN_W;
	scale_y = height / SCREEN_H;
	app_log("display %ux%u\n", width, height);
	return 0;
}

static int init_shaders(void)
{
	u32 size = 0;
	void *ucode = NULL;

	vertex_program = (rsxVertexProgram *)ui_vpo;
	rsxVertexProgramGetUCode(vertex_program, &vertex_ucode, &size);
	attr_position = rsxVertexProgramGetAttrib(vertex_program, "position");
	attr_color = rsxVertexProgramGetAttrib(vertex_program, "color");
	attr_texcoord = rsxVertexProgramGetAttrib(vertex_program, "texcoord");

	shaders[PROGRAM_UI].program = (rsxFragmentProgram *)ui_fpo;
	shaders[PROGRAM_VIDEO].program = (rsxFragmentProgram *)video_fpo;
	for (int i = 0; i < PROGRAM_COUNT; i++) {
		rsxFragmentProgramGetUCode(shaders[i].program, &ucode, &size);
		u32 *in_rsx = rsxMemalign(SURFACE_ALIGN, size);
		if (!in_rsx)
			return -1;
		memcpy(in_rsx, ucode, size);
		shaders[i].offset = gfx_offset(in_rsx);
	}
	attr_texture = rsxFragmentProgramGetAttrib(shaders[PROGRAM_UI].program, "texture");
	attr_y = rsxFragmentProgramGetAttrib(shaders[PROGRAM_VIDEO].program, "texture_y");
	attr_u = rsxFragmentProgramGetAttrib(shaders[PROGRAM_VIDEO].program, "texture_u");
	attr_v = rsxFragmentProgramGetAttrib(shaders[PROGRAM_VIDEO].program, "texture_v");
	if (!attr_y || !attr_u || !attr_v) {
		app_log("a shader's texture is missing\n");
		return -1;
	}

	if (!attr_position || !attr_color || !attr_texcoord || !attr_texture) {
		app_log("shader attribute missing %p %p %p %p\n", attr_position, attr_color, attr_texcoord,
		        attr_texture);
		return -1;
	}
	return 0;
}

int gfx_init_base(void)
{
	void *host = memalign(HOST_ALIGN, HOST_SIZE);
	if (!host)
		return -1;
	rsxInit(&context, CB_SIZE, HOST_SIZE, host);
	if (!context || init_display() != 0)
		return -1;

	wait_finish();
	gcmSetFlipMode(GCM_FLIP_VSYNC);
	gcmSetFlipHandler(on_flip);

	for (u32 i = 0; i < DISPLAY_BUFFERS; i++) {
		display[i] = rsxMemalign(SURFACE_ALIGN, height * pitch);
		vertices[i] = rsxMemalign(SURFACE_ALIGN, VERTEX_BYTES);
		if (!display[i] || !vertices[i])
			return -1;
		display_offset[i] = gfx_offset(display[i]);
		vertices_offset[i] = gfx_offset(vertices[i]);
		gcmSetDisplayBuffer(i, display_offset[i], pitch, width, height);
	}
	depth_pitch = width * DEPTH_BYTES;
	void *depth = rsxMemalign(SURFACE_ALIGN, height * depth_pitch);
	if (!depth)
		return -1;
	depth_offset = gfx_offset(depth);
	gcmResetFlipStatus();
	return init_shaders();
}

int gfx_init_pool(void)
{
	pool_next = rsxMemalign(ALIGN, TEXTURE_POOL);
	if (!pool_next)
		return -1;
	pool_left = TEXTURE_POOL;

	u32 *white = gfx_alloc(WHITE_SIZE * WHITE_SIZE * 4);
	if (!white)
		return -1;
	for (int i = 0; i < WHITE_SIZE * WHITE_SIZE; i++)
		white[i] = 0xFFFFFFFF;
	white_tex.offset = gfx_offset(white);
	white_tex.width = white_tex.height = WHITE_SIZE;
	batch_texture = white_tex;
	return 0;
}

static int build_atlas(void)
{
	FT_Library lib;
	FT_Face face;
	int x = 0, y = 0, row_h = 0;

	if (FT_Init_FreeType(&lib) != 0 || FT_New_Face(lib, FONT_FILE, 0, &face) != 0) {
		app_log("font load failed: %s\n", FONT_FILE);
		return -1;
	}
	u32 *atlas = gfx_alloc(ATLAS_W * ATLAS_H * 4);
	if (!atlas)
		return -1;
	atlas_tex.offset = gfx_offset(atlas);
	atlas_tex.width = ATLAS_W;
	atlas_tex.height = ATLAS_H;
	/* White everywhere, with coverage as alpha, so a vertex color tints it. */
	for (u32 i = 0; i < ATLAS_W * ATLAS_H; i++)
		atlas[i] = 0x00FFFFFF;

	for (int s = 0; s < TEXT_SIZES; s++) {
		FT_Set_Pixel_Sizes(face, 0, (FT_UInt)(text_units[s] * PIXELS_PER_UNIT));
		line_heights[s] = (face->size->metrics.height >> 6) / PIXELS_PER_UNIT;
		ascenders[s] = (face->size->metrics.ascender >> 6) / PIXELS_PER_UNIT;

		for (int c = 0; c < CHAR_COUNT; c++) {
			if (FT_Load_Char(face, (FT_ULong)(FIRST_CHAR + c), FT_LOAD_RENDER) != 0)
				continue;
			FT_GlyphSlot g = face->glyph;
			int w = g->bitmap.width, h = g->bitmap.rows;
			if (x + w + GLYPH_PAD >= ATLAS_W) {
				x = 0;
				y += row_h + GLYPH_PAD;
				row_h = 0;
			}
			if (y + h >= ATLAS_H) {
				app_log("atlas full at size %d char %d\n", s, c);
				return -1;
			}
			for (int j = 0; j < h; j++) {
				const u8 *src = g->bitmap.buffer + j * g->bitmap.pitch;
				u32 *dst = atlas + (y + j) * ATLAS_W + x;
				for (int i = 0; i < w; i++)
					dst[i] = ((u32)src[i] << 24) | 0x00FFFFFF;
			}
			struct glyph *out = &glyphs[s][c];
			out->atlas_x = x;
			out->atlas_y = y;
			out->w = w;
			out->h = h;
			out->left = g->bitmap_left;
			out->top = g->bitmap_top;
			out->advance = (g->advance.x >> 6) / PIXELS_PER_UNIT;
			x += w + GLYPH_PAD;
			if (h > row_h)
				row_h = h;
		}
	}
	FT_Done_Face(face);
	FT_Done_FreeType(lib);
	if (y + row_h + GLYPH_PAD + ICON_PIXELS >= ATLAS_H)
		return -1;
	build_icons(atlas, y + row_h + GLYPH_PAD);
	return 0;
}

int gfx_init_font(void)
{
	return build_atlas();
}

int gfx_init(void)
{
	if (gfx_init_base() != 0 || gfx_init_pool() != 0)
		return -1;
	return gfx_init_font();
}

static void set_render_target(u32 index)
{
	gcmSurface sf;
	memset(&sf, 0, sizeof(sf));
	sf.colorFormat = GCM_SURFACE_X8R8G8B8;
	sf.colorTarget = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0] = display_offset[index];
	sf.colorPitch[0] = pitch;
	for (int i = 1; i < 4; i++) {
		sf.colorLocation[i] = GCM_LOCATION_RSX;
		sf.colorOffset[i] = 0;
		sf.colorPitch[i] = 64;
	}
	sf.depthFormat = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation = GCM_LOCATION_RSX;
	sf.depthOffset = depth_offset;
	sf.depthPitch = depth_pitch;
	sf.type = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias = GCM_SURFACE_CENTER_1;
	sf.width = width;
	sf.height = height;
	rsxSetSurface(context, &sf);
}

static void set_draw_state(void)
{
	f32 scale[4] = {width * 0.5f, height * -0.5f, 0.5f, 0.0f};
	f32 offset[4] = {width * 0.5f, height * 0.5f, 0.5f, 0.0f};

	rsxSetColorMask(context, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
	rsxSetColorMaskMrt(context, 0);
	rsxSetViewport(context, 0, 0, width, height, 0.0f, 1.0f, scale, offset);
	rsxSetScissor(context, 0, 0, width, height);
	rsxSetZMinMaxControl(context, GCM_FALSE, GCM_TRUE, GCM_FALSE);
	for (int i = 0; i < VIEWPORT_CLIPS; i++)
		rsxSetViewportClip(context, i, width, height);
	rsxSetDepthTestEnable(context, GCM_FALSE);
	rsxSetDepthWriteEnable(context, GCM_FALSE);
	rsxSetShadeModel(context, GCM_SHADE_MODEL_SMOOTH);
	rsxSetFrontFace(context, GCM_FRONTFACE_CCW);
	rsxSetCullFaceEnable(context, GCM_FALSE);
	rsxSetUserClipPlaneControl(context, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
	                           GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
	                           GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);
	rsxSetBlendFunc(context, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA, GCM_ONE, GCM_ONE_MINUS_SRC_ALPHA);
	rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
	rsxSetBlendEnable(context, GCM_TRUE);
	rsxSetLogicOpEnable(context, GCM_FALSE);
}

static void bind_vertices(void)
{
	u32 base = vertices_offset[back_buffer];
	u8 stride = sizeof(struct vertex);
	rsxBindVertexArrayAttrib(context, attr_position->index, 0, base + offsetof(struct vertex, x), stride, 4,
	                         GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(context, attr_color->index, 0, base + offsetof(struct vertex, r), stride, 4,
	                         GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(context, attr_texcoord->index, 0, base + offsetof(struct vertex, u), stride, 2,
	                         GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
}

/* An 8-bit plane appears in all channels, so a shader can read it from any. */
static void load_texture_unit(const struct texture_ref *t, u8 unit, int single_plane)
{
	gcmTexture tex;
	memset(&tex, 0, sizeof(tex));
	tex.format = (single_plane ? GCM_TEXTURE_FORMAT_B8 : GCM_TEXTURE_FORMAT_A8R8G8B8) | GCM_TEXTURE_FORMAT_LIN;
	tex.mipmap = 1;
	tex.dimension = GCM_TEXTURE_DIMS_2D;
	tex.cubemap = GCM_FALSE;
	tex.remap = (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
	            (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
	            (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
	            (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT);
	if (single_plane)
		tex.remap |= (GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
		             (GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
		             (GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
		             (GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_A_SHIFT);
	else
		tex.remap |= (GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
		             (GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
		             (GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
		             (GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT);
	tex.width = t->width;
	tex.height = t->height;
	tex.depth = 1;
	tex.location = GCM_LOCATION_RSX;
	tex.pitch = t->pitch ? t->pitch : t->width * 4;
	tex.offset = t->offset;
	rsxInvalidateTextureCache(context, GCM_INVALIDATE_TEXTURE);
	rsxLoadTexture(context, unit, &tex);
	rsxTextureControl(context, unit, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
	rsxTextureFilter(context, unit, 0, GCM_TEXTURE_LINEAR, GCM_TEXTURE_LINEAR,
	                 GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(context, unit, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE,
	                   GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

static void use_program(enum program_id id)
{
	if (loaded_program == id)
		return;
	rsxLoadFragmentProgramLocation(context, shaders[id].program, shaders[id].offset, GCM_LOCATION_RSX);
	loaded_program = id;
}

/* Draw the quads queued so far with the current texture. */
static void flush_batch(void)
{
	u32 count = vertex_next - batch_first;
	if (count == 0)
		return;
	use_program(batch_program);
	if (batch_program != PROGRAM_VIDEO)
		load_texture_unit(&batch_texture, attr_texture->index, 0);
	/* The vertices were written after the last invalidate, and earlier frames used these addresses. */
	rsxInvalidateVertexCache(context);
	rsxDrawVertexArray(context, GCM_TYPE_QUADS, batch_first, count);
	batch_first = vertex_next;
	frame_batches++;
}

void gfx_log_state(void)
{
	gcmControlRegister volatile *ctrl = gcmGetControlRegister();
	u64 used = (u32)context->current - (u32)context->begin;
	u64 size = (u32)context->end - (u32)context->begin;
	app_log("rsx get=%08x put=%08x ref=%08x cmd_used=%llu of %llu flip_status=%d batches_per_frame=%u max_cmd_bytes=%u flips=%u/%u timeouts=%u\n",
	        ctrl->get, ctrl->put, ctrl->ref, (unsigned long long)used, (unsigned long long)size,
	        gcmGetFlipStatus(), last_frame_batches, max_frame_cmd_bytes,
	        flips_completed, flips_queued, flip_timeouts);
}

void gfx_begin_frame(u32 clear_color)
{
	frame_start_cmd = (u32)context->current;
	last_frame_batches = frame_batches;
	frame_batches = 0;
	vertex_next = batch_first = 0;
	batch_texture = white_tex;
	batch_program = PROGRAM_UI;
	loaded_program = PROGRAM_UI;

	set_render_target(back_buffer);
	set_draw_state();
	rsxSetClearColor(context, (clear_color >> 8) | (clear_color << 24));
	rsxSetClearDepthStencil(context, 0xFFFFFF00);
	rsxClearSurface(context, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A | GCM_CLEAR_S | GCM_CLEAR_Z);

	rsxLoadVertexProgram(context, vertex_program, vertex_ucode);
	rsxLoadFragmentProgramLocation(context, shaders[PROGRAM_UI].program, shaders[PROGRAM_UI].offset, GCM_LOCATION_RSX);
	bind_vertices();
	rsxInvalidateVertexCache(context);
}

void gfx_end_frame(void)
{
	flush_batch();
	if ((u32)context->current > frame_start_cmd) {
		u32 bytes = (u32)((u32)context->current - frame_start_cmd);
		if (bytes > max_frame_cmd_bytes)
			max_frame_cmd_bytes = bytes;
	}

	/* Wait for the last flip before queueing another, using the handler: on hardware, polling the
	 * flip status can read "done" at once. Give up eventually so a lost flip can't stop the app. */
	for (u32 waited = 0; (s32)(flips_queued - flips_completed) > FLIPS_PENDING_MAX; waited++) {
		if (waited > FLIP_WAIT_LIMIT) {
			flip_timeouts++;
			flips_completed = flips_queued;
			break;
		}
		usleep(FLIP_WAIT_STEP_US);
	}

	/* This order, as in the Halo PS3 port: flipping first gave it a black screen on hardware. */
	gcmSetWaitFlip(context);
	gcmSetFlip(context, back_buffer);
	rsxFlushBuffer(context);
	flips_queued++;
	back_buffer = (back_buffer + 1) % DISPLAY_BUFFERS;
}

static void put_vertex(struct vertex *v, float x, float y, u32 color, float s, float t)
{
	v->x = x / SCREEN_W * 2.0f - 1.0f;
	v->y = 1.0f - y / SCREEN_H * 2.0f;
	v->z = 0.5f;
	v->w = 1.0f;
	v->r = (color >> 24) / 255.0f;
	v->g = ((color >> 16) & 0xFF) / 255.0f;
	v->b = ((color >> 8) & 0xFF) / 255.0f;
	v->a = (color & 0xFF) / 255.0f;
	v->u = s;
	v->v = t;
}

/* Colors run clockwise from the top left. */
static void quad(const struct texture_ref *t, float x, float y, float w, float h, float u0, float v0,
                 float u1, float v1, u32 c0, u32 c1, u32 c2, u32 c3)
{
	if (t->offset != batch_texture.offset || t->width != batch_texture.width ||
	    t->height != batch_texture.height || batch_program != next_program) {
		flush_batch();
		batch_texture = *t;
		batch_program = next_program;
	}
	if (vertex_next + VERTICES_PER_QUAD > VERTICES_MAX)
		return;
	struct vertex *v = vertices[back_buffer] + vertex_next;
	put_vertex(&v[0], x, y, c0, u0, v0);
	put_vertex(&v[1], x, y + h, c3, u0, v1);
	put_vertex(&v[2], x + w, y + h, c2, u1, v1);
	put_vertex(&v[3], x + w, y, c1, u1, v0);
	vertex_next += VERTICES_PER_QUAD;
}

void gfx_rect(float x, float y, float w, float h, u32 color)
{
	quad(&white_tex, x, y, w, h, 0, 0, 1, 1, color, color, color, color);
}

void gfx_vgradient(float x, float y, float w, float h, u32 top, u32 bottom)
{
	quad(&white_tex, x, y, w, h, 0, 0, 1, 1, top, top, bottom, bottom);
}

void gfx_hgradient(float x, float y, float w, float h, u32 left, u32 right)
{
	quad(&white_tex, x, y, w, h, 0, 0, 1, 1, left, right, right, left);
}

void gfx_outline(float x, float y, float w, float h, float t, u32 color)
{
	gfx_rect(x - t, y - t, w + 2 * t, t, color);
	gfx_rect(x - t, y + h, w + 2 * t, t, color);
	gfx_rect(x - t, y, t, h, color);
	gfx_rect(x + w, y, t, h, color);
}

void gfx_image(u32 offset, u32 tex_w, u32 tex_h, float x, float y, float w, float h)
{
	struct texture_ref t = {offset, tex_w, tex_h, 0};
	quad(&t, x, y, w, h, 0, 0, 1, 1, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
}

void gfx_image_pitch(u32 offset, u32 tex_w, u32 tex_h, u32 pitch_bytes, float x, float y, float w, float h)
{
	struct texture_ref t = {offset, tex_w, tex_h, pitch_bytes};
	quad(&t, x, y, w, h, 0, 0, 1, 1, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
}

void gfx_video(u32 offset, u32 width, u32 height, float x, float y, float w, float h)
{
	u32 luma = width * height;
	u32 chroma_w = width / 2, chroma_h = height / 2;
	const struct texture_ref plane_y = {offset, width, height, width};
	const struct texture_ref plane_u = {offset + luma, chroma_w, chroma_h, chroma_w};
	const struct texture_ref plane_v = {offset + luma + chroma_w * chroma_h, chroma_w, chroma_h, chroma_w};

	/* Queue the quad as a batch of its own, with the three planes loaded for it. */
	flush_batch();
	batch_program = PROGRAM_VIDEO;
	next_program = PROGRAM_VIDEO;
	batch_texture = plane_y;
	quad(&plane_y, x, y, w, h, 0, 0, 1, 1, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
	use_program(PROGRAM_VIDEO);
	load_texture_unit(&plane_y, attr_y->index, 1);
	load_texture_unit(&plane_u, attr_u->index, 1);
	load_texture_unit(&plane_v, attr_v->index, 1);
	flush_batch();
	next_program = PROGRAM_UI;
	batch_program = PROGRAM_UI;
	batch_texture = white_tex;
}

void gfx_image_rows(u32 offset, u32 tex_w, u32 tex_h, float x, float y, float w, float h, float v0, float v1,
                    u32 alpha)
{
	struct texture_ref t = {offset, tex_w, tex_h, 0};
	u32 color = 0xFFFFFF00 | (alpha & 0xFF);
	quad(&t, x, y, w, h, 0, v0, 1, v1, color, color, color, color);
}

float gfx_line_height(enum text_size size)
{
	return line_heights[size];
}

static const struct glyph *glyph_for(char ch, enum text_size size)
{
	unsigned c = (unsigned char)ch;
	if (c < FIRST_CHAR)
		c = ' ';
	return &glyphs[size][c - FIRST_CHAR];
}

/* The small control characters that stand for controller buttons. */
static int icon_for(char ch)
{
	return ch >= 1 && ch <= ICON_COUNT ? ch - 1 : -1;
}

static float icon_size(enum text_size size)
{
	return line_heights[size] * ICON_UNITS_OF_LINE;
}

static float char_advance(char ch, enum text_size size)
{
	if (icon_for(ch) >= 0)
		return icon_size(size) + ICON_GAP_UNITS;
	return glyph_for(ch, size)->advance;
}

float gfx_text_width(const char *text, enum text_size size)
{
	float w = 0;
	for (; *text; text++)
		w += char_advance(*text, size);
	return w;
}

/* Snap to a whole display pixel, so glyphs stay sharp. */
static float snap_x(float x)
{
	return roundf(x * scale_x) / scale_x;
}

static float snap_y(float y)
{
	return roundf(y * scale_y) / scale_y;
}

void gfx_text(const char *text, float x, float y, enum text_size size, u32 color)
{
	float baseline = y + ascenders[size];
	for (; *text; text++) {
		int icon = icon_for(*text);
		if (icon >= 0) {
			float d = icon_size(size);
			float u0 = (float)(icon * (ICON_PIXELS + GLYPH_PAD)) / ATLAS_W;
			float v0 = (float)icons_top / ATLAS_H;
			float u1 = u0 + (float)ICON_PIXELS / ATLAS_W;
			float v1 = v0 + (float)ICON_PIXELS / ATLAS_H;
			float iy = y + (line_heights[size] - d) / 2;
			u32 tint = (icon_colors[icon] & 0xFFFFFF00) | (color & 0xFF);
			quad(&atlas_tex, snap_x(x), snap_y(iy), d, d, u0, v0, u1, v1, tint, tint, tint, tint);
			x += d + ICON_GAP_UNITS;
			continue;
		}
		const struct glyph *g = glyph_for(*text, size);
		if (g->w > 0 && g->h > 0) {
			float gw = g->w / PIXELS_PER_UNIT, gh = g->h / PIXELS_PER_UNIT;
			quad(&atlas_tex, snap_x(x + g->left / PIXELS_PER_UNIT),
			     snap_y(baseline - g->top / PIXELS_PER_UNIT), gw, gh,
			     (float)g->atlas_x / ATLAS_W, (float)g->atlas_y / ATLAS_H,
			     (float)(g->atlas_x + g->w) / ATLAS_W, (float)(g->atlas_y + g->h) / ATLAS_H,
			     color, color, color, color);
		}
		x += g->advance;
	}
}

void gfx_clip(float x, float y, float w, float h)
{
	flush_batch();
	rsxSetScissor(context, (u16)(x * scale_x), (u16)(y * scale_y), (u16)(w * scale_x), (u16)(h * scale_y));
}

void gfx_clip_off(void)
{
	flush_batch();
	rsxSetScissor(context, 0, 0, width, height);
}

int gfx_text_fits(const char *text, float width_units, int max_lines, enum text_size size)
{
	const char *p = text;
	for (int l = 0; l < max_lines; l++) {
		size_t n = 0, last_break = 0;
		float w = 0;
		while (p[n]) {
			float adv = glyph_for(p[n], size)->advance;
			if (w + adv > width_units)
				break;
			if (p[n] == ' ')
				last_break = n;
			w += adv;
			n++;
		}
		if (!p[n])
			return 1;
		if (last_break > 0)
			n = last_break;
		p += n;
		while (*p == ' ')
			p++;
	}
	return !*p;
}

void gfx_text_wrapped(const char *text, float x, float y, float width_units, int max_lines,
                      enum text_size size, u32 color)
{
	char line[256];
	const char *p = text;
	float lh = gfx_line_height(size);
	for (int l = 0; l < max_lines && *p; l++, y += lh) {
		size_t n = 0, last_break = 0;
		float w = 0;
		while (p[n] && n + 1 < sizeof(line)) {
			float adv = glyph_for(p[n], size)->advance;
			if (w + adv > width_units)
				break;
			if (p[n] == ' ')
				last_break = n;
			w += adv;
			n++;
		}
		int fits_rest = !p[n];
		if (!fits_rest && l + 1 == max_lines) {
			float dots = gfx_text_width("...", size);
			while (n > 0 && w + dots > width_units) {
				n--;
				w -= glyph_for(p[n], size)->advance;
			}
			memcpy(line, p, n);
			strcpy(line + n, "...");
			gfx_text(line, x, y, size, color);
			return;
		}
		if (!fits_rest && last_break > 0)
			n = last_break;
		memcpy(line, p, n);
		line[n] = 0;
		gfx_text(line, x, y, size, color);
		p += n;
		while (*p == ' ')
			p++;
	}
}
