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

#ifndef _IPVR_EXECBUF_H_
#define _IPVR_EXECBUF_H_

#include "ipvr_drv_video.h"
#include "ipvr_bufmgr.h"
#include <stdint.h>
#include <libdrm/ipvr_drm.h>

typedef struct ipvr_execbuffer_s ipvr_execbuffer_t;
typedef ipvr_execbuffer_t *ipvr_execbuffer_p;
struct ipvr_execbuffer_s {
    drm_ipvr_bo         *bo;
    drm_ipvr_context    *ctx;
    unsigned long       cur_offset;
    unsigned long       start_offset;
    unsigned char       *vaddr;
    void                *priv;
    unsigned char       valid;

    int (*reloc)(ipvr_execbuffer_p execbuf, drm_ipvr_bo *target_bo,
                 unsigned long offset, unsigned long target_offset, uint32_t flags);
    int (*ready)(ipvr_execbuffer_p execbuf);
    int (*run)(ipvr_execbuffer_p execbuf);
    int (*full)(ipvr_execbuffer_p execbuf);
    void (*put)(ipvr_execbuffer_p execbuf);
    int (*add_command)(ipvr_execbuffer_p execbuf, int cmd, void *arg, size_t arg_size);
};

#define RELOC(execbuf, dest, offset, buf, flags) \
    do { execbuf->reloc(execbuf, buf, ((uint8_t*)&dest - execbuf->vaddr), offset, flags); } while (0)
#define EMIT_DWORD(execbuf, dword) \
    do { *(uint32_t*)(execbuf->vaddr + execbuf->cur_offset) = dword; execbuf->cur_offset += 4; } while (0)
#define EMIT_RELOC(execbuf, dest, offset, buf, flags) \
    do { RELOC(execbuf, dest, offset, buf, flags); execbuf->cur_offset += 4; } while (0)

int ipvr_gem_bo_emit_reloc(drm_ipvr_bo *bo, drm_ipvr_bo *target_bo,
    unsigned long offset, unsigned long target_offset);

int ipvr_gem_bo_emit_dword(drm_ipvr_bo *bo, uint32_t dword);

int ipvr_execbuffer_full(ipvr_execbuffer_p execbuf);

int ipvr_execbuffer_ready(ipvr_execbuffer_p execbuf);

int ipvr_execbuffer_run(ipvr_execbuffer_p execbuf);

int ipvr_execbuffer_get(drm_ipvr_bufmgr *bufmgr, drm_ipvr_context *ctx,
                 ipvr_execbuffer_p execbuf, const char *name,
                 size_t buf_size);

void ipvr_execbuffer_put(ipvr_execbuffer_p execbuf);

int ipvr_execbuffer_add_command(ipvr_execbuffer_p execbuf, int cmd, void *arg, size_t argsize);

#endif /* _PSB_CMDBUF_H_ */
