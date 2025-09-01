#include <sokol_app.h>
#include <sokol_gfx.h>
#include <sokol_glue.h>
#include <sokol_gl.h>
#include <fontstash.h>
#include <sokol_fontstash.h>
#include <sokol_log.h>
#include <sokol_audio.h>
#include <sokol_time.h>
#include <expr.h>
#ifdef __clang__
#	pragma clang diagnostic ignored "-Wnewline-eof"
#endif
#include <am_fft.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "tribuf.h"
#include "resources.rc"

#ifndef TEXTEDIT_BUF_SIZE
#	define TEXTEDIT_BUF_SIZE 1024
#endif

#ifndef FFT_SIZE
#	define FFT_SIZE 1024
#endif

#define SAMPLING_RATE 8000

typedef struct {
	char text_buf[TEXTEDIT_BUF_SIZE + 1];  // +1 for null-terminator
	int text_len;
	int cursor_pos;
	int version;
} text_edit_t;

typedef struct {
	struct expr* expr;
	struct expr_var_list vars;
} formula_t;

typedef struct {
	int t;
	uint64_t timestamp;
} audio_state_t;

static FONScontext* fons = NULL;
static int text_font = 0;

static text_edit_t formula_buf = { 0 };
static int last_formula_version = 0;
static bool formula_is_valid = true;

static formula_t audio_formulas[3] = { 0 };
static tribuf_t audio_formula_buf;

static formula_t current_formula = { 0 };  // Main thread copy

static audio_state_t audio_states[3] = { 0 };
static tribuf_t audio_state_buf;

static am_fft_plan_1d_t* fft = NULL;
static am_fft_complex_t* fft_in = NULL;
static am_fft_complex_t* fft_out = NULL;

static void
audio(float* buffer, int num_frames, int num_channels);

static void
process_text_edit(void);

static bool
parse_formula(const char* text, int len, formula_t* out);

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
	stm_setup();
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
		.sample_rate = SAMPLING_RATE,
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

	fft = am_fft_plan_1d(AM_FFT_FORWARD, FFT_SIZE);
	fft_in = malloc(sizeof(am_fft_complex_t) * FFT_SIZE);
	fft_out = malloc(sizeof(am_fft_complex_t) * FFT_SIZE);
}

static void
cleanup(void) {
	free(fft_in);
	free(fft_out);
	am_fft_plan_1d_free(fft);

	sfons_destroy(fons);

	saudio_shutdown();
	sgl_shutdown();
	sg_shutdown();

	cleanup_formula(&audio_formulas[0]);
	cleanup_formula(&audio_formulas[1]);
	cleanup_formula(&current_formula);
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
				case SAPP_KEYCODE_C:
					if (event->modifiers & SAPP_MODIFIER_CTRL) {
						formula_buf.text_buf[formula_buf.text_len] = '\0';
						sapp_set_clipboard_string(formula_buf.text_buf);
					}
					break;
				default: break;
			}
			break;
		default: break;
	}

	if (last_formula_version != formula_buf.version) {
		process_text_edit();
		last_formula_version = formula_buf.version;
	}
}

static void
frame(void) {
	tribuf_try_swap(&audio_formula_buf);

	sg_begin_pass(&(sg_pass){ .swapchain = sglue_swapchain() });
	{
		sgl_defaults();
		sgl_viewport(0, 0, sapp_width(), sapp_height(), true);
		sgl_ortho(0.f, sapp_widthf(), sapp_heightf(), 0.f, -1.f, 1.f);

		// Visualize output
		if (current_formula.expr != NULL) {
			static audio_state_t audio_state = { 0 };
			if (audio_state.timestamp == 0) {
				audio_state.timestamp = stm_now();
			}

			audio_state_t* audio_state_ptr = tribuf_begin_recv(&audio_state_buf);
			if (audio_state_ptr != NULL) { audio_state = *audio_state_ptr; }
			tribuf_end_recv(&audio_state_buf);

			sgl_begin_points();
			sgl_point_size(2.f);
			sgl_c4b(0, 0, 255, 255);

			struct expr_var* t_var = expr_var(&current_formula.vars, "t", 1);
			double time_diff_s = stm_sec(stm_now()) - stm_sec(audio_state.timestamp);
			int t = audio_state.t + (int)(time_diff_s * (double)SAMPLING_RATE);
			float width = sapp_widthf();
			float height = sapp_heightf();
			for (float i = 0.f; i < SAMPLING_RATE; i += 1.f) {
				t_var->value = (float)t + i;
				unsigned char out = (unsigned char)(int)expr_eval(current_formula.expr);

				sgl_v2f((float)i / (float)SAMPLING_RATE * width, height - height * (float)out / 255.f);

				if (i < (float)FFT_SIZE) {
					fft_in[(int)i][0] = (float)out / 255.f * 2.f - 1.f;
					fft_in[(int)i][1] = 0.f;
				}
			}

			sgl_end();

			am_fft_1d(fft, fft_in, fft_out);

			sgl_begin_line_strip();
			sgl_c4b(0, 255, 255, 255);
			for (int i = 0; i < FFT_SIZE / 2; ++i) {
				float amplitude = sqrtf(fft_out[i][0] * fft_out[i][0] + fft_out[i][1] * fft_out[i][1]) / (float)FFT_SIZE;
				sgl_v2f((float)i / ((float)FFT_SIZE / 2.f) * width + 1.f, height - height * amplitude);
			}
			sgl_end();
		}

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

static bool
parse_formula(const char* text, int len, formula_t* out) {
	static struct expr_func custom_funcs[] = {
		{ .name = "sel", .f = expr_select, },
		{ 0 },
	};

	struct expr_var_list vars = { 0 };
	struct expr* expr = expr_create(text, (size_t)len, &vars, custom_funcs);
	if (expr != NULL) {
		expr_var(&vars, "t", 1);  // Ensure t is always present
		out->expr = expr;
		out->vars = vars;
		return true;
	} else {
		expr_destroy(expr, &vars);
		return false;
	}
}

static void
process_text_edit(void) {
	formula_t* formula = tribuf_begin_send(&audio_formula_buf);
	cleanup_formula(formula);

	formula_is_valid = parse_formula(
		formula_buf.text_buf, formula_buf.text_len,
		formula
	);
	if (formula_is_valid) {
		tribuf_end_send(&audio_formula_buf);

		// Make a separate copy for main thread to use for visualization
		cleanup_formula(&current_formula);
		parse_formula(
			formula_buf.text_buf, formula_buf.text_len,
			&current_formula
		);
	}
}

static void
audio(float* buffer, int num_frames, int num_channels) {
	static formula_t formula = { 0 };
	static int t = 0;

	// Send state update
	audio_state_t* audio_state = tribuf_begin_send(&audio_state_buf);
	audio_state->t = t;
	audio_state->timestamp = stm_now();
	tribuf_end_send(&audio_state_buf);

	// Receive new formula
	formula_t* new_formula = tribuf_begin_recv(&audio_formula_buf);
	if (new_formula != NULL) { formula = *new_formula; }
	tribuf_end_recv(&audio_formula_buf);

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
	if (string == NULL) { return; }
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
	tribuf_init(&audio_formula_buf, &audio_formulas[0], sizeof(audio_formulas[0]));
	tribuf_init(&audio_state_buf, &audio_states[0], sizeof(audio_states[0]));
	process_text_edit();

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
