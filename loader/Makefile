CC      = gcc
# -mstackrealign: Xbox (MSVC) code only keeps 4-byte stack alignment; gcc code
# called from it must realign before using SSE or calling into glibc.
CFLAGS  = -m32 -O2 -g -Wall -Wno-unused-parameter -Wno-unused-function -fno-pie -mstackrealign \
          -fno-omit-frame-pointer -D_FILE_OFFSET_BITS=64 -Ithird_party/include -Ithird_party/include/SDL2 -D_REENTRANT
# Link SDL2 by soname so only the runtime package (libsdl2-2.0-0:i386) is required.
LIBSDL  ?= $(firstword $(wildcard /usr/lib/i386-linux-gnu/libSDL2-2.0.so.0 /usr/lib32/libSDL2-2.0.so.0 /usr/lib/libSDL2-2.0.so.0))
LDFLAGS = -m32 -no-pie -pthread -Wl,-rpath,'$$ORIGIN'
# Movies (xmv.c): a minimal static ffmpeg, built by ../tools/build_ffmpeg.sh (nothing extra to install to run)
FFMPEG  ?= third_party/ffmpeg32
CFLAGS += -I$(FFMPEG)/include
LDLIBS  = $(FFMPEG)/lib/libavformat.a $(FFMPEG)/lib/libavcodec.a $(FFMPEG)/lib/libswscale.a \
          $(FFMPEG)/lib/libswresample.a $(FFMPEG)/lib/libavutil.a $(LIBSDL) -lm -ldl

SRCS = main.c xbe.c thread.c sync.c handles.c fs.c kmem.c kfile.c kthread.c krtl.c kmisc.c kvars.c \
       hle.c input_sdl.c dsound.c audio_sdl.c timing.c debugmenu.c redpose.c redvoice.c redcast.c redcut.c redshrink.c coop.c redctx.c xmv.c gen_kernel.c \
       gfx/d3d.c gfx/vsh.c gfx/psh.c gfx/tex.c gfx/gl.c gfx/overlay.c gfx/freecam.c
OBJS = $(SRCS:.c=.o)
.DEFAULT_GOAL := xboxrun

$(FFMPEG)/lib/libavformat.a:
	../tools/build_ffmpeg.sh

xmv.o: $(FFMPEG)/lib/libavformat.a

xboxrun: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

gen_kernel.c: ../tools/gen_kstubs.py ../data/kernel_imports.json
	python3 -I ../tools/gen_kstubs.py ../data/kernel_imports.json $@

%.o: %.c xbox.h gfx/gfx.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) gen_kernel.c xboxrun
