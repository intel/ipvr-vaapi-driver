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
 *    Waldo Bastian <waldo.bastian@intel.com>
 *    Yao Cheng <yao.cheng@intel.com>
 *
 */

#include <va/va_backend.h>
#include <va/va_backend_tpi.h>
#include <va/va_backend_egl.h>
#include <va/va_backend_glx.h>
#include <va/va_backend_vpp.h>
#include <va/va_backend_wayland.h>
#include <va/va_drmcommon.h>
#ifdef LINUX
#ifdef ANDROID
#include <va/va_android.h>
#else
#include <va/va_x11.h>
#endif
#endif
#include <stdio.h>
#include <dlfcn.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <errno.h>
#include <ipvr_drm.h>
#include <ipvr_bufmgr.h>

#include "ipvr_drv_video.h"
#include "ipvr_execbuf.h"
#include "ipvr_surface.h"
#include "ipvr_output.h"
#include "ipvr_def.h"
#include "ipvr_drv_debug.h"
#include "ipvr_surface_attrib.h"

#include "ved_vp8.h"

#ifdef ANDROID
#include "android/ipvr_android.h"
#endif

#ifndef IPVR_PACKAGE_VERSION
#define IPVR_PACKAGE_VERSION "Undefined"
#endif

#define IPVR_DRV_VERSION  IPVR_PACKAGE_VERSION
#define IPVR_CHG_REVISION "(0X00000072)"

#define IPVR_STR_VENDOR_MERRIFIELD       "Intel GMA500-Merrifield-" IPVR_DRV_VERSION " " IPVR_CHG_REVISION
#define IPVR_STR_VENDOR_BAYTRAIL         "Intel GMA500-Baytrail-" IPVR_DRV_VERSION " " IPVR_CHG_REVISION

#define MAX_UNUSED_BUFFERS      16

#define IPVR_MAX_FLIP_DELAY (1000/30/10)

#include <signal.h>

#define EXPORT __attribute__ ((visibility("default")))

#define INIT_DRIVER_DATA    ipvr_driver_data_p driver_data = (ipvr_driver_data_p) ctx->pDriverData;

#define INIT_FORMAT_VTABLE format_vtable_p format_vtable = ((profile < IPVR_MAX_PROFILES) && (entrypoint < IPVR_MAX_ENTRYPOINTS)) ? (profile == VAProfileNone? driver_data->vpp_profile : driver_data->profile2Format[profile][entrypoint]) : NULL;

#define CONFIG(id)  ((object_config_p) object_heap_lookup( &driver_data->config_heap, id ))
#define CONTEXT(id) ((object_context_p) object_heap_lookup( &driver_data->context_heap, id ))
#define SURFACE(id)    ((object_surface_p) object_heap_lookup( &driver_data->surface_heap, id ))
#define BUFFER(id)  ((object_buffer_p) object_heap_lookup( &driver_data->buffer_heap, id ))

#define CONFIG_ID_OFFSET        0x01000000
#define CONTEXT_ID_OFFSET       0x02000000
#define SURFACE_ID_OFFSET       0x03000000
#define BUFFER_ID_OFFSET        0x04000000
#define IMAGE_ID_OFFSET         0x05000000
#define SUBPIC_ID_OFFSET        0x06000000

static int ipvr_get_device_info(VADriverContextP ctx);

VAStatus ipvr_QueryConfigProfiles(
    VADriverContextP ctx,
    VAProfile *profile_list,    /* out */
    int *num_profiles            /* out */
)
{
    DEBUG_FUNC_ENTER
    (void) ctx; /* unused */
    int i = 0;
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    INIT_DRIVER_DATA

    CHECK_INVALID_PARAM(profile_list == NULL);
    CHECK_INVALID_PARAM(num_profiles == NULL);

    if (IS_BAYTRAIL(driver_data)) {
        profile_list[i++] = VAProfileVP8Version0_3;
        profile_list[i++] = VAProfileMPEG4Simple;
        profile_list[i++] = VAProfileMPEG4AdvancedSimple;
    }

    /* If the assert fails then IPVR_MAX_PROFILES needs to be bigger */
    ASSERT(i <= IPVR_MAX_PROFILES);
    *num_profiles = i;
    DEBUG_FUNC_EXIT
    return VA_STATUS_SUCCESS;
}

VAStatus ipvr_QueryConfigEntrypoints(
    VADriverContextP ctx,
    VAProfile profile,
    VAEntrypoint  *entrypoint_list,    /* out */
    int *num_entrypoints        /* out */
)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    int entrypoints = 0;
    int i;

    CHECK_INVALID_PARAM(entrypoint_list == NULL);
    CHECK_INVALID_PARAM((num_entrypoints == NULL) || (profile >= IPVR_MAX_PROFILES));

    for (i = 0; i < IPVR_MAX_ENTRYPOINTS; i++) {
        if (profile != VAProfileNone && driver_data->profile2Format[profile][i]) {
            entrypoints++;
            *entrypoint_list++ = i;
        }
    }

    /* If the assert fails then IPVR_MAX_ENTRYPOINTS needs to be bigger */
    ASSERT(entrypoints <= IPVR_MAX_ENTRYPOINTS);

    if (0 == entrypoints) {
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    *num_entrypoints = entrypoints;
    DEBUG_FUNC_EXIT
    return VA_STATUS_SUCCESS;
}

/*
 * Figure out if we should return VA_STATUS_ERROR_UNSUPPORTED_PROFILE
 * or VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT
 */
static VAStatus ipvr__error_unsupported_profile_entrypoint(ipvr_driver_data_p driver_data, VAProfile profile, VAEntrypoint entrypoint)
{
    /* Does the driver support _any_ entrypoint for this profile? */
    if (profile < IPVR_MAX_PROFILES) {
        int i;

        /* Do the parameter check for MFLD and MRFLD */
        if (profile == VAProfileNone)
            return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;

        for (i = 0; i < IPVR_MAX_ENTRYPOINTS; i++) {
            if (driver_data->profile2Format[profile][i]) {
                /* There is an entrypoint, so the profile is supported */
                return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
            }
        }
    }
    return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
}

VAStatus ipvr_GetConfigAttributes(
    VADriverContextP ctx,
    VAProfile profile,
    VAEntrypoint entrypoint,
    VAConfigAttrib *attrib_list,    /* in/out */
    int num_attribs
)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA

    INIT_FORMAT_VTABLE

    int i;
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    if (NULL == format_vtable) {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "%s: failed due to NULL format_table\n", __func__);
        DEBUG_FUNC_EXIT
        return ipvr__error_unsupported_profile_entrypoint(driver_data, profile, entrypoint);
    }

    CHECK_INVALID_PARAM(attrib_list == NULL);
    CHECK_INVALID_PARAM(num_attribs <= 0);

    /* Generic attributes */
    for (i = 0; i < num_attribs; i++) {
        switch (attrib_list[i].type) {
        case VAConfigAttribRTFormat:
            attrib_list[i].value = VA_RT_FORMAT_YUV420;
            if (entrypoint == VAEntrypointEncPicture)
                attrib_list[i].value |= VA_RT_FORMAT_YUV422;
            if ((profile == VAProfileJPEGBaseline) && (entrypoint == VAEntrypointVLD))
                attrib_list[i].value |= VA_RT_FORMAT_YUV444;
            break;

        default:
            attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
            break;
        }
    }
    /* format specific attributes */
    format_vtable->queryConfigAttributes(profile, entrypoint, attrib_list, num_attribs);
    DEBUG_FUNC_EXIT
    return VA_STATUS_SUCCESS;
}

static VAStatus ipvr__update_attribute(object_config_p obj_config, VAConfigAttrib *attrib)
{
    int i;
    /* Check existing attributes */
    for (i = 0; i < obj_config->attrib_count; i++) {
        if (obj_config->attrib_list[i].type == attrib->type) {
            /* Update existing attribute */
            obj_config->attrib_list[i].value = attrib->value;
            return VA_STATUS_SUCCESS;
        }
    }
    if (obj_config->attrib_count < IPVR_MAX_CONFIG_ATTRIBUTES) {
        i = obj_config->attrib_count;
        obj_config->attrib_list[i].type = attrib->type;
        obj_config->attrib_list[i].value = attrib->value;
        obj_config->attrib_count++;
        return VA_STATUS_SUCCESS;
    }
    return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
}

static VAStatus ipvr__validate_config(object_config_p obj_config)
{
    int i;
    /* Check all attributes */
    for (i = 0; i < obj_config->attrib_count; i++) {
        switch (obj_config->attrib_list[i].type) {
        case VAConfigAttribRTFormat:
            if (!(obj_config->attrib_list[i].value == VA_RT_FORMAT_YUV420
                  || (obj_config->attrib_list[i].value == VA_RT_FORMAT_YUV422 &&
                      obj_config->entrypoint == VAEntrypointEncPicture)
                  || (obj_config->attrib_list[i].value == (VA_RT_FORMAT_YUV444 | VA_RT_FORMAT_YUV420 )))) {
                return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
            }
            break;

        default:
            /*
             * Ignore unknown attributes here, it
             * may be format specific.
             */
            break;
        }
    }
    return VA_STATUS_SUCCESS;
}

VAStatus ipvr_CreateConfig(
    VADriverContextP ctx,
    VAProfile profile,
    VAEntrypoint entrypoint,
    VAConfigAttrib *attrib_list,
    int num_attribs,
    VAConfigID *config_id        /* out */
)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA
    INIT_FORMAT_VTABLE

    VAStatus vaStatus = VA_STATUS_SUCCESS;
    int configID;
    object_config_p obj_config;
    int i;

    drv_debug_msg(VIDEO_DEBUG_INIT, "CreateConfig profile:%d, entrypoint:%d, num_attribs:%d.\n",
        profile, entrypoint, num_attribs);

    CHECK_INVALID_PARAM(config_id == NULL);
    CHECK_INVALID_PARAM(num_attribs < 0);
    CHECK_INVALID_PARAM(attrib_list == NULL);

    if (NULL == format_vtable) {
        vaStatus = ipvr__error_unsupported_profile_entrypoint(driver_data, profile, entrypoint);
    }

    CHECK_VASTATUS();

    configID = object_heap_allocate(&driver_data->config_heap);
    obj_config = CONFIG(configID);
    CHECK_ALLOCATION(obj_config);

    MEMSET_OBJECT(obj_config, struct object_config_s);

    obj_config->profile = profile;
    obj_config->format_vtable = format_vtable;
    obj_config->entrypoint = entrypoint;
    obj_config->attrib_list[0].type = VAConfigAttribRTFormat;
    obj_config->attrib_list[0].value = VA_RT_FORMAT_YUV420;
    obj_config->attrib_count = 1;

    for (i = 0; i < num_attribs; i++) {
        if (attrib_list[i].type > VAConfigAttribTypeMax) {
            drv_debug_msg(VIDEO_DEBUG_ERROR, "%s: failed due to unsupported ATTR\n", __func__);
            DEBUG_FUNC_EXIT
            return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
        }

        vaStatus = ipvr__update_attribute(obj_config, &(attrib_list[i]));
        if (VA_STATUS_SUCCESS != vaStatus) {
            break;
        }
    }

    if (VA_STATUS_SUCCESS == vaStatus) {
        vaStatus = ipvr__validate_config(obj_config);
    }

    if (VA_STATUS_SUCCESS == vaStatus) {
        vaStatus = format_vtable->validateConfig(obj_config);
    }

    /* Error recovery */
    if (VA_STATUS_SUCCESS != vaStatus) {
        object_heap_free(&driver_data->config_heap, (object_base_p) obj_config);
    } else {
        *config_id = configID;
    }

    if (profile == VAProfileMPEG4Simple ||
        profile == VAProfileMPEG4AdvancedSimple ||
        profile == VAProfileMPEG4Main ||
        profile == VAProfileVP8Version0_3 ||
        profile == VAProfileH264Baseline ||
        profile == VAProfileH264Main ||
        profile == VAProfileH264High ||
        profile == VAProfileH264ConstrainedBaseline)
        driver_data->ec_enabled = 1;
    else
        driver_data->ec_enabled = 0;

    /* TODO: enable it after enough test */
    if (IS_BAYTRAIL(driver_data))
        driver_data->ec_enabled = 0;

    DEBUG_FUNC_EXIT
    return vaStatus;
}

VAStatus ipvr_DestroyConfig(
    VADriverContextP ctx,
    VAConfigID config_id
)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_config_p obj_config;

    obj_config = CONFIG(config_id);
    CHECK_CONFIG(obj_config);

    object_heap_free(&driver_data->config_heap, (object_base_p) obj_config);
    DEBUG_FUNC_EXIT
    return vaStatus;
}

VAStatus ipvr_QueryConfigAttributes(
    VADriverContextP ctx,
    VAConfigID config_id,
    VAProfile *profile,        /* out */
    VAEntrypoint *entrypoint,     /* out */
    VAConfigAttrib *attrib_list,    /* out */
    int *num_attribs        /* out */
)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_config_p obj_config;
    int i;

    CHECK_INVALID_PARAM(profile == NULL);
    CHECK_INVALID_PARAM(entrypoint == NULL);
    CHECK_INVALID_PARAM(attrib_list == NULL);
    CHECK_INVALID_PARAM(num_attribs == NULL);

    obj_config = CONFIG(config_id);
    CHECK_CONFIG(obj_config);

    *profile = obj_config->profile;
    *entrypoint = obj_config->entrypoint;
    *num_attribs =  obj_config->attrib_count;
    for (i = 0; i < obj_config->attrib_count; i++) {
        attrib_list[i] = obj_config->attrib_list[i];
    }

    DEBUG_FUNC_EXIT
    return vaStatus;
}

void ipvr__destroy_surface(ipvr_driver_data_p driver_data, object_surface_p obj_surface)
{
    if (NULL != obj_surface) {
        /* delete subpicture association */
        ipvr_SurfaceDeassociateSubpict(driver_data, obj_surface);

        obj_surface->is_ref_surface = 0;

        ipvr_surface_sync(obj_surface->ipvr_surface);
        ipvr_surface_destroy(obj_surface->ipvr_surface);

        if (obj_surface->out_loop_surface) {
            ipvr_surface_destroy(obj_surface->out_loop_surface);
        }

        if (obj_surface->scaling_surface) {
            ipvr_surface_destroy(obj_surface->scaling_surface);
        }

        free(obj_surface->ipvr_surface);
        object_heap_free(&driver_data->surface_heap, (object_base_p) obj_surface);
    }
}

VAStatus ipvr__checkSurfaceDimensions(ipvr_driver_data_p driver_data, int width, int height)
{
    if (driver_data->video_sd_disabled) {
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
    }
    if ((width <= 0) || (width * height > 5120 * 5120) || (height <= 0)) {
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
    }
    if (driver_data->video_hd_disabled) {
        if ((width > 1024) || (height > 576)) {
            return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
        }
    }

    return VA_STATUS_SUCCESS;
}

VAStatus ipvr_GetSurfaceAttributes(
        VADriverContextP ctx,
        VAConfigID config,
        VASurfaceAttrib *attrib_list,
        unsigned int num_attribs
        )
{
    DEBUG_FUNC_ENTER

    unsigned int i;
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    CHECK_INVALID_PARAM(attrib_list == NULL);
    CHECK_INVALID_PARAM(num_attribs <= 0);

    /* Generic attributes */
    for (i = 0; i < num_attribs; i++) {
        switch (attrib_list[i].type) {
        case VASurfaceAttribMemoryType:
            attrib_list[i].flags = VA_SURFACE_ATTRIB_SETTABLE | VA_SURFACE_ATTRIB_GETTABLE;
            attrib_list[i].value.type = VAGenericValueTypeInteger;
            attrib_list[i].value.value.i =
                VA_SURFACE_ATTRIB_MEM_TYPE_VA |
                VA_SURFACE_ATTRIB_MEM_TYPE_USER_PTR |
                VA_SURFACE_ATTRIB_MEM_TYPE_KERNEL_DRM;
#ifdef ANDROID
            attrib_list[i].value.value.i |=
                VA_SURFACE_ATTRIB_MEM_TYPE_ANDROID_GRALLOC |
                VA_SURFACE_ATTRIB_MEM_TYPE_ANDROID_ION;
#endif
            break;

        case VASurfaceAttribExternalBufferDescriptor:
            attrib_list[i].flags = VA_SURFACE_ATTRIB_SETTABLE;
            attrib_list[i].value.type = VAGenericValueTypePointer;
            break;

        default:
            attrib_list[i].flags = VA_SURFACE_ATTRIB_NOT_SUPPORTED;
            break;
        }
    }

    DEBUG_FUNC_EXIT
    return VA_STATUS_SUCCESS;

}

VAStatus ipvr_CreateSurfaces(
        VADriverContextP ctx,
        int width,
        int height,
        int format,
        int num_surfaces,
        VASurfaceID *surface_list        /* out */
)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus ipvr_CreateSurfaces2(
    VADriverContextP ctx,
    unsigned int format,
    unsigned int width,
    unsigned int height,
    VASurfaceID *surface_list,        /* out */
    unsigned int num_surfaces,
    VASurfaceAttrib *attrib_list,
    unsigned int num_attribs
)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    unsigned int i, height_origin;
    driver_data->is_protected = (VA_RT_FORMAT_PROTECTED & format);
    unsigned long fourcc;
    unsigned int flags = 0;
    int memory_type = -1;
    VASurfaceAttribExternalBuffers  *pExternalBufDesc = NULL;
    VASurfaceAttributeTPI attribute_tpi;

    CHECK_INVALID_PARAM(num_surfaces <= 0);
    CHECK_SURFACE(surface_list);

    if ((attrib_list != NULL) && (num_attribs > 0)) {
        for (i = 0; i < num_attribs; i++, attrib_list++) {
            if (!attrib_list) {
                drv_debug_msg(VIDEO_DEBUG_ERROR, "Invalid VASurfaceAttrib list.\n");
                DEBUG_FUNC_EXIT
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            }
            switch (attrib_list->type) {
            case VASurfaceAttribExternalBufferDescriptor:
                {
                    pExternalBufDesc = (VASurfaceAttribExternalBuffers *)attrib_list->value.value.p;
                    if (pExternalBufDesc == NULL) {
                        drv_debug_msg(VIDEO_DEBUG_ERROR, "Invalid VASurfaceAttribExternalBuffers.\n");
                        DEBUG_FUNC_EXIT
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    attribute_tpi.type = memory_type;
                    attribute_tpi.buffers = malloc(sizeof(int) * pExternalBufDesc->num_buffers);
                    attribute_tpi.width = pExternalBufDesc->width;
                    attribute_tpi.height = pExternalBufDesc->height;
                    attribute_tpi.count = pExternalBufDesc->num_buffers;
                    memcpy((void*)attribute_tpi.buffers, (void*)pExternalBufDesc->buffers,
                            sizeof(pExternalBufDesc->buffers[0]) *
                            pExternalBufDesc->num_buffers);
                    attribute_tpi.pixel_format = pExternalBufDesc->pixel_format;
                    attribute_tpi.size = pExternalBufDesc->data_size;
                    attribute_tpi.luma_stride = pExternalBufDesc->pitches[0];
                    attribute_tpi.chroma_u_stride = pExternalBufDesc->pitches[1];
                    attribute_tpi.chroma_v_stride = pExternalBufDesc->pitches[2];
                    attribute_tpi.luma_offset = pExternalBufDesc->offsets[0];
                    attribute_tpi.chroma_u_offset = pExternalBufDesc->offsets[1];
                    attribute_tpi.chroma_v_offset = pExternalBufDesc->offsets[2];
                    attribute_tpi.reserved[0] = (unsigned long) pExternalBufDesc->private_data;
                    if (pExternalBufDesc->flags & VA_SURFACE_EXTBUF_DESC_ENABLE_TILING)
                        attribute_tpi.tiling = 1;
                    else
                        attribute_tpi.tiling = 0;
                }
                break;
            case VASurfaceAttribMemoryType:
                {
                    switch (attrib_list->value.value.i) {
                        case VA_SURFACE_ATTRIB_MEM_TYPE_USER_PTR:
                            memory_type = VAExternalMemoryUserPointer;
                            break;
                        case VA_SURFACE_ATTRIB_MEM_TYPE_KERNEL_DRM:
                            memory_type = VAExternalMemoryKernelDRMBufffer;
                            break;
#ifdef ANDROID
                        case VA_SURFACE_ATTRIB_MEM_TYPE_ANDROID_GRALLOC:
                            memory_type = VAExternalMemoryAndroidGrallocBuffer;
                            break;
                        case VA_SURFACE_ATTRIB_MEM_TYPE_ANDROID_ION:
                            memory_type = VAExternalMemoryIONSharedFD;
                            break;
#endif
                        case VA_SURFACE_ATTRIB_MEM_TYPE_VA:
                            memory_type = VAExternalMemoryNULL;
                            break;
                        default:
                            drv_debug_msg(VIDEO_DEBUG_ERROR, "Unsupported memory type.\n");
                            DEBUG_FUNC_EXIT
                            return VA_STATUS_ERROR_INVALID_PARAMETER;

                    }
                }
                break;
            default:
                drv_debug_msg(VIDEO_DEBUG_ERROR, "Unsupported attribute.\n");
                DEBUG_FUNC_EXIT
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            }
        }
    }

    if ((memory_type == -1 && pExternalBufDesc != NULL) ||
            (memory_type != -1 && pExternalBufDesc == NULL)) {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "Invalid memtype or buffer descs.\n");
        DEBUG_FUNC_EXIT
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    else if(memory_type !=-1 && pExternalBufDesc != NULL) {
        attribute_tpi.type = memory_type;
        vaStatus = ipvr_CreateSurfacesWithAttribute(ctx, width, height, format, num_surfaces, surface_list, &attribute_tpi);
        if (attribute_tpi.buffers) free(attribute_tpi.buffers);
        DEBUG_FUNC_EXIT
        return vaStatus;
    }

    format = format & (~VA_RT_FORMAT_PROTECTED);

    /* We only support one format */
    if ((VA_RT_FORMAT_YUV420 != format)
        && (VA_RT_FORMAT_YUV422 != format)
        && (VA_RT_FORMAT_YUV444 != format)) {
        vaStatus = VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        DEBUG_FAILURE;
        DEBUG_FUNC_EXIT
        return vaStatus;
    }

    vaStatus = ipvr__checkSurfaceDimensions(driver_data, width, height);
    CHECK_VASTATUS();

    /* Adjust height to be a multiple of 32 (height of macroblock in interlaced mode) */
    height_origin = height;
    height = (height + 0x1f) & ~0x1f;

    
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
        obj_surface->share_info = NULL;

        ipvr_surface = (ipvr_surface_p) calloc(1, sizeof(struct ipvr_surface_s));
        if (NULL == ipvr_surface) {
            object_heap_free(&driver_data->surface_heap, (object_base_p) obj_surface);
            obj_surface->surface_id = VA_INVALID_SURFACE;
            vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
            DEBUG_FAILURE;
            break;
        }

        switch (format) {
        case VA_RT_FORMAT_YUV444:
            fourcc = VA_FOURCC_YV32; /* allocate 4 planar */
            break;
        case VA_RT_FORMAT_YUV422:
            fourcc = VA_FOURCC_YV16;
            break;
        case VA_RT_FORMAT_YUV420:
        default:
            fourcc = VA_FOURCC_NV12;
            break;
        }

        flags |= driver_data->is_protected ? IS_PROTECTED : 0;
        vaStatus = ipvr_surface_create(driver_data, width, height, fourcc,
                                      flags, ipvr_surface);
        drv_debug_msg(VIDEO_DEBUG_INIT, "%s :ipvr_surface_create returns %d.\n",
                           __FUNCTION__, vaStatus);

        if (VA_STATUS_SUCCESS != vaStatus) {
            free(ipvr_surface);
            object_heap_free(&driver_data->surface_heap, (object_base_p) obj_surface);
            obj_surface->surface_id = VA_INVALID_SURFACE;
            DEBUG_FAILURE;
            break;
        }
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
        DEBUG_FUNC_EXIT
        return vaStatus;
    }
    DEBUG_FUNC_EXIT
    return vaStatus;
}

VAStatus ipvr_DestroySurfaces(
    VADriverContextP ctx,
    VASurfaceID *surface_list,
    int num_surfaces
)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA
    int i;
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    if (num_surfaces <= 0) {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "%s: failed, num_surfaces=%d\n", __func__, num_surfaces);
        DEBUG_FUNC_EXIT
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    CHECK_SURFACE(surface_list);

    /* Make validation happy */
    for (i = 0; i < num_surfaces; i++) {
        object_surface_p obj_surface = SURFACE(surface_list[i]);
        if (obj_surface == NULL) {
            drv_debug_msg(VIDEO_DEBUG_ERROR, "%s: failed due to NULL surface\n", __func__);
            DEBUG_FUNC_EXIT
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        if (obj_surface->derived_imgcnt > 0) {
            drv_debug_msg(VIDEO_DEBUG_ERROR, "Some surface is deriving by images\n");
            DEBUG_FUNC_EXIT
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    for (i = 0; i < num_surfaces; i++) {
        object_surface_p obj_surface = SURFACE(surface_list[i]);
        if (obj_surface == NULL) {
            drv_debug_msg(VIDEO_DEBUG_ERROR, "%s: failed due to NULL surface\n", __func__);
            DEBUG_FUNC_EXIT
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        if (driver_data->cur_displaying_surface == surface_list[i]) {
            /* Surface is being displaying. Need to stop overlay here */
        }
        drv_debug_msg(VIDEO_DEBUG_INIT, "%s : obj_surface->surface_id = 0x%x\n",__FUNCTION__, obj_surface->surface_id);
#ifdef ANDROID
        if (obj_surface->share_info) {
            ipvr_DestroySurfaceGralloc(obj_surface);
        }
#endif
        ipvr__destroy_surface(driver_data, obj_surface);
        surface_list[i] = VA_INVALID_SURFACE;
    }

    DEBUG_FUNC_EXIT
    return VA_STATUS_SUCCESS;
}

int ipvr_update_context(ipvr_driver_data_p driver_data, unsigned long ctx_type)
{
    return -1;
}

static inline unsigned long ipvr__tile_stride_log2_256(int w)
{
    int stride_mode = 0;

    if (512 >= w)
        stride_mode = 1;
    else if (1024 >= w)
        stride_mode = 2;
    else if (2048 >= w)
        stride_mode = 3;
    else if (4096 >= w)
        stride_mode = 4;

    return stride_mode;
}

static inline unsigned long ipvr__tile_stride_log2_512(int w)
{
    int stride_mode = 0;

    if (1024 >= w)
        stride_mode = 0;
    else if (2048 >= w)
        stride_mode = 1;
    else if (4096 >= w)
        stride_mode = 2;

    return stride_mode;
}

VAStatus ipvr_CreateContext(
    VADriverContextP ctx,
    VAConfigID config_id,
    int picture_width,
    int picture_height,
    int flag,
    VASurfaceID *render_targets,
    int num_render_targets,
    VAContextID *context        /* out */
)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_config_p obj_config;
    int encode = 0;
    int i;
    uint32_t tiling_scheme;

    if (IS_BAYTRAIL(driver_data))
        tiling_scheme = 1;
    else
        tiling_scheme = 0;

    drv_debug_msg(VIDEO_DEBUG_INIT, "CreateContext config_id:%d, pic_w:%d, pic_h:%d, flag:%d, num_render_targets:%d.\n",
        config_id, picture_width, picture_height, flag, num_render_targets);


    CHECK_INVALID_PARAM(num_render_targets <= 0);

    CHECK_SURFACE(render_targets);
    CHECK_CONTEXT(context);

    vaStatus = ipvr__checkSurfaceDimensions(driver_data, picture_width, picture_height);
    CHECK_VASTATUS();

    obj_config = CONFIG(config_id);
    CHECK_CONFIG(obj_config);

    int contextID = object_heap_allocate(&driver_data->context_heap);
    object_context_p obj_context = CONTEXT(contextID);
    CHECK_ALLOCATION(obj_context);

    *context = contextID;

    MEMSET_OBJECT(obj_context, struct object_context_s);

    obj_context->driver_data = driver_data;
    obj_context->current_render_target = NULL;
    obj_context->ec_target = NULL;
    obj_context->ec_candidate = NULL;
    obj_context->is_oold = driver_data->is_oold;
    obj_context->context_id = contextID;
    obj_context->config_id = config_id;
    obj_context->picture_width = picture_width;
    obj_context->picture_height = picture_height;
    obj_context->num_render_targets = num_render_targets;
    obj_context->ved_scaling = 0;
    obj_context->scaling_width = 0;
    obj_context->scaling_height = 0;
    obj_context->render_targets = (VASurfaceID *) calloc(1, num_render_targets * sizeof(VASurfaceID));
    if (obj_context->render_targets == NULL) {
        vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
        DEBUG_FAILURE;

        object_heap_free(&driver_data->context_heap, (object_base_p) obj_context);

        return vaStatus;
    }

    /* allocate buffer points for vaRenderPicture */
    obj_context->num_buffers = 10;
    obj_context->buffer_list = (object_buffer_p *) calloc(1, sizeof(object_buffer_p) * obj_context->num_buffers);
    if (obj_context->buffer_list == NULL) {
        vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
        DEBUG_FAILURE;

        free(obj_context->render_targets);
        object_heap_free(&driver_data->context_heap, (object_base_p) obj_context);
        drv_debug_msg(VIDEO_DEBUG_ERROR, "%s: failed due to allocation failure\n", __func__);
        DEBUG_FUNC_EXIT

        return vaStatus;
    }

    memset(obj_context->buffers_unused, 0, sizeof(obj_context->buffers_unused));
    memset(obj_context->buffers_unused_count, 0, sizeof(obj_context->buffers_unused_count));
    memset(obj_context->buffers_unused_tail, 0, sizeof(obj_context->buffers_unused_tail));
    memset(obj_context->buffers_active, 0, sizeof(obj_context->buffers_active));

    if (obj_config->entrypoint == VAEntrypointEncSlice
        || obj_config->entrypoint == VAEntrypointEncPicture) {
        encode = 1;
    }

    for (i = 0; i < num_render_targets; i++) {
        object_surface_p obj_surface = SURFACE(render_targets[i]);
        ipvr_surface_p ipvr_surface;

        if (NULL == obj_surface) {
            vaStatus = VA_STATUS_ERROR_INVALID_SURFACE;
            DEBUG_FAILURE;
            break;
        }

        ipvr_surface = obj_surface->ipvr_surface;

        /* Clear format specific surface info */
        obj_context->render_targets[i] = render_targets[i];
        obj_surface->context_id = contextID; /* Claim ownership of surface */

        if (GET_SURFACE_INFO_tiling(ipvr_surface)) {
            if (IS_BAYTRAIL(driver_data)) {
                obj_context->ved_tile = ipvr__tile_stride_log2_512(obj_surface->width);
            }
            else {
                if (obj_config->entrypoint == VAEntrypointVideoProc 
                    && obj_config->profile == VAProfileNone)
                    /* It's for two pass rotation case
                     * Need the source surface width for tile stride setting
                     */
                    obj_context->ved_tile = ipvr__tile_stride_log2_256(obj_context->picture_width);
                else
                    obj_context->ved_tile = ipvr__tile_stride_log2_256(obj_surface->width);
            }
        }
    }

    obj_context->va_flags = flag;
    obj_context->format_vtable = obj_config->format_vtable;
    obj_context->format_data = NULL;

    obj_context->ctp_type = (((obj_config->profile << 8) |
                             obj_config->entrypoint | driver_data->is_protected) & 0xffff);

    if (!encode) {
        obj_context->ctp_type |= ((obj_context->ved_tile & 0xff) << 16);
    }

    obj_context->ipvr_ctx = drm_ipvr_gem_context_create(driver_data->bufmgr,
        obj_context->ctp_type | driver_data->is_protected,
        obj_context->ved_tile, tiling_scheme);

    /* TODO: validate ctx_id */
    drv_debug_msg(VIDEO_DEBUG_GENERAL, "%s created drm context 0x%08x. VADrvContext %p, driver_data %p\n",
        __func__, obj_context->ipvr_ctx->ctx_id, ctx, driver_data);

    vaStatus = obj_context->format_vtable->createContext(obj_context, obj_config);

    /* Error recovery */
    if (VA_STATUS_SUCCESS != vaStatus) {
        obj_context->context_id = -1;
        obj_context->config_id = -1;
        obj_context->picture_width = 0;
        obj_context->picture_height = 0;
        free(obj_context->render_targets);
        free(obj_context->buffer_list);
        obj_context->num_buffers = 0;
        obj_context->render_targets = NULL;
        obj_context->num_render_targets = 0;
        obj_context->va_flags = 0;
        object_heap_free(&driver_data->context_heap, (object_base_p) obj_context);
        drv_debug_msg(VIDEO_DEBUG_ERROR, "%s: failed at %d\n", __func__, __LINE__);
        DEBUG_FUNC_EXIT

        return vaStatus;
    }

    obj_context->frame_count = 0;
    obj_context->slice_count = 0;
    obj_context->profile = obj_config->profile;
    obj_context->entry_point = obj_config->entrypoint;


    DEBUG_FUNC_EXIT
    return vaStatus;
}

static VAStatus ipvr__allocate_malloc_buffer(object_buffer_p obj_buffer, int size)
{
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    obj_buffer->buffer_data = realloc(obj_buffer->buffer_data, size);
    CHECK_ALLOCATION(obj_buffer->buffer_data);

    return vaStatus;
}

static VAStatus ipvr__unmap_buffer(object_buffer_p obj_buffer);

static VAStatus ipvr__allocate_BO_buffer(ipvr_driver_data_p driver_data, object_context_p obj_context, object_buffer_p obj_buffer, int size, unsigned char *data, VABufferType type)
{
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    ASSERT(NULL == obj_buffer->buffer_data);

    /* TODO: implement it */
    if (type == VAProtectedSliceDataBufferType) {
        if (obj_buffer->ipvr_bo) {
            drv_debug_msg(VIDEO_DEBUG_GENERAL, "RAR: force old RAR buffer destroy and new buffer re-allocation by set size=0\n");
            obj_buffer->alloc_size = 0;
        }
    }

    uint32_t cache_level = 0;
    switch (obj_buffer->type) {
    case VAImageBufferType: /* Xserver side PutSurface, Image/subpicture buffer
        * should be shared between two process
        */
        cache_level = DRM_IPVR_WRITECOMBINE;
        break;
    case VAProtectedSliceDataBufferType:
        if (!IS_BAYTRAIL(driver_data))
            /* TODO: implement it */
            cache_level = DRM_IPVR_UNCACHED;
        else
            cache_level = DRM_IPVR_UNCACHED;
        break;
    case VAEncCodedBufferType:
        if(obj_context && obj_context->profile == VAProfileVP8Version0_3) /*VP8 Encoder need  uncacheable coded buf*/
            cache_level = DRM_IPVR_UNCACHED;
        else
            cache_level = DRM_IPVR_WRITEBACK;
        break;
    default:
        cache_level = DRM_IPVR_UNCACHED;
        break;
    }

    /**
     * call libdrm_ipvr to allocate from its internal cache or get new pages
     */
    if (!obj_buffer->ipvr_bo) {
        size = (size + 0x7fff) & ~0x7fff;
        obj_buffer->ipvr_bo = drm_ipvr_gem_bo_alloc(driver_data->bufmgr, obj_context->ipvr_ctx,
            buffer_type_to_string(obj_buffer->type), size, cache_level, DRM_IPVR_WRITECOMBINE, 1);
        if (obj_buffer->ipvr_bo) {
            obj_buffer->alloc_size = obj_buffer->ipvr_bo->alloc_size;
        }
        else {
            obj_buffer->alloc_size = 0;
            obj_buffer->size = 0;
            vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
    }
    return vaStatus;
}

static VAStatus ipvr__map_buffer(object_buffer_p obj_buffer)
{
    int ret;
    if (obj_buffer->ipvr_bo) {
        ret = drm_ipvr_gem_bo_map(obj_buffer->ipvr_bo, 0, obj_buffer->ipvr_bo->size, 1);
        if (ret) {
            drv_debug_msg(VIDEO_DEBUG_ERROR, "Mapping buffer %08x (off 0x%lx) failed: %s (%d)\n",
                obj_buffer->ipvr_bo->handle, obj_buffer->ipvr_bo->offset,
                strerror(ret), ret);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        obj_buffer->buffer_data = obj_buffer->ipvr_bo->virt;
        return VA_STATUS_SUCCESS;
    }
    assert(obj_buffer->buffer_data);
    return VA_STATUS_SUCCESS;
}

static VAStatus ipvr__unmap_buffer(object_buffer_p obj_buffer)
{
    int ret;
    if (obj_buffer->ipvr_bo) {
        obj_buffer->buffer_data = NULL;
        ret = drm_ipvr_gem_bo_unmap(obj_buffer->ipvr_bo);
        if (ret == 0) {
            return VA_STATUS_SUCCESS;
        }
        else {
            drv_debug_msg(VIDEO_DEBUG_ERROR, "Unmapping buffer %08x (off 0x%lx) failed: %s (%d)\n",
                obj_buffer->ipvr_bo->handle, obj_buffer->ipvr_bo->offset,
                strerror(ret), ret);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }
    return VA_STATUS_SUCCESS;
}

static void ipvr__destroy_buffer(ipvr_driver_data_p driver_data, object_buffer_p obj_buffer)
{
    if (obj_buffer->ipvr_bo) {
        if (obj_buffer->buffer_data) {
            ipvr__unmap_buffer(obj_buffer);
        }
        drm_ipvr_gem_bo_unreference(obj_buffer->ipvr_bo);
        obj_buffer->ipvr_bo = NULL;
    }

    if (NULL != obj_buffer->buffer_data) {
        free(obj_buffer->buffer_data);
        obj_buffer->buffer_data = NULL;
        obj_buffer->size = 0;
    }

    object_heap_free(&driver_data->buffer_heap, (object_base_p) obj_buffer);
}

void ipvr__suspend_buffer(ipvr_driver_data_p driver_data, object_buffer_p obj_buffer)
{
    if (obj_buffer->ipvr_bo) {
        /**
         * unreference the bo
         * libdrm_ipvr will manage the life cycle/LRU of it: free or cache it.
         */
        drm_ipvr_gem_bo_unreference(obj_buffer->ipvr_bo);
        obj_buffer->ipvr_bo = NULL;
    }

    /**
     * ipvr_drv_video manages obj_buffer cache
     */
    if (obj_buffer->context) {
        VABufferType type = obj_buffer->type;
        object_context_p obj_context = obj_buffer->context;

        /* Remove buffer from active list */
        *obj_buffer->pptr_prev_next = obj_buffer->ptr_next;

        /* Add buffer to tail of unused list */
        obj_buffer->ptr_next = NULL;
        obj_buffer->last_used = obj_context->frame_count;
        if (obj_context->buffers_unused_tail[type]) {
            obj_buffer->pptr_prev_next = &(obj_context->buffers_unused_tail[type]->ptr_next);
        } else {
            obj_buffer->pptr_prev_next = &(obj_context->buffers_unused[type]);
        }
        *obj_buffer->pptr_prev_next = obj_buffer;
        obj_context->buffers_unused_tail[type] = obj_buffer;
        obj_context->buffers_unused_count[type]++;

        drv_debug_msg(VIDEO_DEBUG_GENERAL, "Adding buffer %08x type %s to unused list. unused count = %d\n", obj_buffer->base.id,
                                 buffer_type_to_string(obj_buffer->type), obj_context->buffers_unused_count[type]);

        object_heap_suspend_object((object_base_p) obj_buffer, 1); /* suspend */
    }
    else {
        ipvr__destroy_buffer(driver_data, obj_buffer);
    }
}

static void ipvr__destroy_context(ipvr_driver_data_p driver_data, object_context_p obj_context)
{
    int i;

    obj_context->format_vtable->destroyContext(obj_context);

    for (i = 0; i < IPVR_MAX_BUFFERTYPES; i++) {
        object_buffer_p obj_buffer;
        obj_buffer = obj_context->buffers_active[i];
        for (; obj_buffer; obj_buffer = obj_buffer->ptr_next) {
            drv_debug_msg(VIDEO_DEBUG_INIT, "%s: destroying active buffer %08x\n", __FUNCTION__, obj_buffer->base.id);
            ipvr__destroy_buffer(driver_data, obj_buffer);
        }
        obj_buffer = obj_context->buffers_unused[i];
        for (; obj_buffer; obj_buffer = obj_buffer->ptr_next) {
            drv_debug_msg(VIDEO_DEBUG_INIT, "%s: destroying unused buffer %08x\n", __FUNCTION__, obj_buffer->base.id);
            ipvr__destroy_buffer(driver_data, obj_buffer);
        }
        obj_context->buffers_unused_count[i] = 0;
    }

    obj_context->context_id = -1;
    obj_context->config_id = -1;
    obj_context->picture_width = 0;
    obj_context->picture_height = 0;
    if (obj_context->render_targets)
        free(obj_context->render_targets);
    obj_context->render_targets = NULL;
    obj_context->num_render_targets = 0;
    obj_context->va_flags = 0;

    obj_context->current_render_target = NULL;
    obj_context->ec_target = NULL;
    obj_context->ec_candidate = NULL;
    if (obj_context->buffer_list)
        free(obj_context->buffer_list);
    obj_context->num_buffers = 0;

    object_heap_free(&driver_data->context_heap, (object_base_p) obj_context);

    drm_ipvr_gem_context_destroy(obj_context->ipvr_ctx);
    obj_context->ipvr_ctx = NULL;
}

VAStatus ipvr_DestroyContext(
    VADriverContextP ctx,
    VAContextID context
)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_context_p obj_context = CONTEXT(context);
    CHECK_CONTEXT(obj_context);

    ipvr__destroy_context(driver_data, obj_context);

    DEBUG_FUNC_EXIT
    return vaStatus;
}

VAStatus ipvr__CreateBuffer(
    ipvr_driver_data_p driver_data,
    object_context_p obj_context,       /* in */
    VABufferType type,  /* in */
    unsigned int size,          /* in */
    unsigned int num_elements, /* in */
    unsigned char *data,         /* in */
    VABufferID *buf_desc    /* out */
)
{
    DEBUG_FUNC_ENTER
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    int bufferID;
    object_buffer_p obj_buffer = obj_context ? obj_context->buffers_unused[type] : NULL;
    int unused_count = obj_context ? obj_context->buffers_unused_count[type] : 0;


    /*
     * Buffer Management
     * For each buffer type, maintain
     *   - a LRU sorted list of unused buffers
     *   - a list of active buffers
     * We only create a new buffer when
     *   - no unused buffers are available
     *   - the last unused buffer is still queued
     *   - the last unused buffer was used very recently and may still be fenced
     *      - used recently is defined as within the current frame_count (subject to tweaks)
     *
     * The buffer that is returned will be moved to the list of active buffers
     *   - vaDestroyBuffer and vaRenderPicture will move the active buffer back to the list of unused buffers
    */
    drv_debug_msg(VIDEO_DEBUG_GENERAL, "Requesting buffer creation, size=%d,elements=%d,type=%s\n", size, num_elements,
                             buffer_type_to_string(type));

    if (obj_buffer) {
        bufferID = obj_buffer->base.id;
        drv_debug_msg(VIDEO_DEBUG_GENERAL, "Reusing buffer %08x type %s from unused list. Unused = %d\n", bufferID,
                                 buffer_type_to_string(type), unused_count);

        /* Remove from unused list */
        obj_context->buffers_unused[type] = obj_buffer->ptr_next;
        if (obj_context->buffers_unused[type]) {
            obj_context->buffers_unused[type]->pptr_prev_next = &(obj_context->buffers_unused[type]);
            ASSERT(obj_context->buffers_unused_tail[type] != obj_buffer);
        } else {
            ASSERT(obj_context->buffers_unused_tail[type] == obj_buffer);
            obj_context->buffers_unused_tail[type] = 0;
        }
        obj_context->buffers_unused_count[type]--;

        object_heap_suspend_object((object_base_p)obj_buffer, 0); /* Make BufferID valid again */
        ASSERT(type == obj_buffer->type);
        ASSERT(obj_context == obj_buffer->context);
    } else {
        bufferID = object_heap_allocate(&driver_data->buffer_heap);
        obj_buffer = BUFFER(bufferID);
        CHECK_ALLOCATION(obj_buffer);

        MEMSET_OBJECT(obj_buffer, struct object_buffer_s);

        drv_debug_msg(VIDEO_DEBUG_GENERAL, "Allocating new buffer %08x type %s.\n", bufferID, buffer_type_to_string(type));
        obj_buffer->type = type;
        obj_buffer->buffer_data = NULL;
        obj_buffer->ipvr_bo = NULL;
        obj_buffer->size = 0;
        obj_buffer->max_num_elements = 0;
        obj_buffer->alloc_size = 0;
        obj_buffer->context = obj_context;
    }
    if (obj_context) {
        /* Add to front of active list */
        obj_buffer->ptr_next = obj_context->buffers_active[type];
        if (obj_buffer->ptr_next) {
            obj_buffer->ptr_next->pptr_prev_next = &(obj_buffer->ptr_next);
        }
        obj_buffer->pptr_prev_next = &(obj_context->buffers_active[type]);
        *obj_buffer->pptr_prev_next = obj_buffer;
    }

    switch (obj_buffer->type) {
    case VABitPlaneBufferType:
    case VASliceDataBufferType:
    case VAResidualDataBufferType:
    case VAImageBufferType:
    case VASliceGroupMapBufferType:
    case VAEncCodedBufferType:
    case VAProtectedSliceDataBufferType:
        vaStatus = ipvr__allocate_BO_buffer(driver_data, obj_context,obj_buffer, size * num_elements, data, obj_buffer->type);
        drv_debug_msg(VIDEO_DEBUG_GENERAL, "succeeded with %p hnd %x offset 0x%lx.\n",
            obj_buffer->ipvr_bo, obj_buffer->ipvr_bo->handle, obj_buffer->ipvr_bo->offset);
        DEBUG_FAILURE;
        break;
    case VAPictureParameterBufferType:
    case VAIQMatrixBufferType:
    case VASliceParameterBufferType:
    case VAMacroblockParameterBufferType:
    case VADeblockingParameterBufferType:
    case VAEncPackedHeaderParameterBufferType:
    case VAEncPackedHeaderDataBufferType:
    case VAEncSequenceParameterBufferType:
    case VAEncPictureParameterBufferType:
    case VAEncSliceParameterBufferType:
    case VAQMatrixBufferType:
    case VAEncMiscParameterBufferType:
    case VAProbabilityBufferType:
    case VAHuffmanTableBufferType:
    case VAProcPipelineParameterBufferType:
    case VAProcFilterParameterBufferType:
        drv_debug_msg(VIDEO_DEBUG_GENERAL, "Allocate new malloc buffers for vaCreateBuffer:type=%s,size=%d, buffer_data=%p.\n",
                                 buffer_type_to_string(type), size, obj_buffer->buffer_data);
        vaStatus = ipvr__allocate_malloc_buffer(obj_buffer, size * num_elements);
        drv_debug_msg(VIDEO_DEBUG_GENERAL, "succeeded with %p.\n", obj_buffer->buffer_data);
        DEBUG_FAILURE;
        break;

    default:
        vaStatus = VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
        DEBUG_FAILURE;
        break;;
    }

    if (VA_STATUS_SUCCESS == vaStatus) {
        obj_buffer->size = size;
        obj_buffer->max_num_elements = num_elements;
        obj_buffer->num_elements = num_elements;
        if (data && (obj_buffer->type != VAProtectedSliceDataBufferType)) {
            if (obj_buffer->ipvr_bo) {
                assert(obj_buffer->ipvr_bo->alloc_size >= obj_buffer->ipvr_bo->size);
                assert(obj_buffer->ipvr_bo->size >= size);
            }
            vaStatus = ipvr__map_buffer(obj_buffer);
            if (VA_STATUS_SUCCESS == vaStatus) {
                drv_debug_msg(VIDEO_DEBUG_GENERAL, "memcpy for %ux%u bytes.\n", size, num_elements);
                memcpy(obj_buffer->buffer_data, data, size * num_elements);

                ipvr__unmap_buffer(obj_buffer);
            }
        }
    }
    if (VA_STATUS_SUCCESS == vaStatus) {
        *buf_desc = bufferID;
        drv_debug_msg(VIDEO_DEBUG_GENERAL, "buffer ID %u.\n", bufferID);
    } else {
        ipvr__destroy_buffer(driver_data, obj_buffer);
    }

    DEBUG_FUNC_EXIT
    return vaStatus;
}

VAStatus ipvr_CreateBuffer(
    VADriverContextP ctx,
    VAContextID context,        /* in */
    VABufferType type,  /* in */
    unsigned int size,          /* in */
    unsigned int num_elements, /* in */
    void *data,         /* in */
    VABufferID *buf_desc    /* out */
)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    
    drv_debug_msg(VIDEO_DEBUG_GENERAL, "%s: VADrvContext %p driver_data %p.\n", __func__, ctx, driver_data);

    CHECK_INVALID_PARAM(num_elements <= 0);

    switch (type) {
    case VABitPlaneBufferType:
    case VASliceDataBufferType:
    case VAProtectedSliceDataBufferType:
    case VAResidualDataBufferType:
    case VASliceGroupMapBufferType:
    case VAPictureParameterBufferType:
    case VAIQMatrixBufferType:
    case VASliceParameterBufferType:
    case VAMacroblockParameterBufferType:
    case VADeblockingParameterBufferType:
    case VAEncCodedBufferType:
    case VAEncSequenceParameterBufferType:
    case VAEncPictureParameterBufferType:
    case VAEncSliceParameterBufferType:
    case VAEncPackedHeaderParameterBufferType:
    case VAEncPackedHeaderDataBufferType:
    case VAQMatrixBufferType:
    case VAEncMiscParameterBufferType:
    case VAProbabilityBufferType:
    case VAHuffmanTableBufferType:
    case VAProcPipelineParameterBufferType:
    case VAProcFilterParameterBufferType:
        break;

    default:
        vaStatus = VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
        DEBUG_FAILURE;
        DEBUG_FUNC_EXIT
        return vaStatus;
    }

    object_context_p obj_context = CONTEXT(context);
    CHECK_CONTEXT(obj_context);
    CHECK_INVALID_PARAM(buf_desc == NULL);

    drv_debug_msg(VIDEO_DEBUG_GENERAL, "%s: before createBuffer VADrvContext %p driver_data %p ctx %p data %p bufDesc %p.\n", __func__, ctx, driver_data, obj_context, data, buf_desc);

    vaStatus = ipvr__CreateBuffer(driver_data, obj_context, type, size, num_elements, data, buf_desc);

    DEBUG_FUNC_EXIT
    return vaStatus;
}


VAStatus ipvr_BufferInfo(
    VADriverContextP ctx,
    VABufferID buf_id,  /* in */
    VABufferType *type, /* out */
    unsigned int *size,         /* out */
    unsigned int *num_elements /* out */
)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    object_buffer_p obj_buffer = BUFFER(buf_id);
    CHECK_BUFFER(obj_buffer);

    *type = obj_buffer->type;
    *size = obj_buffer->size;
    *num_elements = obj_buffer->num_elements;
    DEBUG_FUNC_EXIT
    return VA_STATUS_SUCCESS;
}


VAStatus ipvr_BufferSetNumElements(
    VADriverContextP ctx,
    VABufferID buf_id,    /* in */
    unsigned int num_elements    /* in */
)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_buffer_p obj_buffer = BUFFER(buf_id);
    CHECK_BUFFER(obj_buffer);

    if ((num_elements <= 0) || (num_elements > obj_buffer->max_num_elements)) {
        vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (VA_STATUS_SUCCESS == vaStatus) {
        obj_buffer->num_elements = num_elements;
    }

    DEBUG_FUNC_EXIT
    return vaStatus;
}

VAStatus ipvr_MapBuffer(
    VADriverContextP ctx,
    VABufferID buf_id,    /* in */
    void **pbuf         /* out */
)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA
    drv_debug_msg(VIDEO_DEBUG_GENERAL, "mapping buffer %u\n", buf_id);
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_buffer_p obj_buffer = BUFFER(buf_id);
    CHECK_BUFFER(obj_buffer);

    CHECK_INVALID_PARAM(pbuf == NULL);

    vaStatus = ipvr__map_buffer(obj_buffer);
    CHECK_VASTATUS();

    if (NULL != obj_buffer->buffer_data) {
        *pbuf = obj_buffer->buffer_data;
        /* specifically for Topaz encode
         * write validate coded data offset in CodedBuffer
         */
    } else {
        vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    DEBUG_FUNC_EXIT
    return vaStatus;
}

VAStatus ipvr_UnmapBuffer(
    VADriverContextP ctx,
    VABufferID buf_id    /* in */
)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_buffer_p obj_buffer = BUFFER(buf_id);
    CHECK_BUFFER(obj_buffer);

    vaStatus = ipvr__unmap_buffer(obj_buffer);
    DEBUG_FUNC_EXIT
    return vaStatus;
}


VAStatus ipvr_DestroyBuffer(
    VADriverContextP ctx,
    VABufferID buffer_id
)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_buffer_p obj_buffer = BUFFER(buffer_id);
    if (NULL == obj_buffer) {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "%s: failed due to NULL buffer\n", __func__);
        DEBUG_FUNC_EXIT
        return vaStatus;
    }
    ipvr__suspend_buffer(driver_data, obj_buffer);
    DEBUG_FUNC_EXIT
    return vaStatus;
}


VAStatus ipvr_BeginPicture(
    VADriverContextP ctx,
    VAContextID context,
    VASurfaceID render_target
)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_context_p obj_context;
    object_surface_p obj_surface;
    object_config_p obj_config;

    obj_context = CONTEXT(context);
    CHECK_CONTEXT(obj_context);
    
    /* Must not be within BeginPicture / EndPicture already */
    ASSERT(obj_context->current_render_target == NULL);

    obj_surface = SURFACE(render_target);
    CHECK_SURFACE(obj_surface);

    obj_context->current_render_surface_id = render_target;
    obj_context->current_render_target = obj_surface;
    obj_context->slice_count = 0;

    obj_config = CONFIG(obj_context->config_id);
    if (obj_config == NULL) {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "%s: failed due to NULL config\n", __func__);
        DEBUG_FUNC_EXIT
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    /* if the surface is decode render target, and in displaying */
    if (obj_config &&
        (obj_config->entrypoint != VAEntrypointEncSlice) &&
        (driver_data->cur_displaying_surface == render_target))
        drv_debug_msg(VIDEO_DEBUG_ERROR, "WARNING: rendering a displaying surface, may see tearing\n");

    if (VA_STATUS_SUCCESS == vaStatus) {
        vaStatus = obj_context->format_vtable->beginPicture(obj_context);
    }

    if (IS_MERRIFIELD(driver_data) && driver_data->vpp_on && GET_SURFACE_INFO_tiling(obj_surface->ipvr_surface))
        driver_data->disable_ved_rotate = 0;

    if (obj_context->interlaced_stream || driver_data->disable_ved_rotate) {
        int i;
        obj_context->ved_rotate = 0;
        for (i = 0; i < obj_context->num_render_targets; i++) {
            object_surface_p obj_surface = SURFACE(obj_context->render_targets[i]);
            /*we invalidate all surfaces's rotate buffer share info here.*/
            if (obj_surface && obj_surface->share_info) {
                obj_surface->share_info->surface_rotate = 0;
            }
        }
    }
    else
        obj_context->ved_rotate = driver_data->ved_rotate_want;

    if (driver_data->is_oold &&  !obj_surface->ipvr_surface->in_loop_buf) {
        ipvr_surface_p ipvr_surface = obj_surface->ipvr_surface;

        /* FIXME: For RAR surface, need allocate RAR buffer  */
        ipvr_surface->in_loop_buf = drm_ipvr_gem_bo_alloc(obj_context->driver_data->bufmgr,
            obj_context->ipvr_ctx, "IPVR-in_loop_buf", ipvr_surface->size, 0,
            DRM_IPVR_UNCACHED, 0);
        if (!ipvr_surface->in_loop_buf)
            vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
    } else if (!driver_data->is_oold && obj_surface->ipvr_surface->in_loop_buf) {
        ipvr_surface_p ipvr_surface = obj_surface->ipvr_surface;

        drm_ipvr_gem_bo_unmap(ipvr_surface->in_loop_buf);
        free(ipvr_surface->in_loop_buf);
        ipvr_surface->in_loop_buf = NULL;
    }
    obj_context->is_oold = driver_data->is_oold;

    drv_debug_msg(VIDEO_DEBUG_GENERAL, "---BeginPicture 0x%08x for frame %d --\n",
                             render_target, obj_context->frame_count);
    ipvr__trace_message("------Trace frame %d------\n", obj_context->frame_count);

    DEBUG_FUNC_EXIT
    return vaStatus;
}

VAStatus ipvr_RenderPicture(
    VADriverContextP ctx,
    VAContextID context,
    VABufferID *buffers,
    int num_buffers
)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_context_p obj_context;
    object_buffer_p *buffer_list;
    int i;

    obj_context = CONTEXT(context);
    CHECK_CONTEXT(obj_context);

    CHECK_INVALID_PARAM(num_buffers <= 0);
    /* Don't crash on NULL pointers */
    CHECK_BUFFER(buffers);
    /* Must be within BeginPicture / EndPicture */
    ASSERT(obj_context->current_render_target != NULL);

    if (num_buffers > obj_context->num_buffers) {
        free(obj_context->buffer_list);

        obj_context->buffer_list = (object_buffer_p *) calloc(1, sizeof(object_buffer_p) * num_buffers);
        if (obj_context->buffer_list == NULL) {
            vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
            obj_context->num_buffers = 0;
        }

        obj_context->num_buffers = num_buffers;
    }
    buffer_list = obj_context->buffer_list;

    if (VA_STATUS_SUCCESS == vaStatus) {
        /* Lookup buffer references */
        for (i = 0; i < num_buffers; i++) {
            object_buffer_p obj_buffer = BUFFER(buffers[i]);
            CHECK_BUFFER(obj_buffer);

            buffer_list[i] = obj_buffer;
            drv_debug_msg(VIDEO_DEBUG_GENERAL, "Render buffer %08x type %s\n", obj_buffer->base.id,
                                     buffer_type_to_string(obj_buffer->type));
        }
    }

    if (VA_STATUS_SUCCESS == vaStatus) {
        vaStatus = obj_context->format_vtable->renderPicture(obj_context, buffer_list, num_buffers);
    }

    if (buffer_list) {
        /* Release buffers */
        for (i = 0; i < num_buffers; i++) {
            if (buffer_list[i]) {
                ipvr__suspend_buffer(driver_data, buffer_list[i]);
            }
        }
    }

    DEBUG_FUNC_EXIT
    return vaStatus;
}

VAStatus ipvr_EndPicture(
    VADriverContextP ctx,
    VAContextID context
)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA
    VAStatus vaStatus;
    object_context_p obj_context;

    obj_context = CONTEXT(context);
    CHECK_CONTEXT(obj_context);

    vaStatus = obj_context->format_vtable->endPicture(obj_context);

    drv_debug_msg(VIDEO_DEBUG_GENERAL, "---EndPicture for frame %d --\n", obj_context->frame_count);

    obj_context->current_render_target = NULL;
    obj_context->frame_count++;

    ipvr__trace_message("FrameCount = %03d\n", obj_context->frame_count);
    drv_debug_msg(VIDEO_DEBUG_GENERAL, "FrameCount = %03d\n", obj_context->frame_count);
    ipvr__trace_message(NULL);

    DEBUG_FUNC_EXIT
    return vaStatus;
}


static void ipvr__surface_usage(
    ipvr_driver_data_p driver_data,
    object_surface_p obj_surface,
    int *decode, int *encode, int *rc_enable, int *proc
)
{
    object_context_p obj_context;
    object_config_p obj_config;
    VAEntrypoint tmp;
    unsigned int eRCmode;
    int i;


    *decode = 0;
    *encode = 0;
    *rc_enable = 0;
    *proc = 0;

    obj_context = CONTEXT(obj_surface->context_id);
    if (NULL == obj_context) /* not associate with a context */
        return;

    obj_config = CONFIG(obj_context->config_id);
    if (NULL == obj_config) /* not have a validate context */
        return;

    tmp = obj_config->entrypoint;

    *encode = (tmp == VAEntrypointEncSlice) || (tmp == VAEntrypointEncPicture);
    *decode = (VAEntrypointVLD <= tmp) && (tmp <= VAEntrypointDeblocking);

    if (IS_MERRIFIELD(driver_data))
        *proc = (VAEntrypointVideoProc == tmp);

    if (*encode) {
        for (i = 0; i < obj_config->attrib_count; i++) {
            if (obj_config->attrib_list[i].type == VAConfigAttribRateControl)
                break;
        }

        if (i >= obj_config->attrib_count)
            eRCmode = VA_RC_NONE;
        else
            eRCmode = obj_config->attrib_list[i].value;

        if (eRCmode == VA_RC_NONE)
            *rc_enable = 0;
        else
            *rc_enable = 1;
    }
}

VAStatus ipvr_SyncSurface(
    VADriverContextP ctx,
    VASurfaceID render_target
)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_surface_p obj_surface;
    int decode = 0, encode = 0, rc_enable = 0, proc = 0;
    object_context_p obj_context = NULL;
    object_config_p obj_config = NULL;

    drv_debug_msg(VIDEO_DEBUG_GENERAL, "ipvr_SyncSurface: 0x%08x\n", render_target);

    obj_surface = SURFACE(render_target);
    CHECK_SURFACE(obj_surface);

    obj_context = CONTEXT(obj_surface->context_id);
    if (obj_context) {
        obj_config = CONFIG(obj_context->config_id);
    }

    /* The cur_displaying_surface indicates the surface being displayed by overlay.
     * The diaplay_timestamp records the time point of put surface, which would
     * be set to zero while using texture blit.*/

    /* don't use mutex here for performance concern... */
    //pthread_mutex_lock(&output->output_mutex);
    if (render_target == driver_data->cur_displaying_surface)
        vaStatus = VA_STATUS_ERROR_SURFACE_IN_DISPLAYING;
    else if ((VA_INVALID_SURFACE != driver_data->cur_displaying_surface)    /* use overlay */
             && (render_target == driver_data->last_displaying_surface)) {  /* It's the last displaying surface*/
        object_surface_p cur_obj_surface = SURFACE(driver_data->cur_displaying_surface);
        /*  The flip operation on current displaying surface could be delayed to
         *  next VBlank and hadn't been finished yet. Then, the last displaying
         *  surface shouldn't be freed, because the hardware may not
         *  complete loading data of it. Any change of the last surface could
         *  have a impect on the scrren.*/
        if (NULL != cur_obj_surface) {
            while ((GetTickCount() - cur_obj_surface->display_timestamp) < IPVR_MAX_FLIP_DELAY)
                usleep(IPVR_MAX_FLIP_DELAY * 1000);
        }
    }

    if (vaStatus != VA_STATUS_ERROR_SURFACE_IN_DISPLAYING) {
        /* For VPP buffer, will sync the rotated buffer */
        if (IS_MERRIFIELD(driver_data) && obj_config && obj_config->entrypoint == VAEntrypointVideoProc) {
            if (GET_SURFACE_INFO_tiling(obj_surface->ipvr_surface) &&
                (obj_context->ved_rotate == VA_ROTATION_90 || obj_context->ved_rotate == VA_ROTATION_270) &&
                obj_surface->out_loop_surface)
                vaStatus = ipvr_surface_sync(obj_surface->out_loop_surface);
            else
                vaStatus = ipvr_surface_sync(obj_surface->ipvr_surface);
        } else
            vaStatus = ipvr_surface_sync(obj_surface->ipvr_surface);
    }

    /* report any error of decode for Android */
    ipvr__surface_usage(driver_data, obj_surface, &decode, &encode, &rc_enable, &proc);

    if (proc && IS_MERRIFIELD(driver_data)) {
        /* FIXME: does it need a new surface sync mechanism for FRC? */
    }

    if (obj_surface->scaling_surface)
        ipvr__dump_NV12_buffers(obj_surface->scaling_surface, 0, 0, obj_surface->width_s, obj_surface->height_s);

    DEBUG_FAILURE;
    DEBUG_FUNC_EXIT
    return vaStatus;
}


VAStatus ipvr_QuerySurfaceStatus(
    VADriverContextP ctx,
    VASurfaceID render_target,
    VASurfaceStatus *status    /* out */
)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_surface_p obj_surface;
    VASurfaceStatus surface_status;
    int encode = 0, decode = 0, rc_enable = 0, proc = 0;
    object_context_p obj_context = NULL;

    obj_surface = SURFACE(render_target);
    CHECK_SURFACE(obj_surface);

    CHECK_INVALID_PARAM(status == NULL);

    ipvr__surface_usage(driver_data, obj_surface, &decode, &encode, &rc_enable, &proc);

    /* For VPP 1080P, will query the rotated buffer */
    if (IS_MERRIFIELD(driver_data) && proc) {
        obj_context = CONTEXT(obj_surface->context_id);
        CHECK_CONTEXT(obj_context);
        if (GET_SURFACE_INFO_tiling(obj_surface->ipvr_surface) &&
            (obj_context->ved_rotate == VA_ROTATION_90 || obj_context->ved_rotate == VA_ROTATION_270) &&
            obj_surface->out_loop_surface)
            vaStatus = ipvr_surface_query_status(obj_surface->out_loop_surface, &surface_status);
        else
            vaStatus = ipvr_surface_query_status(obj_surface->ipvr_surface, &surface_status);
    } else
        vaStatus = ipvr_surface_query_status(obj_surface->ipvr_surface, &surface_status);

    /* The cur_displaying_surface indicates the surface being displayed by overlay.
     * The diaplay_timestamp records the time point of put surface, which would
     * be set to zero while using texture blit.*/
    pthread_mutex_lock(&driver_data->output_mutex);
    if (render_target == driver_data->cur_displaying_surface)
        surface_status = VASurfaceDisplaying;
    else if ((VA_INVALID_SURFACE != driver_data->cur_displaying_surface)    /* use overlay */
             && (render_target == driver_data->last_displaying_surface)) {  /* It's the last displaying surface*/
        object_surface_p cur_obj_surface = SURFACE(driver_data->cur_displaying_surface);
        /*The flip operation on current displaying surface could be delayed to
         *  next VBlank and hadn't been finished yet. Then, the last displaying
         *  surface shouldn't be freed, because the hardware may not
         *  complete loading data of it. Any change of the last surface could
         *  have a impect on the scrren.*/
        if ((NULL != cur_obj_surface)
            && ((GetTickCount() - cur_obj_surface->display_timestamp) < IPVR_MAX_FLIP_DELAY)) {
            surface_status = VASurfaceDisplaying;
        }
    }
    pthread_mutex_unlock(&driver_data->output_mutex);

    if (decode) {
#ifdef ANDROID
        if (obj_surface->ipvr_surface->buf->ext_handle) {
            buffer_handle_t handle = (buffer_handle_t)obj_surface->ipvr_surface->buf->ext_handle;
            int display_status = 0;
            int err = 0;

            err = gralloc_getdisplaystatus(handle, &display_status);

            if (!err) {
                if (display_status)
                    surface_status = VASurfaceDisplaying;
                else
                    surface_status = VASurfaceReady;
            } else {
                surface_status = VASurfaceReady;
            }

            /* if not used by display, then check whether surface used by widi */
            if (surface_status == VASurfaceReady && obj_surface->share_info) {
                if (obj_surface->share_info->renderStatus == 1) {
                    surface_status = VASurfaceDisplaying;
                }
            }
        }
#endif
    } else if (proc) {
        /* FIXME: does it need a new surface sync mechanism for FRC? */
    }

    *status = surface_status;
    DEBUG_FUNC_EXIT
    return vaStatus;
}

VAStatus ipvr_QuerySurfaceError(
    VADriverContextP ctx,
    VASurfaceID render_target,
    VAStatus error_status,
    void **error_info /*out*/
)
{
    /* todo: implement it */
    return VA_STATUS_SUCCESS;
}

#define IPVR_MAX_SURFACE_ATTRIBUTES 16

VAStatus ipvr_QuerySurfaceAttributes(VADriverContextP ctx,
                            VAConfigID config,
                            VASurfaceAttrib *attrib_list,
                            unsigned int *num_attribs)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA

    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_config_p obj_config;
    unsigned int i = 0;

    CHECK_INVALID_PARAM(num_attribs == NULL);

    if (attrib_list == NULL) {
        *num_attribs = IPVR_MAX_SURFACE_ATTRIBUTES;
        DEBUG_FUNC_EXIT
        return VA_STATUS_SUCCESS;
    }

    obj_config = CONFIG(config);
    CHECK_CONFIG(obj_config);

    VASurfaceAttrib *attribs = NULL;
    attribs = malloc(IPVR_MAX_SURFACE_ATTRIBUTES *sizeof(VASurfaceAttrib));
    if (attribs == NULL) {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "%s: failed due to allocation failure\n", __func__);
        DEBUG_FUNC_EXIT
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    attribs[i].type = VASurfaceAttribPixelFormat;
    attribs[i].value.type = VAGenericValueTypeInteger;
    attribs[i].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
    attribs[i].value.value.i = VA_FOURCC('N', 'V', '1', '2');
    i++;

    attribs[i].type = VASurfaceAttribMemoryType;
    attribs[i].value.type = VAGenericValueTypeInteger;
    attribs[i].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
    if (obj_config->entrypoint == VAEntrypointEncSlice && obj_config->profile == VAProfileVP8Version0_3) {
        attribs[i].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA
            | VA_SURFACE_ATTRIB_MEM_TYPE_KERNEL_DRM
#ifdef ANDROID
            | VA_SURFACE_ATTRIB_MEM_TYPE_ANDROID_GRALLOC
            | VA_SURFACE_ATTRIB_MEM_TYPE_ANDROID_ION
#endif
            ;
    } else {
        attribs[i].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA
            | VA_SURFACE_ATTRIB_MEM_TYPE_KERNEL_DRM
            | VA_SURFACE_ATTRIB_MEM_TYPE_USER_PTR
#ifdef ANDROID
            | VA_SURFACE_ATTRIB_MEM_TYPE_ANDROID_GRALLOC
            | VA_SURFACE_ATTRIB_MEM_TYPE_ANDROID_ION
#endif
            ;
    }
    i++;

    attribs[i].type = VASurfaceAttribExternalBufferDescriptor;
    attribs[i].value.type = VAGenericValueTypePointer;
    attribs[i].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[i].value.value.p = NULL;
    i++;

    //modules have speical formats to support
    if (obj_config->entrypoint == VAEntrypointVLD) { /* decode */
    } else if (obj_config->entrypoint == VAEntrypointEncSlice ||  /* encode */
                   obj_config->entrypoint == VAEntrypointEncPicture) {
    }
    else if (obj_config->entrypoint == VAEntrypointVideoProc) { /* vpp */
    }

    if (i > *num_attribs) {
        *num_attribs = i;
        drv_debug_msg(VIDEO_DEBUG_ERROR, "%s: failed due to num_attribs\n", __func__);
        DEBUG_FUNC_EXIT
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    *num_attribs = i;
    memcpy(attrib_list, attribs, i * sizeof(*attribs));
    free(attribs);

    DEBUG_FUNC_EXIT
    return vaStatus;
}

VAStatus ipvr_LockSurface(
    VADriverContextP ctx,
    VASurfaceID surface,
    unsigned int *fourcc, /* following are output argument */
    unsigned int *luma_stride,
    unsigned int *chroma_u_stride,
    unsigned int *chroma_v_stride,
    unsigned int *luma_offset,
    unsigned int *chroma_u_offset,
    unsigned int *chroma_v_offset,
    unsigned int *buffer_name,
    void **buffer
)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    unsigned char *surface_data;

    object_surface_p obj_surface = SURFACE(surface);
    ipvr_surface_p ipvr_surface;
    CHECK_SURFACE(obj_surface);

    ipvr_surface = obj_surface->ipvr_surface;
    if (buffer_name)
        drm_ipvr_gem_bo_flink(ipvr_surface->buf, buffer_name);

    if (buffer) { /* map the surface buffer */
        uint32_t srf_buf_ofs = 0;
        if (drm_ipvr_gem_bo_map(ipvr_surface->buf, 0, ipvr_surface->buf->size, 1)) {
            *buffer = NULL;
            vaStatus = VA_STATUS_ERROR_UNKNOWN;
            DEBUG_FAILURE;
            drv_debug_msg(VIDEO_DEBUG_ERROR, "%s: BO mapping failed\n", __func__);
            DEBUG_FUNC_EXIT
            return vaStatus;
        }
        surface_data = ipvr_surface->buf->virt;
        srf_buf_ofs = ipvr_surface->buf->buffer_ofs;
        *buffer = surface_data + srf_buf_ofs;
    }

    *fourcc = VA_FOURCC_NV12;
    *luma_stride = ipvr_surface->stride;
    *chroma_u_stride = ipvr_surface->stride;
    *chroma_v_stride = ipvr_surface->stride;
    *luma_offset = 0;
    *chroma_u_offset = obj_surface->height * ipvr_surface->stride;
    *chroma_v_offset = obj_surface->height * ipvr_surface->stride + 1;
    DEBUG_FUNC_EXIT
    return vaStatus;
}


VAStatus ipvr_UnlockSurface(
    VADriverContextP ctx,
    VASurfaceID surface
)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    object_surface_p obj_surface = SURFACE(surface);
    CHECK_SURFACE(obj_surface);

    ipvr_surface_p ipvr_surface = obj_surface->ipvr_surface;

    drm_ipvr_gem_bo_unmap(ipvr_surface->buf);

    DEBUG_FUNC_EXIT
    return VA_STATUS_SUCCESS;
}

VAStatus ipvr_GetEGLClientBufferFromSurface(
    VADriverContextP ctx,
    VASurfaceID surface,
    void **buffer
)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    object_surface_p obj_surface = SURFACE(surface);
    CHECK_SURFACE(obj_surface);

    ipvr_surface_p ipvr_surface = obj_surface->ipvr_surface;
    *buffer = (void *)(unsigned long)ipvr_surface->bc_buffer;

    DEBUG_FUNC_EXIT
    return vaStatus;
}

VAStatus ipvr_PutSurfaceBuf(
    VADriverContextP ctx,
    VASurfaceID surface,
    unsigned char* data,
    int* data_len,
    short srcx,
    short srcy,
    unsigned short srcw,
    unsigned short srch,
    short destx,
    short desty,
    unsigned short destw,
    unsigned short desth,
    VARectangle *cliprects, /* client supplied clip list */
    unsigned int number_cliprects, /* number of clip rects in the clip list */
    unsigned int flags /* de-interlacing flags */
)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA;
    object_surface_p obj_surface = SURFACE(surface);

    obj_surface = SURFACE(surface);
    if (obj_surface == NULL) {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "%s: failed due to invalid surface\n", __func__);
        DEBUG_FUNC_EXIT
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    DEBUG_FUNC_EXIT
    return VA_STATUS_SUCCESS;
}

VAStatus ipvr_SetTimestampForSurface(
    VADriverContextP ctx,
    VASurfaceID surface,
    long long timestamp
)
{
    INIT_DRIVER_DATA;
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_surface_p obj_surface = SURFACE(surface);

    obj_surface = SURFACE(surface);
    CHECK_SURFACE(obj_surface);

    if (obj_surface->share_info) {
        obj_surface->share_info->timestamp = timestamp;
        return VA_STATUS_SUCCESS;
    } else {
        return VA_STATUS_ERROR_UNKNOWN;
    }
}

int  LOCK_HARDWARE(ipvr_driver_data_p driver_data)
{
    char ret = 0;

    if (driver_data->dri2 || driver_data->dri_dummy)
        return 0;

    pthread_mutex_lock(&driver_data->drm_mutex);
    DRM_CAS(driver_data->drm_lock, driver_data->drm_context,
            (DRM_LOCK_HELD | driver_data->drm_context), ret);
    if (ret) {
        ret = drmGetLock(driver_data->drm_fd, driver_data->drm_context, 0);
        /* driver_data->contended_lock=1; */
    }

    return ret;
}

int UNLOCK_HARDWARE(ipvr_driver_data_p driver_data)
{
    /* driver_data->contended_lock=0; */
    if (driver_data->dri2 || driver_data->dri_dummy)
        return 0;

    DRM_UNLOCK(driver_data->drm_fd, driver_data->drm_lock, driver_data->drm_context);
    pthread_mutex_unlock(&driver_data->drm_mutex);

    return 0;
}


static void ipvr__deinitDRM(VADriverContextP ctx)
{
    INIT_DRIVER_DATA

    drm_ipvr_gem_bufmgr_destroy(driver_data->bufmgr);
    driver_data->bufmgr = NULL;
    driver_data->drm_fd = -1;
}


static VAStatus ipvr__initDRI(VADriverContextP ctx)
{
    INIT_DRIVER_DATA
    struct drm_state *drm_state = (struct drm_state *)ctx->drm_state;

#ifdef BAYTRAIL
    if (drm_state->fd >= 0)
        close(drm_state->fd);
    drm_state->fd = open("/dev/dri/card1", O_RDWR);
    if (drm_state->fd < 0)
        abort();
#endif

    driver_data->drm_fd = drm_state->fd;
    driver_data->dri_dummy = 1;
    driver_data->dri2 = 0;
    driver_data->ws_priv = NULL;
    driver_data->bus_id = NULL;

    return VA_STATUS_SUCCESS;
}


static VAStatus ipvr__initDRM(VADriverContextP ctx)
{
    INIT_DRIVER_DATA
    VAStatus vaStatus = ipvr__initDRI(ctx);

    if (vaStatus == VA_STATUS_SUCCESS) {
        driver_data->bufmgr = drm_ipvr_gem_bufmgr_init(driver_data->drm_fd, 0x8000);
        if (driver_data->bufmgr == NULL) {
            drv_debug_msg(VIDEO_DEBUG_ERROR, "failed to get GEM pool\n");
            return VA_STATUS_ERROR_UNKNOWN;
        }
        return VA_STATUS_SUCCESS;
    }
    else
        return vaStatus;
}

VAStatus ipvr_Terminate(VADriverContextP ctx)
{
    DEBUG_FUNC_ENTER
    INIT_DRIVER_DATA
    object_subpic_p obj_subpic;
    object_image_p obj_image;
    object_buffer_p obj_buffer;
    object_surface_p obj_surface;
    object_context_p obj_context;
    object_config_p obj_config;
    object_heap_iterator iter;

    drv_debug_msg(VIDEO_DEBUG_INIT, "vaTerminate: begin to tear down\n");

    /* Clean up left over contexts */
    obj_context = (object_context_p) object_heap_first(&driver_data->context_heap, &iter);
    while (obj_context) {
        drv_debug_msg(VIDEO_DEBUG_INIT, "vaTerminate: contextID %08x still allocated, destroying\n", obj_context->base.id);
        ipvr__destroy_context(driver_data, obj_context);
        obj_context = (object_context_p) object_heap_next(&driver_data->context_heap, &iter);
    }
    object_heap_destroy(&driver_data->context_heap);

    /* Clean up SubpicIDs */
    obj_subpic = (object_subpic_p) object_heap_first(&driver_data->subpic_heap, &iter);
    while (obj_subpic) {
        drv_debug_msg(VIDEO_DEBUG_INIT, "vaTerminate: subpictureID %08x still allocated, destroying\n", obj_subpic->base.id);
        ipvr__destroy_subpicture(driver_data, obj_subpic);
        obj_subpic = (object_subpic_p) object_heap_next(&driver_data->subpic_heap, &iter);
    }
    object_heap_destroy(&driver_data->subpic_heap);

    /* Clean up ImageIDs */
    obj_image = (object_image_p) object_heap_first(&driver_data->image_heap, &iter);
    while (obj_image) {
        drv_debug_msg(VIDEO_DEBUG_INIT, "vaTerminate: imageID %08x still allocated, destroying\n", obj_image->base.id);
        ipvr__destroy_image(driver_data, obj_image);
        obj_image = (object_image_p) object_heap_next(&driver_data->image_heap, &iter);
    }
    object_heap_destroy(&driver_data->image_heap);

    /* Clean up left over buffers */
    obj_buffer = (object_buffer_p) object_heap_first(&driver_data->buffer_heap, &iter);
    while (obj_buffer) {
        drv_debug_msg(VIDEO_DEBUG_INIT, "vaTerminate: bufferID %08x still allocated, destroying\n", obj_buffer->base.id);
        ipvr__destroy_buffer(driver_data, obj_buffer);
        obj_buffer = (object_buffer_p) object_heap_next(&driver_data->buffer_heap, &iter);
    }
    object_heap_destroy(&driver_data->buffer_heap);

    /* Clean up left over surfaces */
    obj_surface = (object_surface_p) object_heap_first(&driver_data->surface_heap, &iter);
    while (obj_surface) {
        drv_debug_msg(VIDEO_DEBUG_INIT, "vaTerminate: surfaceID %08x still allocated, destroying\n", obj_surface->base.id);
        ipvr__destroy_surface(driver_data, obj_surface);
        obj_surface = (object_surface_p) object_heap_next(&driver_data->surface_heap, &iter);
    }
    object_heap_destroy(&driver_data->surface_heap);

    /* Clean up configIDs */
    obj_config = (object_config_p) object_heap_first(&driver_data->config_heap, &iter);
    while (obj_config) {
        object_heap_free(&driver_data->config_heap, (object_base_p) obj_config);
        obj_config = (object_config_p) object_heap_next(&driver_data->config_heap, &iter);
    }
    object_heap_destroy(&driver_data->config_heap);

    if (driver_data->camera_bo) {
        drv_debug_msg(VIDEO_DEBUG_INIT, "vaTerminate: clearup camera global BO\n");
        /* TODO: destroy it */
        driver_data->camera_bo = NULL;
    }

    if (driver_data->rar_bo) {
        drv_debug_msg(VIDEO_DEBUG_INIT, "vaTerminate: clearup RAR global BO\n");
        /* TODO: destroy it */
        driver_data->rar_bo = NULL;
    }

    if (!IS_BAYTRAIL(driver_data) && driver_data->ws_priv) {
        drv_debug_msg(VIDEO_DEBUG_INIT, "vaTerminate: tear down output portion\n");
        ipvr_deinitOutput(ctx);
        driver_data->ws_priv = NULL;
    }

    drv_debug_msg(VIDEO_DEBUG_INIT, "vaTerminate: de-initialized DRM\n");

    ipvr__deinitDRM(ctx);

    if (driver_data->ved_decode_status)
        free(driver_data->ved_decode_status);

    if (driver_data->surface_mb_error)
        free(driver_data->surface_mb_error);

    pthread_mutex_destroy(&driver_data->drm_mutex);
    free(ctx->pDriverData);
    free(ctx->vtable_egl);
    free(ctx->vtable_tpi);

    ctx->pDriverData = NULL;
    drv_debug_msg(VIDEO_DEBUG_INIT, "vaTerminate: goodbye\n\n");

    ipvr__close_log();
    DEBUG_FUNC_EXIT
    return VA_STATUS_SUCCESS;
}

EXPORT VAStatus __vaDriverInit_0_32(VADriverContextP ctx)
{
    ipvr_driver_data_p driver_data;
    VAStatus va_status = VA_STATUS_SUCCESS;
    struct VADriverVTableTPI *tpi;
    struct VADriverVTableEGL *va_egl;
    int result;
    if (ipvr_video_trace_fp) {
        /* make gdb always stop here */
        signal(SIGUSR1, SIG_IGN);
        kill(getpid(), SIGUSR1);
    }

    ipvr__open_log();

    drv_debug_msg(VIDEO_DEBUG_INIT, "vaInitilize: start the journey\n");

    ctx->version_major = 0;
    ctx->version_minor = 31;

    ctx->max_profiles = IPVR_MAX_PROFILES;
    ctx->max_entrypoints = IPVR_MAX_ENTRYPOINTS;
    ctx->max_attributes = IPVR_MAX_CONFIG_ATTRIBUTES;
    ctx->max_image_formats = IPVR_MAX_IMAGE_FORMATS;
    ctx->max_subpic_formats = IPVR_MAX_SUBPIC_FORMATS;
    ctx->max_display_attributes = IPVR_MAX_DISPLAY_ATTRIBUTES;

    ctx->vtable->vaTerminate = ipvr_Terminate;
    ctx->vtable->vaQueryConfigEntrypoints = ipvr_QueryConfigEntrypoints;
    ctx->vtable->vaQueryConfigProfiles = ipvr_QueryConfigProfiles;
    ctx->vtable->vaQueryConfigEntrypoints = ipvr_QueryConfigEntrypoints;
    ctx->vtable->vaQueryConfigAttributes = ipvr_QueryConfigAttributes;
    ctx->vtable->vaCreateConfig = ipvr_CreateConfig;
    ctx->vtable->vaDestroyConfig = ipvr_DestroyConfig;
    ctx->vtable->vaGetConfigAttributes = ipvr_GetConfigAttributes;
    ctx->vtable->vaCreateSurfaces2 = ipvr_CreateSurfaces2; 
    ctx->vtable->vaCreateSurfaces = ipvr_CreateSurfaces;
    ctx->vtable->vaGetSurfaceAttributes = ipvr_GetSurfaceAttributes;
    ctx->vtable->vaDestroySurfaces = ipvr_DestroySurfaces;
    ctx->vtable->vaCreateContext = ipvr_CreateContext;
    ctx->vtable->vaDestroyContext = ipvr_DestroyContext;
    ctx->vtable->vaCreateBuffer = ipvr_CreateBuffer;
    ctx->vtable->vaBufferSetNumElements = ipvr_BufferSetNumElements;
    ctx->vtable->vaMapBuffer = ipvr_MapBuffer;
    ctx->vtable->vaUnmapBuffer = ipvr_UnmapBuffer;
    ctx->vtable->vaDestroyBuffer = ipvr_DestroyBuffer;
    ctx->vtable->vaBeginPicture = ipvr_BeginPicture;
    ctx->vtable->vaRenderPicture = ipvr_RenderPicture;
    ctx->vtable->vaEndPicture = ipvr_EndPicture;
    ctx->vtable->vaSyncSurface = ipvr_SyncSurface;
    ctx->vtable->vaQuerySurfaceStatus = ipvr_QuerySurfaceStatus;
    ctx->vtable->vaQuerySurfaceError = ipvr_QuerySurfaceError;
    ctx->vtable->vaPutSurface = ipvr_PutSurface;
    ctx->vtable->vaQueryImageFormats = ipvr_QueryImageFormats;
    ctx->vtable->vaCreateImage = ipvr_CreateImage;
    ctx->vtable->vaDeriveImage = ipvr_DeriveImage;
    ctx->vtable->vaDestroyImage = ipvr_DestroyImage;
    ctx->vtable->vaSetImagePalette = ipvr_SetImagePalette;
    ctx->vtable->vaGetImage = ipvr_GetImage;
    ctx->vtable->vaPutImage = ipvr_PutImage;
    ctx->vtable->vaQuerySubpictureFormats = ipvr_QuerySubpictureFormats;
    ctx->vtable->vaCreateSubpicture = ipvr_CreateSubpicture;
    ctx->vtable->vaDestroySubpicture = ipvr_DestroySubpicture;
    ctx->vtable->vaSetSubpictureImage = ipvr_SetSubpictureImage;
    ctx->vtable->vaSetSubpictureChromakey = ipvr_SetSubpictureChromakey;
    ctx->vtable->vaSetSubpictureGlobalAlpha = ipvr_SetSubpictureGlobalAlpha;
    ctx->vtable->vaAssociateSubpicture = ipvr_AssociateSubpicture;
    ctx->vtable->vaDeassociateSubpicture = ipvr_DeassociateSubpicture;
    ctx->vtable->vaQueryDisplayAttributes = ipvr_QueryDisplayAttributes;
    ctx->vtable->vaGetDisplayAttributes = ipvr_GetDisplayAttributes;
    ctx->vtable->vaSetDisplayAttributes = ipvr_SetDisplayAttributes;
    ctx->vtable->vaQuerySurfaceAttributes = ipvr_QuerySurfaceAttributes;
    ctx->vtable->vaBufferInfo = ipvr_BufferInfo;
    ctx->vtable->vaLockSurface = ipvr_LockSurface;
    ctx->vtable->vaUnlockSurface = ipvr_UnlockSurface;

    ctx->vtable_tpi = calloc(1, sizeof(struct VADriverVTableTPI));
    if (NULL == ctx->vtable_tpi) {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "ERROR: failed to allocate TPI table\n");
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    tpi = (struct VADriverVTableTPI *)ctx->vtable_tpi;
#ifdef ANDROID
    tpi->vaCreateSurfacesWithAttribute = ipvr_CreateSurfacesWithAttribute;
    tpi->vaSetTimestampForSurface = ipvr_SetTimestampForSurface;
#endif
    tpi->vaPutSurfaceBuf = ipvr_PutSurfaceBuf;

    ctx->vtable_egl = calloc(1, sizeof(struct VADriverVTableEGL));
    if (NULL == ctx->vtable_egl) {
        free(ctx->vtable_tpi);
        drv_debug_msg(VIDEO_DEBUG_ERROR, "ERROR: failed to allocate EGL table\n");
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    va_egl = (struct VADriverVTableEGL *)ctx->vtable_egl;
    va_egl->vaGetEGLClientBufferFromSurface = ipvr_GetEGLClientBufferFromSurface;

    driver_data = (ipvr_driver_data_p) calloc(1, sizeof(*driver_data));
    ctx->pDriverData = (unsigned char *) driver_data;
    if (NULL == driver_data) {
        free(ctx->vtable_tpi);
        free(ctx->vtable_egl);
        drv_debug_msg(VIDEO_DEBUG_ERROR, "ERROR: failed to allocate driverData\n");
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    if (VA_STATUS_SUCCESS != ipvr__initDRM(ctx)) {
        free(ctx->vtable_tpi);
        free(ctx->vtable_egl);
        free(ctx->pDriverData);
        ctx->pDriverData = NULL;
        drv_debug_msg(VIDEO_DEBUG_ERROR, "ERROR: failed to init DRI\n");
        return VA_STATUS_ERROR_UNKNOWN;
    }

    pthread_mutex_init(&driver_data->drm_mutex, NULL);

    /*
     * To read PBO.MSR.CCF Mode and Status Register C-Spec -p112
     */
#define PCI_PORT5_REG80_VIDEO_SD_DISABLE        0x0008
#define PCI_PORT5_REG80_VIDEO_HD_DISABLE        0x0010

    if (ipvr_get_device_info(ctx)) {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "ERROR: failed to get video device info\n");
    }

    if (IS_BAYTRAIL(driver_data))
        ctx->str_vendor = IPVR_STR_VENDOR_BAYTRAIL;
    else if (IS_MERRIFIELD(driver_data))
        ctx->str_vendor = IPVR_STR_VENDOR_MERRIFIELD;
    else {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "Unsupported video device, abort\n");
        pthread_mutex_destroy(&driver_data->drm_mutex);
        ipvr__deinitDRM(ctx);
        free(ctx->vtable_tpi);
        free(ctx->vtable_egl);
        free(ctx->pDriverData);
        ctx->pDriverData = NULL;
        return VA_STATUS_ERROR_UNKNOWN;
    }

    if (VA_STATUS_SUCCESS != ipvr_initOutput(ctx)) {
        pthread_mutex_destroy(&driver_data->drm_mutex);
        ipvr__deinitDRM(ctx);
        free(ctx->vtable_tpi);
        free(ctx->vtable_egl);
        free(ctx->pDriverData);
        ctx->pDriverData = NULL;
        return VA_STATUS_ERROR_UNKNOWN;
    }

    driver_data->ved_context_base = (((unsigned int) getpid()) & 0xffff) << 16;

    if (IS_BAYTRAIL(driver_data)) {
        drv_debug_msg(VIDEO_DEBUG_GENERAL, "baytrail msvdx decoder\n");
        driver_data->profile2Format[VAProfileVP8Version0_3][VAEntrypointVLD] = &tng_VP8_vtable;
    }
    if (IS_MERRIFIELD(driver_data)) {
        drv_debug_msg(VIDEO_DEBUG_GENERAL, "merrifield msvdx decoder/topaz encoder/vsp postproc\n");
    }

    result = object_heap_init(&driver_data->config_heap, sizeof(struct object_config_s), CONFIG_ID_OFFSET);
    if (result) {
        va_status = VA_STATUS_ERROR_OPERATION_FAILED;
        goto out_err;
    }

    result = object_heap_init(&driver_data->context_heap, sizeof(struct object_context_s), CONTEXT_ID_OFFSET);
    if (result) {
        va_status = VA_STATUS_ERROR_OPERATION_FAILED;
        goto out_err;
    }

    result = object_heap_init(&driver_data->surface_heap, sizeof(struct object_surface_s), SURFACE_ID_OFFSET);
    if (result) {
        va_status = VA_STATUS_ERROR_OPERATION_FAILED;
        goto out_err;
    }

    result = object_heap_init(&driver_data->buffer_heap, sizeof(struct object_buffer_s), BUFFER_ID_OFFSET);
    if (result) {
        va_status = VA_STATUS_ERROR_OPERATION_FAILED;
        goto out_err;
    }

    result = object_heap_init(&driver_data->image_heap, sizeof(struct object_image_s), IMAGE_ID_OFFSET);
    if (result) {
        va_status = VA_STATUS_ERROR_OPERATION_FAILED;
        goto out_err;
    }

    result = object_heap_init(&driver_data->subpic_heap, sizeof(struct object_subpic_s), SUBPIC_ID_OFFSET);
    if (result) {
        va_status = VA_STATUS_ERROR_OPERATION_FAILED;
        goto out_err;
    }

    driver_data->cur_displaying_surface = VA_INVALID_SURFACE;
    driver_data->last_displaying_surface = VA_INVALID_SURFACE;

    driver_data->clear_color = 0;
    driver_data->blend_color = 0;
    driver_data->blend_mode = 0;
    driver_data->overlay_auto_paint_color_key = 0;

    driver_data->ved_decode_status = calloc(1, sizeof(struct drm_ipvr_decode_status));
    if (NULL == driver_data->ved_decode_status) {
        va_status = VA_STATUS_ERROR_ALLOCATION_FAILED;
        goto out_err;
    }
    driver_data->surface_mb_error = calloc(MAX_MB_ERRORS, sizeof(VASurfaceDecodeMBErrors));
    if (NULL == driver_data->surface_mb_error) {
        va_status = VA_STATUS_ERROR_ALLOCATION_FAILED;
        goto out_err;
    }

    drv_debug_msg(VIDEO_DEBUG_INIT, "vaInitilize: succeeded!\n\n");

    return VA_STATUS_SUCCESS;

out_err:
    ipvr_Terminate(ctx);
    return va_status;
}

static int ipvr_get_device_info(VADriverContextP ctx)
{
    INIT_DRIVER_DATA;

    uint16_t dev_id, caps;
    int ret;

    driver_data->dev_id = 0xFFFF; /* by default unknown */
    ret = drm_ipvr_gem_bufmgr_get_device_info(driver_data->bufmgr, &dev_id, &caps);
    if (ret) {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "failed to get video device info\n");
        return ret;
    }

    driver_data->dev_id = dev_id;
    drv_debug_msg(VIDEO_DEBUG_INIT, "Retrieve Device ID 0x%08x\n", driver_data->dev_id);

    if (!IS_BAYTRAIL(driver_data))
        driver_data->encode_supported = 1;

    driver_data->decode_supported = 1;
    driver_data->hd_decode_supported = 1;
    driver_data->hd_encode_supported = 1;
    return 0;
}
