#include "strokes.h"

#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <math.h>
#include <libgen.h>
#include <errno.h>

#include "gl.h"
#include "util.h"
#include "pool.h"

#include <GLFW/glfw3.h>


#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

//TODO: Reduce footprint by removing image formats
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define RANDOM_SAMPLE_SIZE 1024
static float random_samples[RANDOM_SAMPLE_SIZE];

#define MAX_STROKES 64
#define MAX_STROKE_VERTICES 64

static struct {
	struct pool* vertices_pool;
	struct lb_stroke strokes[MAX_STROKES];
	uint32_t strokes_len;
} data;

static struct lb_stroke* create_stroke() {
	assert(data.strokes_len < MAX_STROKES);
	
	struct lb_stroke* stroke = &data.strokes[data.strokes_len++];
	stroke->vertices = pool_alloc(data.vertices_pool);
	stroke->vertices_len = 0;
	return stroke;
}

static void delete_stroke(struct lb_stroke* stroke) {
	assert(stroke);
	
	pool_free(data.vertices_pool, stroke->vertices);
	size_t idx = stroke - data.strokes;
	data.strokes_len--;
	if(idx < data.strokes_len) data.strokes[idx] = data.strokes[data.strokes_len]; // swap
}

static struct lb_stroke* duplicate_stroke(const struct lb_stroke* stroke) {
	assert(stroke);
	struct lb_stroke* s = create_stroke();
	void* vertices_pool = s->vertices;
	memcpy(s, stroke, sizeof(struct lb_stroke));
	s->vertices = vertices_pool;
	memcpy(s->vertices, stroke->vertices, sizeof(struct bezier_point)*stroke->vertices_len);
	return s;
}

static struct bezier_point* add_vertex(struct lb_stroke* stroke) {
	assert(stroke);
	return &stroke->vertices[stroke->vertices_len++];
}

static void delete_vertex(struct lb_stroke* stroke, struct bezier_point* vertex) {
	assert(stroke);
	size_t idx = vertex - stroke->vertices;
	assert(idx >= 0);
	assert(idx < MAX_STROKE_VERTICES);
	stroke->vertices_len--;
	if(idx < stroke->vertices_len) stroke->vertices[idx] = stroke->vertices[stroke->vertices_len]; // swap
}


// Timeline
color32 lb_clear_color = (color32){.r = 255, .g = 255, .b = 255, .a = 255};
bool lb_strokes_playing = false;
float lb_strokes_timelineDuration = 10.0f;
float lb_strokes_timelinePosition = 1.0f;
bool lb_strokes_draggingPlayhead = false;
enum lb_input_mode input_mode = INPUT_DRAW;
enum lb_drag_mode drag_mode = DRAG_NONE;
bool lb_strokes_artboard_set = false;
int lb_strokes_artboard_set_idx = -1;
bool lb_strokes_export_range_set = false;
int lb_strokes_export_range_set_idx = -1;
float lb_strokes_export_range_begin = -1.0f;
float lb_strokes_export_range_duration = 0.0f;
float lb_strokes_export_fps = 15.0f;
vec2 lb_strokes_pan;
vec2 lb_strokes_artboard[2];

float lb_strokes_setTimelinePosition(float pos) {
	pos = (pos < 0.0f ? 0.0f : pos);
	pos = (pos > lb_strokes_timelineDuration ? lb_strokes_timelineDuration : pos);
	return lb_strokes_timelinePosition = pos;
}

void lb_strokes_updateTimeline(float dt) {
	if(lb_strokes_timelinePosition > lb_strokes_timelineDuration) lb_strokes_timelinePosition = lb_strokes_timelineDuration;
	if(lb_strokes_timelinePosition < 0) lb_strokes_timelinePosition = 0;
	
	if(!lb_strokes_playing || lb_strokes_draggingPlayhead || input_mode == INPUT_TRIM) return;
	lb_strokes_timelinePosition += dt;

	if(lb_strokes_export_range_set) {
		if(lb_strokes_timelinePosition < lb_strokes_export_range_begin) lb_strokes_timelinePosition = lb_strokes_export_range_begin;
		else if(lb_strokes_timelinePosition > lb_strokes_export_range_begin + lb_strokes_export_range_duration) lb_strokes_timelinePosition = lb_strokes_export_range_begin;	
	} else if(lb_strokes_timelinePosition > lb_strokes_timelineDuration) {
		lb_strokes_timelinePosition = 0;
	}
}

// Drawing
static struct {
	GLuint vao;
	GLuint vbo;
} gl_lines;

static struct shaderProgram line_shader;
#include "../build/assets/shaders/line.frag.c"
#include "../build/assets/shaders/line.vert.c"
enum line_shader_uniform {
	LINE_UNIFORM_PROJECTION = 0,
	LINE_UNIFORM_PAN,
	LINE_UNIFORM_COLOR,
	LINE_UNIFORM_POINT_SIZE
};

static struct shaderProgram brush_shader;
#include "../build/assets/shaders/brush.frag.c"
#include "../build/assets/shaders/brush.vert.c"
enum brush_shader_uniform {
	BRUSH_UNIFORM_PROJECTION = 0,
	BRUSH_UNIFORM_PAN,
	BRUSH_UNIFORM_TRANSLATION,
	BRUSH_UNIFORM_SCALE,
	BRUSH_UNIFORM_ROTATION,
	BRUSH_UNIFORM_COLOR,
	BRUSH_UNIFORM_ALPHA,
	BRUSH_UNIFORM_MASK_TEXTURE,
	BRUSH_UNIFORM_BRUSH_TEXTURE
};

static GLuint mask_texture;
static GLuint brush_texture;

#define RADIAL_GRADIENT_SIZE 64

#include "../build/assets/images/pencil.png.c"
void upload_texture() {

	static uint8_t pix[RADIAL_GRADIENT_SIZE][RADIAL_GRADIENT_SIZE];
	const uint8_t midpoint = RADIAL_GRADIENT_SIZE / 2;
	const float scale = 2.5f;

	for(uint8_t y = 0; y < RADIAL_GRADIENT_SIZE; y++) {
		for(uint8_t x = 0; x < RADIAL_GRADIENT_SIZE; x++) {
			double a = sqrt(pow(midpoint - x, 2) + pow(midpoint - y, 2));

			a = (a - midpoint) / (a - RADIAL_GRADIENT_SIZE) * scale;
			
			if(a > 1) a = 1;
			else if(a < 0) a = 0;

			pix[y][x] = a * 255;
		}
	}

	glGenTextures(1, &mask_texture);
	glBindTexture(GL_TEXTURE_2D, mask_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, RADIAL_GRADIENT_SIZE, RADIAL_GRADIENT_SIZE, 0, GL_RED, GL_UNSIGNED_BYTE, pix);

	int brush_width, brush_height, brush_channels;
	stbi_set_flip_vertically_on_load(1);
	GLubyte* brush_pix = stbi_load_from_memory(src_assets_images_pencil_png, src_assets_images_pencil_png_len, &brush_width, &brush_height, &brush_channels, 0);
	assert(brush_pix);
	
	glGenTextures(1, &brush_texture);
	glBindTexture(GL_TEXTURE_2D, brush_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, brush_width, brush_height, 0, GL_RED, GL_UNSIGNED_BYTE, brush_pix);

	stbi_image_free(brush_pix);
}

static GLuint plane_vao;
static GLuint plane_vbo;

void upload_plane() {
	static float vertices[] = {
	//  Position   // Texcoords
		-0.5f,  0.5f, 0.0f, 0.0f, // Top-left
		 0.5f,  0.5f, 1.0f, 0.0f, // Top-right
		 0.5f, -0.5f, 1.0f, 1.0f, // Bottom-right
		-0.5f,  0.5f, 0.0f, 0.0f, // Top-left
		 0.5f, -0.5f, 1.0f, 1.0f, // Bottom-right
		-0.5f, -0.5f, 0.0f, 1.0f  // Bottom-left
	};

	glGenVertexArrays(1, &plane_vao);
	glBindVertexArray(plane_vao);
	glCheckError();

	glGenBuffers(1, &plane_vbo);
	glCheckError();

	size_t vertex_stride = 0;
	vertex_stride += sizeof(GLfloat) * 2; // XY
	vertex_stride += sizeof(GLfloat) * 2; // UV

	glBindBuffer(GL_ARRAY_BUFFER, plane_vbo);
	glBufferData(GL_ARRAY_BUFFER, (GLsizei) sizeof(GLfloat) * 4 * 6, vertices, GL_STATIC_DRAW);
	glCheckError();

	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, vertex_stride, (char*)0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, vertex_stride, (char*)8);
	glEnableVertexAttribArray(1);
	glCheckError();
}

void lb_strokes_init() {
	srand(0);
	for(size_t i = 0; i < RANDOM_SAMPLE_SIZE; i++) random_samples[i] = rand() / (float)RAND_MAX;
		
	// Line shader
	{
		static const char* uniformNames[] = {
			"projection",
			"pan",
			"color",
			"pointSize"
		};

		buildProgram(
			loadShader(GL_VERTEX_SHADER, (char*)src_assets_shaders_line_vert, (int*)&src_assets_shaders_line_vert_len),
			loadShader(GL_FRAGMENT_SHADER, (char*)src_assets_shaders_line_frag, (int*)&src_assets_shaders_line_frag_len),
			uniformNames, sizeof(uniformNames)/sizeof(uniformNames[0]), &line_shader);
	}

	// Brush shader
	{
		static const char* uniformNames[] = {
			"projection",
			"pan",
			"translation",
			"scale",
			"rotation",
			"brushColor",
			"brushAlpha",
			"maskTex",
			"brushTex"
		};

		buildProgram(
			loadShader(GL_VERTEX_SHADER, (char*)src_assets_shaders_brush_vert, (int*)&src_assets_shaders_brush_vert_len),
			loadShader(GL_FRAGMENT_SHADER, (char*)src_assets_shaders_brush_frag, (int*)&src_assets_shaders_brush_frag_len),
			uniformNames, sizeof(uniformNames)/sizeof(uniformNames[0]), &brush_shader);
	}
	

	glGenVertexArrays(1, &gl_lines.vao);
	glBindVertexArray(gl_lines.vao);
	glCheckError();
	
	glGenBuffers(1, &gl_lines.vbo);
	glCheckError();
	
	size_t vertexStride = 0;
	vertexStride += sizeof(GLfloat) * 2;
	
	glBindBuffer(GL_ARRAY_BUFFER, gl_lines.vbo);
	glBufferData(GL_ARRAY_BUFFER, (GLsizei) sizeof(vec2) * MAX_STROKE_VERTICES, NULL, GL_DYNAMIC_DRAW);
	glCheckError();
	
	// Enable vertex attributes
	void* attrib_offset = 0;
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, vertexStride, attrib_offset);
	attrib_offset += sizeof(GLfloat) * 2;
	glEnableVertexAttribArray(0);
	glCheckError();

	upload_plane();
	upload_texture();
	
	data.vertices_pool = pool_init(sizeof(struct bezier_point) * MAX_STROKE_VERTICES, MAX_STROKES);
}

struct lb_stroke* lb_strokes_selected = NULL;
struct lb_stroke* lb_strokes_selected_tmp = NULL;
struct bezier_point* lb_strokes_selected_vertex = NULL;
static vec2* drag_vec = NULL;
static uint8_t drag_handle_idx = 0;
static vec2 drag_start;
static float select_tolerance_dist = 8.0f;

enum mods {
	MOD_ALT = 0,
	MOD_SHIFT,
	MOD_META,
	MOD__COUNT
};
static bool mods_pressed[MOD__COUNT];

enum draw_state {
	NONE = 0,
	ENTERING,
	FULL,
	EXITING
};

//TODO: Easy unit test
static enum draw_state lb_stroke_getDrawStateForTime(const struct lb_stroke* stroke, const float time) {
	assert(stroke);
	if(time < stroke->global_start_time) return NONE;
	float acc = stroke->global_start_time;
	if(stroke->enter.animate_method != ANIMATE_NONE) {
		acc += stroke->enter.duration;
		if(time < acc) return ENTERING;
	}
	
	acc += stroke->full_duration;
	if(time < acc) return FULL;
	
	if(stroke->exit.animate_method != ANIMATE_NONE) {
		acc += stroke->exit.duration;
		if(time < acc) return EXITING;
	}
	return NONE;
}

void lb_strokes_render_strokes(const float time, const mat4 matrix, const vec2 pan) {
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	
	glDisable(GL_DEPTH_TEST);
	
	glUseProgram(brush_shader.program);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	glUniform2f(brush_shader.uniforms[BRUSH_UNIFORM_PAN], pan.x, pan.y);
	glUniformMatrix4fv(brush_shader.uniforms[BRUSH_UNIFORM_PROJECTION], 1, GL_FALSE, (const GLfloat*) matrix);
	for(size_t i = 0; i < data.strokes_len; i++) {
		if(data.strokes[i].vertices_len < 2) continue;
		
		enum draw_state state = lb_stroke_getDrawStateForTime(&data.strokes[i], time);
		bool reverse = false;
		
		float percent_drawn;
		enum lb_animate_method method;
		switch(state) {
			case NONE:
				continue;
			case FULL:
				percent_drawn = 1;
				break;
			case ENTERING:
				percent_drawn = EasingFuncs[data.strokes[i].enter.easing_method](map(
					time,
					data.strokes[i].global_start_time,
					data.strokes[i].global_start_time + data.strokes[i].enter.duration,
					0, 1));
				reverse = data.strokes[i].enter.draw_reverse;
				method = data.strokes[i].enter.animate_method;
				break;
			case EXITING: {
				float begin = data.strokes[i].global_start_time +
					(data.strokes[i].enter.animate_method == ANIMATE_NONE ? 0 : data.strokes[i].enter.duration) +
					data.strokes[i].full_duration;
				float end = begin + data.strokes[i].exit.duration;
				percent_drawn = EasingFuncs[data.strokes[i].exit.easing_method](map(
					time,
					begin, end,
					1, 0));
				reverse = data.strokes[i].exit.draw_reverse;
				method = data.strokes[i].exit.animate_method;
				break;
			}
		}
		
		size_t v;
		int dir = reverse ? -1 : 1;
		
		float total_length = 0.0f;
		for(size_t vi = 0; vi < data.strokes[i].vertices_len-1; vi++) {
			v = reverse ? (data.strokes[i].vertices_len-1) - vi : vi;
			
			struct bezier_point* a = &data.strokes[i].vertices[v];
			struct bezier_point* b = &data.strokes[i].vertices[v+dir];
			vec2 h1, h2;
			if(reverse) {
				h1 = a->handles[0];
				h2 = b->handles[1];
			} else {
				h1 = a->handles[1];
				h2 = b->handles[0];
			}
			
			total_length += bezier_distance_update_cache(a->anchor, h1, h2, b->anchor);
		}
		
		if(method == ANIMATE_FADE) {
			glUniform1f(brush_shader.uniforms[BRUSH_UNIFORM_ALPHA], percent_drawn);
			percent_drawn = 1;
		} else {
			glUniform1f(brush_shader.uniforms[BRUSH_UNIFORM_ALPHA], 1);
		}
		
		glUniform4f(brush_shader.uniforms[BRUSH_UNIFORM_COLOR], data.strokes[i].color.r, data.strokes[i].color.g, data.strokes[i].color.b, data.strokes[i].color.a);
		
		float total_length_drawn = total_length*percent_drawn;
		//TODO: Optimize out the double calculation of length, cache the total length if possible
		
		// Brush
		
		float length_accum = 0.0f;
		for(size_t vi = 0; vi < data.strokes[i].vertices_len-1; vi++) {
			v = reverse ? (data.strokes[i].vertices_len-1) - vi : vi;
			
			struct bezier_point* a = &data.strokes[i].vertices[v];
			struct bezier_point* b = &data.strokes[i].vertices[v+dir];
			vec2 h1, h2;
			if(reverse) {
				h1 = a->handles[0];
				h2 = b->handles[1];
			} else {
				h1 = a->handles[1];
				h2 = b->handles[0];
			}
			
			
			float segment_length = bezier_distance_update_cache(a->anchor, h1, h2, b->anchor);
			float percent_segment_drawn = (total_length_drawn - length_accum) / segment_length;
			if(percent_segment_drawn <= 0) break;
			if(percent_segment_drawn > 1) percent_segment_drawn = 1;
			
			unsigned int total_equidistant_points_len = (unsigned int)ceil(segment_length / (data.strokes[i].scale / 2.0f));
			unsigned int drawn_points_len = (unsigned int)ceil(percent_segment_drawn * total_equidistant_points_len) + 1;

			//TODO: Instanced drawing
			for(size_t p = 0; p < drawn_points_len; p++) {
				vec2 loc = bezier_cubic(a->anchor, h1, h2, b->anchor, bezier_distance_closest_t(p/(float)total_equidistant_points_len));
				glUniform1f(brush_shader.uniforms[BRUSH_UNIFORM_ROTATION], reverse ? (float)total_equidistant_points_len - (float)p : (float)p);
				glUniform2f(brush_shader.uniforms[BRUSH_UNIFORM_TRANSLATION], loc.x, loc.y);
				
				float scale = data.strokes[i].scale;
				if(data.strokes[i].jitter > 0) {
					scale += scale * map(random_samples[(reverse ? total_equidistant_points_len-p : p) % RANDOM_SAMPLE_SIZE], 0, 1, -data.strokes[i].jitter, data.strokes[i].jitter);
				}
				glUniform2f(brush_shader.uniforms[BRUSH_UNIFORM_SCALE], scale, scale);
		
				glUniform1i(brush_shader.uniforms[BRUSH_UNIFORM_MASK_TEXTURE], 0);
				glActiveTexture(GL_TEXTURE0);
				glBindTexture(GL_TEXTURE_2D, mask_texture);
				
				glUniform1i(brush_shader.uniforms[BRUSH_UNIFORM_BRUSH_TEXTURE], 1);
				glActiveTexture(GL_TEXTURE0+1);
				glBindTexture(GL_TEXTURE_2D, brush_texture);

				glBindVertexArray(plane_vao);
				glDrawArrays(GL_TRIANGLES, 0, 6);
			}
			
			length_accum += segment_length;
			if(percent_segment_drawn < 1.0f) break;
		}
	}
}

static GLuint export_fbo;
static GLuint export_rbo;

static void render_stroke_export_frame(const float time, uint8_t* data, vec2 size, vec2 framebuffer_size, vec2 offset) {
	
	glBindFramebuffer(GL_FRAMEBUFFER, export_fbo);
	glBindRenderbuffer(GL_RENDERBUFFER, export_rbo);
	glClearColor(0,0,0,0);
	glClear(GL_COLOR_BUFFER_BIT);
	
	update_ortho(crop_ortho, offset.x, offset.x + size.x, offset.y + size.y, offset.y, 0, 1);
	lb_strokes_render_strokes(time, crop_ortho, (vec2){0,0});
	
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glReadPixels(0, 0, framebuffer_size.x, framebuffer_size.y, GL_RGBA, GL_UNSIGNED_BYTE, data);
}

void lb_strokes_render_export(const char* outdir, const float fps, struct lb_export_options options) {
	assert(lb_strokes_export_range_set);
	const float frametime = 1 / fps;
	const uint32_t frames = ceil(lb_strokes_export_range_duration / frametime);
	
	vec2 size = {
		.x = fabsf(lb_strokes_artboard[0].x - lb_strokes_artboard[1].x),
		.y = fabsf(lb_strokes_artboard[0].y - lb_strokes_artboard[1].y)
	};
	vec2 framebuffer_size = size;
	if(options.retina_2x) {
		framebuffer_size.x *= 2;
		framebuffer_size.y *= 2;
	}
	
	vec2 offset = {
		.x = lb_strokes_artboard[0].x < lb_strokes_artboard[1].x ? lb_strokes_artboard[0].x : lb_strokes_artboard[1].x,
		.y = lb_strokes_artboard[0].y < lb_strokes_artboard[1].y ? lb_strokes_artboard[0].y : lb_strokes_artboard[1].y
	};
	
	glGenFramebuffers(1, &export_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, export_fbo);
	glGenRenderbuffers(1, &export_rbo);
	glBindRenderbuffer(GL_RENDERBUFFER, export_rbo);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, (GLsizei)framebuffer_size.x, (GLsizei)framebuffer_size.y);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, export_rbo);
	if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "Incomplete framebuffer.\n");
		return;
	}
	glCheckError();
	
	glViewport(0, 0, (GLsizei)framebuffer_size.x, (GLsizei)framebuffer_size.y);
	
	char out_file[4096]; // TODO: PATH_MAX
	
	switch(options.type) {
		case EXPORT_IMAGE_SEQUENCE: {
			uint8_t* data = malloc(framebuffer_size.x*framebuffer_size.y*4);
			for(uint32_t i = 0; i < frames; i++) {
				
				render_stroke_export_frame(lb_strokes_export_range_begin + i * frametime, data, size, framebuffer_size, offset);
				
				snprintf(out_file, 4096, "%s/line_%04d.png", outdir, i);
				stbi_flip_vertically_on_write(1);
				stbi_write_png(out_file, framebuffer_size.x, framebuffer_size.y, 4, data, 0);
			}
			free(data);
			break;
		}
		case EXPORT_SPRITESHEET: {
			uint8_t* data = malloc(frames*framebuffer_size.x*framebuffer_size.y*4);
			uint8_t* cursor = data + (int)(frames*framebuffer_size.x*framebuffer_size.y*4) - (int)(framebuffer_size.x*framebuffer_size.y*4); // start at the end because they're flipped backwards
			for(uint32_t i = 0; i < frames; i++) {
				render_stroke_export_frame(lb_strokes_export_range_begin + i * frametime, cursor, size, framebuffer_size, offset);
				cursor -= (int)(framebuffer_size.x*framebuffer_size.y*4);
			}
			
			stbi_flip_vertically_on_write(1);
			stbi_write_png(outdir, framebuffer_size.x, framebuffer_size.y*frames, 4, data, 0);
			
			free(data);

			if(options.spritesheet.include_css) {
				strncpy(out_file, outdir, 4096);
				char html_out_file[4096];
				strncpy(html_out_file, out_file, 4096);
				strncat(html_out_file, ".html", 4096);
				
				FILE* file = fopen(html_out_file, "w");
				if(!file) {
					fprintf(stderr, "Could not open output file %s\nError: %s\n", html_out_file, strerror(errno));
					return;
				}
				
				fprintf(file, "<!DOCTYPE html>\n\
<html>\n\
<head>\n\
	<style>\n\
		@keyframes play {\n\
			from { background-position: 0 0; }\n\
			to { background-position: 0 -%.0fpx; }\n\
		}\n\
		\n\
		#drawing {\n\
			width: %.0fpx;\n\
			height: %.0fpx;\n\
			background-image: url(\"%s\");\n\
			animation: play %.2fs steps(%d) infinite;\n\
		}\n\
	</style>\n\
</head>\n\
<body>\n\
	<div id=\"drawing\"></div>\n\
</body>\n\
</html>\n", framebuffer_size.y*frames, size.x, size.y, basename(out_file), lb_strokes_export_range_duration, frames);
				fclose(file);
			}
			
			break;
		}
	}
	
	glDeleteRenderbuffers(1, &export_rbo);
	glDeleteFramebuffers(1, &export_fbo);
}

void lb_strokes_render_app() {
	
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	glViewport(0, 0, (GLsizei)framebufferWidth, (GLsizei)framebufferHeight);
	
	update_ortho(screen_ortho, 0, windowWidth, windowHeight, 0, 0, 1);
	lb_strokes_render_strokes(lb_strokes_timelinePosition, screen_ortho, lb_strokes_pan);

	if(lb_strokes_selected && input_mode != INPUT_ARTBOARD && input_mode != INPUT_TRIM) {
		// Draw lines
		glUseProgram(line_shader.program);
		glEnable(GL_PROGRAM_POINT_SIZE);
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
		glUniform2f(line_shader.uniforms[LINE_UNIFORM_PAN], lb_strokes_pan.x, lb_strokes_pan.y);
		
		glUniformMatrix4fv(line_shader.uniforms[LINE_UNIFORM_PROJECTION], 1, GL_FALSE, (const GLfloat*) screen_ortho);
		glUniform3f(line_shader.uniforms[LINE_UNIFORM_COLOR], 1.0f, 0.0f, 0.0f);
		glUniform1f(line_shader.uniforms[LINE_UNIFORM_POINT_SIZE], 5.0f * (framebufferWidth / windowWidth));

		glBindVertexArray(gl_lines.vao);
		glBindBuffer(GL_ARRAY_BUFFER, gl_lines.vbo);
		
		// -- Curves
		for(size_t v = 0; v < lb_strokes_selected->vertices_len-1; v++) {
			struct bezier_point* a = &lb_strokes_selected->vertices[v];
			struct bezier_point* b = &lb_strokes_selected->vertices[v+1];
			float len = bezier_estimate_length(a->anchor, a->handles[1], b->handles[0], b->anchor);
			uint16_t segments = hyperbola_min_segments(len);
			float step = 1.0f / (float)segments;
			size_t lines = 0;
			for(float t = 0; t <= 1; t += step) {
				vec2 loc = bezier_cubic(a->anchor, a->handles[1], b->handles[0], b->anchor, t);
				glBufferSubData(GL_ARRAY_BUFFER, lines*sizeof(vec2), sizeof(vec2), &loc);
				lines++;
			}
			glDrawArrays(GL_LINE_STRIP, 0, lines);
		}
		
		// -- Handle lines
		{
			glUniform3f(line_shader.uniforms[LINE_UNIFORM_COLOR], 1.0f, 0.0f, 0.0f);
			for(size_t v = 0; v < lb_strokes_selected->vertices_len; v++) {
				glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vec2), &lb_strokes_selected->vertices[v].handles[0]);
				glBufferSubData(GL_ARRAY_BUFFER, sizeof(vec2), sizeof(vec2), &lb_strokes_selected->vertices[v].anchor);
				glBufferSubData(GL_ARRAY_BUFFER, sizeof(vec2)*2, sizeof(vec2), &lb_strokes_selected->vertices[v].handles[1]);
				glDrawArrays(GL_LINE_STRIP, 0, 3);
			}
		}
		
		// -- Control points
		{
			// Indicate the selected vertex
			if(lb_strokes_selected_vertex) {
				glUniform3f(line_shader.uniforms[LINE_UNIFORM_COLOR], 1.0f, 0.0f, 1.0f);
				glUniform1f(line_shader.uniforms[LINE_UNIFORM_POINT_SIZE], 9.0f * (framebufferWidth / windowWidth));
				glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vec2), &lb_strokes_selected_vertex->anchor);
				glDrawArrays(GL_POINTS, 0, 1);
			}
			
			glUniform3f(line_shader.uniforms[LINE_UNIFORM_COLOR], 1.0f, 0.0f, 0.0f);
			glUniform1f(line_shader.uniforms[LINE_UNIFORM_POINT_SIZE], 5.0f * (framebufferWidth / windowWidth));

			glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vec2) * 3 * lb_strokes_selected->vertices_len, lb_strokes_selected->vertices);
			glDrawArrays(GL_POINTS, 0, 3 * lb_strokes_selected->vertices_len);

			glUniform3f(line_shader.uniforms[LINE_UNIFORM_COLOR], 1.0f, 1.0f, 1.0f);
			glUniform1f(line_shader.uniforms[LINE_UNIFORM_POINT_SIZE], 3.0f * (framebufferWidth / windowWidth));
			
			glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vec2) * 3 * lb_strokes_selected->vertices_len, lb_strokes_selected->vertices);
			glDrawArrays(GL_POINTS, 0, 3 * lb_strokes_selected->vertices_len);
		}

		glCheckError();
	}
	
	// Artboard box
	if(lb_strokes_artboard_set || lb_strokes_artboard_set_idx == 1) {
		
		glUseProgram(line_shader.program);
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
		glUniform2f(line_shader.uniforms[LINE_UNIFORM_PAN], lb_strokes_pan.x, lb_strokes_pan.y);
		glUniformMatrix4fv(line_shader.uniforms[LINE_UNIFORM_PROJECTION], 1, GL_FALSE, (const GLfloat*) screen_ortho);

		glBindVertexArray(gl_lines.vao);
		glBindBuffer(GL_ARRAY_BUFFER, gl_lines.vbo);
		
		glUniform3f(line_shader.uniforms[LINE_UNIFORM_COLOR], 0.75f, 0.75f, 0.75f);
		vec2 corners[5];
		corners[0] = lb_strokes_artboard[0];
		corners[2] = lb_strokes_artboard[1];
		corners[1] = (vec2){corners[0].x, corners[2].y};
		corners[3] = (vec2){corners[2].x, corners[0].y};
		corners[4] = lb_strokes_artboard[0];
		
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vec2)*5, corners);
		glDrawArrays(GL_LINE_STRIP, 0, 5);
	}
}

void lb_strokes_handleMouseDown(int button, vec2 point, float time) {
	point = vec2_sub(point, lb_strokes_pan);
	switch(button) {
		case GLFW_MOUSE_BUTTON_LEFT:
			switch(input_mode) {
				case INPUT_SELECT: {
					if(!lb_strokes_selected) {
						for(size_t i = 0; i < data.strokes_len; i++) {
							for(size_t v = 0; v < data.strokes[i].vertices_len-1; v++) {
								struct bezier_point* a = &data.strokes[i].vertices[v];
								struct bezier_point* b = &data.strokes[i].vertices[v+1];
								vec2 closest = bezier_closest_point(a->anchor, a->handles[1], b->handles[0], b->anchor, 20, 3, point);
								if(vec2_dist(point, closest) <= select_tolerance_dist) {
									lb_strokes_selected = &data.strokes[i];
									lb_strokes_selected_vertex = NULL;
									goto exit;
								}
							}
						}
						
						goto exit;
					}
					
					// Check all control points
					for(size_t i = 0; i < lb_strokes_selected->vertices_len; i++) {
						if(vec2_dist(point, lb_strokes_selected->vertices[i].anchor) <= select_tolerance_dist) {
							drag_mode = DRAG_ANCHOR;
							drag_vec = &lb_strokes_selected->vertices[i].anchor;
							lb_strokes_selected_vertex = &lb_strokes_selected->vertices[i];
							break;
						} else if(vec2_dist(point, lb_strokes_selected->vertices[i].handles[0]) <= select_tolerance_dist) {
							drag_mode = DRAG_HANDLE;
							drag_vec = &lb_strokes_selected->vertices[i].handles[0];
							lb_strokes_selected_vertex = &lb_strokes_selected->vertices[i];
							drag_handle_idx = 0;
							break;
						} else if(vec2_dist(point, lb_strokes_selected->vertices[i].handles[1]) <= select_tolerance_dist) {
							drag_mode = DRAG_HANDLE;
							drag_vec = &lb_strokes_selected->vertices[i].handles[1];
							lb_strokes_selected_vertex = &lb_strokes_selected->vertices[i];
							drag_handle_idx = 1;
							break;
						}
					}
					if(drag_mode != DRAG_NONE) break;
					
					// Check the stroke itself
					for(size_t v = 0; v < lb_strokes_selected->vertices_len-1; v++) {
						struct bezier_point* a = &lb_strokes_selected->vertices[v];
						struct bezier_point* b = &lb_strokes_selected->vertices[v+1];
						vec2 closest = bezier_closest_point(a->anchor, a->handles[1], b->handles[0], b->anchor, 20, 3, point);
						if(vec2_dist(point, closest) <= select_tolerance_dist) {
							drag_start = point;
							drag_mode = DRAG_STROKE;
							goto exit;
						}
					}
					lb_strokes_selected = NULL; // not close enough to any points, so must be a deselect
					lb_strokes_selected_vertex = NULL;
					
					break;
					
					// Search the other strokes			
					// for(size_t v = 0; v < lb_strokes_selected->vertices_len-1; v++) {
					// 	struct bezier_point* a = &lb_strokes_selected->vertices[v];
					// 	struct bezier_point* b = &lb_strokes_selected->vertices[v+1];
					// 	float len = bezier_estimate_length(a, b);
					// 	uint16_t segments = hyperbola_min_segments(len);
					// 	float step = 1.0f / (float)segments;
					// 	for(float t = 0; t <= 1; t += step) {
					// 		if(vec2_dist(point, bezier_cubic(a, b, t)) <= select_tolerance_dist) {
					// 			goto set_drag_stroke;
					// 		}
					// 	}
					// }
					
					exit:
					break;
				}

				case INPUT_DRAW: {
					if(!lb_strokes_selected) {
						// Prevent exceeding maximum strokes
						if(data.strokes_len >= MAX_STROKES) return;
						
						lb_strokes_selected = create_stroke();
						lb_strokes_selected->global_start_time = lb_strokes_timelinePosition - 0.35f;
						lb_strokes_selected->full_duration = 1.0f;
						lb_strokes_selected->scale = 5.25f;
						lb_strokes_selected->jitter = 0.25f;
						lb_strokes_selected->color = (colorf){0,0,0,1};
						lb_strokes_selected->enter = (struct lb_stroke_transition){
							.animate_method = ANIMATE_DRAW,
							.duration = 0.35f,
							.draw_reverse = false,
						};
						lb_strokes_selected->exit = (struct lb_stroke_transition){
							.animate_method = ANIMATE_DRAW,
							.duration = 0.35f,
							.draw_reverse = true,
						};
					}
					
					// Prevent exceeding maximum vertices
					if(lb_strokes_selected->vertices_len >= MAX_STROKE_VERTICES) return;
					
					struct bezier_point* vert = add_vertex(lb_strokes_selected);
					vert->anchor = point;
					vert->handles[0] = (vec2){point.x, point.y};
					vert->handles[1] = (vec2){point.x, point.y};

					// Enable dragging of handle
					drag_mode = DRAG_HANDLE;
					lb_strokes_selected_vertex = vert;
					drag_vec = &lb_strokes_selected_vertex->handles[0];
					drag_handle_idx = 0;
					
					break;
				}
				
				case INPUT_ARTBOARD: {
					lb_strokes_artboard[lb_strokes_artboard_set_idx] = point;
					switch(lb_strokes_artboard_set_idx) {
						case 0:
							lb_strokes_artboard_set_idx++;
							lb_strokes_artboard[lb_strokes_artboard_set_idx] = point;
							break;
						case 1:
							lb_strokes_artboard_set = true;
							lb_strokes_artboard_set_idx = -1;
							input_mode = INPUT_SELECT;
							break;
					}
				}
				
				case INPUT_TRIM: {
					if(lb_strokes_export_range_set_idx == 0) {
						lb_strokes_export_range_begin = lb_strokes_timelinePosition;
						lb_strokes_export_range_set_idx++;
					} else if(lb_strokes_export_range_set_idx == 1) {
						lb_strokes_export_range_set_idx = -1;
						lb_strokes_export_range_set = true;
						lb_strokes_export_range_duration = lb_strokes_timelinePosition - lb_strokes_export_range_begin;
						input_mode = INPUT_SELECT;
					}
					break;
				}
			}
			break;
		case GLFW_MOUSE_BUTTON_MIDDLE:
			drag_start = point;
			drag_mode = DRAG_PAN;
			break;
	}
}

void lb_strokes_handleScroll(vec2 dist) {
	lb_strokes_pan = vec2_add(lb_strokes_pan, dist);
}

void lb_strokes_handleMouseMove(vec2 point, float time) {
	
	if(input_mode == INPUT_ARTBOARD && lb_strokes_artboard_set_idx == 1) {
		lb_strokes_artboard[1] = vec2_sub(point, lb_strokes_pan);
		return;
	} else if(input_mode == INPUT_TRIM) {
		lb_strokes_timelinePosition = point.x / windowWidth * lb_strokes_timelineDuration;
		if(lb_strokes_export_range_set_idx == 1 && lb_strokes_timelinePosition < lb_strokes_export_range_begin) lb_strokes_timelinePosition = lb_strokes_export_range_begin; 
		return;
	}
	
	switch(drag_mode) {
		case DRAG_NONE:
			return;
		case DRAG_ANCHOR: {
			assert(lb_strokes_selected_vertex);
			assert(drag_vec);
			vec2 diff = vec2_sub(vec2_sub(point, lb_strokes_pan), *drag_vec);
			*drag_vec = vec2_sub(point, lb_strokes_pan);
			lb_strokes_selected_vertex->handles[0] = vec2_add(lb_strokes_selected_vertex->handles[0], diff);
			lb_strokes_selected_vertex->handles[1] = vec2_add(lb_strokes_selected_vertex->handles[1], diff);
			break;
		}
		case DRAG_HANDLE: {
			assert(lb_strokes_selected_vertex);			
			*drag_vec = vec2_sub(point, lb_strokes_pan);
			if(mods_pressed[MOD_ALT]) break;
			
			// mirror the other point
			lb_strokes_selected_vertex->handles[drag_handle_idx ? 0 : 1].x = 2*lb_strokes_selected_vertex->anchor.x - drag_vec->x;
			lb_strokes_selected_vertex->handles[drag_handle_idx ? 0 : 1].y = 2*lb_strokes_selected_vertex->anchor.y - drag_vec->y;
			break;
		}
		case DRAG_STROKE: {
			assert(lb_strokes_selected);
			vec2 diff = vec2_sub(vec2_sub(point, lb_strokes_pan), drag_start);
			drag_start = vec2_add(drag_start, diff);
			for(size_t i = 0; i < lb_strokes_selected->vertices_len; i++) {
				lb_strokes_selected->vertices[i].anchor = vec2_add(lb_strokes_selected->vertices[i].anchor, diff);
				lb_strokes_selected->vertices[i].handles[0] = vec2_add(lb_strokes_selected->vertices[i].handles[0], diff);
				lb_strokes_selected->vertices[i].handles[1] = vec2_add(lb_strokes_selected->vertices[i].handles[1], diff);
			}
			break;
		}
		case DRAG_PAN: {
			vec2 diff = vec2_sub(point, drag_start);
			drag_start = vec2_add(drag_start, diff);
			lb_strokes_pan = vec2_add(lb_strokes_pan, diff);
		}
	}
}

void lb_strokes_handleMouseUp(int button) {
	drag_mode = DRAG_NONE;
}

void lb_strokes_handleKeyDown(int key, int scancode, int mods) {
	float multiplier = 1;
	if(mods & GLFW_MOD_SHIFT) multiplier = 10.0f;
	
	switch(key) {
		case GLFW_KEY_LEFT:
			lb_strokes_timelinePosition -= 0.016f * multiplier;
			break;
		case GLFW_KEY_RIGHT:
			lb_strokes_timelinePosition += 0.016f * multiplier;
			break;
		case GLFW_KEY_SPACE:
			if(input_mode == INPUT_TRIM) break;
			lb_strokes_playing = !lb_strokes_playing;
			break;
		case GLFW_KEY_TAB:
			if(input_mode == INPUT_DRAW) input_mode = INPUT_SELECT;
			else if(input_mode == INPUT_SELECT) input_mode = INPUT_DRAW;
			break;
		case GLFW_KEY_ESCAPE:
			lb_strokes_selected = NULL;
			lb_strokes_selected_vertex = NULL;
			break;
		case GLFW_KEY_BACKSPACE:
		case GLFW_KEY_DELETE:
			if(lb_strokes_selected && lb_strokes_selected_vertex) {
				delete_vertex(lb_strokes_selected, lb_strokes_selected_vertex);
				lb_strokes_selected_vertex = NULL;
				if(lb_strokes_selected->vertices_len <= 1) delete_stroke(lb_strokes_selected), lb_strokes_selected = NULL;
			} else if(lb_strokes_selected) {
				delete_stroke(lb_strokes_selected);
				lb_strokes_selected = NULL;
			}
			break;
		case GLFW_KEY_D:
			if(lb_strokes_selected && mods & GLFW_MOD_CONTROL) {
				lb_strokes_selected = duplicate_stroke(lb_strokes_selected);
				lb_strokes_selected_vertex = NULL;
			}
		case GLFW_KEY_LEFT_ALT:
		case GLFW_KEY_RIGHT_ALT:
			mods_pressed[MOD_ALT] = true;
			break;
	}
}

void lb_strokes_handleKeyRepeat(int key, int scancode, int mods) {
	float multiplier = 1;
	if(mods & GLFW_MOD_SHIFT) multiplier = 10.0f;
	
	switch(key) {
		case GLFW_KEY_LEFT:
			lb_strokes_timelinePosition -= 0.1f * multiplier;
			break;
		case GLFW_KEY_RIGHT:
			lb_strokes_timelinePosition += 0.1f * multiplier;
			break;
	}
}

void lb_strokes_handleKeyUp(int key, int scancode, int mods) {
	switch(key) {
		case GLFW_KEY_LEFT_ALT:
		case GLFW_KEY_RIGHT_ALT:
			mods_pressed[MOD_ALT] = false;
			break;
	}
}

void lb_strokes_save(const char* filename) {
	FILE* file = fopen(filename, "wb");
	if(!file) {
		fprintf(stderr, "Could not open output file %s\n", filename);
		return;
	}
	
	static const unsigned int version = 0;
	fwrite("LINE", 1, 4, file);
	fwrite(&version, 4, 1, file);
	fwrite(&lb_strokes_timelineDuration, 4, 1, file);
	fwrite(&lb_strokes_artboard_set, 1, 1, file);
	fwrite(&lb_strokes_artboard, 8, 2, file);
	fwrite(&lb_strokes_export_range_set, 1, 1, file);
	fwrite(&lb_strokes_export_range_begin, 4, 1, file);
	fwrite(&lb_strokes_export_range_duration, 4, 1, file);
	fwrite(&lb_strokes_export_fps, 4, 1, file);
	fwrite(&data.strokes_len, 4, 1, file);
	for(size_t i = 0; i < data.strokes_len; i++) {
		fwrite(&data.strokes[i].global_start_time, 4, 1, file);
		fwrite(&data.strokes[i].full_duration, 4, 1, file);
		fwrite(&data.strokes[i].scale, 4, 1, file);
		fwrite(&data.strokes[i].color, 4, 4, file);
		fwrite(&data.strokes[i].jitter, 4, 1, file);
		
		fwrite(&data.strokes[i].enter.animate_method, 4, 1, file);
		fwrite(&data.strokes[i].enter.easing_method, 4, 1, file);
		fwrite(&data.strokes[i].enter.duration, 4, 1, file);
		fwrite(&data.strokes[i].enter.draw_reverse, 1, 1, file);
		
		fwrite(&data.strokes[i].exit.animate_method, 4, 1, file);
		fwrite(&data.strokes[i].exit.easing_method, 4, 1, file);
		fwrite(&data.strokes[i].exit.duration, 4, 1, file);
		fwrite(&data.strokes[i].exit.draw_reverse, 1, 1, file);
		
		fwrite(&data.strokes[i].vertices_len, 2, 1, file);
		for(size_t v = 0; v < data.strokes[i].vertices_len; v++) {
			fwrite(&data.strokes[i].vertices[v], 8, 3, file);
		}
	}
	
	fclose(file);
}

void lb_strokes_open(const char* filename) {
	FILE* file = fopen(filename, "rb");
	if(!file) {
		fprintf(stderr, "Could not open file %s\n", filename);
		return;
	}
	
	// Reset current state
	data.strokes_len = 0;
	pool_reset(data.vertices_pool);
	lb_strokes_selected_vertex = NULL;
	lb_strokes_selected = NULL;
	lb_strokes_pan = (vec2){0,0};
	
	char buf[4];
	fread(buf, 1, 4, file);
	if(strncmp(buf, "LINE", 4) != 0) {
		goto error;
	}
	
	unsigned int version;
	fread(&version, 4, 1, file);
	
	fread(&lb_strokes_timelineDuration, 4, 1, file);
	fread(&lb_strokes_artboard_set, 1, 1, file);
	fread(&lb_strokes_artboard, 8, 2, file);
	fread(&lb_strokes_export_range_set, 1, 1, file);
	fread(&lb_strokes_export_range_begin, 4, 1, file);
	fread(&lb_strokes_export_range_duration, 4, 1, file);
	fread(&lb_strokes_export_fps, 4, 1, file);
	fread(&data.strokes_len, 4, 1, file);

	for(size_t i = 0; i < data.strokes_len; i++) {
		fread(&data.strokes[i].global_start_time, 4, 1, file);
		fread(&data.strokes[i].full_duration, 4, 1, file);
		fread(&data.strokes[i].scale, 4, 1, file);
		fread(&data.strokes[i].color, 4, 4, file);
		fread(&data.strokes[i].jitter, 4, 1, file);
		
		fread(&data.strokes[i].enter.animate_method, 4, 1, file);
		fread(&data.strokes[i].enter.easing_method, 4, 1, file);
		fread(&data.strokes[i].enter.duration, 4, 1, file);
		fread(&data.strokes[i].enter.draw_reverse, 1, 1, file);
		
		fread(&data.strokes[i].exit.animate_method, 4, 1, file);
		fread(&data.strokes[i].exit.easing_method, 4, 1, file);
		fread(&data.strokes[i].exit.duration, 4, 1, file);
		fread(&data.strokes[i].exit.draw_reverse, 1, 1, file);
		
		fread(&data.strokes[i].vertices_len, 2, 1, file);
		data.strokes[i].vertices = pool_alloc(data.vertices_pool);
		for(size_t v = 0; v < data.strokes[i].vertices_len; v++) {
			fread(&data.strokes[i].vertices[v], 8, 3, file);
		}
	}
	
	fclose(file);
	return;
	
	error:
		fclose(file);
		fprintf(stderr, "Invalid or corrupt file format.\n");
		return;
}