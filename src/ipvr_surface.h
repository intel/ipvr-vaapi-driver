/*
 * Copyright (c) 2011 Intel Corporation. All Rights Reserved.
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
 *    Waldo Bastian <waldo.bastian@intel.com>
 */

#ifndef _IPVR_SURFACE_H_
#define _IPVR_SURFACE_H_

#include <va/va.h>
#include <va/va_tpi.h>
#include <ipvr_bufmgr.h>
#include "ipvr_drv_video.h"
//#include "xf86mm.h"

/* MSVDX specific */
typedef enum {
    STRIDE_352  = 0,
    STRIDE_720  = 1,
    STRIDE_1280 = 2,
    STRIDE_1920 = 3,
    STRIDE_512  = 4,
    STRIDE_1024 = 5,
    STRIDE_2048 = 6,
    STRIDE_4096 = 7,
    STRIDE_NA,
    STRIDE_UNDEFINED,
} ipvr_surface_stride_t;

#define IPVR_SURFACE_TILED_X_ALIGNMENT 512
#define IPVR_SURFACE_TILED_Y_ALIGNMENT 8

typedef enum {
    IS_PROTECTED = 0x1,
    IS_ROTATED   = 0x2,
} ipvr_surface_flags_t;

typedef struct ipvr_surface_s *ipvr_surface_p;

struct ipvr_surface_s {
    drm_ipvr_bo *buf;
    drm_ipvr_bo *in_loop_buf;
    drm_ipvr_bo *ref_buf;
    ipvr_surface_stride_t stride_mode;
    unsigned int stride;
    unsigned int luma_offset;
    unsigned int chroma_offset;
    /* Used to store driver private data, e.g. decoder specific intermediate status data
     * extra_info[0-3]: used for decode
     * extra_info[4]: surface fourcc
     * extra_info[5]: surface skippeded or not for encode, rotate info for decode
     * extra_info[6]: mfld protected surface
     * extra_info[7]: linear or tiled
     */
    int extra_info[8];
    unsigned int size;
    unsigned int bc_buffer;
    void *handle;
};

/*
 * Create surface
 */
VAStatus ipvr_surface_create(ipvr_driver_data_p driver_data,
                            int width, int height, int fourcc, unsigned int flags,
                            ipvr_surface_p ipvr_surface /* out */
                           );


#define SET_SURFACE_INFO_rotate(ipvr_surface, rotate) ipvr_surface->extra_info[5] = (uint32_t) rotate;
#define GET_SURFACE_INFO_rotate(ipvr_surface) ((int) ipvr_surface->extra_info[5])
#define GET_SURFACE_INFO_protect(ipvr_surface) ((int) ipvr_surface->extra_info[6])
#define SET_SURFACE_INFO_protect(ipvr_surface, protect) (ipvr_surface->extra_info[6] = protect)
#define SET_SURFACE_INFO_tiling(ipvr_surface, tiling) ipvr_surface->extra_info[7] = (uint32_t) tiling;
#define GET_SURFACE_INFO_tiling(ipvr_surface) ((unsigned long) ipvr_surface->extra_info[7])


/*
 * Temporarily map surface and set all chroma values of surface to 'chroma'
 */
VAStatus ipvr_surface_set_chroma(ipvr_surface_p ipvr_surface, int chroma);

/*
 * Destroy surface
 */
void ipvr_surface_destroy(ipvr_surface_p ipvr_surface);

/*
 * Wait for surface to become idle
 */
VAStatus ipvr_surface_sync(ipvr_surface_p ipvr_surface);

/*
 * Return surface status
 */
VAStatus ipvr_surface_query_status(ipvr_surface_p ipvr_surface, VASurfaceStatus *status);

/*
 * Set current displaying surface info to kernel
 */
int ipvr_surface_set_displaying(ipvr_driver_data_p driver_data,
                               int width, int height,
                               ipvr_surface_p ipvr_surface);

#endif /* _IPVR_SURFACE_H_ */
