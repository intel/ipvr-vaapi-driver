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
 * Authors:
 *    Shengquan Yuan  <shengquan.yuan@intel.com>
 *    Zhaohan Ren  <zhaohan.ren@intel.com>
 *    Jason Hu <jason.hu@intel.com>
 *    Yao Cheng <yao.cheng@intel.com>
 *
 */

#ifdef ANDROID
#include <va/va_drmcommon.h>
#include "android/ipvr_android.h"
#elif defined LINUX
#include <va/va_dricommon.h>
#else
#error "Unsupported window system"
#endif

#include <va/va_backend.h>
#include <dlfcn.h>
#include <stdlib.h>
#include "ipvr_output.h"
#include "ipvr_surface.h"
#include "ipvr_surface_ext.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "ipvr_drv_debug.h"
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define INIT_DRIVER_DATA        ipvr_driver_data_p driver_data = (ipvr_driver_data_p) ctx->pDriverData;

#define SURFACE(id)     ((object_surface_p) object_heap_lookup( &driver_data->surface_heap, id ))
#define BUFFER(id)  ((object_buffer_p) object_heap_lookup( &driver_data->buffer_heap, id ))
#define IMAGE(id)  ((object_image_p) object_heap_lookup( &driver_data->image_heap, id ))
#define SUBPIC(id)  ((object_subpic_p) object_heap_lookup( &driver_data->subpic_heap, id ))
#define CONTEXT(id) ((object_context_p) object_heap_lookup( &driver_data->context_heap, id ))

extern void ipvr__suspend_buffer(ipvr_driver_data_p driver_data, object_buffer_p obj_buffer);

/* surfaces link list associated with a subpicture */
typedef struct _subpic_surface {
    VASurfaceID surface_id;
    struct _subpic_surface *next;
} subpic_surface_s, *subpic_surface_p;


static VAImageFormat ipvr__SubpicFormat[] = {
    ipvr__ImageRGBA,
};

static VAImageFormat ipvr__CreateImageFormat[] = {
    ipvr__ImageNV12,
    ipvr__ImageRGBA,
    ipvr__ImageYV16,
    ipvr__ImageYV32
};

int ipvr_coverlay_init(VADriverContextP ctx);
int ipvr_coverlay_deinit(VADriverContextP ctx);

VAStatus ipvr_initOutput(VADriverContextP ctx)
{
    return VA_STATUS_SUCCESS;
}

VAStatus ipvr_deinitOutput(
    VADriverContextP ctx
)
{
    return VA_STATUS_SUCCESS;
}

#ifndef VA_STATUS_ERROR_INVALID_IMAGE_FORMAT
#define VA_STATUS_ERROR_INVALID_IMAGE_FORMAT VA_STATUS_ERROR_UNKNOWN
#endif

static VAImageFormat *ipvr__VAImageCheckFourCC(
    VAImageFormat       *src_format,
    VAImageFormat       *dst_format,
    int                 dst_num
)
{
    int i;
    if (NULL == src_format || dst_format == NULL) {
        return NULL;
    }

    /* check VAImage at first */
    for (i = 0; i < dst_num; i++) {
        if (dst_format[i].fourcc == src_format->fourcc)
            return &dst_format[i];
    }

    drv_debug_msg(VIDEO_DEBUG_ERROR, "Unsupport fourcc 0x%x\n", src_format->fourcc);
    return NULL;
}

static void ipvr__VAImageCheckRegion(
    object_surface_p surface,
    VAImage *image,
    int *src_x,
    int *src_y,
    int *dest_x,
    int *dest_y,
    int *width,
    int *height
)
{
    /* check for image */
    if (*src_x < 0) *src_x = 0;
    if (*src_x > image->width) *src_x = image->width - 1;
    if (*src_y < 0) *src_y = 0;
    if (*src_y > image->height) *src_y = image->height - 1;

    if (((*width) + (*src_x)) > image->width) *width = image->width - *src_x;
    if (((*height) + (*src_y)) > image->height) *height = image->height - *src_x;

    /* check for surface */
    if (*dest_x < 0) *dest_x = 0;
    if (*dest_x > surface->width) *dest_x = surface->width - 1;
    if (*dest_y < 0) *dest_y = 0;
    if (*dest_y > surface->height) *dest_y = surface->height - 1;

    if (((*width) + (*dest_x)) > surface->width) *width = surface->width - *dest_x;
    if (((*height) + (*dest_y)) > surface->height) *height = surface->height - *dest_x;
}


VAStatus ipvr_QueryImageFormats(
    VADriverContextP ctx,
    VAImageFormat *format_list,        /* out */
    int *num_formats           /* out */
)
{
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    CHECK_INVALID_PARAM(format_list == NULL);
    CHECK_INVALID_PARAM(num_formats == NULL);

    memcpy(format_list, ipvr__CreateImageFormat, sizeof(ipvr__CreateImageFormat));
    *num_formats = IPVR_MAX_IMAGE_FORMATS;

    return VA_STATUS_SUCCESS;
}

inline int min_POT(int n)
{
    if ((n & (n - 1)) == 0) /* already POT */
        return n;

    return n |= n >> 16, n |= n >> 8, n |= n >> 4, n |= n >> 2, n |= n >> 1, n + 1;
    /* return ((((n |= n>>16) |= n>>8) |= n>>4) |= n>>2) |= n>>1, n + 1; */
}

VAStatus ipvr_CreateImage(
    VADriverContextP ctx,
    VAImageFormat *format,
    int width,
    int height,
    VAImage *image     /* out */
)
{
    INIT_DRIVER_DATA;
    VAImageID imageID;
    object_image_p obj_image;
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    VAImageFormat *img_fmt;
    int pitch_pot;

    (void)driver_data;

    img_fmt = ipvr__VAImageCheckFourCC(format, ipvr__CreateImageFormat,
                                      sizeof(ipvr__CreateImageFormat) / sizeof(VAImageFormat));
    if (img_fmt == NULL)
        return VA_STATUS_ERROR_UNKNOWN;

    CHECK_INVALID_PARAM(image == NULL);

    imageID = object_heap_allocate(&driver_data->image_heap);
    obj_image = IMAGE(imageID);
    CHECK_ALLOCATION(obj_image);

    MEMSET_OBJECT(obj_image, struct object_image_s);

    obj_image->image.image_id = imageID;
    obj_image->image.format = *img_fmt;
    obj_image->subpic_ref = 0;

    pitch_pot = min_POT(width);

    switch (format->fourcc) {
    case VA_FOURCC_NV12: {
        obj_image->image.width = width;
        obj_image->image.height = height;
        obj_image->image.data_size = pitch_pot * height /*Y*/ + 2 * (pitch_pot / 2) * (height / 2);/*UV*/
        obj_image->image.num_planes = 2;
        obj_image->image.pitches[0] = pitch_pot;
        obj_image->image.pitches[1] = pitch_pot;
        obj_image->image.offsets[0] = 0;
        obj_image->image.offsets[1] = pitch_pot * height;
        obj_image->image.num_palette_entries = 0;
        obj_image->image.entry_bytes = 0;
        obj_image->image.component_order[0] = 'Y';
        obj_image->image.component_order[1] = 'U';/* fixed me: packed UV packed here! */
        obj_image->image.component_order[2] = 'V';
        obj_image->image.component_order[3] = '\0';
        break;
    }
    case VA_FOURCC_AYUV: {
        obj_image->image.width = width;
        obj_image->image.height = height;
        obj_image->image.data_size = 4 * pitch_pot * height;
        obj_image->image.num_planes = 1;
        obj_image->image.pitches[0] = 4 * pitch_pot;
        obj_image->image.num_palette_entries = 0;
        obj_image->image.entry_bytes = 0;
        obj_image->image.component_order[0] = 'V';
        obj_image->image.component_order[1] = 'U';
        obj_image->image.component_order[2] = 'Y';
        obj_image->image.component_order[3] = 'A';
        break;
    }
    case VA_FOURCC_RGBA: {
        obj_image->image.width = width;
        obj_image->image.height = height;
        obj_image->image.data_size = 4 * pitch_pot * height;
        obj_image->image.num_planes = 1;
        obj_image->image.pitches[0] = 4 * pitch_pot;
        obj_image->image.num_palette_entries = 0;
        obj_image->image.entry_bytes = 0;
        obj_image->image.component_order[0] = 'R';
        obj_image->image.component_order[1] = 'G';
        obj_image->image.component_order[2] = 'B';
        obj_image->image.component_order[3] = 'A';
        break;
    }
    case VA_FOURCC_AI44: {
        obj_image->image.width = width;
        obj_image->image.height = height;
        obj_image->image.data_size = pitch_pot * height;/* one byte one element */
        obj_image->image.num_planes = 1;
        obj_image->image.pitches[0] = pitch_pot;
        obj_image->image.num_palette_entries = 16;
        obj_image->image.entry_bytes = 4; /* AYUV */
        obj_image->image.component_order[0] = 'I';
        obj_image->image.component_order[1] = 'A';
        obj_image->image.component_order[2] = '\0';
        obj_image->image.component_order[3] = '\0';
        break;
    }
    case VA_FOURCC_IYUV: {
        obj_image->image.width = width;
        obj_image->image.height = height;
        obj_image->image.data_size = pitch_pot * height /*Y*/ + 2 * (pitch_pot / 2) * (height / 2);/*UV*/
        obj_image->image.num_planes = 3;
        obj_image->image.pitches[0] = pitch_pot;
        obj_image->image.pitches[1] = pitch_pot / 2;
        obj_image->image.pitches[2] = pitch_pot / 2;
        obj_image->image.offsets[0] = 0;
        obj_image->image.offsets[1] = pitch_pot * height;
        obj_image->image.offsets[2] = pitch_pot * height + (pitch_pot / 2) * (height / 2);
        obj_image->image.num_palette_entries = 0;
        obj_image->image.entry_bytes = 0;
        obj_image->image.component_order[0] = 'Y';
        obj_image->image.component_order[1] = 'U';
        obj_image->image.component_order[2] = 'V';
        obj_image->image.component_order[3] = '\0';
        break;
    }
    case VA_FOURCC_YV32: {
        obj_image->image.width = width;
        obj_image->image.height = height;
        obj_image->image.data_size = 4 * pitch_pot * height;
        obj_image->image.num_planes = 4;
        obj_image->image.pitches[0] = pitch_pot;
        obj_image->image.pitches[1] = pitch_pot;
        obj_image->image.pitches[2] = pitch_pot;

        obj_image->image.offsets[0] = 0;
        obj_image->image.offsets[1] = pitch_pot * height;
        obj_image->image.offsets[2] = pitch_pot * height * 2;

        obj_image->image.num_palette_entries = 0;
        obj_image->image.entry_bytes = 0;
        obj_image->image.component_order[0] = 'V';
        obj_image->image.component_order[1] = 'U';
        obj_image->image.component_order[2] = 'Y';
        obj_image->image.component_order[3] = 'A';
        break;
    }
    default: {
        vaStatus = VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
        break;
    }
    }

    if (VA_STATUS_SUCCESS == vaStatus) {
        /* create the buffer */
        vaStatus = ipvr__CreateBuffer(driver_data, NULL, VAImageBufferType,
                                     obj_image->image.data_size, 1, NULL, &obj_image->image.buf);
    }

    obj_image->derived_surface = 0;

    if (VA_STATUS_SUCCESS != vaStatus) {
        object_heap_free(&driver_data->image_heap, (object_base_p) obj_image);
    } else {
        memcpy(image, &obj_image->image, sizeof(VAImage));
    }

    return vaStatus;
}

VAStatus ipvr_DeriveImage(
    VADriverContextP ctx,
    VASurfaceID surface,
    VAImage *image     /* out */
)
{
    INIT_DRIVER_DATA;
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    VABufferID bufferID;
    object_buffer_p obj_buffer;
    VAImageID imageID;
    object_image_p obj_image;
    object_surface_p obj_surface = SURFACE(surface);
    unsigned int fourcc, fourcc_index = ~0, i;
    uint32_t srf_buf_ofs = 0;

    CHECK_SURFACE(obj_surface);
    CHECK_INVALID_PARAM(image == NULL);
    /* Can't derive image from reconstrued frame which is in tiled format */
    if (obj_surface->is_ref_surface == 1 || obj_surface->is_ref_surface == 2) {
        if (getenv("IPVR_VIDEO_IGNORE_TILED_FORMAT")) {
            drv_debug_msg(VIDEO_DEBUG_GENERAL, "Ignore tiled memory format" \
                "of rec-frames\n");
        } else {
            drv_debug_msg(VIDEO_DEBUG_ERROR, "Can't derive reference surface" \
                "which is tiled format\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    fourcc = obj_surface->ipvr_surface->extra_info[4];
    for (i = 0; i < IPVR_MAX_IMAGE_FORMATS; i++) {
        if (ipvr__CreateImageFormat[i].fourcc == fourcc) {
            fourcc_index = i;
            break;
        }
    }
    if (i == IPVR_MAX_IMAGE_FORMATS) {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "Can't support the Fourcc\n");
        vaStatus = VA_STATUS_ERROR_OPERATION_FAILED;
        return vaStatus;
    }

    /* create the image */
    imageID = object_heap_allocate(&driver_data->image_heap);
    obj_image = IMAGE(imageID);
    CHECK_ALLOCATION(obj_image);

    MEMSET_OBJECT(obj_image, struct object_image_s);

    /* create a buffer to represent surface buffer */
    bufferID = object_heap_allocate(&driver_data->buffer_heap);
    obj_buffer = BUFFER(bufferID);
    if (NULL == obj_buffer) {
        object_heap_free(&driver_data->image_heap, (object_base_p) obj_image);
        vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
        DEBUG_FAILURE;
        return vaStatus;
    }
    MEMSET_OBJECT(obj_buffer, struct object_buffer_s);

    obj_buffer->type = VAImageBufferType;
    obj_buffer->buffer_data = NULL;
    obj_buffer->ipvr_bo = obj_surface->ipvr_surface->buf;
    obj_buffer->size = obj_surface->ipvr_surface->size;
    obj_buffer->max_num_elements = 0;
    obj_buffer->alloc_size = obj_buffer->size;

    /* fill obj_image data structure */
    obj_image->image.image_id = imageID;
    obj_image->image.format = ipvr__CreateImageFormat[fourcc_index];
    obj_image->subpic_ref = 0;

    obj_image->image.buf = bufferID;
    obj_image->image.width = obj_surface->width;
    obj_image->image.height = obj_surface->height;
    obj_image->image.data_size = obj_surface->ipvr_surface->size;

    srf_buf_ofs = obj_surface->ipvr_surface->buf->buffer_ofs;

    switch (fourcc) {
    case VA_FOURCC_NV12: {
        obj_image->image.num_planes = 2;
        obj_image->image.pitches[0] = obj_surface->ipvr_surface->stride;
        obj_image->image.pitches[1] = obj_surface->ipvr_surface->stride;

        obj_image->image.offsets[0] = srf_buf_ofs;
        obj_image->image.offsets[1] = srf_buf_ofs + obj_surface->height * obj_surface->ipvr_surface->stride;
        obj_image->image.num_palette_entries = 0;
        obj_image->image.entry_bytes = 0;
        obj_image->image.component_order[0] = 'Y';
        obj_image->image.component_order[1] = 'U';/* fixed me: packed UV packed here! */
        obj_image->image.component_order[2] = 'V';
        obj_image->image.component_order[3] = '\0';
        break;
    }
    case VA_FOURCC_YV16: {
        obj_image->image.num_planes = 3;
        obj_image->image.pitches[0] = obj_surface->ipvr_surface->stride;
        obj_image->image.pitches[1] = obj_surface->ipvr_surface->stride / 2;
        obj_image->image.pitches[2] = obj_surface->ipvr_surface->stride / 2;

        obj_image->image.offsets[0] = srf_buf_ofs;
        obj_image->image.offsets[1] = srf_buf_ofs + obj_surface->height * obj_surface->ipvr_surface->stride;
        obj_image->image.offsets[2] = srf_buf_ofs + obj_surface->height * obj_surface->ipvr_surface->stride * 3 / 2;
        obj_image->image.num_palette_entries = 0;
        obj_image->image.entry_bytes = 0;
        obj_image->image.component_order[0] = 'Y';
        obj_image->image.component_order[1] = 'V';/* fixed me: packed UV packed here! */
        obj_image->image.component_order[2] = 'U';
        obj_image->image.component_order[3] = '\0';
        break;
    }
    default:
        break;
    }

    obj_image->derived_surface = surface; /* this image is derived from a surface */
    obj_surface->derived_imgcnt++;

    memcpy(image, &obj_image->image, sizeof(VAImage));

    return vaStatus;
}

VAStatus ipvr__destroy_image(ipvr_driver_data_p driver_data, object_image_p obj_image)
{
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    if (obj_image->subpic_ref > 0) {
        vaStatus = VA_STATUS_ERROR_OPERATION_FAILED;
        return vaStatus;
    }

    object_surface_p obj_surface = SURFACE(obj_image->derived_surface);

    if (obj_surface == NULL) { /* destroy the buffer */
        object_buffer_p obj_buffer = BUFFER(obj_image->image.buf);
        CHECK_BUFFER(obj_buffer);
        ipvr__suspend_buffer(driver_data, obj_buffer);
    } else {
        object_buffer_p obj_buffer = BUFFER(obj_image->image.buf);
        object_heap_free(&driver_data->buffer_heap, &obj_buffer->base);
        obj_surface->derived_imgcnt--;
    }
    object_heap_free(&driver_data->image_heap, (object_base_p) obj_image);

    return VA_STATUS_SUCCESS;
}

VAStatus ipvr_DestroyImage(
    VADriverContextP ctx,
    VAImageID image
)
{
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_image_p obj_image;

    obj_image = IMAGE(image);
    CHECK_IMAGE(obj_image);
    return ipvr__destroy_image(driver_data, obj_image);
}

VAStatus ipvr_SetImagePalette(
    VADriverContextP ctx,
    VAImageID image,
    /*
     * pointer to an array holding the palette data.  The size of the array is
     * num_palette_entries * entry_bytes in size.  The order of the components
     * in the palette is described by the component_order in VAImage struct
     */
    unsigned char *palette
)
{
    INIT_DRIVER_DATA;
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    object_image_p obj_image = IMAGE(image);
    CHECK_IMAGE(obj_image);

    if (obj_image->image.format.fourcc != VA_FOURCC_AI44) {
        /* only support AI44 palette */
        vaStatus = VA_STATUS_ERROR_OPERATION_FAILED;
        return vaStatus;
    }

    if (obj_image->image.num_palette_entries > 16) {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "image.num_palette_entries(%d) is too big\n", obj_image->image.num_palette_entries);
        memcpy(obj_image->palette, palette, 16);
    } else
        memcpy(obj_image->palette, palette, obj_image->image.num_palette_entries * sizeof(unsigned int));

    return vaStatus;
}

VAStatus ipvr_GetImage(
    VADriverContextP ctx,
    VASurfaceID surface,
    int x,     /* coordinates of the upper left source pixel */
    int y,
    unsigned int width, /* width and height of the region */
    unsigned int height,
    VAImageID image_id
)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus ipvr_PutImage2(
    VADriverContextP ctx,
    VASurfaceID surface,
    VAImageID image_id,
    int src_x,
    int src_y,
    unsigned int width,
    unsigned int height,
    int dest_x,
    int dest_y
)
{
    INIT_DRIVER_DATA;
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    int ret;

    object_image_p obj_image = IMAGE(image_id);
    CHECK_IMAGE(obj_image);

    object_surface_p obj_surface = SURFACE(surface);
    CHECK_SURFACE(obj_surface);

    if (obj_image->image.format.fourcc != VA_FOURCC_NV12) {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "target VAImage fourcc should be NV12 or IYUV\n");
        vaStatus = VA_STATUS_ERROR_OPERATION_FAILED;
        return vaStatus;
    }

    ipvr__VAImageCheckRegion(obj_surface, &obj_image->image, &src_x, &src_y, &dest_x, &dest_y,
                            (int *)&width, (int *)&height);

    ipvr_surface_p ipvr_surface = obj_surface->ipvr_surface;
    unsigned char *surface_data;
    ret = drm_ipvr_gem_bo_map(ipvr_surface->buf, 0, ipvr_surface->buf->size, 1);
    if (ret) {
        return VA_STATUS_ERROR_UNKNOWN;
    }
    surface_data = ipvr_surface->buf->virt;

    object_buffer_p obj_buffer = BUFFER(obj_image->image.buf);
    CHECK_BUFFER(obj_buffer);

    unsigned char *image_data;
    ret = drm_ipvr_gem_bo_map(obj_buffer->ipvr_bo, 0, obj_buffer->ipvr_bo->size, 1);
    if (ret) {
        drm_ipvr_gem_bo_unmap(ipvr_surface->buf);
        return VA_STATUS_ERROR_UNKNOWN;
    }
    image_data = obj_buffer->ipvr_bo->virt;

    image_data += obj_surface->ipvr_surface->buf->buffer_ofs;

    switch (obj_image->image.format.fourcc) {
    case VA_FOURCC_NV12: {
        unsigned char *source_y, *src_uv, *dst_y, *dst_uv;
        unsigned int i;

        /* copy Y plane */
        source_y = image_data + obj_image->image.offsets[0] + src_y * obj_image->image.pitches[0] + src_x;
        dst_y = surface_data + dest_y * ipvr_surface->stride + dest_x;
        for (i = 0; i < height; i++)  {
            memcpy(dst_y, source_y, width);
            source_y += obj_image->image.pitches[0];
            dst_y += ipvr_surface->stride;
        }

        /* copy UV plane */
        src_uv = image_data + obj_image->image.offsets[1] + (src_y / 2) * obj_image->image.pitches[1] + src_x;
        dst_uv = surface_data + ipvr_surface->stride * obj_surface->height + (dest_y / 2) * ipvr_surface->stride + dest_x;
        for (i = 0; i < obj_image->image.height / 2; i++) {
            memcpy(dst_uv, src_uv, width);
            src_uv += obj_image->image.pitches[1];
            dst_uv += ipvr_surface->stride;
        }
        break;
    }
    default:
        break;
    }

    drm_ipvr_gem_bo_unmap(obj_buffer->ipvr_bo);
    drm_ipvr_gem_bo_unmap(ipvr_surface->buf);

    return VA_STATUS_SUCCESS;
}


static void ipvr__VAImageCheckRegion2(
    object_surface_p surface,
    VAImage *image,
    int *src_x,
    int *src_y,
    unsigned int *src_width,
    unsigned int *src_height,
    int *dest_x,
    int *dest_y,
    int *dest_width,
    int *dest_height
)
{
    /* check for image */
    if (*src_x < 0) *src_x = 0;
    if (*src_x > image->width) *src_x = image->width - 1;
    if (*src_y < 0) *src_y = 0;
    if (*src_y > image->height) *src_y = image->height - 1;

    if (((*src_width) + (*src_x)) > image->width) *src_width = image->width - *src_x;
    if (((*src_height) + (*src_y)) > image->height) *src_height = image->height - *src_x;

    /* check for surface */
    if (*dest_x < 0) *dest_x = 0;
    if (*dest_x > surface->width) *dest_x = surface->width - 1;
    if (*dest_y < 0) *dest_y = 0;
    if (*dest_y > surface->height) *dest_y = surface->height - 1;

    if (((*dest_width) + (*dest_x)) > (int)surface->width) *dest_width = surface->width - *dest_x;
    if (((*dest_height) + (*dest_y)) > (int)surface->height) *dest_height = surface->height - *dest_x;
}

VAStatus ipvr_PutImage(
    VADriverContextP ctx,
    VASurfaceID surface,
    VAImageID image_id,
    int src_x,
    int src_y,
    unsigned int src_width,
    unsigned int src_height,
    int dest_x,
    int dest_y,
    unsigned int dest_width,
    unsigned int dest_height
)
{
    INIT_DRIVER_DATA;
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    int ret;

    if ((src_width == dest_width) && (src_height == dest_height)) {
        /* Shortcut if scaling is not required */
        return ipvr_PutImage2(ctx, surface, image_id, src_x, src_y, src_width, src_height, dest_x, dest_y);
    }

    object_image_p obj_image = IMAGE(image_id);
    CHECK_IMAGE(obj_image);

    if (obj_image->image.format.fourcc != VA_FOURCC_NV12) {
        /* only support NV12 getImage/putImage */
        vaStatus = VA_STATUS_ERROR_OPERATION_FAILED;
        return vaStatus;
    }

    object_surface_p obj_surface = SURFACE(surface);
    CHECK_SURFACE(obj_surface);

    ipvr__VAImageCheckRegion2(obj_surface, &obj_image->image,
                             &src_x, &src_y, &src_width, &src_height,
                             &dest_x, &dest_y, (int *)&dest_width, (int *)&dest_height);

    ipvr_surface_p ipvr_surface = obj_surface->ipvr_surface;
    unsigned char *surface_data;
    ret = drm_ipvr_gem_bo_map(ipvr_surface->buf, 0, ipvr_surface->buf->size, 1);
    if (ret) {
        return VA_STATUS_ERROR_UNKNOWN;
    }
    surface_data = ipvr_surface->buf->virt;

    object_buffer_p obj_buffer = BUFFER(obj_image->image.buf);
    CHECK_BUFFER(obj_buffer);

    unsigned char *image_data;
    ret = drm_ipvr_gem_bo_map(obj_buffer->ipvr_bo, 0, obj_buffer->ipvr_bo->size, 1);
    if (ret) {
        drm_ipvr_gem_bo_unmap(ipvr_surface->buf);
        return VA_STATUS_ERROR_UNKNOWN;
    }
    image_data = obj_buffer->ipvr_bo->virt;

    /* just a prototype, the algorithm is ugly and not optimized */
    switch (obj_image->image.format.fourcc) {
    case VA_FOURCC_NV12: {
        unsigned char *source_y, *dst_y;
        unsigned short *source_uv, *dst_uv;
        unsigned int i, j;
        float xratio = (float) src_width / dest_width;
        float yratio = (float) src_height / dest_height;

        /* dst_y/dst_uv: Y/UV plane of destination */
        dst_y = (unsigned char *)(surface_data + dest_y * ipvr_surface->stride + dest_x);
        dst_uv = (unsigned short *)(surface_data + ipvr_surface->stride * obj_surface->height
                                    + (dest_y / 2) * ipvr_surface->stride + dest_x);

        for (j = 0; j < dest_height; j++)  {
            unsigned char *dst_y_tmp = dst_y;
            unsigned short *dst_uv_tmp = dst_uv;

            for (i = 0; i < dest_width; i++)  {
                int x = (int)(i * xratio);
                int y = (int)(j * yratio);

                source_y = image_data + obj_image->image.offsets[0]
                           + (src_y + y) * obj_image->image.pitches[0]
                           + (src_x + x);
                *dst_y_tmp = *source_y;
                dst_y_tmp++;

                if (((i & 1) == 0)) {
                    source_uv = (unsigned short *)(image_data + obj_image->image.offsets[1]
                                                   + ((src_y + y) / 2) * obj_image->image.pitches[1])
                                + ((src_x + x) / 2);
                    *dst_uv_tmp = *source_uv;
                    dst_uv_tmp++;
                }
            }
            dst_y += ipvr_surface->stride;

            if (j & 1)
                dst_uv = (unsigned short *)((unsigned char *)dst_uv + ipvr_surface->stride);
        }
        break;
    }
    default:/* will not reach here */
        break;
    }

    drm_ipvr_gem_bo_unmap(obj_buffer->ipvr_bo);
    drm_ipvr_gem_bo_unmap(ipvr_surface->buf);

    return VA_STATUS_SUCCESS;
}

/*
 * Link supbicture into one surface, when update is zero, not need to
 * update the location information
 * The image informatio and its BO of subpicture will copied to surface
 * so need to update it when a vaSetSubpictureImage is called
 */
static VAStatus ipvr__LinkSubpictIntoSurface(
    ipvr_driver_data_p driver_data,
    object_surface_p obj_surface,
    object_subpic_p obj_subpic,
    short src_x,
    short src_y,
    unsigned short src_w,
    unsigned short src_h,
    short dest_x,
    short dest_y,
    unsigned short dest_w,
    unsigned short dest_h,
    int update /* update subpicture location */
)
{
    IpvrVASurfaceRec *surface_subpic;
    object_image_p obj_image = IMAGE(obj_subpic->image_id);
    if (NULL == obj_image) {
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }

    VAImage *image = &obj_image->image;
    object_buffer_p obj_buffer = BUFFER(image->buf);
    if (NULL == obj_buffer) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    int found = 0;

    if (obj_surface->subpictures != NULL) {
        surface_subpic = (IpvrVASurfaceRec *)obj_surface->subpictures;
        do {
            if (surface_subpic->subpic_id == obj_subpic->subpic_id) {
                found = 1;
                break;
            } else
                surface_subpic = surface_subpic->next;
        } while (surface_subpic);
    }

    if (found == 0) { /* new node */
        if (obj_surface->subpic_count >= IPVR_SUBPIC_MAX_NUM) {
            drv_debug_msg(VIDEO_DEBUG_ERROR, "can't support so many sub-pictures for the surface\n");
            return VA_STATUS_ERROR_UNKNOWN;
        }

        surface_subpic = (IpvrVASurfaceRec *)calloc(1, sizeof(*surface_subpic));
        if (NULL == surface_subpic)
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    surface_subpic->subpic_id = obj_subpic->subpic_id;
    surface_subpic->fourcc = image->format.fourcc;
    surface_subpic->size = image->data_size;
    surface_subpic->bo = obj_buffer->ipvr_bo;
    drm_ipvr_gem_bo_flink(obj_buffer->ipvr_bo, &surface_subpic->bufid);
    //surface_subpic->pl_flags = obj_buffer->ipvr_buffer->pl_flags;
    surface_subpic->subpic_flags = obj_subpic->flags;

    surface_subpic->width = image->width;
    surface_subpic->height = image->height;
    switch (surface_subpic->fourcc) {
    case VA_FOURCC_AYUV:
        surface_subpic->stride = image->pitches[0] / 4;
        break;
    case VA_FOURCC_RGBA:
        surface_subpic->stride = image->pitches[0] / 4;
        break;
    case VA_FOURCC_AI44:
        surface_subpic->stride = image->pitches[0];
        /* point to Image palette */
        surface_subpic->palette_ptr = (IpvrAYUVSample8 *) & obj_image->palette[0];
        break;
    }

    if (update) {
        surface_subpic->subpic_srcx = src_x;
        surface_subpic->subpic_srcy = src_y;
        surface_subpic->subpic_dstx = dest_x;
        surface_subpic->subpic_dsty = dest_y;
        surface_subpic->subpic_srcw = src_w;
        surface_subpic->subpic_srch = src_h;
        surface_subpic->subpic_dstw = dest_w;
        surface_subpic->subpic_dsth = dest_h;
    }

    if (found == 0) { /* new node, link into the list */
        if (NULL == obj_surface->subpictures) {
            obj_surface->subpictures = (void *)surface_subpic;
        } else { /* insert as the head */
            surface_subpic->next = (IpvrVASurfacePtr)obj_surface->subpictures;
            obj_surface->subpictures = (void *)surface_subpic;
        }
        obj_surface->subpic_count++;
    }

    return VA_STATUS_SUCCESS;
}


static VAStatus ipvr__LinkSurfaceIntoSubpict(
    object_subpic_p obj_subpic,
    VASurfaceID surface_id
)
{
    subpic_surface_s *subpic_surface;

    if (obj_subpic->surfaces != NULL) {
        subpic_surface = (subpic_surface_s *)obj_subpic->surfaces;
        do  {
            if (subpic_surface->surface_id == surface_id) {
                return VA_STATUS_SUCCESS; /* reture directly */
            } else
                subpic_surface = subpic_surface->next;
        } while (subpic_surface);
    }

    /* not found */
    subpic_surface = (subpic_surface_s *)calloc(1, sizeof(*subpic_surface));
    if (NULL == subpic_surface)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    subpic_surface->surface_id = surface_id;
    subpic_surface->next = NULL;

    if (NULL == obj_subpic->surfaces) {
        obj_subpic->surfaces = (void *)subpic_surface;
    } else { /* insert as the head */
        subpic_surface->next = (subpic_surface_p)obj_subpic->surfaces;
        obj_subpic->surfaces = (void *)subpic_surface;
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus ipvr__DelinkSubpictFromSurface(
    object_surface_p obj_surface,
    VASubpictureID subpic_id
)
{
    IpvrVASurfaceRec *surface_subpic, *pre_surface_subpic = NULL;
    int found = 0;

    if (obj_surface->subpictures != NULL) {
        surface_subpic = (IpvrVASurfaceRec *)obj_surface->subpictures;
        do  {
            if (surface_subpic->subpic_id == subpic_id) {
                found = 1;
                break;
            } else {
                pre_surface_subpic = surface_subpic;
                surface_subpic = surface_subpic->next;
            }
        } while (surface_subpic);
    }

    if (found == 1) {
        if (pre_surface_subpic == NULL) { /* remove the first node */
            obj_surface->subpictures = (void *)surface_subpic->next;
        } else {
            pre_surface_subpic->next = surface_subpic->next;
        }
        free(surface_subpic);
        obj_surface->subpic_count--;
    }

    return VA_STATUS_SUCCESS;
}


static VAStatus ipvr__DelinkSurfaceFromSubpict(
    object_subpic_p obj_subpic,
    VASurfaceID surface_id
)
{
    subpic_surface_s *subpic_surface, *pre_subpic_surface = NULL;
    int found = 0;

    if (obj_subpic->surfaces != NULL) {
        subpic_surface = (subpic_surface_s *)obj_subpic->surfaces;
        do {
            if (subpic_surface->surface_id == surface_id) {
                found = 1;
                break;
            } else {
                pre_subpic_surface = subpic_surface;
                subpic_surface = subpic_surface->next;
            }
        } while (subpic_surface);
    }

    if (found == 1) {
        if (pre_subpic_surface == NULL) { /* remove the first node */
            obj_subpic->surfaces = (void *)subpic_surface->next;
        } else {
            pre_subpic_surface->next = subpic_surface->next;
        }
        free(subpic_surface);
    }

    return VA_STATUS_SUCCESS;
}


VAStatus ipvr_QuerySubpictureFormats(
    VADriverContextP ctx,
    VAImageFormat *format_list,        /* out */
    unsigned int *flags,       /* out */
    unsigned int *num_formats  /* out */
)
{
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    CHECK_INVALID_PARAM(format_list == NULL);
    CHECK_INVALID_PARAM(flags == NULL);
    CHECK_INVALID_PARAM(num_formats == NULL);

    memcpy(format_list, ipvr__SubpicFormat, sizeof(ipvr__SubpicFormat));
    *num_formats = IPVR_MAX_SUBPIC_FORMATS;
    *flags = IPVR_SUPPORTED_SUBPIC_FLAGS;

    return VA_STATUS_SUCCESS;
}


VAStatus ipvr_CreateSubpicture(
    VADriverContextP ctx,
    VAImageID image,
    VASubpictureID *subpicture   /* out */
)
{
    INIT_DRIVER_DATA;
    VASubpictureID subpicID;
    object_subpic_p obj_subpic;
    object_image_p obj_image;
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    VAImageFormat *img_fmt;

    obj_image = IMAGE(image);
    CHECK_IMAGE(obj_image);
    CHECK_SUBPICTURE(subpicture);

    img_fmt = ipvr__VAImageCheckFourCC(&obj_image->image.format, ipvr__SubpicFormat,
                                      sizeof(ipvr__SubpicFormat) / sizeof(VAImageFormat));
    if (img_fmt == NULL)
        return VA_STATUS_ERROR_UNKNOWN;

    subpicID = object_heap_allocate(&driver_data->subpic_heap);
    obj_subpic = SUBPIC(subpicID);
    CHECK_ALLOCATION(obj_subpic);

    MEMSET_OBJECT(obj_subpic, struct object_subpic_s);

    obj_subpic->subpic_id = subpicID;
    obj_subpic->image_id = obj_image->image.image_id;
    obj_subpic->surfaces = NULL;
    obj_subpic->global_alpha = 255;

    obj_image->subpic_ref ++;

    *subpicture = subpicID;

    return VA_STATUS_SUCCESS;
}



VAStatus ipvr__destroy_subpicture(ipvr_driver_data_p driver_data, object_subpic_p obj_subpic)
{
    subpic_surface_s *subpic_surface = (subpic_surface_s *)obj_subpic->surfaces;
    VASubpictureID subpicture = obj_subpic->subpic_id;

    if (subpic_surface) {
        do {
            subpic_surface_s *tmp = subpic_surface;
            object_surface_p obj_surface = SURFACE(subpic_surface->surface_id);

            if (obj_surface) { /* remove subpict from surface */
                ipvr__DelinkSubpictFromSurface(obj_surface, subpicture);
            }
            subpic_surface = subpic_surface->next;
            free(tmp);
        } while (subpic_surface);
    }

    object_heap_free(&driver_data->subpic_heap, (object_base_p) obj_subpic);
    return VA_STATUS_SUCCESS;
}


VAStatus ipvr_DestroySubpicture(
    VADriverContextP ctx,
    VASubpictureID subpicture
)
{
    INIT_DRIVER_DATA;
    object_subpic_p obj_subpic;
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    obj_subpic = SUBPIC(subpicture);
    CHECK_SUBPICTURE(obj_subpic);

    return ipvr__destroy_subpicture(driver_data, obj_subpic);
}

VAStatus ipvr_SetSubpictureImage(
    VADriverContextP ctx,
    VASubpictureID subpicture,
    VAImageID image
)
{
    INIT_DRIVER_DATA;
    object_subpic_p obj_subpic;
    object_image_p obj_image;
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    subpic_surface_s *subpic_surface;
    VAImageFormat *img_fmt;

    obj_image = IMAGE(image);
    CHECK_IMAGE(obj_image);

    img_fmt = ipvr__VAImageCheckFourCC(&obj_image->image.format,
                                      ipvr__SubpicFormat,
                                      sizeof(ipvr__SubpicFormat) / sizeof(VAImageFormat));
    CHECK_IMAGE(img_fmt);

    obj_subpic = SUBPIC(subpicture);
    CHECK_SUBPICTURE(obj_subpic);

    object_image_p old_obj_image = IMAGE(obj_subpic->image_id);
    if (old_obj_image) {
        old_obj_image->subpic_ref--;/* decrease reference count */
    }

    /* reset the image */
    obj_subpic->image_id = obj_image->image.image_id;
    obj_image->subpic_ref ++;

    /* relink again */
    if (obj_subpic->surfaces != NULL) {
        /* the subpicture already linked into surfaces
         * so not check the return value of ipvr__LinkSubpictIntoSurface
         */
        subpic_surface = (subpic_surface_s *)obj_subpic->surfaces;
        do {
            object_surface_p obj_surface = SURFACE(subpic_surface->surface_id);
            CHECK_SURFACE(obj_surface);

            ipvr__LinkSubpictIntoSurface(driver_data, obj_surface, obj_subpic,
                                        0, 0, 0, 0, 0, 0, 0, 0,
                                        0 /* not update location */
                                       );
            subpic_surface = subpic_surface->next;
        } while (subpic_surface);
    }


    return VA_STATUS_SUCCESS;
}


VAStatus ipvr_SetSubpictureChromakey(
    VADriverContextP ctx,
    VASubpictureID subpicture,
    unsigned int chromakey_min,
    unsigned int chromakey_max,
    unsigned int chromakey_mask
)
{
    INIT_DRIVER_DATA;
    (void)driver_data;
    /* TODO */
    if ((chromakey_mask < chromakey_min) || (chromakey_mask > chromakey_max)) {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "Invalid chromakey value %d, chromakey value should between min and max\n", chromakey_mask);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    object_subpic_p obj_subpic = SUBPIC(subpicture);
    if (NULL == obj_subpic) {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "Invalid subpicture value %d\n", subpicture);
        return VA_STATUS_ERROR_INVALID_SUBPICTURE;
    }

    return VA_STATUS_SUCCESS;
}

VAStatus ipvr_SetSubpictureGlobalAlpha(
    VADriverContextP ctx,
    VASubpictureID subpicture,
    float global_alpha
)
{
    INIT_DRIVER_DATA;

    if (global_alpha < 0 || global_alpha > 1) {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "Invalid global alpha value %07f, global alpha value should between 0 and 1\n", global_alpha);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    object_subpic_p obj_subpic = SUBPIC(subpicture);
    if (NULL == obj_subpic) {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "Invalid subpicture value %d\n", subpicture);
        return VA_STATUS_ERROR_INVALID_SUBPICTURE;
    }

    obj_subpic->global_alpha = global_alpha * 255;

    return VA_STATUS_SUCCESS;
}


VAStatus ipvr__AssociateSubpicture(
    VADriverContextP ctx,
    VASubpictureID subpicture,
    VASurfaceID *target_surfaces,
    int num_surfaces,
    short src_x, /* upper left offset in subpicture */
    short src_y,
    unsigned short src_w,
    unsigned short src_h,
    short dest_x, /* upper left offset in surface */
    short dest_y,
    unsigned short dest_w,
    unsigned short dest_h,
    /*
     * whether to enable chroma-keying or global-alpha
     * see VA_SUBPICTURE_XXX values
     */
    unsigned int flags
)
{
    INIT_DRIVER_DATA;

    object_subpic_p obj_subpic;
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    int i;

    CHECK_INVALID_PARAM(num_surfaces <= 0);

    obj_subpic = SUBPIC(subpicture);
    CHECK_SUBPICTURE(obj_subpic);
    CHECK_SURFACE(target_surfaces);

    if (flags & ~IPVR_SUPPORTED_SUBPIC_FLAGS) {
#ifdef VA_STATUS_ERROR_FLAG_NOT_SUPPORTED
        vaStatus = VA_STATUS_ERROR_FLAG_NOT_SUPPORTED;
#else
        vaStatus = VA_STATUS_ERROR_UNKNOWN;
#endif
        DEBUG_FAILURE;
        return vaStatus;
    } else {

        /* If flags are ok, copy them to the subpicture object */
        obj_subpic->flags = flags;

    }

    /* Validate input params */
    for (i = 0; i < num_surfaces; i++) {
        object_surface_p obj_surface = SURFACE(target_surfaces[i]);
        CHECK_SURFACE(obj_surface);
    }

    VASurfaceID *surfaces = target_surfaces;
    for (i = 0; i < num_surfaces; i++) {
        object_surface_p obj_surface = SURFACE(*surfaces);
        if (obj_surface) {
            vaStatus = ipvr__LinkSubpictIntoSurface(driver_data, obj_surface, obj_subpic,
                                                   src_x, src_y, src_w, src_h,
                                                   dest_x, dest_y, dest_w, dest_h, 1);
            if (VA_STATUS_SUCCESS == vaStatus) {
                vaStatus = ipvr__LinkSurfaceIntoSubpict(obj_subpic, *surfaces);
            }
            CHECK_VASTATUS();/* failed with malloc */
        } else {
            /* Should never get here */
            drv_debug_msg(VIDEO_DEBUG_ERROR, "Invalid surfaces,SurfaceID=0x%x\n", *surfaces);
        }

        surfaces++;
    }

    return VA_STATUS_SUCCESS;
}


VAStatus ipvr_AssociateSubpicture(
    VADriverContextP ctx,
    VASubpictureID subpicture,
    VASurfaceID *target_surfaces,
    int num_surfaces,
    short src_x, /* upper left offset in subpicture */
    short src_y,
    unsigned short src_width,
    unsigned short src_height,
    short dest_x, /* upper left offset in surface */
    short dest_y,
    unsigned short dest_width,
    unsigned short dest_height,
    /*
     * whether to enable chroma-keying or global-alpha
     * see VA_SUBPICTURE_XXX values
     */
    unsigned int flags
)
{
    return ipvr__AssociateSubpicture(ctx, subpicture, target_surfaces, num_surfaces,
                                    src_x, src_y, src_width, src_height,
                                    dest_x, dest_y, dest_width, dest_height,
                                    flags
                                   );
}


VAStatus ipvr_DeassociateSubpicture(
    VADriverContextP ctx,
    VASubpictureID subpicture,
    VASurfaceID *target_surfaces,
    int num_surfaces
)
{
    INIT_DRIVER_DATA;

    object_subpic_p obj_subpic;
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_image_p obj_image;
    int i;

    CHECK_INVALID_PARAM(num_surfaces <= 0);

    obj_subpic = SUBPIC(subpicture);
    CHECK_SUBPICTURE(obj_subpic);
    CHECK_SURFACE(target_surfaces);

    VASurfaceID *surfaces = target_surfaces;
    for (i = 0; i < num_surfaces; i++) {
        object_surface_p obj_surface = SURFACE(*surfaces);

        if (obj_surface) {
            ipvr__DelinkSubpictFromSurface(obj_surface, subpicture);
            ipvr__DelinkSurfaceFromSubpict(obj_subpic, obj_surface->surface_id);
        } else {
            drv_debug_msg(VIDEO_DEBUG_ERROR, "vaDeassociateSubpicture: Invalid surface, VASurfaceID=0x%08x\n", *surfaces);
        }

        surfaces++;
    }

    obj_image = IMAGE(obj_subpic->image_id);
    if (obj_image)
        obj_image->subpic_ref--;/* decrease reference count */

    return VA_STATUS_SUCCESS;
}


void ipvr_SurfaceDeassociateSubpict(
    ipvr_driver_data_p driver_data,
    object_surface_p obj_surface
)
{
    IpvrVASurfaceRec *surface_subpic = (IpvrVASurfaceRec *)obj_surface->subpictures;

    if (surface_subpic != NULL) {
        do  {
            IpvrVASurfaceRec *tmp = surface_subpic;
            object_subpic_p obj_subpic = SUBPIC(surface_subpic->subpic_id);
            if (obj_subpic)
                ipvr__DelinkSurfaceFromSubpict(obj_subpic, obj_surface->surface_id);
            surface_subpic = surface_subpic->next;
            free(tmp);
        } while (surface_subpic);
    }
}


static  VADisplayAttribute ipvr__DisplayAttribute[] = {
    {
        VADisplayAttribBrightness,
        BRIGHTNESS_MIN,
        BRIGHTNESS_MAX,
        BRIGHTNESS_DEFAULT_VALUE,
        VA_DISPLAY_ATTRIB_GETTABLE | VA_DISPLAY_ATTRIB_SETTABLE
    },

    {
        VADisplayAttribContrast,
        CONTRAST_MIN,
        CONTRAST_MAX,
        CONTRAST_DEFAULT_VALUE,
        VA_DISPLAY_ATTRIB_GETTABLE | VA_DISPLAY_ATTRIB_SETTABLE
    },

    {
        VADisplayAttribHue,
        HUE_MIN,
        HUE_MAX,
        HUE_DEFAULT_VALUE,
        VA_DISPLAY_ATTRIB_GETTABLE | VA_DISPLAY_ATTRIB_SETTABLE
    },

    {
        VADisplayAttribSaturation,
        SATURATION_MIN,
        SATURATION_MAX,
        SATURATION_DEFAULT_VALUE,
        VA_DISPLAY_ATTRIB_GETTABLE | VA_DISPLAY_ATTRIB_SETTABLE
    },
    {
        VADisplayAttribBackgroundColor,
        0x00000000,
        0xffffffff,
        0x00000000,
        VA_DISPLAY_ATTRIB_GETTABLE | VA_DISPLAY_ATTRIB_SETTABLE
    },
    {
        VADisplayAttribRotation,
        VA_ROTATION_NONE,
        VA_ROTATION_270,
        VA_ROTATION_NONE,
        VA_DISPLAY_ATTRIB_GETTABLE | VA_DISPLAY_ATTRIB_SETTABLE
    },
    {
        VADisplayAttribOutofLoopDeblock,
        VA_OOL_DEBLOCKING_FALSE,
        VA_OOL_DEBLOCKING_TRUE,
        VA_OOL_DEBLOCKING_FALSE,
        VA_DISPLAY_ATTRIB_GETTABLE | VA_DISPLAY_ATTRIB_SETTABLE
    },
    {
        VADisplayAttribBlendColor,
        0x00000000,
        0xffffffff,
        0x00000000,
        VA_DISPLAY_ATTRIB_GETTABLE | VA_DISPLAY_ATTRIB_SETTABLE
    },
    {
        VADisplayAttribOverlayColorKey,
        0x00000000,
        0xffffffff,
        0x00000000,
        VA_DISPLAY_ATTRIB_GETTABLE | VA_DISPLAY_ATTRIB_SETTABLE
    },
    {
        VADisplayAttribOverlayAutoPaintColorKey,
        0x00000000,
        0xffffffff,
        0x00000000,
        VA_DISPLAY_ATTRIB_GETTABLE | VA_DISPLAY_ATTRIB_SETTABLE
    },
    {
        VADisplayAttribCSCMatrix,
        0x00000000,
        0xffffffff,
        0x00000000,
        VA_DISPLAY_ATTRIB_GETTABLE | VA_DISPLAY_ATTRIB_SETTABLE
    },
    {
        VADisplayAttribRenderDevice,
        0x00000000,
        0xffffffff,
        0x00000000,
        VA_DISPLAY_ATTRIB_GETTABLE | VA_DISPLAY_ATTRIB_SETTABLE
    },
    {
        VADisplayAttribRenderMode,
        0x00000000,
        0xffffffff,
        0x00000000,
        VA_DISPLAY_ATTRIB_GETTABLE | VA_DISPLAY_ATTRIB_SETTABLE
    },
    {
        VADisplayAttribRenderRect,
        0x00000000,
        0xffffffff,
        0x00000000,
        VA_DISPLAY_ATTRIB_GETTABLE | VA_DISPLAY_ATTRIB_SETTABLE
    }
};

/*
 * Query display attributes
 * The caller must provide a "attr_list" array that can hold at
 * least vaMaxNumDisplayAttributes() entries. The actual number of attributes
 * returned in "attr_list" is returned in "num_attributes".
 */
VAStatus ipvr_QueryDisplayAttributes(
    VADriverContextP ctx,
    VADisplayAttribute *attr_list,      /* out */
    int *num_attributes         /* out */
)
{
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    CHECK_INVALID_PARAM(attr_list == NULL);
    CHECK_INVALID_PARAM(num_attributes == NULL);

    *num_attributes = min(*num_attributes, IPVR_MAX_DISPLAY_ATTRIBUTES);
    memcpy(attr_list, ipvr__DisplayAttribute, (*num_attributes)*sizeof(VADisplayAttribute));
    return VA_STATUS_SUCCESS;
}

/*
 * Get display attributes
 * This function returns the current attribute values in "attr_list".
 * Only attributes returned with VA_DISPLAY_ATTRIB_GETTABLE set in the "flags" field
 * from vaQueryDisplayAttributes() can have their values retrieved.
 */
VAStatus ipvr_GetDisplayAttributes(
    VADriverContextP ctx,
    VADisplayAttribute *attr_list,      /* in/out */
    int num_attributes
)
{
    INIT_DRIVER_DATA;
    VADisplayAttribute *p = attr_list;
    int i;
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    CHECK_INVALID_PARAM(attr_list == NULL);
    CHECK_INVALID_PARAM(num_attributes <= 0);

    for (i = 0; i < num_attributes; i++) {
        switch (p->type) {
        case VADisplayAttribBrightness:
            /* -50*(1<<10) ~ 50*(1<<10) ==> 0~100*/
            p->value = (driver_data->brightness.value / (1 << 10)) + 50;
            p->min_value = 0;
            p->max_value = 100;
            break;
        case VADisplayAttribContrast:
            /* 0 ~ 2*(1<<25) ==> 0~100 */
            p->value = (driver_data->contrast.value / (1 << 25)) * 50;
            p->min_value = 0;
            p->max_value = 100;
            break;
        case VADisplayAttribHue:
            /* -30*(1<<25) ~ 30*(1<<25) ==> 0~100*/
            p->value = ((driver_data->hue.value / (1 << 25)) + 30) * 10 / 6;
            p->min_value = 0;
            p->max_value = 100;
            break;
        case VADisplayAttribSaturation:
            /* 0 ~ 2*(1<<25) ==> 0~100 */
            p->value = (driver_data->saturation.value / (1 << 25)) * 50;
            p->min_value = 0;
            p->max_value = 100;
            break;
        case VADisplayAttribBackgroundColor:
            p->value = driver_data->clear_color;
            break;
        case VADisplayAttribBlendColor:
            p->value = driver_data->blend_color;
            break;
        case VADisplayAttribOverlayColorKey:
            p->value = driver_data->color_key;
            p->min_value = 0;
            p->max_value = 0xFFFFFF;
            break;
        case VADisplayAttribOverlayAutoPaintColorKey:
            p->value = driver_data->overlay_auto_paint_color_key;
            p->min_value = 0;
            p->max_value = 1;
            break;
        case VADisplayAttribRotation:
            p->value = driver_data->va_rotate = p->value;
            p->min_value = VA_ROTATION_NONE;
            p->max_value = VA_ROTATION_270;
            break;
        case VADisplayAttribOutofLoopDeblock:
            p->value = driver_data->is_oold = p->value;
            p->min_value = VA_OOL_DEBLOCKING_FALSE;
            p->max_value = VA_OOL_DEBLOCKING_TRUE;
            break;
        case VADisplayAttribCSCMatrix:
            p->value = driver_data->load_csc_matrix = p->value;
            p->min_value = 0;
            p->max_value = 255;
            break;
        case VADisplayAttribRenderDevice:
            p->value = driver_data->render_device = p->value;
            p->min_value = 0;
            p->max_value = 255;
            break;
        case VADisplayAttribRenderMode:
            p->value = driver_data->render_mode = p->value;
            p->min_value = 0;
            p->max_value = 255;
            break;
        case VADisplayAttribRenderRect:
            ((VARectangle *)(unsigned long)(p->value))->x = driver_data->render_rect.x = ((VARectangle *)(unsigned long)(p->value))->x;
            ((VARectangle *)(unsigned long)(p->value))->y = driver_data->render_rect.y = ((VARectangle *)(unsigned long)(p->value))->y;
            ((VARectangle *)(unsigned long)(p->value))->width = driver_data->render_rect.width = ((VARectangle *)(unsigned long)(p->value))->width;
            ((VARectangle *)(unsigned long)(p->value))->height = driver_data->render_rect.height = ((VARectangle *)(unsigned long)(p->value))->height;
            p->min_value = 0;
            p->max_value = 255;
            break;

        default:
            break;
        }
        p++;
    }

    return VA_STATUS_SUCCESS;
}

/*
 * Set display attributes
 * Only attributes returned with VA_DISPLAY_ATTRIB_SETTABLE set in the "flags" field
 * from vaQueryDisplayAttributes() can be set.  If the attribute is not settable or
 * the value is out of range, the function returns VA_STATUS_ERROR_ATTR_NOT_SUPPORTED
 */
#define CLAMP_ATTR(a,max,min) (a>max?max:(a<min?min:a))
VAStatus ipvr_SetDisplayAttributes(
    VADriverContextP ctx,
    VADisplayAttribute *attr_list,
    int num_attributes
)
{
    return VA_STATUS_SUCCESS;
}

