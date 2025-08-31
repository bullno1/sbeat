#include <sokol_app.h>
#include <sokol_gfx.h>
#include <sokol_glue.h>
#include <fontstash.h>
#include <sokol_gl.h>
#include <fontstash.h>
#include <sokol_fontstash.h>
#include <sokol_log.h>
#include "resources.rc"

static FONScontext* fons = NULL;
static int text_font = 0;

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
	sgl_shutdown();
    sg_shutdown();
}

static void
event(const sapp_event* event) {
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
			fonsSetSize(fons, 20);
			fonsSetFont(fons, text_font);
			fonsSetAlign(fons, FONS_ALIGN_LEFT | FONS_ALIGN_TOP);
			fonsSetColor(fons, sfons_rgba(255, 255, 255, 255));
			fonsDrawText(fons, 0.f, 0.f, "Test", NULL);
		}
		fonsPopState(fons);
		sfons_flush(fons);

		sgl_draw();
	}
    sg_end_pass();
    sg_commit();
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
        .logger.func = slog_func,
    };
}
