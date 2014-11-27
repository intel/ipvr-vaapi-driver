/*
 * Copyright (c) 2011, 2014 Intel Corporation. All Rights Reserved.
 * Copyright (c) Imagination Technologies Limited, UK
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

#include "ipvr_def.h"
#include "ipvr_surface.h"
#include "ipvr_drv_debug.h"
#include <libdrm/ipvr_drm.h>

/*
 * Create surface
 */
VAStatus ipvr_surface_create(ipvr_driver_data_p driver_data,
                            int width, int height, int fourcc, unsigned int flags,
                            ipvr_surface_p ipvr_surface /* out */
                           )
{
    int tiling = GET_SURFACE_INFO_tiling(ipvr_surface);

    if (fourcc == VA_FOURCC_NV12) {
        if ((width <= 0) || (width * height > 5120 * 5120) || (height <= 0)) {
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }

        if (512 >= width) {
            ipvr_surface->stride_mode = STRIDE_512;
            ipvr_surface->stride = 512;
        } else if (1024 >= width) {
            ipvr_surface->stride_mode = STRIDE_1024;
            ipvr_surface->stride = 1024;
        } else if (1280 >= width) {
            if (tiling) {
                ipvr_surface->stride_mode = STRIDE_2048;
                ipvr_surface->stride = 2048;
            }
            else {
                ipvr_surface->stride_mode = STRIDE_1280;
                ipvr_surface->stride = 1280;
            }
        } else if (2048 >= width) {
            ipvr_surface->stride_mode = STRIDE_2048;
            ipvr_surface->stride = 2048;
        } else if (4096 >= width) {
            ipvr_surface->stride_mode = STRIDE_4096;
            ipvr_surface->stride = 4096;
        } else {
            ipvr_surface->stride_mode = STRIDE_NA;
            ipvr_surface->stride = (width + 0x3f) & ~0x3f;
        }

        ipvr_surface->luma_offset = 0;
        ipvr_surface->chroma_offset = ipvr_surface->stride * height;
        ipvr_surface->size = (ipvr_surface->stride * height * 3) / 2;
        ipvr_surface->fourcc = VA_FOURCC_NV12;
    } 
    else {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "%s::%d unsupported fourcc \"%c%c%c%c\"\n",
            __FILE__, __LINE__,
            fourcc & 0xf, (fourcc >> 8) & 0xf, (fourcc >> 16) & 0xf, (fourcc >> 24) & 0xf);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    ipvr_surface->buf = drm_ipvr_gem_bo_alloc(driver_data->bufmgr, NULL,
        "VASurface", ipvr_surface->size, tiling, IPVR_CACHE_NOACCESS, 0);

    return ipvr_surface->buf ? VA_STATUS_SUCCESS: VA_STATUS_ERROR_ALLOCATION_FAILED;
}

/*
 * Create surface by importing PRIME BO
 */
VAStatus ipvr_surface_create_from_prime(
    ipvr_driver_data_p driver_data,
    int width, int height, int fourcc, int tiling,
    unsigned int *pitches, unsigned int *offsets,
    unsigned int size,
    ipvr_surface_p ipvr_surface, /* out */
    int prime_fd,
    unsigned int flags)
{
    drv_debug_msg(VIDEO_DEBUG_ERROR, "%s::%d, ipvr_surface=%p\n", __func__, __LINE__, ipvr_surface);
    ASSERT (!tiling);
    if (fourcc == VA_FOURCC_NV12) {
        ipvr_surface->stride = pitches[0];
        if (0) {
            ;
        } else if (512 == pitches[0]) {
            ipvr_surface->stride_mode = STRIDE_512;
        } else if (1024 == pitches[0]) {
            ipvr_surface->stride_mode = STRIDE_1024;
        } else if (1280 == pitches[0]) {
            ipvr_surface->stride_mode = STRIDE_1280;
            if (tiling) {
                ipvr_surface->stride_mode = STRIDE_2048;
                ipvr_surface->stride = 2048;
            }
        } else if (2048 == pitches[0]) {
            ipvr_surface->stride_mode = STRIDE_2048;
        } else if (4096 == pitches[0]) {
            ipvr_surface->stride_mode = STRIDE_4096;
        } else {
            ipvr_surface->stride_mode = STRIDE_NA;
        }
        if (ipvr_surface->stride != pitches[0]) {
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }

        ipvr_surface->luma_offset = offsets[0];
        ipvr_surface->chroma_offset = offsets[1];

        drv_debug_msg(VIDEO_DEBUG_GENERAL, "%s surface stride %d height %d, width %d, "
            "luma_off %d, chroma off %d\n", __func__, ipvr_surface->stride,
            height, width, ipvr_surface->luma_offset, ipvr_surface->chroma_offset);
        ipvr_surface->size = size;
        ipvr_surface->fourcc = VA_FOURCC_NV12;
    } else {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "%s unknown fourcc %c%c%c%c\n",
            __func__,
            fourcc & 0xff, (fourcc >> 8) & 0xff,
            (fourcc >> 16) & 0xff, (fourcc >> 24) & 0xff);
        return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
    }
    ASSERT (ipvr_surface->size > 0);
    drv_debug_msg(VIDEO_DEBUG_ERROR, "%s create_from_prime with fd %d and size 0x%x\n",
        __func__, prime_fd, ipvr_surface->size);
    ipvr_surface->buf = drm_ipvr_gem_bo_create_from_prime(driver_data->bufmgr,
        prime_fd, ipvr_surface->size);
    return ipvr_surface->buf ? VA_STATUS_SUCCESS: VA_STATUS_ERROR_ALLOCATION_FAILED;
}

/*
 * Destroy surface
 */
void ipvr_surface_destroy(ipvr_surface_p ipvr_surface)
{
    drm_ipvr_gem_bo_unreference(ipvr_surface->buf);
}

VAStatus ipvr_surface_sync(ipvr_surface_p ipvr_surface)
{
    drm_ipvr_gem_bo_wait(ipvr_surface->buf);
    return VA_STATUS_SUCCESS;
}

VAStatus ipvr_surface_query_status(ipvr_surface_p ipvr_surface, VASurfaceStatus *status)
{
    if (drm_ipvr_gem_bo_busy(ipvr_surface->buf))
        *status = VASurfaceRendering;
    else
        *status = VASurfaceReady;
    return VA_STATUS_SUCCESS;
}

/*
 * Set current displaying surface info to kernel
 * so that other component can access it in another process
 */
int ipvr_surface_set_displaying(ipvr_driver_data_p driver_data,
                               int width, int height,
                               ipvr_surface_p ipvr_surface)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}


