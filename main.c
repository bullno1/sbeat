#include <sokol_app.h>
#include <sokol_gfx.h>
#include <sokol_glue.h>
#include <fontstash.h>
#include <sokol_gl.h>
#include <fontstash.h>
#include <sokol_fontstash.h>
#include <sokol_log.h>
#include <sokol_audio.h>
#include <expr.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
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

typedef struct {
	struct expr* expr;
	struct expr_var_list vars;
} formula_t;

static FONScontext* fons = NULL;
static int text_font = 0;
static text_edit_t formula_buf = { 0 };
static int last_formula_version = 0;
static bool formula_is_valid = true;
static atomic_uintptr_t incoming_formula_ptr = 0;
static formula_t submission_buffer[2] = { 0 };
static uintptr_t outgoing_formula_ptr = (uintptr_t)&submission_buffer[0];
static bool should_submit_formula = false;

static void
audio(float* buffer, int num_frames, int num_channels);

static void
parse_formula(const char* text, int len);

static void
try_submit_formula(void);

static void
cleanup_formula(formula_t* formula);

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

	cleanup_formula(&submission_buffer[0]);
	cleanup_formula(&submission_buffer[1]);
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

	if (last_formula_version != formula_buf.version) {
		parse_formula(formula_buf.text_buf, formula_buf.text_len);
		last_formula_version = formula_buf.version;
	}
}

static void
frame(void) {
	try_submit_formula();

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
			fonsSetColor(
				fons,
				formula_is_valid
					? sfons_rgba(0, 255, 0, 255)
					: sfons_rgba(255, 0, 0, 255)
			);
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
			if (formula_is_valid) {
				sgl_c4b(0, 255, 0, 255);
			} else {
				sgl_c4b(255, 0, 0, 255);
			}

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
cleanup_formula(formula_t* formula) {
	if (formula->expr != NULL) {
		expr_destroy(formula->expr, &formula->vars);
		formula->expr = NULL;
	}
}

static float
expr_select(struct expr_func* f, vec_expr_t* args, void* c) {
	if (vec_len(args) != 3) { return 0.f; }

	if (expr_eval(&vec_nth(args, 0)) != 0.f) {
		return expr_eval(&vec_nth(args, 1));
	} else {
		return expr_eval(&vec_nth(args, 2));
	}
}

static void
parse_formula(const char* text, int len) {
	static struct expr_func custom_funcs[] = {
		{ .name = "select", .f = expr_select, },
		{ 0 },
	};

	struct expr_var_list vars = { 0 };
	struct expr* expr = expr_create(text, (size_t)len, &vars, custom_funcs);
	if ((formula_is_valid = expr != NULL)) {
		expr_var(&vars, "t", 1);  // Ensure t is always present

		formula_t* outgoing_formula = (void*)outgoing_formula_ptr;
		cleanup_formula(outgoing_formula);
		outgoing_formula->expr = expr;
		outgoing_formula->vars = vars;
		should_submit_formula = true;

		try_submit_formula();
	} else {
		expr_destroy(expr, &vars);
	}
}

static void
try_submit_formula(void) {
	if (!should_submit_formula) { return; }

	// Attempt to submit the buffer
	uintptr_t null = 0;
	bool submitted = atomic_compare_exchange_strong_explicit(
		&incoming_formula_ptr, &null, outgoing_formula_ptr,
		memory_order_release, memory_order_relaxed
	);

	// If successful, swap the buffers
	if (submitted) {
		outgoing_formula_ptr = outgoing_formula_ptr == (uintptr_t)&submission_buffer[0]
			? (uintptr_t)&submission_buffer[1]
			: (uintptr_t)&submission_buffer[0];
	}

	should_submit_formula = !submitted;
}

static void
audio(float* buffer, int num_frames, int num_channels) {
	static formula_t formula = { 0 };
	static int t = 0;

	// Poll for new formula
	formula_t* new_formula = (void*)atomic_load_explicit(
		&incoming_formula_ptr, memory_order_acquire
	);
	if (new_formula != NULL) {
		formula = *new_formula;
		atomic_store_explicit(&incoming_formula_ptr, 0, memory_order_release);
	}

	// Render audio
	if (formula.expr != NULL) {
		struct expr_var* t_var = expr_var(&formula.vars, "t", 1);

		for (int i = 0; i < num_frames; ++i, ++t) {
			t_var->value = (float)t;
			unsigned char out = (unsigned char)(int)expr_eval(formula.expr);
			buffer[i] = (float)out / 255.f * 2.f - 1.f;
		}
	} else {
		memset(buffer, 0, sizeof(float) * num_frames * num_channels);
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
	if (argc > 1) {
		text_edit_insert_string(&formula_buf, argv[1]);
	} else {
		text_edit_insert_string(&formula_buf, "t");
	}
	parse_formula(formula_buf.text_buf, formula_buf.text_len);

	return (sapp_desc){
		.init_cb = init,
		.event_cb = event,
		.frame_cb = frame,
		.cleanup_cb = cleanup,
		.width = 640,
		.height = 480,
		.window_title = "sbeat",
		.icon.sokol_default = true,
		.enable_clipboard = true,
		.logger.func = slog_func,
	};
}
