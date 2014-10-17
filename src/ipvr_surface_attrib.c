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

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdlib.h>
#ifdef ANDROID
#include <linux/ion.h>
#endif
#include <va/va_tpi.h>
#include "ipvr_drv_video.h"
#include "ipvr_drv_debug.h"
#include "ipvr_surface.h"
#include "ipvr_surface_attrib.h"


#define INIT_DRIVER_DATA    ipvr_driver_data_p driver_data = (ipvr_driver_data_p) ctx->pDriverData;

#define CONFIG(id)  ((object_config_p) object_heap_lookup( &driver_data->config_heap, id ))
#define CONTEXT(id) ((object_context_p) object_heap_lookup( &driver_data->context_heap, id ))
#define SURFACE(id)    ((object_surface_p) object_heap_lookup( &driver_data->surface_heap, id ))
#define BUFFER(id)  ((object_buffer_p) object_heap_lookup( &driver_data->buffer_heap, id ))

/*
 * Create surface
 */
VAStatus ipvr_surface_create_from_ub(
    ipvr_driver_data_p driver_data,
    int width, int height, int fourcc,
    VASurfaceAttributeTPI *graphic_buffers,
    ipvr_surface_p ipvr_surface, /* out */
    void *vaddr,
    unsigned flags
)
{
    if ((fourcc == VA_FOURCC_NV12) || (fourcc == VA_FOURCC_YV16) || (fourcc == VA_FOURCC_IYUV)) {
        if ((width <= 0) || (width * height > 5120 * 5120) || (height <= 0)) {
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }

        ipvr_surface->stride = graphic_buffers->luma_stride;
        if (0) {
            ;
        } else if (512 == graphic_buffers->luma_stride) {
            ipvr_surface->stride_mode = STRIDE_512;
        } else if (1024 == graphic_buffers->luma_stride) {
            ipvr_surface->stride_mode = STRIDE_1024;
        } else if (1280 == graphic_buffers->luma_stride) {
            ipvr_surface->stride_mode = STRIDE_1280;
            if (graphic_buffers->tiling) {
                ipvr_surface->stride_mode = STRIDE_2048;
                ipvr_surface->stride = 2048;
            }
        } else if (2048 == graphic_buffers->luma_stride) {
            ipvr_surface->stride_mode = STRIDE_2048;
        } else if (4096 == graphic_buffers->luma_stride) {
            ipvr_surface->stride_mode = STRIDE_4096;
        } else {
            ipvr_surface->stride_mode = STRIDE_NA;
        }
        if (ipvr_surface->stride != graphic_buffers->luma_stride) {
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }

        ipvr_surface->luma_offset = 0;
        ipvr_surface->chroma_offset = ipvr_surface->stride * height;

        drv_debug_msg(VIDEO_DEBUG_GENERAL, "%s surface stride %d height %d, width %d, "
            "luma_off %d, chroma off %d\n", __func__, ipvr_surface->stride,
            height, width, ipvr_surface->luma_offset, ipvr_surface->chroma_offset);
        if (VA_FOURCC_NV12 == fourcc) {
            ipvr_surface->size = ((ipvr_surface->stride * height) * 3) / 2;
            ipvr_surface->extra_info[4] = VA_FOURCC_NV12;
        }
        else if (VA_FOURCC_YV16 == fourcc) {
            ipvr_surface->size = (ipvr_surface->stride * height) * 2;
            ipvr_surface->extra_info[4] = VA_FOURCC_YV16;
        }
        else if (VA_FOURCC_IYUV == fourcc) {
            ipvr_surface->size = ((ipvr_surface->stride * height) * 3) / 2;
            ipvr_surface->extra_info[4] = VA_FOURCC_IYUV;
        }
        else {
            drv_debug_msg(VIDEO_DEBUG_ERROR, "unknown fourcc, abort\n");
            abort();
        }
    } else {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (ipvr_surface->size == 0) {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "%s invali surf size, abort\n", __func__);
        abort();
    }
    if (graphic_buffers->tiling)
        ipvr_surface->buf = drm_ipvr_gem_bo_alloc_vmap(driver_data->bufmgr, NULL,
            "VASurfaceUB", vaddr, graphic_buffers->tiling,
            DRM_IPVR_UNCACHED, ipvr_surface->stride, ipvr_surface->size);
    else
        ipvr_surface->buf = drm_ipvr_gem_bo_alloc_vmap(driver_data->bufmgr, NULL,
            "VASurfaceUB", vaddr, graphic_buffers->tiling,
            DRM_IPVR_UNCACHED, ipvr_surface->stride, ipvr_surface->size);
    return ipvr_surface->buf ? VA_STATUS_SUCCESS: VA_STATUS_ERROR_ALLOCATION_FAILED;
}

VAStatus  ipvr_CreateSurfaceFromUserspace(
        VADriverContextP ctx,
        int width,
        int height,
        int format,
        int num_surfaces,
        VASurfaceID *surface_list,        /* out */
        VASurfaceAttributeTPI *attribute_tpi
)
{
    VAStatus vaStatus = VA_STATUS_SUCCESS;
#ifdef ANDROID
    unsigned int *vaddr;
    unsigned long fourcc;
    int surfaceID;
    object_surface_p obj_surface;
    ipvr_surface_p ipvr_surface;
    int i;

    switch (format) {
    case VA_RT_FORMAT_YUV422:
        fourcc = VA_FOURCC_YV16;
        break;
    case VA_RT_FORMAT_YUV420:
    default:
        fourcc = VA_FOURCC_NV12;
        break;
    }

    for (i=0; i < num_surfaces; i++) {
        vaddr = attribute_tpi->buffers[i];
        surfaceID = object_heap_allocate(&driver_data->surface_heap);
        obj_surface = SURFACE(surfaceID);
        if (NULL == obj_surface) {
            vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
            DEBUG_FAILURE;
            break;
        }
        MEMSET_OBJECT(obj_surface, struct object_surface_s);

        obj_surface->surface_id = surfaceID;
        surface_list[i] = surfaceID;
        obj_surface->context_id = -1;
        obj_surface->width = attribute_tpi->width;
        obj_surface->height = attribute_tpi->height;
        obj_surface->width_r = attribute_tpi->width;
        obj_surface->height_r = attribute_tpi->height;
    obj_surface->is_ref_surface = 0;

        ipvr_surface = (ipvr_surface_p) calloc(1, sizeof(struct ipvr_surface_s));
        if (NULL == ipvr_surface) {
            object_heap_free(&driver_data->surface_heap, (object_base_p) obj_surface);
            obj_surface->surface_id = VA_INVALID_SURFACE;
            vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
            DEBUG_FAILURE;
            break;
        }

        if (attribute_tpi->type == VAExternalMemoryNoneCacheUserPointer)
            vaStatus = ipvr_surface_create_from_ub(driver_data, width, height, fourcc,
                    attribute_tpi, ipvr_surface, vaddr, IPVR_USER_BUFFER_UNCACHED);
        else
            vaStatus = ipvr_surface_create_from_ub(driver_data, width, height, fourcc,
                    attribute_tpi, ipvr_surface, vaddr, 0);
        obj_surface->ipvr_surface = ipvr_surface;

        if (VA_STATUS_SUCCESS != vaStatus) {
            free(ipvr_surface);
            object_heap_free(&driver_data->surface_heap, (object_base_p) obj_surface);
            obj_surface->surface_id = VA_INVALID_SURFACE;
            DEBUG_FAILURE;
            break;
        }
        /* by default, surface fourcc is NV12 */
        memset(ipvr_surface->extra_info, 0, sizeof(ipvr_surface->extra_info));
        ipvr_surface->extra_info[4] = fourcc;
        obj_surface->ipvr_surface = ipvr_surface;

        /* Error recovery */
        if (VA_STATUS_SUCCESS != vaStatus) {
            object_surface_p obj_surface = SURFACE(surfaceID);
            ipvr__destroy_surface(driver_data, obj_surface);
        }
    }
#endif
    return vaStatus;
}

VAStatus ipvr_CreateSurfacesWithAttribute(
    VADriverContextP ctx,
    int width,
    int height,
    int format,
    int num_surfaces,
    VASurfaceID *surface_list,        /* out */
    VASurfaceAttributeTPI *attribute_tpi
)
{
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    CHECK_INVALID_PARAM(attribute_tpi == NULL);

    drv_debug_msg(VIDEO_DEBUG_GENERAL, "Create %d surface(%dx%d) with type %d, tiling is %d\n",
            num_surfaces, width, height, attribute_tpi->type, attribute_tpi->tiling);

    switch (attribute_tpi->type) {
    case VAExternalMemoryUserPointer:
        vaStatus = ipvr_CreateSurfaceFromUserspace(ctx, width, height,
                                                 format, num_surfaces, surface_list,
                                                 attribute_tpi);
        return vaStatus;
#ifdef ANDROID
    case VAExternalMemoryAndroidGrallocBuffer:
        vaStatus = ipvr_CreateSurfacesFromGralloc(ctx, width, height,
                                                 format, num_surfaces, surface_list,
                                                 attribute_tpi);
        return vaStatus;
#endif
    default:
        drv_debug_msg(VIDEO_DEBUG_ERROR, "%s Invalid attrib tpi type.\n", __func__);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    drv_debug_msg(VIDEO_DEBUG_ERROR, "%s unexpected exit.\n", __func__);
    return VA_STATUS_ERROR_INVALID_PARAMETER;
}
