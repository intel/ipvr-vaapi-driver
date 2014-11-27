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

#ifndef _IPVR_DRV_VIDEO_H_
#define _IPVR_DRV_VIDEO_H_

#include <pthread.h> /* POSIX threads headers */
#include <va/va_backend.h>
#include <va/va.h>
#include <va/va_vpp.h>
#include <va/va_dricommon.h>
#include <stdint.h>
#include "xf86drm.h"
#include <ipvr_bufmgr.h>
#include "object_heap.h"
#include "ipvr_def.h"
#include "hwdefs/dxva_fw_flags.h"

#ifndef min
#define min(a, b) ((a) < (b)) ? (a) : (b)
#endif

#ifndef max
#define max(a, b) ((a) > (b)) ? (a) : (b)
#endif

#define TOPAZHP_PIPE_NUM 2

#define _TNG_RELOC_

#define FORCED_REFERENCE 1
#define LTREFHEADER 1

/*
 * WORKAROUND_DMA_OFF_BY_ONE: LLDMA requests may access one additional byte which can cause
 * a MMU fault if the next byte after the buffer end is on a different page that isn't mapped.
 */
#define WORKAROUND_DMA_OFF_BY_ONE
//#define FOURCC_XVVA     (('A' << 24) + ('V' << 16) + ('V' << 8) + 'X')

#define IPVR_MAX_PROFILES                        18
#define IPVR_MAX_ENTRYPOINTS                     (VAEntrypointVideoProc + 1)
#define IPVR_MAX_CONFIG_ATTRIBUTES               10
#define IPVR_MAX_BUFFERTYPES                     VABufferTypeMax

/* Max # of command submission buffers */
#define VED_MAX_CMDBUFS                10

#define IPVR_SURFACE_DISPLAYING_F (0x1U<<0)
#define IPVR_SURFACE_IS_FLAG_SET(flags, mask) (((flags)& IPVR_SURFACE_DISPLAYING_F) != 0)

#define IPVR_CTX_TILING_MASK    0x00FF0000

#define MAX_SLICES_PER_PICTURE 72
#define MAX_MB_ERRORS 72

/* Some funtions aren't used but we'd like to keep them as reference code in future */
//#define IPVR_MFLD_DUMMY_CODE     0

typedef struct object_config_s *object_config_p;
typedef struct object_context_s *object_context_p;
typedef struct object_surface_s *object_surface_p;
typedef struct object_buffer_s *object_buffer_p;
typedef struct object_image_s *object_image_p;
typedef struct object_subpic_s *object_subpic_p;
typedef struct format_vtable_s *format_vtable_p;
typedef struct ipvr_driver_data_s *ipvr_driver_data_p;

/* flags: 0 indicates cache */
#define IPVR_USER_BUFFER_UNCACHED    (0x1)
#define IPVR_USER_BUFFER_WC        (0x1<<1)

struct ipvr_driver_data_s {
    struct object_heap_s        config_heap;
    struct object_heap_s        context_heap;
    struct object_heap_s        surface_heap;
    struct object_heap_s        buffer_heap;
    struct object_heap_s        image_heap;
    struct object_heap_s        subpic_heap;
    char *                      bus_id;
    uint32_t                    dev_id;
    int                         drm_fd;
    int                         dup_drm_fd;

    /*  PM_QoS */
    int                         pm_qos_fd;
    int                         dri2;
    int                         dri_dummy;
    XID                         context_id;
    drm_context_t               drm_context;
    drmLock                     *drm_lock;
    int                         contended_lock;
    pthread_mutex_t             drm_mutex;
    format_vtable_p             profile2Format[IPVR_MAX_PROFILES][IPVR_MAX_ENTRYPOINTS];

    format_vtable_p             vpp_profile;

    drm_ipvr_bufmgr *bufmgr;

    uint32_t ec_enabled;

    /* VA_RT_FORMAT_PROTECTED is set to protected for Widevine case */
    int is_protected;
};

#define IS_BAYTRAIL(driver_data) ((driver_data->dev_id & 0xFFFF) == 0x0F31)

struct object_config_s {
    struct object_base_s base;
    VAProfile profile;
    VAEntrypoint entrypoint;
    VAConfigAttrib attrib_list[IPVR_MAX_CONFIG_ATTRIBUTES];
    int attrib_count;
    format_vtable_p format_vtable;
};

struct ipvr_execbuffer_s;
typedef struct ipvr_execbuffer_s ipvr_execbuffer_t;
typedef ipvr_execbuffer_t *ipvr_execbuffer_p;
struct object_context_s {
    struct object_base_s base;
    VAContextID context_id;
    VAConfigID config_id;
    VAProfile profile;
    VAEntrypoint entry_point;
    int picture_width;
    int picture_height;
    int num_render_targets;
    VASurfaceID *render_targets;
    int va_flags;

    object_surface_p current_render_target;
    object_surface_p ec_target;
    object_surface_p ec_candidate;
    VASurfaceID current_render_surface_id;
    ipvr_driver_data_p driver_data;
    format_vtable_p format_vtable;
    unsigned char *format_data;

    ipvr_execbuffer_p execbuf;

    /* Buffers */
    object_buffer_p buffers_unused[IPVR_MAX_BUFFERTYPES]; /* Linked lists (HEAD) of unused buffers for each buffer type */
    int buffers_unused_count[IPVR_MAX_BUFFERTYPES]; /* Linked lists (HEAD) of unused buffers for each buffer type */
    object_buffer_p buffers_unused_tail[IPVR_MAX_BUFFERTYPES]; /* Linked lists (TAIL) of unused buffers for each buffer type */
    object_buffer_p buffers_active[IPVR_MAX_BUFFERTYPES]; /* Linked lists of active buffers for each buffer type */

    object_buffer_p *buffer_list; /* for vaRenderPicture */
    int num_buffers;

    enum {
        ipvr_video_none = 0,
        ipvr_video_mc,
        ipvr_video_vld,
        ipvr_video_deblock
    } video_op;

    uint32_t operating_mode;
    uint32_t flags; /* See render flags below */
    uint32_t first_mb;
    uint32_t last_mb;

    unsigned long ctx_type;
    unsigned long ved_tile; /* normal tile | (rotate tile << 4) */

    drm_ipvr_context *ipvr_ctx;

    /* Debug */
    uint32_t frame_count;
    uint32_t slice_count;

};

struct object_surface_s {
    struct object_base_s base;
    VASurfaceID surface_id;
    VAContextID context_id;
    int width;
    int height;
    int height_origin;
    struct ipvr_surface_s *ipvr_surface;
    unsigned int derived_imgcnt; /* is the surface derived by a VAImage? */
    unsigned long display_timestamp; /* record the time point of put surface*/
    int is_ref_surface; /* If true, vaDeriveImage returns error */
};

#define IPVR_CODEDBUF_SLICE_NUM_MASK (0xff)
#define IPVR_CODEDBUF_SLICE_NUM_SHIFT (0)

#define IPVR_CODEDBUF_NONE_VCL_NUM_MASK (0xff)
#define IPVR_CODEDBUF_NONE_VCL_NUM_SHIFT (8)

#define SET_CODEDBUF_INFO(flag, aux_info, slice_num) \
    do {\
    (aux_info) &= ~(IPVR_CODEDBUF_##flag##_MASK<<IPVR_CODEDBUF_##flag##_SHIFT);\
    (aux_info) |= ((slice_num) & IPVR_CODEDBUF_##flag##_MASK)\
    <<IPVR_CODEDBUF_##flag##_SHIFT;\
    } while (0)

#define CLEAR_CODEDBUF_INFO(flag, aux_info) \
    do {\
    (aux_info) &= ~(IPVR_CODEDBUF_##flag##_MASK<<IPVR_CODEDBUF_##flag##_SHIFT);\
    } while (0)

#define GET_CODEDBUF_INFO(flag, aux_info) \
    (((aux_info)>>IPVR_CODEDBUF_##flag##_SHIFT) & IPVR_CODEDBUF_##flag##_MASK)


#define IPVR_CODEDBUF_SEGMENT_MAX  (9)

struct object_buffer_s {
    struct object_base_s base;
    object_buffer_p ptr_next; /* Generic ptr for linked list */
    object_buffer_p *pptr_prev_next; /* Generic ptr for linked list */
    drm_ipvr_bo *ipvr_bo;
    unsigned char *buffer_data;
    unsigned int size;
    unsigned int alloc_size;
    unsigned int max_num_elements;
    unsigned int num_elements;
    object_context_p context;
    VABufferType type;
    uint32_t last_used;
    /* Export state */
    unsigned int export_refcount;
    VABufferInfo export_state;
};

struct object_image_s {
    struct object_base_s base;
    VAImage image;
    unsigned int palette[16];
    int subpic_ref;
    VASurfaceID derived_surface;
};

struct object_subpic_s {
    struct object_base_s base;
    VASubpictureID subpic_id;

    VAImageID image_id;

    /* chromakey range */
    unsigned int chromakey_min;
    unsigned int chromakey_max;
    unsigned int chromakey_mask;

    /* global alpha */
    unsigned int global_alpha;

    /* flags */
    unsigned int flags; /* see below */

    unsigned char *surfaces; /* surfaces, associated with this subpicture */
};

#define MEMSET_OBJECT(ptr, data_struct) \
        memset((unsigned char *)ptr + sizeof(struct object_base_s),\
                0,                          \
               sizeof(data_struct) - sizeof(struct object_base_s))

struct format_vtable_s {
    void (*queryConfigAttributes)(
        VAProfile profile,
        VAEntrypoint entrypoint,
        VAConfigAttrib *attrib_list,
        int num_attribs
    );
    VAStatus(*validateConfig)(
        object_config_p obj_config
    );
    VAStatus(*createContext)(
        object_context_p obj_context,
        object_config_p obj_config
    );
    void (*destroyContext)(
        object_context_p obj_context
    );
    VAStatus(*beginPicture)(
        object_context_p obj_context
    );
    VAStatus(*renderPicture)(
        object_context_p obj_context,
        object_buffer_p *buffers,
        int num_buffers
    );
    VAStatus(*endPicture)(
        object_context_p obj_context
    );
};

#define ipvr__bounds_check(x, max)                                       \
    do { ASSERT(x < max); if (x >= max) x = max - 1; } while(0);

static inline unsigned long GetTickCount()
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL))
        return 0;
    return tv.tv_usec / 1000 + tv.tv_sec * 1000;
}

inline static const char * buffer_type_to_string(int type)
{
    switch (type) {
    case VAPictureParameterBufferType:
        return "VAPictureParameterBufferType";
    case VAIQMatrixBufferType:
        return "VAIQMatrixBufferType";
    case VABitPlaneBufferType:
        return "VABitPlaneBufferType";
    case VASliceGroupMapBufferType:
        return "VASliceGroupMapBufferType";
    case VASliceParameterBufferType:
        return "VASliceParameterBufferType";
    case VASliceDataBufferType:
        return "VASliceDataBufferType";
    case VAProtectedSliceDataBufferType:
        return "VAProtectedSliceDataBufferType";
    case VAMacroblockParameterBufferType:
        return "VAMacroblockParameterBufferType";
    case VAResidualDataBufferType:
        return "VAResidualDataBufferType";
    case VADeblockingParameterBufferType:
        return "VADeblockingParameterBufferType";
    case VAImageBufferType:
        return "VAImageBufferType";
    case VAEncCodedBufferType:
        return "VAEncCodedBufferType";
    case VAEncSequenceParameterBufferType:
        return "VAEncSequenceParameterBufferType";
    case VAEncPictureParameterBufferType:
        return "VAEncPictureParameterBufferType";
    case VAEncSliceParameterBufferType:
        return "VAEncSliceParameterBufferType";
    case VAEncMiscParameterBufferType:
        return "VAEncMiscParameterBufferType";
    case VAProbabilityBufferType:
        return "VAProbabilityBufferType";
    case VAHuffmanTableBufferType:
        return "VAHuffmanTableBufferType";
    case VAQMatrixBufferType:
        return "VAQMatrixBufferType";
    default:
        return "UnknowBuffer";
    }
}

int ipvr_parse_config(char *env, char *env_value);
void ipvr__destroy_surface(ipvr_driver_data_p driver_data, object_surface_p obj_surface);

#define CHECK_SURFACE(obj_surface) \
    do { \
        if (NULL == obj_surface) { \
            vaStatus = VA_STATUS_ERROR_INVALID_SURFACE; \
            DEBUG_FAILURE; \
            return vaStatus; \
        } \
    } while (0)

#define CHECK_CONFIG(obj_config) \
    do { \
        if (NULL == obj_config) { \
            vaStatus = VA_STATUS_ERROR_INVALID_CONFIG; \
            DEBUG_FAILURE; \
            return vaStatus; \
        } \
    } while (0)

#define CHECK_CONTEXT(obj_context) \
    do { \
        if (NULL == obj_context) { \
            vaStatus = VA_STATUS_ERROR_INVALID_CONTEXT; \
            DEBUG_FAILURE; \
            return vaStatus; \
        } \
    } while (0)

#define CHECK_BUFFER(obj_buffer) \
    do { \
        if (NULL == obj_buffer) { \
            vaStatus = VA_STATUS_ERROR_INVALID_BUFFER; \
            DEBUG_FAILURE; \
            return vaStatus; \
        } \
    } while (0)

#define CHECK_IMAGE(obj_image) \
    do { \
        if (NULL == obj_image) { \
            vaStatus = VA_STATUS_ERROR_INVALID_IMAGE; \
            DEBUG_FAILURE; \
            return vaStatus; \
        } \
    } while (0)

#define CHECK_SUBPICTURE(obj_subpic) \
    do { \
        if (NULL == obj_subpic) { \
            vaStatus = VA_STATUS_ERROR_INVALID_SUBPICTURE; \
            DEBUG_FAILURE; \
            return vaStatus; \
        } \
    } while (0)

#define CHECK_ALLOCATION(buf) \
    do { \
        if (buf == NULL) { \
            vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED; \
            DEBUG_FAILURE; \
            return vaStatus; \
        } \
    } while (0)

#define CHECK_VASTATUS() \
    do { \
        if (VA_STATUS_SUCCESS != vaStatus) { \
            DEBUG_FAILURE; \
            return vaStatus; \
        } \
    } while (0)

#define CHECK_INVALID_PARAM(param) \
    do { \
        if (param) { \
            vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER; \
            DEBUG_FAILURE; \
            return vaStatus; \
        } \
    } while (0)

#endif /* _IPVR_DRV_VIDEO_H_ */
