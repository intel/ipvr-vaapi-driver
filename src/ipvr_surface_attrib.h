/*
 * Copyright (c) 2011 Intel Corporation. All Rights Reserved.
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
 */

#ifndef _IPVR_SURFACE_ATTRIB_H_
#define _IPVR_SURFACE_ATTRIB_H_

#include <va/va_tpi.h>
#include "ipvr_drv_video.h"
#include "ipvr_surface.h"

/*
 * Create surface from virtual address
 * flags: 0 indicates cache, IPVR_USER_BUFFER_UNCACHED, IPVR_USER_BUFFER_WC
 */
VAStatus ipvr_surface_create_from_ub(
    ipvr_driver_data_p driver_data,
    int width, int height, int fourcc, VASurfaceAttributeTPI *graphic_buffers,
    ipvr_surface_p ipvr_surface, /* out */
    void *vaddr,
    unsigned int flags
);

#ifdef ANDROID
VAStatus ipvr_DestroySurfaceGralloc(object_surface_p obj_surface);
VAStatus ipvr_CreateSurfacesFromGralloc(
    VADriverContextP ctx,
    int width,
    int height,
    int format,
    int num_surfaces,
    VASurfaceID *surface_list,        /* out */
    VASurfaceAttributeTPI *attribute_tpi
);
#endif


VAStatus ipvr_CreateSurfacesWithAttribute(
    VADriverContextP ctx,
    int width,
    int height,
    int format,
    int num_surfaces,
    VASurfaceID *surface_list,        /* out */
    VASurfaceAttributeTPI *attribute_tpi
);

#endif

