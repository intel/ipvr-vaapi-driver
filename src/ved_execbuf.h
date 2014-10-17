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

#ifndef _VED_EXECBUF_H_
#define _VED_EXECBUF_H_

#include "ipvr_drv_video.h"
#include "ipvr_bufmgr.h"
#include "ipvr_execbuf.h"
#include <stdint.h>

/*
 * Advances "obj_context" to the next execbuf
 *
 * Returns 0 on success
 */

enum {
    VED_COMMAND_FE_DECODE    = 0,
    VED_COMMAND_HOST_BE_OPP = 1,
};

struct ved_fe_decode_arg
{
    uint8_t ec_enabled;
    uint16_t flags;
    uint32_t operating_mode;
};

int ved_context_get_execbuf(object_context_p obj_context);

int ved_context_submit_host_be_opp(object_context_p obj_context,
                                  drm_ipvr_bo *buf_a,
                                  drm_ipvr_bo *buf_b,
                                  drm_ipvr_bo *buf_c,
                                  uint32_t picture_widht_mb,
                                  uint32_t frame_height_mb,
                                  uint32_t rotation_flags,
                                  uint32_t field_type,
                                  uint32_t ext_stride_a,
                                  uint32_t chroma_offset_a,
                                  uint32_t chroma_offset_b);

int ved_context_submit_hw_deblock(object_context_p obj_context,
                                  drm_ipvr_bo *buf_a,
                                  drm_ipvr_bo *buf_b,
                                  drm_ipvr_bo *colocate_buffer,
                                  uint32_t picture_widht_mb,
                                  uint32_t frame_height_mb,
                                  uint32_t rotation_flags,
                                  uint32_t field_type,
                                  uint32_t ext_stride_a,
                                  uint32_t chroma_offset_a,
                                  uint32_t chroma_offset_b,
                                  uint32_t is_oold);


/*
 * Submits the current execbuf
 *
 * Returns 0 on success
 */
int ved_context_submit_execbuf(object_context_p obj_context, ipvr_execbuffer_p mtxmsg);

/*
 * Flushes the pending execbuf
 *
 * Return 0 on success
 */
int ved_context_flush_execbuf(object_context_p obj_context, ipvr_execbuffer_p mtxmsg);


int
ved_context_insert_DEVA_FE_DECODE(object_context_p obj_context);

/*
 * Write a SR_SETUP_CMD referencing a bitstream buffer to the command buffer
 *
 * The slice data runs from buffer_offset_in_bytes to buffer_offset_in_bytes + size_in_bytes
 * The first bit to be processed is buffer_offset_in_bytes + offset_in_bits
 *
 * TODO: Return something
 */

void ved_execbuf_dma_write_bitstream(ipvr_execbuffer_p execbuf,
                                      drm_ipvr_bo *bitstream_buf,
                                      uint32_t buffer_offset,
                                      uint32_t size_in_bytes,
                                      uint32_t offset_in_bits,
                                      uint32_t flags);

void ved_execbuf_dma_write_bitstream_chained(ipvr_execbuffer_p execbuf,
        drm_ipvr_bo *bitstream_buf,
        uint32_t size_in_bytes);

/*
 * Create a command to set registers
 */
void ved_execbuf_reg_start_block(ipvr_execbuffer_p execbuf, uint32_t flags);

void ved_execbuf_reg_set(ipvr_execbuffer_p execbuf, uint32_t reg, uint32_t val);

#define ved_execbuf_reg_set_RELOC( execbuf, reg, buffer,buffer_offset)             \
    do { \
        execbuf->emit_dword(execbuf, reg); \
        execbuf->emit_reloc_bo(execbuf, buffer->drm_buf, execbuf->cur_offset, buffer_offset); \
    } while (0)


enum DMA_TYPE;
typedef enum DMA_TYPE DMA_TYPE;
void ved_execbuf_dma_write_execbuf(ipvr_execbuffer_p execbuf,
                                   drm_ipvr_bo *bitstream_buf,
                                   uint32_t buffer_offset,
                                   uint32_t size,
                                   uint32_t dest_offset,
                                   DMA_TYPE type);

void ved_execbuf_reg_set_address(ipvr_execbuffer_p execbuf,
                                uint32_t reg,
                                drm_ipvr_bo *buffer,
                                uint32_t buffer_offset);

/*
 * Finish a command to set registers
 */
void ved_execbuf_reg_end_block(ipvr_execbuffer_p execbuf);

/*
 * Create a RENDEC command block
 */
void ved_execbuf_rendec_start(ipvr_execbuffer_p execbuf, uint32_t dest_address);

#define ved_execbuf_rendec_write( execbuf, val ) \
    EMIT_DWORD(execbuf, val)

void ved_execbuf_rendec_write_block(ipvr_execbuffer_p execbuf,
                                   unsigned char *block,
                                   uint32_t size);

void ved_execbuf_rendec_write_address(ipvr_execbuffer_p execbuf,
                                     drm_ipvr_bo *buffer,
                                     uint32_t buffer_offset,
                                     uint32_t flags);

typedef enum {
    SKIP_ON_CONTEXT_SWITCH = 1,
} E_SKIP_CONDITION;

/*
 * Create a conditional SKIP block
 */
void ved_execbuf_skip_start_block(ipvr_execbuffer_p execbuf, uint32_t skip_condition);

/*
 * Terminate a conditional SKIP block
 */
void ved_execbuf_skip_end_block(ipvr_execbuffer_p execbuf);

/*
 * Terminate a conditional SKIP block
 */
void ved_execbuf_rendec_end(ipvr_execbuffer_p execbuf);

void *ved_execbuf_alloc_space(ipvr_execbuffer_p execbuf, uint32_t byte_size);

#endif
