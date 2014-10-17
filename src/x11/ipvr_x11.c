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
 *    Sean V Kelley <sean.v.kelley@intel.com>
 *    Yao Cheng <yao.cheng@intel.com>
 *
 */

#include <va/va_dricommon.h>
#include <va/va_backend.h>
#include <X11/Xutil.h>
#include "ipvr_output.h"
#include "ipvr_surface.h"
#include "ipvr_drv_debug.h"
#include "ipvr_drv_video.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <ipvr_bufmgr.h>

#define INIT_DRIVER_DATA    ipvr_driver_data_p driver_data = (ipvr_driver_data_p) ctx->pDriverData;
#define SURFACE(id)    ((object_surface_p) object_heap_lookup( &driver_data->surface_heap, id ))

static uint32_t mask2shift(uint32_t mask)
{
    uint32_t shift = 0;
    while ((mask & 0x1) == 0) {
        mask = mask >> 1;
        shift++;
    }
    return shift;
}

VAStatus ipvr_PutSurface(
    VADriverContextP ctx,
    VASurfaceID surface,
    void *drawable, /* X Drawable */
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
    INIT_DRIVER_DATA;
    GC gc;
    XImage *ximg = NULL;
    Visual *visual;
    unsigned short width, height;
    int depth;
    int x = 0, y = 0;
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    uint8_t  *surface_data = NULL;
    ipvr_surface_p ipvr_surface;
    int ret;

    uint32_t rmask = 0;
    uint32_t gmask = 0;
    uint32_t bmask = 0;

    uint32_t rshift = 0;
    uint32_t gshift = 0;
    uint32_t bshift = 0;

    object_surface_p obj_surface;
    Drawable draw = (Drawable)drawable;
    obj_surface = SURFACE(surface);

    if (!obj_surface) {
        vaStatus = VA_STATUS_ERROR_INVALID_SURFACE;
        DEBUG_FAILURE;
        return vaStatus;
    }

    if (srcw <= destw)
        width = srcw;
    else
        width = destw;

    if (srch <= desth)
        height = srch;
    else
        height = desth;

    ipvr_surface = obj_surface->ipvr_surface;

    drv_debug_msg(VIDEO_DEBUG_GENERAL, "PutSurface: src   w x h = %d x %d\n", srcw, srch);
    drv_debug_msg(VIDEO_DEBUG_GENERAL, "PutSurface: dest      w x h = %d x %d\n", destw, desth);
    drv_debug_msg(VIDEO_DEBUG_GENERAL, "PutSurface: clipped w x h = %d x %d\n", width, height);

    visual = DefaultVisual((Display *)ctx->native_dpy, ctx->x11_screen);
    gc = XCreateGC((Display *)ctx->native_dpy, draw, 0, NULL);
    depth = DefaultDepth((Display *)ctx->native_dpy, ctx->x11_screen);

    if (TrueColor != visual->class) {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "PutSurface: Default visual of X display must be TrueColor.\n");
        vaStatus = VA_STATUS_ERROR_UNKNOWN;
        goto out;
    }

    ret = drm_ipvr_gem_bo_map(ipvr_surface->buf, 0, ipvr_surface->size, 0);
    if (ret) {
        vaStatus = VA_STATUS_ERROR_UNKNOWN;
        goto out;
    }

    surface_data = ipvr_surface->buf->virt;
    rmask = visual->red_mask;
    gmask = visual->green_mask;
    bmask = visual->blue_mask;

    rshift = mask2shift(rmask);
    gshift = mask2shift(gmask);
    bshift = mask2shift(bmask);

    drv_debug_msg(VIDEO_DEBUG_GENERAL, "PutSurface: Pixel masks: R = %08x G = %08x B = %08x\n", rmask, gmask, bmask);
    drv_debug_msg(VIDEO_DEBUG_GENERAL, "PutSurface: Pixel shifts: R = %d G = %d B = %d\n", rshift, gshift, bshift);
    ximg = XCreateImage((Display *)ctx->native_dpy, visual, depth, ZPixmap, 0, NULL, width, height, 32, 0);

    if (ximg->byte_order == MSBFirst)
        drv_debug_msg(VIDEO_DEBUG_GENERAL, "PutSurface: XImage pixels has MSBFirst, %d bits / pixel\n", ximg->bits_per_pixel);
    else
        drv_debug_msg(VIDEO_DEBUG_GENERAL, "PutSurface: XImage pixels has LSBFirst, %d bits / pixel\n", ximg->bits_per_pixel);

    if (ximg->bits_per_pixel != 32) {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "PutSurface: Display uses %d bits/pixel which is not supported\n");
        vaStatus = VA_STATUS_ERROR_UNKNOWN;
        goto out;
    }

    void yuv2pixel(uint32_t * pixel, int y, int u, int v) {
        int r, g, b;
        /* Warning, magic values ahead */
        r = y + ((351 * (v - 128)) >> 8);
        g = y - (((179 * (v - 128)) + (86 * (u - 128))) >> 8);
        b = y + ((444 * (u - 128)) >> 8);

        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        if (r < 0) r = 0;
        if (g < 0) g = 0;
        if (b < 0) b = 0;

        *pixel = ((r << rshift) & rmask) | ((g << gshift) & gmask) | ((b << bshift) & bmask);
    }
    ximg->data = (char *) malloc(ximg->bytes_per_line * height);
    if (NULL == ximg->data) {
        vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
        goto out;
    }

    uint8_t *src_y = surface_data + ipvr_surface->stride * srcy;
    uint8_t *src_uv = surface_data + ipvr_surface->stride * (obj_surface->height + srcy / 2);

    for (y = srcy; y < (srcy + height); y += 2) {
        uint32_t *dest_even = (uint32_t *)(ximg->data + y * ximg->bytes_per_line);
        uint32_t *dest_odd = (uint32_t *)(ximg->data + (y + 1) * ximg->bytes_per_line);
        for (x = srcx; x < (srcx + width); x += 2) {
            /* Y1 Y2 */
            /* Y3 Y4 */
            int y1 = *(src_y + x);
            int y2 = *(src_y + x + 1);
            int y3 = *(src_y + x + ipvr_surface->stride);
            int y4 = *(src_y + x + ipvr_surface->stride + 1);

            /* U V */
            int u = *(src_uv + x);
            int v = *(src_uv + x + 1);

            yuv2pixel(dest_even++, y1, u, v);
            yuv2pixel(dest_even++, y2, u, v);
            yuv2pixel(dest_odd++, y3, u, v);
            yuv2pixel(dest_odd++, y4, u, v);
        }
        src_y += ipvr_surface->stride * 2;
        src_uv += ipvr_surface->stride;
    }

    XPutImage((Display *)ctx->native_dpy, draw, gc, ximg, 0, 0, destx, desty, width, height);
    XFlush((Display *)ctx->native_dpy);

out:
    if (NULL != ximg)
        XDestroyImage(ximg);
    if (NULL != surface_data) {
        drm_ipvr_gem_bo_unmap(ipvr_surface->buf);
        ipvr_surface->buf->virt = NULL;
    }

    XFreeGC((Display *)ctx->native_dpy, gc);


    return VA_STATUS_SUCCESS;
}
