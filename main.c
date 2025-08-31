#include <sokol_app.h>
#include <sokol_gfx.h>
#include <sokol_glue.h>
#include <fontstash.h>
#include <sokol_gl.h>
#include <fontstash.h>
#include <sokol_fontstash.h>
#include <sokol_log.h>
#include <sokol_audio.h>
#include <string.h>
#include "resources.rc"

#ifndef TEXTEDIT_BUF_SIZE
#	define TEXTEDIT_BUF_SIZE 1024
#endif

typedef struct {
	char text_buf[TEXTEDIT_BUF_SIZE];
	int text_len;
	int cursor_pos;
	int version;
} text_edit_t;

static FONScontext* fons = NULL;
static int text_font = 0;
static text_edit_t formula_buf = { 0 };

static void
audio(float* buffer, int num_frames, int num_channels);

static void
text_edit_insert_char(text_edit_t* text_edit, uint32_t codepoint);

static void
text_edit_insert_string(text_edit_t* text_edit, const char* string);

static void
text_edit_move_cursor(text_edit_t* text_edit, int delta);

static void
text_edit_delete(text_edit_t* text_edit);

static void
text_edit_backspace(text_edit_t* text_edit);

static void
init(void) {
	sg_setup(&(sg_desc){
		.environment = sglue_environment(),
		.logger.func = slog_func,
	});
	sgl_setup(&(sgl_desc_t){
		.logger = {
			.func = slog_func,
		},
	});

	saudio_setup(&(saudio_desc){
		.sample_rate = 8000,
		.num_channels = 1,
		.stream_cb = audio,
		.logger = {
			.func = slog_func,
		},
	});

	fons = sfons_create(&(sfons_desc_t){
		.width = 4096,
		.height = 4096,
	});
	xincbin_data_t font = XINCBIN_GET(inconsolata_medium_ttf);
	text_font = fonsAddFontMem(fons, "InconsolataMedium", (unsigned char*)font.data, (int)font.size, 0);
}

static void
cleanup(void) {
	sfons_destroy(fons);

	saudio_shutdown();
	sgl_shutdown();
	sg_shutdown();
}

static void
event(const sapp_event* event) {
	switch (event->type) {
		case SAPP_EVENTTYPE_CHAR:
			if ((event->modifiers & SAPP_MODIFIER_CTRL) == 0) {
				text_edit_insert_char(&formula_buf, event->char_code);
			}
			break;
		case SAPP_EVENTTYPE_CLIPBOARD_PASTED:
			text_edit_insert_string(&formula_buf, sapp_get_clipboard_string());
			break;
		case SAPP_EVENTTYPE_KEY_DOWN:
			switch (event->key_code) {
				case SAPP_KEYCODE_LEFT:
					text_edit_move_cursor(&formula_buf, -1);
					break;
				case SAPP_KEYCODE_RIGHT:
					text_edit_move_cursor(&formula_buf, 1);
					break;
				case SAPP_KEYCODE_HOME:
					text_edit_move_cursor(&formula_buf, -TEXTEDIT_BUF_SIZE);
					break;
				case SAPP_KEYCODE_END:
					text_edit_move_cursor(&formula_buf, TEXTEDIT_BUF_SIZE);
					break;
				case SAPP_KEYCODE_DELETE:
					text_edit_delete(&formula_buf);
					break;
				case SAPP_KEYCODE_BACKSPACE:
					text_edit_backspace(&formula_buf);
					break;
				default: break;
			}
			break;
		default: break;
	}
}

static void
render(void) {
	sg_begin_pass(&(sg_pass){ .swapchain = sglue_swapchain() });
	{
		sgl_defaults();
		sgl_viewport(0, 0, sapp_width(), sapp_height(), true);
		sgl_ortho(0.f, sapp_widthf(), sapp_heightf(), 0.f, -1.f, 1.f);

		fonsPushState(fons);
		{
			// Text
			fonsSetSize(fons, 20);
			fonsSetFont(fons, text_font);
			fonsSetAlign(fons, FONS_ALIGN_LEFT | FONS_ALIGN_TOP);
			fonsSetColor(fons, sfons_rgba(255, 255, 255, 255));
			fonsDrawText(
				fons,
				1.f, 0.f,
				formula_buf.text_buf, formula_buf.text_buf + formula_buf.text_len
			);
			sfons_flush(fons);

			// Cursor
			FONStextIter iter = { 0 };
			FONSquad quad;
			fonsTextIterInit(
				fons,
				&iter,
				1.f, 0.f,
				formula_buf.text_buf, formula_buf.text_buf + formula_buf.text_len
			);
			for (int i = 0; i <= formula_buf.cursor_pos; ++i) {
				fonsTextIterNext(fons, &iter, &quad);
			}
			float cursor_x = formula_buf.cursor_pos < formula_buf.text_len
				? quad.x0
				: iter.nextx;
			sgl_c4b(255, 255, 255, 255);

			float line_height;
			fonsVertMetrics(fons, NULL, NULL, &line_height);
			sgl_begin_lines();
			sgl_v2f(cursor_x, 0.f);
			sgl_v2f(cursor_x, line_height);
			sgl_end();
		}
		fonsPopState(fons);

		sgl_draw();
	}
	sg_end_pass();
	sg_commit();
}

static void
audio(float* buffer, int num_frames, int num_channels) {
	static int t = 0;
	for (int i = 0; i < num_frames; ++i, ++t) {
		unsigned char out = (unsigned char)(t*(42&t>>10));

		buffer[i] = (float)out / 255.f - 0.5f;
		buffer[i] = 0;
	}
}

static void
text_edit_insert_char(text_edit_t* text_edit, uint32_t codepoint) {
	if (text_edit->text_len >= TEXTEDIT_BUF_SIZE) { return; }

	// TODO: Handle unicode
	memmove(
		text_edit->text_buf + text_edit->cursor_pos + 1,
		text_edit->text_buf + text_edit->cursor_pos,
		text_edit->text_len - text_edit->cursor_pos
	);
	text_edit->text_buf[text_edit->cursor_pos] = (char)codepoint;

	++text_edit->version;
	++text_edit->cursor_pos;
	++text_edit->text_len;
}

static void
text_edit_insert_string(text_edit_t* text_edit, const char* string) {
	// TODO: this is inefficient
	for (const char* itr = string; *itr != '\0'; ++itr) {
		text_edit_insert_char(text_edit, *itr);
	}
}

static void
text_edit_move_cursor(text_edit_t* text_edit, int delta) {
	text_edit->cursor_pos += delta;

	if (text_edit->cursor_pos < 0) {
		text_edit->cursor_pos = 0;
	}

	if (text_edit->cursor_pos > text_edit->text_len) {
		text_edit->cursor_pos = text_edit->text_len;
	}
}

static void
text_edit_delete(text_edit_t* text_edit) {
	if (text_edit->cursor_pos == text_edit->text_len) { return; }

	memmove(
		text_edit->text_buf + text_edit->cursor_pos,
		text_edit->text_buf + text_edit->cursor_pos + 1,
		text_edit->text_len - text_edit->cursor_pos - 1
	);

	--text_edit->text_len;
	++text_edit->version;
}

static void
text_edit_backspace(text_edit_t* text_edit) {
	if (text_edit->cursor_pos == 0) { return; }

	--text_edit->cursor_pos;
	text_edit_delete(text_edit);
}

sapp_desc
sokol_main(int argc, char* argv[]) {
	(void)argc; (void)argv;
	return (sapp_desc){
		.init_cb = init,
		.event_cb = event,
		.frame_cb = render,
		.cleanup_cb = cleanup,
		.width = 640,
		.height = 480,
		.window_title = "sbeat",
		.icon.sokol_default = true,
		.enable_clipboard = true,
		.logger.func = slog_func,
	};
}
