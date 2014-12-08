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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <va/va_dricommon.h>
#include <va/va_backend.h>
#include "ipvr_output.h"
#include "ipvr_surface.h"
#include "ipvr_drv_debug.h"
#ifdef ANDROID
#include "android/ipvr_android.h"
#endif

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

static VAImageFormat ipvr__CreateImageFormat[] = {
    ipvr__ImageNV12,
};

#ifndef VA_STATUS_ERROR_INVALID_IMAGE_FORMAT
#define VA_STATUS_ERROR_INVALID_IMAGE_FORMAT VA_STATUS_ERROR_UNKNOWN
#endif

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
    return VA_STATUS_ERROR_UNIMPLEMENTED;
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

    fourcc = obj_surface->ipvr_surface->fourcc;
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

    switch (fourcc) {
    case VA_FOURCC_NV12: {
        obj_image->image.num_planes = 2;
        obj_image->image.pitches[0] = obj_surface->ipvr_surface->stride;
        obj_image->image.pitches[1] = obj_surface->ipvr_surface->stride;

        obj_image->image.offsets[0] = 0;
        obj_image->image.offsets[1] = obj_surface->height * obj_surface->ipvr_surface->stride;
        obj_image->image.num_palette_entries = 0;
        obj_image->image.entry_bytes = 0;
        obj_image->image.component_order[0] = 'Y';
        obj_image->image.component_order[1] = 'U';/* fixed me: packed UV packed here! */
        obj_image->image.component_order[2] = 'V';
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
    ret = drm_ipvr_gem_bo_map(ipvr_surface->buf, 1);
    if (ret) {
        return VA_STATUS_ERROR_UNKNOWN;
    }
    surface_data = ipvr_surface->buf->virt;

    object_buffer_p obj_buffer = BUFFER(obj_image->image.buf);
    CHECK_BUFFER(obj_buffer);

    unsigned char *image_data;
    ret = drm_ipvr_gem_bo_map(obj_buffer->ipvr_bo, 1);
    if (ret) {
        drm_ipvr_gem_bo_unmap(ipvr_surface->buf);
        return VA_STATUS_ERROR_UNKNOWN;
    }
    image_data = obj_buffer->ipvr_bo->virt;

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
    ret = drm_ipvr_gem_bo_map(ipvr_surface->buf, 1);
    if (ret) {
        return VA_STATUS_ERROR_UNKNOWN;
    }
    surface_data = ipvr_surface->buf->virt;

    object_buffer_p obj_buffer = BUFFER(obj_image->image.buf);
    CHECK_BUFFER(obj_buffer);

    unsigned char *image_data;
    ret = drm_ipvr_gem_bo_map(obj_buffer->ipvr_bo, 1);
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

VAStatus ipvr_QuerySubpictureFormats(
    VADriverContextP ctx,
    VAImageFormat *format_list,        /* out */
    unsigned int *flags,       /* out */
    unsigned int *num_formats  /* out */
)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}


VAStatus ipvr_CreateSubpicture(
    VADriverContextP ctx,
    VAImageID image,
    VASubpictureID *subpicture   /* out */
)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}


VAStatus ipvr__destroy_subpicture(ipvr_driver_data_p driver_data, object_subpic_p obj_subpic)
{
    subpic_surface_s *subpic_surface = (subpic_surface_s *)obj_subpic->surfaces;

    if (subpic_surface) {
        do {
            subpic_surface_s *tmp = subpic_surface;
            object_surface_p obj_surface = SURFACE(subpic_surface->surface_id);

            if (obj_surface) { /* remove subpict from surface */
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
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus ipvr_SetSubpictureImage(
    VADriverContextP ctx,
    VASubpictureID subpicture,
    VAImageID image
)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}


VAStatus ipvr_SetSubpictureChromakey(
    VADriverContextP ctx,
    VASubpictureID subpicture,
    unsigned int chromakey_min,
    unsigned int chromakey_max,
    unsigned int chromakey_mask
)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus ipvr_SetSubpictureGlobalAlpha(
    VADriverContextP ctx,
    VASubpictureID subpicture,
    float global_alpha
)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
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
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}


VAStatus ipvr_DeassociateSubpicture(
    VADriverContextP ctx,
    VASubpictureID subpicture,
    VASurfaceID *target_surfaces,
    int num_surfaces
)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

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
    return VA_STATUS_ERROR_UNIMPLEMENTED;
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
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

/*
 * Set display attributes
 * Only attributes returned with VA_DISPLAY_ATTRIB_SETTABLE set in the "flags" field
 * from vaQueryDisplayAttributes() can be set.  If the attribute is not settable or
 * the value is out of range, the function returns VA_STATUS_ERROR_ATTR_NOT_SUPPORTED
 */
//#define CLAMP_ATTR(a,max,min) (a>max?max:(a<min?min:a))
VAStatus ipvr_SetDisplayAttributes(
    VADriverContextP ctx,
    VADisplayAttribute *attr_list,
    int num_attributes
)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

