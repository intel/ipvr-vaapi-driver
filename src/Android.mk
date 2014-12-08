# Copyright (c) 2014 Intel Corporation. All Rights Reserved.
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

LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

IPVR_VIDEO_LOG_ENABLE := true

LOCAL_SRC_FILES :=               \
    object_heap.c                  \
    ipvr_drv_debug.c                \
    ipvr_drv_video.c                \
    ipvr_surface.c                  \
    ipvr_output.c                  \
    android/ipvr_android.c           \
    ipvr_execbuf.c                 \
    ved_execbuf.c          \
    ved_vld.c                  \
    ved_vp8.c
LOCAL_SHARED_LIBRARIES := libdl libdrm libdrm_ipvr libcutils

LOCAL_CFLAGS := -DLINUX -DANDROID -g -Wall -Wno-unused -O0

LOCAL_C_INCLUDES :=                              \
    $(TARGET_OUT_HEADERS)/libva                    \
    $(TARGET_OUT_HEADERS)/libdrm                   \
    $(LOCAL_PATH)/hwdefs                           \

LOCAL_MODULE_TAGS := eng
LOCAL_MODULE := pvr_drv_video

ifeq ($(strip $(IPVR_VIDEO_LOG_ENABLE)),true)
LOCAL_CFLAGS += -DIPVR_VIDEO_LOG_ENABLE -DLOG_TAG=\"pvr_drv_video\"
LOCAL_SHARED_LIBRARIES += liblog
endif

ifeq ($(TARGET_BOARD_PLATFORM),baytrail)
LOCAL_CFLAGS += -DBAYTRAIL
endif

include $(BUILD_SHARED_LIBRARY)

