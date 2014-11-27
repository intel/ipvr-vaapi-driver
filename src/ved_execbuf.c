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
#include "ved_execbuf.h"
#include <unistd.h>
#include <stdio.h>

#include "hwdefs/mem_io.h"
#include "hwdefs/msvdx_offsets.h"
#include "hwdefs/dma_api.h"
#include "hwdefs/reg_io2.h"
#include "hwdefs/msvdx_vec_reg_io2.h"
#include "hwdefs/msvdx_vdmc_reg_io2.h"
#include "hwdefs/msvdx_mtx_reg_io2.h"
#include "hwdefs/msvdx_dmac_linked_list.h"
#include "hwdefs/msvdx_rendec_mtx_slice_cntrl_reg_io2.h"
#include "hwdefs/dxva_cmdseq_msg.h"
#include "hwdefs/dxva_fw_ctrl.h"
#include "hwdefs/fwrk_msg_mem_io.h"
#include "hwdefs/dxva_msg.h"
#include "hwdefs/msvdx_cmds_io2.h"
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>

#include "ipvr_def.h"
#include "ipvr_drv_debug.h"

#define MTXMSG_SIZE           (0x1000)
#define CMD_SIZE              (0x1000)
#define MTXMSG_MARGIN         (0x0040)
#define CMD_MARGIN            (0x0400)

typedef struct ved_execbuf_private_s {
    drm_ipvr_bo        *bo;

    /* for debugging and dumping */
    int decode_count;
    int host_be_opp_count;

    /* Pointer to bitstream size field in last SR_SETUP */
    uint32_t *cmd_bitstream_size;

    /* Pointer for Register commands */
    uint32_t *reg_start;
    uint32_t *reg_wt_p;
    uint32_t reg_next;
    uint32_t reg_flags;

    /* Pointer for Rendec Block commands */
    uint32_t *rendec_block_start;
    uint32_t *rendec_chunk_start;

    /* Pointer for Skip block commands */
    uint32_t *skip_block_start;
    uint32_t skip_condition;

    uint16_t *last_decode_flags;

    unsigned long cur_offset;
    unsigned long start_offset;
} ved_execbuf_private_t, *ved_execbuf_private_p;

static int ved_execbuffer_get(drm_ipvr_bufmgr *bufmgr, drm_ipvr_context *ctx,
                 ipvr_execbuffer_p execbuf, const char *name,
                 size_t buf_size, int reusable);

static int ved__execbuffer_ready(ipvr_execbuffer_p execbuf)
{
    ved_execbuf_private_p execbuf_priv = (ved_execbuf_private_p)execbuf->priv;
    return execbuf->bo && !drm_ipvr_gem_bo_busy(execbuf->bo) &&
        execbuf_priv && execbuf_priv->bo && !drm_ipvr_gem_bo_busy(execbuf_priv->bo);
}

/*static void
ved__execbuffer_reset(ipvr_execbuffer_p execbuf)
{
    ved_execbuf_private_p execbuf_priv = (ved_execbuf_private_p)execbuf->priv;
    execbuf_priv->cmd_bitstream_size = NULL;
    execbuf_priv->reg_flags = 0;
    execbuf_priv->reg_next = 0;
    execbuf_priv->reg_start = NULL;
    execbuf_priv->reg_wt_p = NULL;
    execbuf_priv->rendec_block_start = NULL;
    execbuf_priv->rendec_chunk_start = NULL;
    execbuf_priv->skip_block_start = NULL;
    execbuf_priv->skip_condition = 0;
    execbuf_priv->last_decode_flags = NULL;
    execbuf_priv->decode_count = 0;
    execbuf_priv->host_be_opp_count = 0;

    execbuf_priv->cur_offset = 0;
    execbuf_priv->start_offset = 0;
}*/

/*
 * Advances "obj_context" to the next execbuf
 *
 * Returns 0 on success
 */
int ved_context_get_execbuf(object_context_p obj_context)
{
    int ret;

    if (obj_context->execbuf)
        ASSERT(!obj_context->execbuf->valid);

    ret = ved_execbuffer_get(obj_context->driver_data->bufmgr, obj_context->ipvr_ctx,
        obj_context->execbuf, "VED-CtrlAlloc", CMD_SIZE, 1);
    
    return ret;
}


static void *
ved__execbuf_alloc_space_from_mtxmsg(ipvr_execbuffer_p execbuf,
    uint32_t byte_size)
{
    ved_execbuf_private_p execbuf_priv = (ved_execbuf_private_p)execbuf->priv;
    uint8_t *pos = (uint8_t*)execbuf_priv->bo->virt + execbuf_priv->cur_offset;
    ASSERT(!(byte_size % 4));

    execbuf_priv->cur_offset += byte_size;

    return pos;
}

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
                                  uint32_t chroma_offset_b)
{
    return -EINVAL;
}
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
                                  uint32_t is_oold)
{
    return -EINVAL;
}

static int
ved__add_decode_command(ipvr_execbuffer_p execbuf, struct ved_fe_decode_arg *arg)
{
    ved_execbuf_private_p execbuf_priv = (ved_execbuf_private_p)execbuf->priv;
    int ret;
    uint32_t cmdbuffer_size = execbuf->cur_offset - execbuf->start_offset; // In bytes
    unsigned long msg_offset = execbuf_priv->cur_offset;
    uint32_t *msg = (uint32_t*)ved__execbuf_alloc_space_from_mtxmsg(execbuf, FW_DEVA_DECODE_SIZE);

    /**
     * only the last message needs interrupt
     */
    if (execbuf_priv->last_decode_flags) {
        *execbuf_priv->last_decode_flags &= ~FW_VA_RENDER_HOST_INT;
        *execbuf_priv->last_decode_flags |= FW_VA_RENDER_NO_RESPONCE_MSG;
    }

    if (ipvr_video_trace_fp && (ipvr_video_trace_level & CMDMSG_TRACE)) {
        debug_cmd_start[execbuf_priv->decode_count] = execbuf->start_offset;
        debug_cmd_size[execbuf_priv->decode_count] = execbuf->cur_offset;
        debug_cmd_count = execbuf_priv->decode_count + 1;
    }

    ret = ipvr_cmdbuf_dump((unsigned int *)(execbuf->vaddr + execbuf->start_offset), cmdbuffer_size);
    if(ret)
        drv_debug_msg(VIDEO_DEBUG_GENERAL, "ipvr_cmdbuf: dump cmdbuf fail\n");

    ASSERT(!ipvr_execbuffer_full(execbuf));
    /* set MTXMSG as not triggering host interrupt. when execbuffer is flushed,
     * the last DECODE msg flag will be modified */
    uint32_t decode_flags = arg->flags;
    if (arg->ec_enabled)
        decode_flags |= FW_ERROR_DETECTION_AND_RECOVERY;
    decode_flags |= FW_VA_RENDER_HOST_INT;
    MEMIO_WRITE_FIELD(msg, FWRK_GENMSG_SIZE,                  FW_DEVA_DECODE_SIZE);
    MEMIO_WRITE_FIELD(msg, FW_DEVA_DECODE_MSG_TYPE,           VA_MSGID_RENDER);
    MEMIO_WRITE_FIELD(msg, FW_DEVA_DECODE_MSG_ID,             0x0);
    MEMIO_WRITE_FIELD(msg, FW_DEVA_DECODE_FLAGS,              decode_flags);
    /* save the MSG flag address for later modification */
    execbuf_priv->last_decode_flags = (uint16_t*)((uint8_t*)msg + FW_DEVA_DECODE_FLAGS_OFFSET);
    MEMIO_WRITE_FIELD(msg, FW_DEVA_DECODE_BUFFER_SIZE,          cmdbuffer_size / sizeof(uint32_t)); // In dwords

    drv_debug_msg(VIDEO_DEBUG_GENERAL, "%s: relocating CtrlAlloc 0x%lx to MTX-msg 0x%lx + 0x%lx\n",
        __func__, execbuf->start_offset, msg_offset, FW_DEVA_DECODE_CTRL_ALLOC_ADDR_OFFSET);
    ret = drm_ipvr_gem_bo_emit_reloc(execbuf_priv->bo, msg_offset + FW_DEVA_DECODE_CTRL_ALLOC_ADDR_OFFSET,
         execbuf->bo, execbuf->start_offset, 0);
    if (ret) {
        /* todo: roll back */
        drv_debug_msg(VIDEO_DEBUG_ERROR, "%s::%d emit_reloc failed\n", __func__, __LINE__);
        return ret;
    }
    *(msg + FW_DEVA_DECODE_CTRL_ALLOC_ADDR_OFFSET/sizeof(uint32_t)) = execbuf->bo->offset
        + execbuf->start_offset;
    
    MEMIO_WRITE_FIELD(msg, FW_DEVA_DECODE_CONTEXT, execbuf->ctx->ctx_id & 0xff); /* context is 8 bits */
    MEMIO_WRITE_FIELD(msg, FW_DEVA_DECODE_MMUPTD, 0x0); /* mmuptd is 24 bits */
    MEMIO_WRITE_FIELD(msg, FW_DEVA_DECODE_OPERATING_MODE, arg->operating_mode);

    ipvr__trace_message("MSG BUFFER_SIZE       = %08x\n", MEMIO_READ_FIELD(msg, FW_DEVA_DECODE_BUFFER_SIZE));
    ipvr__trace_message("MSG OPERATING_MODE      = %08x\n", MEMIO_READ_FIELD(msg, FW_DEVA_DECODE_OPERATING_MODE));
    ipvr__trace_message("MSG FLAGS              = %08x\n", MEMIO_READ_FIELD(msg, FW_DEVA_DECODE_FLAGS));


    execbuf->start_offset = execbuf->cur_offset;
    execbuf_priv->decode_count ++;

    drv_debug_msg(VIDEO_DEBUG_GENERAL, "%s: add decode_command success\n", __func__);
    return 0;
}


static int
ved__execbuffer_add_command(ipvr_execbuffer_p execbuf,
        int cmd, void *arg, size_t arg_size)
{
    switch (cmd) {
    case VED_COMMAND_FE_DECODE:
        if (arg_size < sizeof(struct ved_fe_decode_arg)) {
            return -EINVAL;
        }
        return ved__add_decode_command(execbuf, (struct ved_fe_decode_arg*)arg);
    case VED_COMMAND_HOST_BE_OPP:
    default:
        return -EINVAL;
    }
}

/*
 * Submits the current execbuf
 *
 * Returns 0 on success
 */


#define MAX_DMA_LEN     ( 0xffff )

void *ved_execbuf_alloc_space(ipvr_execbuffer_p execbuf, uint32_t byte_size)
{
    void *pos = (void *)(execbuf->vaddr + execbuf->cur_offset);
    ASSERT(!(byte_size % 4));

    execbuf->cur_offset += byte_size;

    return pos;
}

void ved_execbuf_dma_write_execbuf(ipvr_execbuffer_p execbuf,
                                   drm_ipvr_bo *bitstream_buf,
                                   uint32_t buffer_offset,
                                   uint32_t size,
                                   uint32_t dest_offset,
                                   DMA_TYPE type)
{
    ASSERT(size < 0xFFFF);
    ASSERT(buffer_offset < 0xFFFF);

    DMA_CMD_WITH_OFFSET* dma_cmd;

    if(dest_offset==0)
    {
        dma_cmd = (DMA_CMD_WITH_OFFSET*)ved_execbuf_alloc_space(execbuf, sizeof(DMA_CMD));
        dma_cmd->ui32Cmd = 0;
    }
    else
    {
        dma_cmd = (DMA_CMD_WITH_OFFSET*)ved_execbuf_alloc_space(execbuf, sizeof(DMA_CMD_WITH_OFFSET));
        dma_cmd->ui32Cmd = CMD_DMA_OFFSET_FLAG; // Set flag indicating that offset is deffined
        dma_cmd->ui32ByteOffset = dest_offset;
    }

    dma_cmd->ui32Cmd |= CMD_DMA;
    dma_cmd->ui32Cmd |= (IMG_UINT32)type;
    dma_cmd->ui32Cmd |= size;
    /* dma_cmd->ui32DevVirtAdd  = ui32DevVirtAddress; */
    RELOC(execbuf, dma_cmd->ui32DevVirtAdd, buffer_offset, bitstream_buf, 0);
}

/*
 * Write a CMD_SR_SETUP referencing a bitstream buffer to the command buffer
 */
void ved_execbuf_dma_write_bitstream(ipvr_execbuffer_p execbuf,
                                      drm_ipvr_bo *bitstream_buf,
                                      uint32_t buffer_offset,
                                      uint32_t size_in_bytes,
                                      uint32_t offset_in_bits,
                                      uint32_t flags)
{
    /*
     * We use byte alignment instead of 32bit alignment.
     * The third frame of sa10164.vc1 results in the following bitstream
     * patttern:
     * [0000] 00 00 03 01 76 dc 04 8d
     * with offset_in_bits = 0x1e
     * This causes an ENTDEC failure because 00 00 03 is a start code
     * By byte aligning the datastream the start code will be eliminated.
     */
    ved_execbuf_private_p execbuf_priv = (ved_execbuf_private_p)execbuf->priv;
    uint32_t *bs = (uint32_t*)ved_execbuf_alloc_space(execbuf, 20);
    *bs++ = CMD_SR_SETUP | flags;
    *bs++ = offset_in_bits;
    execbuf_priv->cmd_bitstream_size = bs;
    *bs++ = size_in_bytes;
    *bs++ = (CMD_BITSTREAM_DMA | size_in_bytes);
    RELOC(execbuf, *bs, buffer_offset, bitstream_buf, 0);
}

/*
 * Chain a LLDMA bitstream command to the previous one
 */
void ved_execbuf_dma_write_bitstream_chained(ipvr_execbuffer_p execbuf,
        drm_ipvr_bo *bitstream_buf,
        uint32_t size_in_bytes)
{
    ved_execbuf_private_p execbuf_priv = (ved_execbuf_private_p)execbuf->priv;
    EMIT_DWORD(execbuf, CMD_BITSTREAM_DMA | size_in_bytes);
    EMIT_RELOC(execbuf, *(execbuf->vaddr + execbuf->cur_offset),
        bitstream_buf->buffer_ofs, bitstream_buf, 0);

    *(execbuf_priv->cmd_bitstream_size) += size_in_bytes;
}

void ved_execbuf_reg_start_block(ipvr_execbuffer_p execbuf, uint32_t flags)
{
    ved_execbuf_private_p execbuf_priv = (ved_execbuf_private_p)execbuf->priv;
    ASSERT(NULL == execbuf->reg_start); /* Can't have both */

    execbuf_priv->reg_wt_p = (uint32_t*)(execbuf->vaddr + execbuf->cur_offset);
    execbuf_priv->reg_next = 0;
    execbuf_priv->reg_flags = (flags << 4); /* flags are diff between DE2 & DE3 */
    execbuf_priv->reg_start = NULL;
}

void ved_execbuf_reg_set(ipvr_execbuffer_p execbuf, uint32_t reg, uint32_t val)
{
    ved_execbuf_private_p execbuf_priv = (ved_execbuf_private_p)execbuf->priv;
    if(execbuf_priv->reg_start && (reg == execbuf_priv->reg_next))
    {
        /* Incrament header size */
        *execbuf_priv->reg_start += (0x1 << 16);
    }
    else
    {
        execbuf_priv->reg_start = execbuf_priv->reg_wt_p++;
        *execbuf_priv->reg_start = CMD_REGVALPAIR_WRITE | execbuf_priv->reg_flags | 0x10000 | (reg & 0xfffff); /* We want host reg addr */
    }
    *execbuf_priv->reg_wt_p++ = val;
    execbuf_priv->reg_next = reg + 4;
}

void ved_execbuf_reg_set_address(ipvr_execbuffer_p execbuf,
                                         uint32_t reg,
                                         drm_ipvr_bo *buffer,
                                         uint32_t buffer_offset)
{
    ved_execbuf_private_p execbuf_priv = (ved_execbuf_private_p)execbuf->priv;
    if(execbuf_priv->reg_start && (reg == execbuf_priv->reg_next))
    {
        /* Incrament header size */
        *execbuf_priv->reg_start += (0x1 << 16);
    }
    else
    {
        execbuf_priv->reg_start = execbuf_priv->reg_wt_p++;
        *execbuf_priv->reg_start = CMD_REGVALPAIR_WRITE | execbuf_priv->reg_flags | 0x10000 | (reg & 0xfffff); /* We want host reg addr */
    }
    RELOC(execbuf, *execbuf_priv->reg_wt_p++, buffer_offset, buffer, 0);
    execbuf_priv->reg_next = reg + 4;
}

void ved_execbuf_reg_end_block(ipvr_execbuffer_p execbuf)
{
    ved_execbuf_private_p execbuf_priv = (ved_execbuf_private_p)execbuf->priv;
    execbuf->cur_offset = (uint8_t*)execbuf_priv->reg_wt_p - execbuf->vaddr;
    execbuf_priv->reg_start = NULL;
}

/*
 * Start a new rendec block of another format
 */
void ved_execbuf_rendec_start(ipvr_execbuffer_p execbuf, uint32_t dest_address)
{
    ved_execbuf_private_p execbuf_priv = (ved_execbuf_private_p)execbuf->priv;
    ASSERT(((dest_address >> 2)& ~0xfff) == 0);
    execbuf_priv->rendec_chunk_start = (uint32_t*)(execbuf->vaddr + execbuf->cur_offset);
    *execbuf_priv->rendec_chunk_start = CMD_RENDEC_BLOCK | dest_address;
    execbuf->cur_offset += 4;
}

void ved_execbuf_rendec_write_block(ipvr_execbuffer_p execbuf,
                                   unsigned char *block,
                                   uint32_t size)
{
    ASSERT((size & 0x3) == 0);
    unsigned int i;
    for (i = 0; i < size; i += 4) {
        uint32_t val = block[i] | (block[i+1] << 8) | (block[i+2] << 16) | (block[i+3] << 24);
        ved_execbuf_rendec_write(execbuf, val);
    }
}

void ved_execbuf_rendec_write_address(ipvr_execbuffer_p execbuf,
                                     drm_ipvr_bo *buffer,
                                     uint32_t buffer_offset,
                                     uint32_t flags)
{
    EMIT_RELOC(execbuf, *(execbuf->vaddr + execbuf->cur_offset), buffer_offset, buffer, flags);
}

/*
 * Finish a RENDEC block
 */
void ved_execbuf_rendec_end(ipvr_execbuffer_p execbuf)
{
    ved_execbuf_private_p execbuf_priv = (ved_execbuf_private_p)execbuf->priv;
    ASSERT(NULL != execbuf_priv->rendec_chunk_start); /* Must have an open RENDEC chunk */
    uint32_t dword_count = (uint32_t*)(execbuf->vaddr + execbuf->cur_offset)- execbuf_priv->rendec_chunk_start;

    ASSERT((dword_count - 1) <= 0xff);

    *execbuf_priv->rendec_chunk_start += ((dword_count - 1) << 16);
    execbuf_priv->rendec_chunk_start = NULL;
}

/*
 * Create a conditional SKIP block
 */
void ved_execbuf_skip_start_block(ipvr_execbuffer_p execbuf, uint32_t skip_condition)
{
    ved_execbuf_private_p execbuf_priv = (ved_execbuf_private_p)execbuf->priv;
    ASSERT(NULL == execbuf_priv->rendec_block_start); /* Can't be inside a rendec block */
    ASSERT(NULL == execbuf_priv->reg_start); /* Can't be inside a reg block */
    ASSERT(NULL == execbuf_priv->skip_block_start); /* Can't be inside another skip block (limitation of current sw design)*/

    execbuf_priv->skip_condition = skip_condition;
    execbuf_priv->skip_block_start = (uint32_t*)(execbuf->vaddr + execbuf->cur_offset);
    execbuf->cur_offset += 4;
}

/*
 * Terminate a conditional SKIP block
 */
void ved_execbuf_skip_end_block(ipvr_execbuffer_p execbuf)
{
    ved_execbuf_private_p execbuf_priv = (ved_execbuf_private_p)execbuf->priv;
    ASSERT(NULL == execbuf_priv->rendec_block_start); /* Rendec block must be closed */
    ASSERT(NULL == execbuf_priv->reg_start); /* Reg block must be closed */
    ASSERT(NULL != execbuf_priv->skip_block_start); /* Skip block must still be open */

    uint32_t block_size = (uint32_t*)(execbuf->vaddr + execbuf->cur_offset)
                            - (execbuf_priv->skip_block_start + 1);

    *execbuf_priv->skip_block_start = CMD_CONDITIONAL_SKIP | (execbuf_priv->skip_condition << 20) | block_size;
    execbuf_priv->skip_block_start = NULL;
}


static void
ved__execbuffer_put(ipvr_execbuffer_p execbuf)
{
    ved_execbuf_private_p execbuf_priv = (ved_execbuf_private_p)execbuf->priv;
    if (execbuf_priv && execbuf_priv->bo) {
        drm_ipvr_gem_bo_unmap(execbuf_priv->bo);
        drm_ipvr_gem_bo_unreference(execbuf_priv->bo);
        execbuf_priv->bo = NULL;
    }
    if (execbuf->bo) {
        drm_ipvr_gem_bo_unmap(execbuf->bo);
        drm_ipvr_gem_bo_unreference(execbuf->bo);
    }
    execbuf->bo = NULL;
    execbuf->put = NULL;
    execbuf->run = NULL;
    execbuf->full = NULL;
    execbuf->ready = NULL;
    execbuf->add_command = NULL;
    execbuf->priv = NULL;
    execbuf->cur_offset = 0;
    execbuf->vaddr = NULL;
    execbuf->start_offset = 0;
    drv_debug_msg(VIDEO_DEBUG_GENERAL, "%s: add decode_command success\n", __func__);
}

static int
ved__execbuffer_full(ipvr_execbuffer_p execbuf)
{
    ved_execbuf_private_p execbuf_priv = (ved_execbuf_private_p)execbuf->priv;
    return (execbuf_priv->decode_count >= MAX_CMD_COUNT) ||
            (MTXMSG_SIZE - execbuf_priv->cur_offset < MTXMSG_MARGIN) ||
            (CMD_SIZE - execbuf->cur_offset < CMD_MARGIN);
}

static int
ved__execbuffer_run(ipvr_execbuffer_p execbuf)
{
    int ret, i;
    ved_execbuf_private_p execbuf_priv = (ved_execbuf_private_p)execbuf->priv;
    drm_ipvr_bo *mtxmsg_bo = execbuf_priv->bo;
    uint32_t mtxmsg_len = execbuf_priv->cur_offset;
    drv_debug_msg(VIDEO_DEBUG_GENERAL, "%s submit execbuffer %x (0x%lx) len %u, on context %u\n",
        __func__, mtxmsg_bo->handle, mtxmsg_bo->offset, mtxmsg_len,
        execbuf->ctx->ctx_id);

    ipvr__trace_message("lldma_count = %d, vitual=0x%08x\n",
                       0,  0);
    ipvr__trace_message("debug_dump_count = %d\n", 0);

    if (ipvr_video_trace_level & CMDMSG_TRACE) {
        ipvr__trace_message("cmd_count = %d, virtual=0x%08x\n",
                           1, execbuf->bo->offset);
        for (i = 0; i < 1; i++) {
            uint32_t *msg = (uint32_t*)execbuf_priv->bo->virt + i * FW_DEVA_DECODE_SIZE/sizeof(uint32_t);
            int j;
            debug_dump_cmdbuf((uint32_t *)(execbuf->vaddr), execbuf->cur_offset + 1);
    
            for (j = 0; j < FW_DEVA_DECODE_SIZE / 4; j++) {
                ipvr__trace_message("MTX msg[%d] = 0x%08x", j, *(msg + j));
                switch (j) {
                case 0:
                    ipvr__trace_message("[BufferSize|ID|MSG_SIZE]\n");
                    break;
                case 1:
                    ipvr__trace_message("[MMUPTD]\n");
                    break;
                case 2:
                    ipvr__trace_message("[LLDMA_address]\n");
                    break;
                case 3:
                    ipvr__trace_message("[Context]\n");
                    break;
                case 4:
                    ipvr__trace_message("[Fence_Value]\n");
                    break;
                case 5:
                    ipvr__trace_message("[Operating_Mode]\n");
                    break;
                case 6:
                    ipvr__trace_message("[LastMB|FirstMB]\n");
                    break;
                case 7:
                    ipvr__trace_message("[Flags]\n");
                    break;
                default:
                    ipvr__trace_message("[overflow]\n");
                    break;
                }
            }
        }
    }

    /**
     * make sure that these BOs' cache are flushed
     */
    drm_ipvr_gem_bo_unmap(mtxmsg_bo);
    drm_ipvr_gem_bo_unmap(execbuf->bo);

    if (mtxmsg_len == 0) {
        drv_debug_msg(VIDEO_DEBUG_GENERAL, "%s empty cmd, skip exec\n", __func__);
        return 0;
    }
    ret = drm_ipvr_gem_bo_exec(mtxmsg_bo, 0, mtxmsg_len, -1, NULL);
    if (ret) {
        drv_debug_msg(VIDEO_DEBUG_ERROR, "%s submit execbuffer failed %d %s\n",
            __func__, ret, strerror(ret));
    }
    
    drv_debug_msg(VIDEO_DEBUG_GENERAL, "%s: success\n", __func__);
    return ret;
}

static int ved_execbuffer_get(drm_ipvr_bufmgr *bufmgr, drm_ipvr_context *ctx,
                 ipvr_execbuffer_p execbuf, const char *name,
                 size_t buf_size, int reusable)
{
    int ret;
    ret = ipvr_execbuffer_get(bufmgr, ctx, execbuf, name, buf_size, reusable);
    if (ret) {
        return -ENOMEM;
    }
    static ved_execbuf_private_t execbuf_priv;
    memset(&execbuf_priv, 0, sizeof(execbuf_priv));
    execbuf_priv.bo = drm_ipvr_gem_bo_alloc(bufmgr, ctx, "VED-MtxMessage",
        MTXMSG_SIZE, 0, IPVR_CACHE_WRITECOMBINE, reusable);
    if (!execbuf_priv.bo) {
        ipvr_execbuffer_put(execbuf);
        drv_debug_msg(VIDEO_DEBUG_ERROR, "%s failed to allocate CMD buf\n", __func__);
        return -ENOMEM;
    }
    ret = drm_ipvr_gem_bo_map(execbuf_priv.bo, 0, MTXMSG_SIZE, 1);
    if (ret) {
        drm_ipvr_gem_bo_unreference(execbuf_priv.bo);
        ipvr_execbuffer_put(execbuf);
        drv_debug_msg(VIDEO_DEBUG_ERROR, "%s failed to map CMD buf\n", __func__);
        return -ENOMEM;
    }
    /**
     * override the callbacks of execbuffer
     */
    execbuf->run = ved__execbuffer_run;
    execbuf->put = ved__execbuffer_put;
    execbuf->full = ved__execbuffer_full;
    execbuf->ready = ved__execbuffer_ready;
    execbuf->add_command = ved__execbuffer_add_command;
    execbuf->priv = &execbuf_priv;
    drv_debug_msg(VIDEO_DEBUG_GENERAL, "%s got cmd %p, mtxmsg %p, ctx %u\n",
        __func__, execbuf->vaddr, execbuf_priv.bo->virt, execbuf->ctx->ctx_id);
    execbuf->valid = 1;
    return 0;
}
