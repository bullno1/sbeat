#if defined(__linux__)
#	define SOKOL_GLES3
#	define _GNU_SOURCE
#else
#	error "Unsupported platform"
#endif

#define FONTSTASH_IMPLEMENTATION
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stb_truetype.h>
#include <fontstash.h>

#define SOKOL_IMPL
#include <sokol_app.h>
#include <sokol_log.h>
#include <sokol_gfx.h>
#include <sokol_glue.h>
#include <sokol_gl.h>
#include <sokol_fontstash.h>

#define XINCBIN_IMPLEMENTATION
#include "resources.rc"
