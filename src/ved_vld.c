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
 */

/*
 * Authors:
 *    Li Zeng <li.zeng@intel.com>
 *    Yao Cheng <yao.cheng@intel.com>
 */
#include "ved_vld.h"
#include "ipvr_drv_debug.h"
#include "hwdefs/img_types.h"
#include "hwdefs/dxva_fw_ctrl.h"
#include "hwdefs/reg_io2.h"
#include "hwdefs/msvdx_offsets.h"
#include "hwdefs/msvdx_cmds_io2.h"
#include "va/va_dec_jpeg.h"
#include "va/va_dec_vp8.h"

#define GET_SURFACE_INFO_colocated_index(ipvr_surface) ((int) (ipvr_surface->extra_info[3]))
#define SET_SURFACE_INFO_colocated_index(ipvr_surface, val) ipvr_surface->extra_info[3] = (uint32_t) val;

/* Set MSVDX Front end register */
void vld_dec_FE_state(object_context_p obj_context, drm_ipvr_bo *buf)
{
    ipvr_execbuffer_p execbuf = obj_context->execbuf;
    context_DEC_p ctx = (context_DEC_p) obj_context->format_data;
    CTRL_ALLOC_HEADER *cmd_header = (CTRL_ALLOC_HEADER *)ved_execbuf_alloc_space(execbuf, sizeof(CTRL_ALLOC_HEADER));

    cmd_header->ui32Cmd_AdditionalParams = CMD_CTRL_ALLOC_HEADER;
    cmd_header->ui32ExternStateBuffAddr = 0;
    if (buf)
        RELOC(obj_context->execbuf, cmd_header->ui32ExternStateBuffAddr, 0, buf, 0);
    cmd_header->ui32MacroblockParamAddr = 0; /* Only EC needs to set this */

    ctx->cmd_params = (uint32_t*)&cmd_header->ui32Cmd_AdditionalParams;
    ctx->p_slice_params = (uint32_t*)&cmd_header->ui32SliceParams;
    cmd_header->ui32SliceParams = 0;

    ctx->slice_first_pic_last = (uint32_t*)&cmd_header->uiSliceFirstMbYX_uiPicLastMbYX;
    *ctx->slice_first_pic_last = 0;

    ctx->p_range_mapping_base0 = (uint32_t*)&cmd_header->ui32AltOutputAddr[0];
    ctx->p_range_mapping_base1 = (uint32_t*)&cmd_header->ui32AltOutputAddr[1];

    ctx->alt_output_flags = (uint32_t*)&cmd_header->ui32AltOutputFlags;

    cmd_header->ui32AltOutputFlags = 0;
    cmd_header->ui32AltOutputAddr[0] = 0;
    cmd_header->ui32AltOutputAddr[1] = 0;
}

/* Programme the Alt output if there is a rotation*/
void vld_dec_setup_alternative_frame(object_context_p obj_context)
{
    uint32_t cmd = 0;
    ipvr_execbuffer_p execbuf = obj_context->execbuf;
    context_DEC_p ctx = (context_DEC_p) obj_context->format_data;
    ipvr_surface_p src_surface = obj_context->current_render_target->ipvr_surface;

    if (obj_context->profile == VAProfileVP8Version0_3 ||
        obj_context->profile == VAProfileJPEGBaseline) {
        ved_execbuf_rendec_start(execbuf, (REG_MSVDX_CMD_OFFSET + MSVDX_CMDS_AUX_LINE_BUFFER_BASE_ADDRESS_OFFSET));
        ved_execbuf_rendec_write_address(execbuf, ctx->aux_line_buffer_vld, ctx->aux_line_buffer_vld->buffer_ofs, 0);
        ved_execbuf_rendec_end(execbuf);

        REGIO_WRITE_FIELD_LITE(cmd, MSVDX_CMDS, ALTERNATIVE_OUTPUT_PICTURE_ROTATION, USE_AUX_LINE_BUF, 1);
    }

    /* Set the rotation registers */
    ved_execbuf_rendec_start(execbuf, RENDEC_REGISTER_OFFSET(MSVDX_CMDS, ALTERNATIVE_OUTPUT_PICTURE_ROTATION));
    ved_execbuf_rendec_write(execbuf, cmd);
    *ctx->alt_output_flags = cmd;

    cmd = 0;
    REGIO_WRITE_FIELD_LITE(cmd, MSVDX_CMDS, EXTENDED_ROW_STRIDE, EXT_ROW_STRIDE, src_surface->stride / 64);
    ved_execbuf_rendec_write(execbuf, cmd);

    ved_execbuf_rendec_end(execbuf);
}

int vld_dec_slice_parameter_size(object_context_p obj_context)
{
    int size;

    switch (obj_context->profile) {
    case VAProfileVP8Version0_3:
        size = sizeof(VASliceParameterBufferVP8);
    default:
        size = 0;
        break;
    }

    return size;
}
VAStatus vld_dec_process_slice(context_DEC_p ctx,
                                        void *vld_slice_param,
                                        object_buffer_p obj_buffer);

VAStatus vld_dec_process_slice_data(context_DEC_p ctx, object_buffer_p obj_buffer)
{
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    uint8_t *slice_param;
    int buffer_idx = 0;
    unsigned int element_idx = 0, element_size;

    ASSERT((obj_buffer->type == VASliceDataBufferType) || (obj_buffer->type == VAProtectedSliceDataBufferType));

    ASSERT(ctx->pic_params);
    ASSERT(ctx->slice_param_list_idx);

    if ((NULL == obj_buffer->ipvr_bo) ||
        (0 == obj_buffer->size)) {
        /* We need to have data in the bitstream buffer */
        return VA_STATUS_ERROR_UNKNOWN;
    }

    element_size = vld_dec_slice_parameter_size(ctx->obj_context);

    while (buffer_idx < ctx->slice_param_list_idx) {
        object_buffer_p slice_buf = ctx->slice_param_list[buffer_idx];
        if (element_idx >= slice_buf->num_elements) {
            /* Move to next buffer */
            element_idx = 0;
            buffer_idx++;
            continue;
        }

        slice_param = slice_buf->buffer_data;
        slice_param += element_idx * element_size;
        element_idx++;
        vaStatus = vld_dec_process_slice(ctx, slice_param, obj_buffer);
        if (vaStatus != VA_STATUS_SUCCESS) {
            DEBUG_FAILURE;
            break;
        }
    }
    ctx->slice_param_list_idx = 0;

    return vaStatus;
}
/*
 * Adds a VASliceParameterBuffer to the list of slice params
 */
VAStatus vld_dec_add_slice_param(context_DEC_p ctx, object_buffer_p obj_buffer)
{
    ASSERT(obj_buffer->type == VASliceParameterBufferType);
    if (ctx->slice_param_list_idx >= ctx->slice_param_list_size) {
        unsigned char *new_list;
        ctx->slice_param_list_size += 8;
        new_list = realloc(ctx->slice_param_list,
                           sizeof(object_buffer_p) * ctx->slice_param_list_size);
        if (NULL == new_list) {
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        ctx->slice_param_list = (object_buffer_p*) new_list;
    }
    ctx->slice_param_list[ctx->slice_param_list_idx] = obj_buffer;
    ctx->slice_param_list_idx++;
    return VA_STATUS_SUCCESS;
}

void vld_dec_write_kick(object_context_p obj_context)
{
    ipvr_execbuffer_p execbuf = obj_context->execbuf;
    ved_execbuf_rendec_write(execbuf, CMD_COMPLETION);
}

VAStatus vld_dec_process_slice(context_DEC_p ctx,
                                        void *vld_slice_param,
                                        object_buffer_p obj_buffer)
{
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    VASliceParameterBufferBase *slice_param = (VASliceParameterBufferBase *) vld_slice_param;

    ASSERT((obj_buffer->type == VASliceDataBufferType) || (obj_buffer->type == VAProtectedSliceDataBufferType));

    if ((slice_param->slice_data_flag == VA_SLICE_DATA_FLAG_BEGIN) ||
        (slice_param->slice_data_flag == VA_SLICE_DATA_FLAG_ALL)) {
        ASSERT(!ctx->split_buffer_pending);

        vld_dec_FE_state(ctx->obj_context, ctx->preload_buffer);
        ctx->begin_slice(ctx, slice_param);
        drv_debug_msg(VIDEO_DEBUG_GENERAL, "setting slice data buffer to %x (off 0x%lx)\n",
            obj_buffer->ipvr_bo->handle, obj_buffer->ipvr_bo->offset);

        ctx->slice_data_buffer = obj_buffer->ipvr_bo;
            ved_execbuf_dma_write_bitstream(ctx->obj_context->execbuf,
                                         obj_buffer->ipvr_bo,
                                         obj_buffer->ipvr_bo->buffer_ofs + slice_param->slice_data_offset,
                                         slice_param->slice_data_size,
                                         ctx->bits_offset,
                                         ctx->SR_flags);

        if (slice_param->slice_data_flag == VA_SLICE_DATA_FLAG_BEGIN) {
            ctx->split_buffer_pending = TRUE;
        }
    } else {
        ASSERT(ctx->split_buffer_pending);
        ASSERT(0 == slice_param->slice_data_offset);
        if (slice_param->slice_data_size) {
            ved_execbuf_dma_write_bitstream_chained(ctx->obj_context->execbuf,
                    obj_buffer->ipvr_bo,
                    slice_param->slice_data_size);
        }
    }

    if ((slice_param->slice_data_flag == VA_SLICE_DATA_FLAG_ALL) ||
        (slice_param->slice_data_flag == VA_SLICE_DATA_FLAG_END)) {
        if (slice_param->slice_data_flag == VA_SLICE_DATA_FLAG_END) {
            ASSERT(ctx->split_buffer_pending);
        }

        ctx->process_slice(ctx, slice_param);
        vld_dec_write_kick(ctx->obj_context);

        ctx->split_buffer_pending = FALSE;
        ctx->obj_context->video_op = ipvr_video_vld;
        ctx->obj_context->flags = 0;

        ctx->end_slice(ctx);
        struct ved_fe_decode_arg arg;
        arg.ec_enabled = ctx->obj_context->driver_data->ec_enabled;
        arg.flags = ctx->obj_context->flags;
        arg.operating_mode = ctx->obj_context->operating_mode;
        if (ipvr_execbuffer_add_command(ctx->obj_context->execbuf,
                VED_COMMAND_FE_DECODE, &arg, sizeof(arg))) {
            vaStatus = VA_STATUS_ERROR_UNKNOWN;
        }
        if (ipvr_execbuffer_full(ctx->obj_context->execbuf)) {
            ipvr_execbuffer_run(ctx->obj_context->execbuf);
            ipvr_execbuffer_put(ctx->obj_context->execbuf);
            if (ved_context_get_execbuf(ctx->obj_context)) {
                vaStatus = VA_STATUS_ERROR_UNKNOWN;
                DEBUG_FAILURE;
                return vaStatus;
            }
            
        }
    }
    return vaStatus;
}

VAStatus vld_dec_allocate_colocated_buffer(context_DEC_p ctx, object_surface_p obj_surface, uint32_t size)
{
    drm_ipvr_bo *buf;
    VAStatus vaStatus;
    char boname[256];
    ipvr_surface_p surface = obj_surface->ipvr_surface;
    int index = GET_SURFACE_INFO_colocated_index(surface);
    memset(boname, 0, sizeof(boname));
    snprintf(boname, sizeof(boname), "VED-colocated_buffer_%d", index);

    if (!index) {
        index = ctx->colocated_buffers_idx;
        if (index >= ctx->colocated_buffers_size) {
            return VA_STATUS_ERROR_UNKNOWN;
        }

        drv_debug_msg(VIDEO_DEBUG_GENERAL, "Allocating colocated buffer for surface %08x size = %08x\n", surface, size);

        buf = ctx->colocated_buffers[index];
        buf = drm_ipvr_gem_bo_alloc(ctx->obj_context->driver_data->bufmgr, ctx->obj_context->ipvr_ctx,
            boname, size, 0, DRM_IPVR_UNCACHED, 0);
        if (!buf)
            vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
        if (VA_STATUS_SUCCESS != vaStatus) {
            return vaStatus;
        }
        ctx->colocated_buffers_idx++;
        SET_SURFACE_INFO_colocated_index(surface, index + 1); /* 0 means unset, index is offset by 1 */
    } else {
        buf = ctx->colocated_buffers[index - 1];
        if (buf->size < size) {
            drm_ipvr_gem_bo_unreference(buf);
            buf = drm_ipvr_gem_bo_alloc(ctx->obj_context->driver_data->bufmgr, ctx->obj_context->ipvr_ctx,
                boname, size, 0, DRM_IPVR_UNCACHED, 0);
            if (!buf)
                vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
            if (VA_STATUS_SUCCESS != vaStatus) {
                return vaStatus;
            }
            SET_SURFACE_INFO_colocated_index(surface, index); /* replace the original buffer */
        }
    }
    return VA_STATUS_SUCCESS;
}

drm_ipvr_bo* vld_dec_lookup_colocated_buffer(context_DEC_p ctx, ipvr_surface_p surface)
{
    int index = GET_SURFACE_INFO_colocated_index(surface);
    if (!index) {
        return NULL;
    }
    return ctx->colocated_buffers[index-1]; /* 0 means unset, index is offset by 1 */
}

VAStatus vld_dec_BeginPicture(
    context_DEC_p ctx, object_context_p obj_context)
{
    int ret;
    ctx->aux_line_buffer_vld = drm_ipvr_gem_bo_alloc(obj_context->driver_data->bufmgr,
        ctx->obj_context->ipvr_ctx, "VED-aux_line_buffer_vld",
        AUX_LINE_BUFFER_VLD_SIZE, 0, DRM_IPVR_UNCACHED, 1);
    if (!ctx->aux_line_buffer_vld) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    ret = ved_context_get_execbuf(obj_context);
    if (ret) {
        return VA_STATUS_ERROR_HW_BUSY;
    }
    return VA_STATUS_SUCCESS;
}

VAStatus vld_dec_EndPicture(
    context_DEC_p ctx)
{
    ipvr_execbuffer_put(ctx->obj_context->execbuf);
    if (ctx->aux_line_buffer_vld)
        drm_ipvr_gem_bo_unreference(ctx->aux_line_buffer_vld);
    return VA_STATUS_SUCCESS;
}

VAStatus vld_dec_CreateContext(context_DEC_p ctx, object_context_p obj_context)
{
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    obj_context->execbuf = calloc(1, sizeof(ipvr_execbuffer_t));
    if (!obj_context->execbuf) {
        vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
        DEBUG_FAILURE
        goto err;
    }

    ctx->obj_context = obj_context;
    ctx->split_buffer_pending = FALSE;
    ctx->slice_param_list_size = 8;
    ctx->slice_param_list = (object_buffer_p*) calloc(1, sizeof(object_buffer_p) * ctx->slice_param_list_size);
    ctx->slice_param_list_idx = 0;

    if (!ctx->slice_param_list) {
        vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
        DEBUG_FAILURE;
        goto err;
    }

    ctx->colocated_buffers_size = obj_context->num_render_targets;
    ctx->colocated_buffers_idx = 0;
    ctx->colocated_buffers = (drm_ipvr_bo**) calloc(1, sizeof(drm_ipvr_bo*) * ctx->colocated_buffers_size);
    if (NULL == ctx->colocated_buffers) {
        vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
        DEBUG_FAILURE;
        free(ctx->slice_param_list);
    }

    return VA_STATUS_SUCCESS;

err:
    if (obj_context->execbuf) {
        free(obj_context->execbuf);
    }
    if (ctx->slice_param_list) {
        free(ctx->slice_param_list);
    }
    if (ctx->colocated_buffers) {
        free(ctx->colocated_buffers);
    }
    return vaStatus;
}

void vld_dec_DestroyContext(context_DEC_p ctx)
{
    int i;
    object_context_p obj_context = ctx->obj_context;
    ctx->preload_buffer = NULL;
    
    if (ctx->slice_param_list) {
        free(ctx->slice_param_list);
        ctx->slice_param_list = NULL;
    }

    if (ctx->colocated_buffers) {
        for (i = 0; i < ctx->colocated_buffers_idx; ++i)
            drm_ipvr_gem_bo_unreference(ctx->colocated_buffers[i]);

        free(ctx->colocated_buffers);
        ctx->colocated_buffers = NULL;
      }
    
    ipvr_execbuffer_put(obj_context->execbuf);

    free(obj_context->execbuf);
    
    obj_context->execbuf = NULL;
    
}

VAStatus vld_dec_RenderPicture(
    object_context_p obj_context,
    object_buffer_p *buffers,
    int num_buffers)
{
    int i;
    context_DEC_p ctx = (context_DEC_p) obj_context->format_data;
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    for (i = 0; i < num_buffers; i++) {
        object_buffer_p obj_buffer = buffers[i];
        ipvr__dump_va_buffers_verbose(obj_buffer);

        switch (obj_buffer->type) {
        case VASliceParameterBufferType:
            vaStatus = vld_dec_add_slice_param(ctx, obj_buffer);
            DEBUG_FAILURE;
            break;

        case VASliceDataBufferType:
        case VAProtectedSliceDataBufferType:
            vaStatus = vld_dec_process_slice_data(ctx, obj_buffer);
            DEBUG_FAILURE;
            break;

        default:
            vaStatus = ctx->process_buffer(ctx, obj_buffer);
            DEBUG_FAILURE;
        }
        if (vaStatus != VA_STATUS_SUCCESS) {
            break;
        }
    }

    return vaStatus;
}

