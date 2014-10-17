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

#include <sys/mman.h>
#include <va/va_tpi.h>
#include <cutils/log.h>
#include <utils/threads.h>
#include <ui/PixelFormat.h>
#include <hardware/gralloc.h>
#include <system/graphics.h>
#include <hardware/hardware.h>
#ifdef BAYTRAIL
#include <ufo/gralloc.h>
#endif
#include <gralloc.h>
#include "ipvr_drv_video.h"
#include "ipvr_drv_debug.h"
#include "ipvr_surface.h"
#include "ipvr_surface_attrib.h"
#include "android/ipvr_android.h"
#include "ipvr_drv_debug.h"
using namespace android;

static hw_module_t const *module;
static gralloc_module_t *mAllocMod; /* get by force hw_module_t */

#define INIT_DRIVER_DATA    ipvr_driver_data_p driver_data = (ipvr_driver_data_p) ctx->pDriverData;
#define CONFIG(id)  ((object_config_p) object_heap_lookup( &driver_data->config_heap, id ))
#define CONTEXT(id) ((object_context_p) object_heap_lookup( &driver_data->context_heap, id ))
#define SURFACE(id)    ((object_surface_p) object_heap_lookup( &driver_data->surface_heap, id ))
#define BUFFER(id)  ((object_buffer_p) object_heap_lookup( &driver_data->buffer_heap, id ))

/*FIXME: include hal_public.h instead of define it here*/
enum {
    GRALLOC_SUB_BUFFER0 = 0,
    GRALLOC_SUB_BUFFER1,
    GRALLOC_SUB_BUFFER2,
    GRALLOC_SUB_BUFFER_MAX,
};

int gralloc_lock(buffer_handle_t handle,
                int usage, int left, int top, int width, int height,
                void** vaddr)
{
    int err, j;

    if (!mAllocMod) {
        drv_debug_msg(VIDEO_DEBUG_GENERAL, "%s: gralloc module has not been initialized. Should initialize it first", __func__);
        if (gralloc_init()) {
            LOGE("%s: can't find the %s module", __func__, GRALLOC_HARDWARE_MODULE_ID);
            return -1;
        }
    }

    err = mAllocMod->lock(mAllocMod, handle, usage,
                          left, top, width, height,
                          vaddr);
    drv_debug_msg(VIDEO_DEBUG_GENERAL, "gralloc_lock: handle is %lx, usage is %x, vaddr is %x.\n", handle, usage, *vaddr);

    if (err){
        drv_debug_msg(VIDEO_DEBUG_ERROR, "lock(...) failed %d (%s).\n", err, strerror(-err));
        return -1;
    } else {
        drv_debug_msg(VIDEO_DEBUG_GENERAL, "lock returned with address %p\n", *vaddr);
    }

    return err;
}

int gralloc_unlock(buffer_handle_t handle)
{
    int err;

    if (!mAllocMod) {
        drv_debug_msg(VIDEO_DEBUG_GENERAL, "%s: gralloc module has not been initialized. Should initialize it first", __func__);
        if (gralloc_init()) {
            drv_debug_msg(VIDEO_DEBUG_ERROR, "%s: can't find the %s module", __func__, GRALLOC_HARDWARE_MODULE_ID);
            return -1;
        }
    }

    err = mAllocMod->unlock(mAllocMod, handle);
    if (err) {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "unlock(...) failed %d (%s)", err, strerror(-err));
        return -1;
    } else {
        drv_debug_msg(VIDEO_DEBUG_GENERAL, "unlock returned\n");
    }

    return err;
}

int gralloc_init(void)
{
    int err = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &module);
    if (err) {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "FATAL: can't find the %s module", GRALLOC_HARDWARE_MODULE_ID);
        return -1;
    } else
        drv_debug_msg(VIDEO_DEBUG_GENERAL, "hw_get_module returned\n");
    mAllocMod = (gralloc_module_t *)module;

    return 0;
}

int gralloc_getdisplaystatus(buffer_handle_t handle,  int* status)
{
    int err;
#ifndef BAYTRAIL
    int (*get_display_status)(gralloc_module_t*, buffer_handle_t, int*);

    get_display_status = (int (*)(gralloc_module_t*, buffer_handle_t, int*))(mAllocMod->reserved_proc[0]);
    if (get_display_status == NULL) {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "can't get gralloc_getdisplaystatus(...) \n");
        return -1;
    }
    err = (*get_display_status)(mAllocMod, handle, status);
#else
    err = 0;
    *status = mAllocMod->perform(mAllocMod, INTEL_UFO_GRALLOC_MODULE_PERFORM_GET_BO_STATUS, handle);
#endif
    if (err){
        drv_debug_msg(VIDEO_DEBUG_ERROR, "gralloc_getdisplaystatus(...) failed %d (%s).\n", err, strerror(-err));
        return -1;
    }

    return err;
}


VAStatus ipvr_DestroySurfaceGralloc(object_surface_p obj_surface)
{
    void *vaddr[GRALLOC_SUB_BUFFER_MAX];
    int usage = GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_COMPOSER;
    buffer_handle_t handle = (buffer_handle_t)obj_surface->ipvr_surface->buf->ext_handle;

    if (!gralloc_lock(handle, usage, 0, 0,
                      obj_surface->width, obj_surface->height, (void **)&vaddr[GRALLOC_SUB_BUFFER0])){
        if (obj_surface->share_info && vaddr[GRALLOC_SUB_BUFFER1] == obj_surface->share_info) {
            int metadata_rotate = obj_surface->share_info->metadata_rotate;
            int surface_protected = obj_surface->share_info->surface_protected;
            int force_output_method = obj_surface->share_info->force_output_method;
            int bob_deinterlace = obj_surface->share_info->bob_deinterlace;

            memset(obj_surface->share_info, 0, sizeof(struct ipvr_surface_share_info_s));
            /* Still need to keep these info so that hwc can get them after suspend/resume cycle */
            obj_surface->share_info->metadata_rotate = metadata_rotate;
            obj_surface->share_info->surface_protected = surface_protected;
            obj_surface->share_info->force_output_method = force_output_method;
            obj_surface->share_info->bob_deinterlace = bob_deinterlace;
        }
        gralloc_unlock(handle);
    }

    return VA_STATUS_SUCCESS;
}

VAStatus ipvr_CreateSurfacesFromGralloc(
    VADriverContextP ctx,
    int width,
    int height,
    int format,
    int num_surfaces,
    VASurfaceID *surface_list,        /* out */
    VASurfaceAttributeTPI *attribute_tpi
)
{
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    int i, height_origin, usage, buffer_stride = 0;
    int is_protected = (VA_RT_FORMAT_PROTECTED & format);
    unsigned long fourcc;
    VASurfaceAttributeTPI *external_buffers = NULL;
    buffer_handle_t handle;
    int size = num_surfaces * sizeof(unsigned int);
    void *vaddr;


    /* follow are gralloc-buffers */
    format = format & (~VA_RT_FORMAT_PROTECTED);
    driver_data->is_protected = is_protected;

    CHECK_INVALID_PARAM(num_surfaces <= 0);
    CHECK_SURFACE(surface_list);

    external_buffers = attribute_tpi;

    drv_debug_msg(VIDEO_DEBUG_GENERAL, "format is 0x%x, width is %d, height is %d, num_surfaces is %d.\n", format, width, height, num_surfaces);
    /* We only support one format */
    if ((VA_RT_FORMAT_YUV420 != format)
        && (VA_RT_FORMAT_YUV422 != format)) {
        vaStatus = VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        DEBUG_FAILURE;
        return vaStatus;
    }

    CHECK_INVALID_PARAM(external_buffers == NULL);

    /*
    vaStatus = ipvr__checkSurfaceDimensions(driver_data, width, height);
    CHECK_VASTATUS();
    */
    /* Adjust height to be a multiple of 32 (height of macroblock in interlaced mode) */
    height_origin = height;
    height = (height + 0x1f) & ~0x1f;
    drv_debug_msg(VIDEO_DEBUG_GENERAL, "external_buffers->pixel_format is 0x%x.\n", external_buffers->pixel_format);
    /* get native window from the reserved field */
    driver_data->native_window = (void *)external_buffers->reserved[0];

    for (i = 0; i < num_surfaces; i++) {
        int surfaceID;
        object_surface_p obj_surface;
        ipvr_surface_p ipvr_surface;

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
        obj_surface->width = width;
        obj_surface->height = height;
        obj_surface->width_r = width;
        obj_surface->height_r = height;
        obj_surface->height_origin = height_origin;
        obj_surface->is_ref_surface = 0;

        ipvr_surface = (ipvr_surface_p) calloc(1, sizeof(struct ipvr_surface_s));
        if (NULL == ipvr_surface) {
            object_heap_free(&driver_data->surface_heap, (object_base_p) obj_surface);
            obj_surface->surface_id = VA_INVALID_SURFACE;

            vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;

            DEBUG_FAILURE;
            break;
        }

        switch (format) {
        case VA_RT_FORMAT_YUV422:
            fourcc = VA_FOURCC_YV16;
            break;
        case VA_RT_FORMAT_YUV420:
        default:
            fourcc = VA_FOURCC_NV12;
            break;
        }

        /*hard code the gralloc buffer usage*/
        usage = GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_COMPOSER;

        /* usage hack for byt */
        usage |= GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN;
        /* usage hack to force pages alloc and CPU/GPU cache flush */
        usage |= GRALLOC_USAGE_HW_VIDEO_ENCODER;

        handle = (buffer_handle_t)external_buffers->buffers[i];
        if (gralloc_lock(handle, usage, 0, 0, width, height, (void **)&vaddr)) {
            vaStatus = VA_STATUS_ERROR_UNKNOWN;
        } else {
            int cache_flag = IPVR_USER_BUFFER_UNCACHED;

            vaStatus = ipvr_surface_create_from_ub(driver_data, width, height, fourcc,
                    external_buffers, ipvr_surface, vaddr,
                    cache_flag);

            ipvr_surface->buf->ext_handle = (unsigned long)handle;
            obj_surface->share_info = NULL;
            gralloc_unlock(handle);
        }

        if (VA_STATUS_SUCCESS != vaStatus) {
            free(ipvr_surface);
            object_heap_free(&driver_data->surface_heap, (object_base_p) obj_surface);
            obj_surface->surface_id = VA_INVALID_SURFACE;

            DEBUG_FAILURE;
            break;
        }
        buffer_stride = ipvr_surface->stride;

        ipvr_surface->extra_info[7] = external_buffers->tiling;

        /* by default, surface fourcc is NV12 */
        ipvr_surface->extra_info[4] = fourcc;
        obj_surface->ipvr_surface = ipvr_surface;
    }

    /* Error recovery */
    if (VA_STATUS_SUCCESS != vaStatus) {
        /* surface_list[i-1] was the last successful allocation */
        for (; i--;) {
            object_surface_p obj_surface = SURFACE(surface_list[i]);
            ipvr__destroy_surface(driver_data, obj_surface);
            surface_list[i] = VA_INVALID_SURFACE;
        }
        drv_debug_msg(VIDEO_DEBUG_ERROR, "CreateSurfaces failed\n");

        return vaStatus;
    }

    return vaStatus;
}

