# Copyright (c) 2011, 2014 Intel Corporation. All Rights Reserved.
#
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sub license, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice (including the
# next paragraph) shall be included in all copies or substantial portions
# of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
# OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
# IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
# ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
# TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
# SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#

pvr_drv_video_la_LTLIBRARIES = pvr_drv_video.la
pvr_drv_video_ladir = $(LIBVA_DRIVERS_PATH)
pvr_drv_video_la_LDFLAGS = $(DRM_LIBS) -ldrm -ldrm_ipvr -pthread -module -avoid-version -Wl,--no-undefined
pvr_drv_video_la_LIBADD = -ldrm -ldrm_ipvr -lX11 -lva-x11 -lm -ldl
AM_CFLAGS = -DDEBUG -DLINUX -I$(top_srcdir)/src/hwdefs $(DRM_CFLAGS) -fvisibility=hidden

pvr_drv_video_la_SOURCES = \
		object_heap.c ipvr_drv_debug.c ipvr_drv_video.c ipvr_surface.c ipvr_output.c \
		ipvr_execbuf.c ved_execbuf.c ved_vld.c ved_vp8.c x11/ipvr_x11.c

CFLAGS += -Wall -ffloat-store -fvisibility=hidden -DIPVR_VIDEO_LOG_ENABLE -DBAYTRAIL

if VA_EGL
CFLAGS += -DVA_EGL
endif

if EC
CFLAGS += -DEC_ENABLED
endif

symbol_info:	pvr_drv_video.la
	objdump -T .libs/pvr_drv_video.so | grep UND | sort -k 5 > Linker_dependencies.txt
	objdump -T .libs/pvr_drv_video.so | grep -v UND | sort -k 5 > Linker_exports.txt

#install-exec-am:
#	-rm -f $(ipvr_drv_video_ladir)/ipvr_drv_video.so
#	-$(LN_S) $(ipvr_drv_video_ladir)/ipvr_drv_video.so $(ipvr_drv_video_ladir)/ipvr_drv_video.so
