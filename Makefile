.PHONY: all clean

SANITIZE := -fsanitize=address,undefined -fno-sanitize=vptr

all: sbeat

clean:
	rm -rf *.dbg *.o sbeat

sbeat: main.o libs.o
	clang \
		-O3 \
		-fno-omit-frame-pointer \
		-fuse-ld=mold \
		-Wl,--separate-debug-file \
		${SANITIZE} \
		-lX11 -lXi -lXcursor -lEGL -lGL -lasound \
		$^ \
		-o $@

%.o: %.c
	clang \
		-c \
		-g \
		-O3 \
		-fno-omit-frame-pointer \
		${SANITIZE} \
		-Wall \
		-Werror \
		-pedantic \
		-std=c11 \
		-Ideps/blibs \
		-Ideps/sokol \
		-Ideps/sokol/util \
		-Ideps/fontstash/src \
		-Ideps/expr \
		-o $@ \
		$^
