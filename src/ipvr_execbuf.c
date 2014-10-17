/*
 * Copyright (c) 2014 Intel Corporation. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Yao Cheng <yao.cheng@intel.com>
 *
 */

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include "ipvr_execbuf.h"

#include <unistd.h>
#include <stdio.h>

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>

#include "ipvr_def.h"
#include "ipvr_drv_debug.h"
#include "ipvr_bufmgr.h"


#define PSB_SLICE_EXTRACT_UPDATE (0x2)

static int
ipvr__execbuffer_reloc(ipvr_execbuffer_p execbuf, drm_ipvr_bo *target_bo,
    unsigned long offset, unsigned long delta, uint32_t flags)
{
    int ret;
    if (offset + 4 > execbuf->bo->size)
        return -ENOMEM;
    ret = drm_ipvr_gem_bo_emit_reloc(execbuf->bo, offset, target_bo, delta, flags);
    if (ret) {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "%s failed to emit reloc for bo %u (offset 0x%lx) at offset 0x%lx: %d (%s)\n",
            __func__, target_bo->handle, target_bo->offset, offset, ret, strerror(ret));
        return ret;
    }
    drv_debug_msg(VIDEO_DEBUG_GENERAL, "%s write reloc for bo %u (offset 0x%lx) at offset 0x%lx: %d (%s)\n",
        __func__, target_bo->handle, target_bo->offset, offset, ret, strerror(ret));
    *(uint32_t*)(execbuf->vaddr + offset) = target_bo->offset + delta;
    ipvr__trace_message("[RE] Reloc at offset %08x (%08x), offset = %08x background = %08x buffer = %d (%08x)\n",
        offset >> 2, offset, delta, 0, 0, target_bo->offset);
    return 0;
}

static void
ipvr__execbuffer_put(ipvr_execbuffer_p execbuf)
{
    if (execbuf->bo) {
        drm_ipvr_gem_bo_unmap(execbuf->bo);
        drm_ipvr_gem_bo_unreference(execbuf->bo);
    }
}

static int
ipvr__execbuffer_full(ipvr_execbuffer_p execbuf)
{
    if (execbuf->bo) {
        return execbuf->cur_offset + 20 >= execbuf->bo->size;
    }
    return 0;
}

int ipvr_execbuffer_run(ipvr_execbuffer_p execbuf)
{
    if(execbuf->run)
        return execbuf->run(execbuf);
    else {
        drv_debug_msg(VIDEO_DEBUG_WARNING, "%s: missing execbuffer run callback!\n", __func__);
        return 0;
    }
}

void ipvr_execbuffer_put(ipvr_execbuffer_p execbuf)
{
    if(execbuf->put)
        execbuf->put(execbuf);
    else
        drv_debug_msg(VIDEO_DEBUG_WARNING, "%s: missing execbuffer put callback!\n", __func__);
}

int ipvr_execbuffer_ready(ipvr_execbuffer_p execbuf)
{
    if(execbuf->ready)
        return execbuf->ready(execbuf);
    else {
        drv_debug_msg(VIDEO_DEBUG_WARNING, "%s: missing execbuffer ready callback!\n", __func__);
        return 1;
    }
}

int ipvr_execbuffer_full(ipvr_execbuffer_p execbuf)
{
    if(execbuf->full)
        return execbuf->full(execbuf);
    else {
        drv_debug_msg(VIDEO_DEBUG_WARNING, "%s: missing execbuffer full callback!\n", __func__);
        return 0;
    }
}

int ipvr_execbuffer_reloc(ipvr_execbuffer_p execbuf, drm_ipvr_bo *target_bo,
                 unsigned long offset, unsigned long target_offset, uint32_t flags)
{
    if(execbuf->reloc)
        return execbuf->reloc(execbuf, target_bo, offset, target_offset, flags);
    else {
        drv_debug_msg(VIDEO_DEBUG_WARNING, "%s: missing execbuffer reloc callback!\n", __func__);
        return 0;
    }
}

int ipvr_execbuffer_add_command(ipvr_execbuffer_p execbuf, int cmd, void *arg, size_t arg_size)
{
    if(execbuf->add_command)
        return execbuf->add_command(execbuf, cmd, arg, arg_size);
    else {
        drv_debug_msg(VIDEO_DEBUG_WARNING, "%s: missing execbuffer add_command callback!\n", __func__);
        return 0;
    }
}

int ipvr_execbuffer_get(drm_ipvr_bufmgr *bufmgr, drm_ipvr_context *ctx,
                 ipvr_execbuffer_p execbuf, const char *name,
                 size_t buf_size, int reusable)
{
    int ret;
    execbuf->put = ipvr__execbuffer_put;
    execbuf->reloc = ipvr__execbuffer_reloc;
    execbuf->full = ipvr__execbuffer_full;
    execbuf->cur_offset = 0;
    execbuf->start_offset = 0;
    execbuf->bo = drm_ipvr_gem_bo_alloc(bufmgr, ctx, name, buf_size, 0,
        DRM_IPVR_WRITECOMBINE, reusable);
    if (!execbuf->bo) {
        return -ENOMEM;
    }
    ret = drm_ipvr_gem_bo_map(execbuf->bo, 0, execbuf->bo->size, 1);
    if (ret) {
        drm_ipvr_gem_bo_unreference(execbuf->bo);
        return ret;
    }
    execbuf->ctx = ctx;
    execbuf->vaddr = execbuf->bo->virt;
    return 0;
}

