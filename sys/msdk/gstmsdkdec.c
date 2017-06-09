/* GStreamer Intel MSDK plugin
 * Copyright (c) 2016, Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdlib.h>

#include "gstmsdkdec.h"
#include "gstmsdkopaqueallocator.h"

GST_DEBUG_CATEGORY_EXTERN (gst_msdkdec_debug);
#define GST_CAT_DEFAULT gst_msdkdec_debug

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw, "
        "format = (string) { NV12 }, "
        "framerate = (fraction) [0, MAX], "
        "width = (int) [ 16, MAX ], height = (int) [ 16, MAX ],"
        "interlace-mode = (string) progressive")
    );

enum
{
  PROP_0,
  PROP_HARDWARE,
  PROP_ASYNC_DEPTH,
};

#define PROP_HARDWARE_DEFAULT            TRUE
#define PROP_ASYNC_DEPTH_DEFAULT         4

#define gst_msdkdec_parent_class parent_class
G_DEFINE_TYPE (GstMsdkDec, gst_msdkdec, GST_TYPE_VIDEO_DECODER);

typedef struct _MsdkSurface
{
  mfxFrameSurface1 surface;
  GstVideoFrame frame;
} MsdkSurface;

static GstFlowReturn
allocate_decode_buffer (GstMsdkDec * thiz, GstBuffer ** buffer)
{
  GstFlowReturn flow;
  GstVideoCodecFrame *frame;
  GstVideoDecoder *decoder = GST_VIDEO_DECODER (thiz);

  /* Operating with separate decode and output pools, with a copy. */
  if (thiz->decode_pool)
    return gst_buffer_pool_acquire_buffer (thiz->decode_pool, buffer, NULL);

  /* Operating with a single pool for decode and output. */
  frame = gst_video_decoder_get_oldest_frame (decoder);
  if (!frame) {
    if (GST_PAD_IS_FLUSHING (decoder->srcpad))
      return GST_FLOW_FLUSHING;
    else
      return GST_FLOW_ERROR;
  }
  if (!frame->output_buffer) {
    flow = gst_video_decoder_allocate_output_frame (decoder, frame);
    if (flow != GST_FLOW_OK) {
      gst_video_codec_frame_unref (frame);
      return flow;
    }
  }
  *buffer = gst_buffer_ref (frame->output_buffer);
  gst_buffer_replace (&frame->output_buffer, NULL);
  gst_video_codec_frame_unref (frame);
  return GST_FLOW_OK;
}

static void
free_surface (gpointer surface)
{
  MsdkSurface *s = surface;

  if (s->surface.Data.Locked)
    /* MSDK is using the surface, defer unmapping/unreffing. */
    return;

  if (s->frame.buffer) {
    gst_video_frame_unmap (&s->frame);
    g_clear_pointer (&s->frame.buffer, gst_buffer_unref);
  }
}

static void
clear_surface (gpointer surface)
{
  MsdkSurface *s = surface;
  s->surface.Data.Locked = 0;
  free_surface (surface);
}

static MsdkSurface *
get_surface (GstMsdkDec * thiz)
{
  MsdkSurface *i;

  for (i = (MsdkSurface *) thiz->surfaces->data;
      i < (MsdkSurface *) thiz->surfaces->data + thiz->surfaces->len; i++) {
    if (!i->surface.Data.Locked)
      break;
  }
  if (i == (MsdkSurface *) thiz->surfaces->data + thiz->surfaces->len)
    return NULL;

  /* MSDK may have been using a surface for its own purposes and then
     released it. Release any buffers still held and then
     re-allocate */
  free_surface (i);

  return i;
}

static gboolean
surface_set_buffer (GstMsdkDec * thiz, MsdkSurface * surface,
    GstBuffer * buffer)
{
  mfxFrameSurface1 *s = &surface->surface;
  GstVideoFrame *frame = &surface->frame;
  GstVideoInfo *info;
  GstMapFlags flags;
  gboolean ret;

  if (thiz->decode_pool) {
    info = &thiz->decode_pool_info;
    flags = GST_MAP_READWRITE;
  } else {
    info = &thiz->output_info;
    flags = GST_MAP_WRITE;
  }

  memset (frame, 0, sizeof (*frame));

  ret = gst_video_frame_map (frame, info, buffer, flags);
  if (!ret)
    return ret;

  s->Data.Y = GST_VIDEO_FRAME_PLANE_DATA (frame, 0);
  s->Data.UV = GST_VIDEO_FRAME_PLANE_DATA (frame, 1);
  s->Data.PitchLow = GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0);

  return TRUE;
}

static void
gst_msdkdec_close_decoder (GstMsdkDec * thiz)
{
  mfxStatus status;

  if (!thiz->context)
    return;

  GST_DEBUG_OBJECT (thiz, "Closing decoder 0x%p", thiz->context);

  status = MFXVideoDECODE_Close (msdk_context_get_session (thiz->context));
  if (status != MFX_ERR_NONE && status != MFX_ERR_NOT_INITIALIZED) {
    GST_WARNING_OBJECT (thiz, "Decoder close failed (%s)",
        msdk_status_to_string (status));
  }

  g_array_set_size (thiz->tasks, 0);
  g_array_set_size (thiz->surfaces, 0);
  g_ptr_array_set_size (thiz->extra_params, 0);

  g_clear_pointer (&thiz->param, g_free);
  g_clear_pointer (&thiz->context, msdk_close_context);
  g_clear_pointer (&thiz->output_pool, gst_object_unref);

  thiz->decoder_initialized = FALSE;
}

static gboolean
gst_msdkdec_init_param (GstMsdkDec * thiz)
{
  GstMsdkDecClass *klass = GST_MSDKDEC_GET_CLASS (thiz);
  GstVideoInfo *info;
  mfxSession session;
  mfxStatus status;

  if (!thiz->input_state) {
    GST_DEBUG_OBJECT (thiz, "Have no input state yet");
    return FALSE;
  }
  info = &thiz->input_state->info;

  /* make sure that the decoder is closed */
  gst_msdkdec_close_decoder (thiz);

  thiz->context = msdk_open_context (thiz->hardware);
  if (!thiz->context) {
    GST_ERROR_OBJECT (thiz, "Context creation failed");
    return FALSE;
  }

  thiz->param = g_new0 (mfxVideoParam, 1);

  GST_OBJECT_LOCK (thiz);

  thiz->param->AsyncDepth = thiz->async_depth;
  thiz->param->IOPattern = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;

  thiz->param->mfx.FrameInfo.Width = GST_ROUND_UP_32 (info->width);
  thiz->param->mfx.FrameInfo.Height = GST_ROUND_UP_32 (info->height);
  thiz->param->mfx.FrameInfo.CropW = info->width;
  thiz->param->mfx.FrameInfo.CropH = info->height;
  thiz->param->mfx.FrameInfo.FrameRateExtN = info->fps_n;
  thiz->param->mfx.FrameInfo.FrameRateExtD = info->fps_d;
  thiz->param->mfx.FrameInfo.AspectRatioW = info->par_n;
  thiz->param->mfx.FrameInfo.AspectRatioH = info->par_d;
  thiz->param->mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
  thiz->param->mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
  thiz->param->mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
  thiz->param->mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;

  GST_OBJECT_UNLOCK (thiz);

  /* allow subclass configure further */
  if (klass->configure) {
    if (!klass->configure (thiz))
      goto failed;
  }

  thiz->param->NumExtParam = thiz->extra_params->len;
  thiz->param->ExtParam = (mfxExtBuffer **) thiz->extra_params->pdata;

  session = msdk_context_get_session (thiz->context);
  /* validate parameters and allow the Media SDK to make adjustments */
  status = MFXVideoDECODE_Query (session, thiz->param, thiz->param);
  if (status < MFX_ERR_NONE) {
    GST_ERROR_OBJECT (thiz, "Video Decode Query failed (%s)",
        msdk_status_to_string (status));
    goto failed;
  } else if (status > MFX_ERR_NONE) {
    GST_WARNING_OBJECT (thiz, "Video Decode Query returned: %s",
        msdk_status_to_string (status));
  }

  status = MFXVideoDECODE_QueryIOSurf (session, thiz->param,
      &thiz->alloc_request);
  if (status < MFX_ERR_NONE) {
    GST_ERROR_OBJECT (thiz, "Query IO surfaces failed (%s)",
        msdk_status_to_string (status));
    goto failed;
  } else if (status > MFX_ERR_NONE) {
    GST_WARNING_OBJECT (thiz, "Query IO surfaces returned: %s",
        msdk_status_to_string (status));
  }

  if (thiz->alloc_request.NumFrameSuggested < thiz->param->AsyncDepth) {
    GST_ERROR_OBJECT (thiz, "Required %d surfaces (%d suggested), async %d",
        thiz->alloc_request.NumFrameMin, thiz->alloc_request.NumFrameSuggested,
        thiz->param->AsyncDepth);
    goto failed;
  }

  return TRUE;

failed:
  g_clear_pointer (&thiz->param, g_free);
  msdk_close_context (thiz->context);
  thiz->context = NULL;
  return FALSE;
}

static gboolean
gst_msdkdec_init_decoder (GstMsdkDec * thiz)
{
  mfxStatus status;
  mfxSession session;
  guint i;

  if (!thiz->context || !thiz->param) {
    GST_ERROR_OBJECT (thiz,
        "init_decoder called without context or video param");
    return FALSE;
  }

  session = msdk_context_get_session (thiz->context);

  thiz->param->NumExtParam = thiz->extra_params->len;
  thiz->param->ExtParam = (mfxExtBuffer **) thiz->extra_params->pdata;

  status = MFXVideoDECODE_Init (session, thiz->param);
  if (status < MFX_ERR_NONE) {
    GST_ERROR_OBJECT (thiz, "Init failed (%s)", msdk_status_to_string (status));
    goto failed;
  } else if (status > MFX_ERR_NONE) {
    GST_WARNING_OBJECT (thiz, "Init returned: %s",
        msdk_status_to_string (status));
  }

  status = MFXVideoDECODE_GetVideoParam (session, thiz->param);
  if (status < MFX_ERR_NONE) {
    GST_ERROR_OBJECT (thiz, "Get Video Parameters failed (%s)",
        msdk_status_to_string (status));
    goto failed;
  } else if (status > MFX_ERR_NONE) {
    GST_WARNING_OBJECT (thiz, "Get Video Parameters returned: %s",
        msdk_status_to_string (status));
  }

  if (thiz->surfaces->len == 0) {
    g_array_set_size (thiz->surfaces, thiz->alloc_request.NumFrameSuggested);
    for (i = 0; i < thiz->surfaces->len; i++) {
      memcpy (&g_array_index (thiz->surfaces, MsdkSurface, i).surface.Info,
          &thiz->param->mfx.FrameInfo, sizeof (mfxFrameInfo));
    }
  }

  g_array_set_size (thiz->tasks, 0);
  g_array_set_size (thiz->tasks, thiz->param->AsyncDepth);
  thiz->next_task = 0;

  thiz->decoder_initialized = TRUE;

  return TRUE;

failed:
  g_clear_pointer (&thiz->param, g_free);
  msdk_close_context (thiz->context);
  thiz->context = NULL;
  return FALSE;
}

static gboolean
gst_msdkdec_set_src_caps (GstMsdkDec * thiz)
{
  GstVideoCodecState *output_state;
  GstVideoAlignment align;
  guint width, height;

  width = GST_VIDEO_INFO_WIDTH (&thiz->input_state->info);
  height = GST_VIDEO_INFO_HEIGHT (&thiz->input_state->info);

  output_state =
      gst_video_decoder_set_output_state (GST_VIDEO_DECODER (thiz),
      GST_VIDEO_FORMAT_NV12, width, height, thiz->input_state);

  msdk_video_alignment (&align, &output_state->info);
  gst_video_info_align (&output_state->info, &align);
  memcpy (&thiz->output_info, &output_state->info, sizeof (GstVideoInfo));
  if (output_state->caps)
    gst_caps_unref (output_state->caps);
  output_state->caps = gst_video_info_to_caps (&output_state->info);
  gst_video_codec_state_unref (output_state);

  return TRUE;
}

static void
gst_msdkdec_set_latency (GstMsdkDec * thiz)
{
  GstVideoInfo *info = &thiz->input_state->info;
  gint min_delayed_frames;
  GstClockTime latency;

  min_delayed_frames = thiz->tasks->len;

  if (info->fps_n) {
    latency = gst_util_uint64_scale_ceil (GST_SECOND * info->fps_d,
        min_delayed_frames, info->fps_n);
  } else {
    /* FIXME: Assume 25fps. This is better than reporting no latency at
     * all and then later failing in live pipelines
     */
    latency = gst_util_uint64_scale_ceil (GST_SECOND * 1,
        min_delayed_frames, 25);
  }

  GST_INFO_OBJECT (thiz,
      "Updating latency to %" GST_TIME_FORMAT " (%d frames)",
      GST_TIME_ARGS (latency), min_delayed_frames);

  gst_video_decoder_set_latency (GST_VIDEO_DECODER (thiz), latency, latency);
}

static GstFlowReturn
output_to_frame (GstMsdkDec * thiz, MsdkSurface * surface,
    GstVideoCodecFrame * codec_frame)
{
  GstFlowReturn flow;
  GstVideoFrame frame;

  if (!thiz->decode_pool) {
    gst_buffer_replace (&codec_frame->output_buffer, surface->frame.buffer);
    return GST_FLOW_OK;
  }

  /* surface's buffer is from decode_pool, need to allocate from
     output_pool and copy the frame over */
  if (!codec_frame->output_buffer) {
    flow =
        gst_video_decoder_allocate_output_frame (GST_VIDEO_DECODER (thiz),
        codec_frame);
    if (flow != GST_FLOW_OK)
      return flow;
  }
  if (!gst_video_frame_map (&frame, &thiz->output_info,
          codec_frame->output_buffer, GST_MAP_WRITE))
    return GST_FLOW_ERROR;
  if (!gst_video_frame_copy (&frame, &surface->frame))
    return GST_FLOW_ERROR;
  gst_video_frame_unmap (&frame);

  return GST_FLOW_OK;
}

static GstFlowReturn
gst_msdkdec_finish_task (GstMsdkDec * thiz, MsdkDecTask * task)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER (thiz);
  GstFlowReturn flow;
  GstVideoCodecFrame *frame;
  MsdkSurface *surface;
  mfxStatus status;

  if (G_LIKELY (task->sync_point)) {
    status =
        MFXVideoCORE_SyncOperation (msdk_context_get_session (thiz->context),
        task->sync_point, 10000);
    if (status != MFX_ERR_NONE)
      return GST_FLOW_ERROR;
    frame = gst_video_decoder_get_oldest_frame (decoder);

    task->sync_point = NULL;
    task->surface->Data.Locked--;
    surface = (MsdkSurface *) task->surface;

    if (G_LIKELY (frame))
      flow = output_to_frame (thiz, surface, frame);

    free_surface (surface);

    if (!frame)
      return GST_FLOW_FLUSHING;

    if (flow == GST_FLOW_OK)
      flow = gst_video_decoder_finish_frame (decoder, frame);

    gst_video_codec_frame_unref (frame);
    return flow;
  }
  return GST_FLOW_OK;
}

static gboolean
gst_msdkdec_close (GstVideoDecoder * decoder)
{
  GstMsdkDec *thiz = GST_MSDKDEC (decoder);
  if (thiz->context) {
    msdk_close_context (thiz->context);
    thiz->context = NULL;
  }
  return TRUE;
}

static gboolean
gst_msdkdec_stop (GstVideoDecoder * decoder)
{
  GstMsdkDec *thiz = GST_MSDKDEC (decoder);
  if (thiz->input_state) {
    gst_video_codec_state_unref (thiz->input_state);
    thiz->input_state = NULL;
  }
  if (thiz->decode_pool) {
    gst_object_unref (thiz->decode_pool);
    thiz->decode_pool = NULL;
  }
  gst_video_info_init (&thiz->output_info);
  gst_video_info_init (&thiz->decode_pool_info);
  return TRUE;
}

static gboolean
gst_msdkdec_set_format (GstVideoDecoder * decoder, GstVideoCodecState * state)
{
  GstMsdkDec *thiz = GST_MSDKDEC (decoder);

  if (thiz->input_state)
    gst_video_codec_state_unref (thiz->input_state);
  thiz->input_state = gst_video_codec_state_ref (state);

  if (!gst_msdkdec_init_param (thiz))
    return FALSE;

  if (!gst_msdkdec_set_src_caps (thiz)) {
    gst_msdkdec_close_decoder (thiz);
    return FALSE;
  }

  gst_msdkdec_set_latency (thiz);

  return TRUE;
}

static GstFlowReturn
gst_msdkdec_handle_frame (GstVideoDecoder * decoder, GstVideoCodecFrame * frame)
{
  GstMsdkDec *thiz = GST_MSDKDEC (decoder);
  GstFlowReturn flow;
  GstBuffer *buffer = NULL;
  MsdkDecTask *task = NULL;
  MsdkSurface *surface = NULL;
  mfxBitstream bitstream;
  mfxSession session;
  mfxStatus status;
  GstMapInfo map_info;
  guint i;

  if (!gst_buffer_map (frame->input_buffer, &map_info, GST_MAP_READ))
    return GST_FLOW_ERROR;
  memset (&bitstream, 0, sizeof (bitstream));
  bitstream.Data = map_info.data;
  bitstream.DataLength = map_info.size;
  bitstream.MaxLength = map_info.size;

  if (G_UNLIKELY (!thiz->decoder_initialized)) {
    flow = gst_video_decoder_allocate_output_frame (decoder, frame);
    if (flow != GST_FLOW_OK)
      return flow;
  }

  session = msdk_context_get_session (thiz->context);
  for (;;) {
    task = &g_array_index (thiz->tasks, MsdkDecTask, thiz->next_task);
    flow = gst_msdkdec_finish_task (thiz, task);
    if (flow != GST_FLOW_OK)
      goto exit;
    if (G_LIKELY (!surface)) {
      surface = get_surface (thiz);
      if (G_UNLIKELY (!surface)) {
        /* Can't get a surface for some reason, finish tasks to see if
           a surface becomes available. */
        for (i = 0; i < thiz->tasks->len - 1; i++) {
          thiz->next_task = (thiz->next_task + 1) % thiz->tasks->len;
          task = &g_array_index (thiz->tasks, MsdkDecTask, thiz->next_task);
          flow = gst_msdkdec_finish_task (thiz, task);
          if (flow != GST_FLOW_OK)
            goto exit;
          surface = get_surface (thiz);
          if (surface)
            break;
        }
        if (!surface) {
          /* After finishing all tasks there's still not a surface
             available somehow. */
          GST_ERROR_OBJECT (thiz, "Couldn't get a surface");
          flow = GST_FLOW_ERROR;
          goto exit;
        }
      }

      flow = allocate_decode_buffer (thiz, &buffer);
      if (flow != GST_FLOW_OK)
        goto exit;

      if (!surface_set_buffer (thiz, surface, buffer)) {
        flow = GST_FLOW_ERROR;
        goto exit;
      }
    }

    status =
        MFXVideoDECODE_DecodeFrameAsync (session, &bitstream, &surface->surface,
        &task->surface, &task->sync_point);
    if (G_LIKELY (status == MFX_ERR_NONE)) {
      /* Locked may not be incremented immediately by the SDK, but
         this surface should not be given as a work surface again
         until after SyncOperation has been called. We may loop right
         back up to get_surface, if more bitstream is available to
         handle.  So increment Locked ourselves and then decrement it
         after SyncOperation. */
      task->surface->Data.Locked++;
      thiz->next_task = (thiz->next_task + 1) % thiz->tasks->len;
      surface = NULL;
      if (bitstream.DataLength == 0) {
        flow = GST_FLOW_OK;
        break;
      }
    } else if (status == MFX_ERR_MORE_DATA) {
      flow = GST_FLOW_OK;
      break;
    } else if (status == MFX_ERR_MORE_SURFACE) {
      surface = NULL;
      continue;
    } else if (status == MFX_WRN_DEVICE_BUSY)
      /* If device is busy, wait 1ms and retry, as per MSDK's recomendation */
      g_usleep (1000);
    else if (status < MFX_ERR_NONE) {
      GST_ERROR_OBJECT (thiz, "DecodeFrameAsync failed (%s)",
          msdk_status_to_string (status));
      flow = GST_FLOW_ERROR;
      break;
    }
  }

exit:
  if (surface)
    free_surface (surface);

  gst_buffer_unmap (frame->input_buffer, &map_info);
  return flow;
}

static gboolean
gst_msdkdec_decide_allocation_opaque (GstVideoDecoder * decoder,
    GstQuery * query)
{
  GstMsdkDec *thiz = GST_MSDKDEC (decoder);
  mfxSession session;
  mfxStatus status;
  mfxFrameAllocRequest request;
  const GstStructure *opaque_params;
  guint opaque_index, num_surfaces, type, i;
  mfxFrameSurface1 **surfaces;
  GstMsdkOpaqueAllocator *allocator;
  mfxExtBuffer *ext_buffer;
  GstBufferPool *pool;
  GstStructure *pool_config;
  GstCaps *caps;

  gst_query_find_allocation_meta (query, GST_MSDK_OPAQUE_META_API_TYPE,
      &opaque_index);
  gst_query_parse_nth_allocation_meta (query, opaque_index, &opaque_params);
  if (!gst_structure_get_uint (opaque_params, "gst.msdk.opaque.num_surfaces",
          &num_surfaces))
    return FALSE;
  if (!gst_structure_get_uint (opaque_params, "gst.msdk.opaque.type", &type))
    return FALSE;

  thiz->param->IOPattern = MFX_IOPATTERN_OUT_OPAQUE_MEMORY;
  session = msdk_context_get_session (thiz->context);
  status = MFXVideoDECODE_QueryIOSurf (session, thiz->param, &request);
  if (status < MFX_ERR_NONE) {
    GST_ERROR_OBJECT (decoder, "QueryIOSurf failed (%s)",
        msdk_status_to_string (status));
    return FALSE;
  }

  num_surfaces += request.NumFrameSuggested;
  type |= request.Type;

  g_array_set_size (thiz->surfaces, 0);
  g_array_set_size (thiz->surfaces, num_surfaces);
  surfaces = g_new0 (mfxFrameSurface1 *, num_surfaces);
  for (i = 0; i < num_surfaces; i++)
    surfaces[i] = &g_array_index (thiz->surfaces, MsdkSurface, i).surface;
  allocator = gst_msdk_opaque_allocator_new (surfaces, num_surfaces, type);
  ext_buffer = gst_msdk_opaque_allocator_ext_buffer (allocator);
  g_ptr_array_add (thiz->extra_params, g_memdup (ext_buffer,
          ext_buffer->BufferSz));

  gst_query_parse_nth_allocation_pool (query, 0, &pool, NULL, NULL, NULL);
  pool_config = gst_buffer_pool_get_config (pool);
  if (!gst_buffer_pool_config_get_params (pool_config, &caps, NULL, NULL, NULL))
    return FALSE;
  gst_buffer_pool_config_set_params (pool_config, caps, 1, num_surfaces,
      num_surfaces);
  gst_buffer_pool_config_set_allocator (pool_config, GST_ALLOCATOR (allocator),
      NULL);
  if (!gst_buffer_pool_set_config (pool, pool_config))
    return FALSE;

  gst_query_set_nth_allocation_param (query, 0, GST_ALLOCATOR (allocator),
      NULL);
  gst_query_set_nth_allocation_pool (query, 0, pool, 1, num_surfaces,
      num_surfaces);

  return TRUE;
}

static gboolean
gst_msdkdec_decide_allocation_sysmem (GstVideoDecoder * decoder,
    GstQuery * query)
{
  GstMsdkDec *thiz = GST_MSDKDEC (decoder);
  GstVideoInfo info_from_caps, info_aligned;
  GstVideoAlignment alignment;
  GstBufferPool *pool = NULL;
  GstStructure *pool_config = NULL;
  GstCaps *pool_caps;
  gboolean need_aligned;
  guint size, min_buffers, max_buffers;

  /* Get the buffer pool config decided by the base class. The base
     class ensures that there will always be at least a 0th pool in
     the query. */
  gst_query_parse_nth_allocation_pool (query, 0, &pool, NULL, NULL, NULL);
  pool_config = gst_buffer_pool_get_config (pool);

  /* Increase the min and max buffers by async_depth, we will
     always have that number of decode operations in-flight */
  gst_buffer_pool_config_get_params (pool_config, &pool_caps, &size,
      &min_buffers, &max_buffers);
  min_buffers += thiz->async_depth;
  if (max_buffers)
    max_buffers += thiz->async_depth;
  gst_buffer_pool_config_set_params (pool_config, pool_caps, size, min_buffers,
      max_buffers);

  /* Check if the pool's caps will meet msdk's alignment
     requirements by default. */
  gst_video_info_from_caps (&info_from_caps, pool_caps);
  gst_caps_unref (pool_caps);
  memcpy (&info_aligned, &info_from_caps, sizeof (info_aligned));
  msdk_video_alignment (&alignment, &info_from_caps);
  gst_video_info_align (&info_aligned, &alignment);
  need_aligned = !gst_video_info_is_equal (&info_from_caps, &info_aligned);

  g_clear_pointer (&thiz->decode_pool, gst_object_unref);

  if (need_aligned) {
    /* The pool's caps do not meet msdk's alignment requirements. Make
       a pool config that does meet the requirements. We will use this
       config for the allocation pool if possible, or as the config
       for a side-pool if the downstream can't handle it. */

    size = MAX (size, GST_VIDEO_INFO_SIZE (&info_aligned));
    gst_buffer_pool_config_set_params (pool_config, pool_caps, size,
        min_buffers, max_buffers);
    gst_buffer_pool_config_add_option (pool_config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);
    gst_buffer_pool_config_add_option (pool_config,
        GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);
    gst_buffer_pool_config_set_video_alignment (pool_config, &alignment);

    if (gst_query_find_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL)
        && gst_buffer_pool_has_option (pool,
            GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT)) {
      /* The aligned pool config can be used directly. */
      if (!gst_buffer_pool_set_config (pool, pool_config))
        return FALSE;
    } else {
      /* The aligned pool config cannot be used directly so we will
         make a side-pool that will be decoded into and the copied
         from. */
      thiz->decode_pool = gst_video_buffer_pool_new ();
      gst_buffer_pool_config_set_params (pool_config, pool_caps, size,
          thiz->async_depth, max_buffers);
      memcpy (&thiz->output_info, &info_from_caps, sizeof (GstVideoInfo));
      memcpy (&thiz->decode_pool_info, &info_aligned, sizeof (GstVideoInfo));
      if (!gst_buffer_pool_set_config (thiz->decode_pool, pool_config) ||
          !gst_buffer_pool_set_active (thiz->decode_pool, TRUE))
        return FALSE;
    }
  }

  gst_query_set_nth_allocation_pool (query, 0, pool, size, min_buffers,
      max_buffers);

  return TRUE;
}

static gboolean
gst_msdkdec_decide_allocation_existing (GstVideoDecoder * decoder,
    GstQuery * query)
{
  GstMsdkDec *thiz = GST_MSDKDEC (decoder);
  GstStructure *pool_config;
  GstCaps *pool_caps;
  guint size, min_buffers, max_buffers;

  pool_config = gst_buffer_pool_get_config (thiz->output_pool);
  gst_buffer_pool_config_get_params (pool_config, &pool_caps, &size,
      &min_buffers, &max_buffers);
  gst_query_set_nth_allocation_pool (query, 0, thiz->output_pool, size,
      min_buffers, max_buffers);

  return TRUE;
}

static gboolean
gst_msdkdec_decide_allocation (GstVideoDecoder * decoder, GstQuery * query)
{
  GstMsdkDec *thiz = GST_MSDKDEC (decoder);
  gboolean ret;

  if (thiz->output_pool)
    /* Renegotiating the pool while the element is running is not
       supported. If negotiation has happened once then keep deciding
       on that same pool until a close_decoder() happens. */
    return gst_msdkdec_decide_allocation_existing (decoder, query);

  if (!GST_VIDEO_DECODER_CLASS (parent_class)->decide_allocation (decoder,
          query))
    return FALSE;

  if (gst_query_find_allocation_meta (query, GST_MSDK_OPAQUE_META_API_TYPE,
          NULL))
    ret = gst_msdkdec_decide_allocation_opaque (decoder, query);
  else
    ret = gst_msdkdec_decide_allocation_sysmem (decoder, query);

  if (ret)
    ret = gst_msdkdec_init_decoder (thiz);

  if (ret)
    gst_query_parse_nth_allocation_pool (query, 0, &thiz->output_pool, NULL,
        NULL, NULL);

  return ret;
}

static gboolean
gst_msdkdec_flush (GstVideoDecoder * decoder)
{
  GstMsdkDec *thiz = GST_MSDKDEC (decoder);
  mfxSession session;
  mfxStatus status;
  guint i;
  MsdkDecTask *task;

  if (!thiz->context || !thiz->param || !thiz->decoder_initialized)
    return TRUE;

  session = msdk_context_get_session (thiz->context);
  status = MFXVideoDECODE_Reset (session, thiz->param);
  if (status < MFX_ERR_NONE) {
    GST_ERROR_OBJECT (thiz, "Reset failed (%s)",
        msdk_status_to_string (status));
    return FALSE;
  }

  for (i = 0; i < thiz->tasks->len; i++) {
    task = &g_array_index (thiz->tasks, MsdkDecTask, i);
    if (task->sync_point) {
      task->surface->Data.Locked--;
      task->sync_point = NULL;
    }
  }
  return TRUE;
}

static GstFlowReturn
gst_msdkdec_drain (GstVideoDecoder * decoder)
{
  GstMsdkDec *thiz = GST_MSDKDEC (decoder);
  GstFlowReturn flow;
  GstBuffer *buffer;
  MsdkDecTask *task;
  MsdkSurface *surface = NULL;
  mfxSession session;
  mfxStatus status;
  guint i;

  if (G_UNLIKELY (!thiz->context || !thiz->decoder_initialized))
    return GST_FLOW_OK;
  session = msdk_context_get_session (thiz->context);

  for (;;) {
    task = &g_array_index (thiz->tasks, MsdkDecTask, thiz->next_task);
    if (!gst_msdkdec_finish_task (thiz, task))
      return GST_FLOW_ERROR;
    if (!surface) {
      surface = get_surface (thiz);
      if (!surface)
        return GST_FLOW_ERROR;
      flow = allocate_decode_buffer (thiz, &buffer);
      if (flow != GST_FLOW_OK)
        return flow;

      surface_set_buffer (thiz, surface, buffer);
    }

    status =
        MFXVideoDECODE_DecodeFrameAsync (session, NULL, &surface->surface,
        &task->surface, &task->sync_point);
    if (G_LIKELY (status == MFX_ERR_NONE)) {
      task->surface->Data.Locked++;
      thiz->next_task = (thiz->next_task + 1) % thiz->tasks->len;
      surface = NULL;
    } else if (status == MFX_WRN_VIDEO_PARAM_CHANGED)
      continue;
    else if (status == MFX_WRN_DEVICE_BUSY) {
      /* If device is busy, wait 1ms and retry, as per MSDK's recomendation */
      g_usleep (1000);
    } else if (status == MFX_ERR_MORE_DATA)
      break;
    else if (status < MFX_ERR_NONE)
      return GST_FLOW_ERROR;
  }
  if (surface)
    free_surface (surface);

  for (i = 0; i < thiz->tasks->len; i++) {
    task = &g_array_index (thiz->tasks, MsdkDecTask, thiz->next_task);
    flow = gst_msdkdec_finish_task (thiz, task);
    if (flow != GST_FLOW_OK)
      return flow;
    thiz->next_task = (thiz->next_task + 1) % thiz->tasks->len;
  }
  return GST_FLOW_OK;
}

static GstFlowReturn
gst_msdkdec_finish (GstVideoDecoder * decoder)
{
  return gst_msdkdec_drain (decoder);
}

static void
gst_msdkdec_set_property (GObject * object, guint prop_id, const GValue * value,
    GParamSpec * pspec)
{
  GstMsdkDec *thiz = GST_MSDKDEC (object);
  GstState state;

  GST_OBJECT_LOCK (thiz);

  state = GST_STATE (thiz);
  if ((state != GST_STATE_READY && state != GST_STATE_NULL) &&
      !(pspec->flags & GST_PARAM_MUTABLE_PLAYING))
    goto wrong_state;

  switch (prop_id) {
    case PROP_HARDWARE:
      thiz->hardware = g_value_get_boolean (value);
      break;
    case PROP_ASYNC_DEPTH:
      thiz->async_depth = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (thiz);
  return;

  /* ERROR */
wrong_state:
  {
    GST_WARNING_OBJECT (thiz, "setting property in wrong state");
    GST_OBJECT_UNLOCK (thiz);
  }
}

static void
gst_msdkdec_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstMsdkDec *thiz = GST_MSDKDEC (object);

  GST_OBJECT_LOCK (thiz);
  switch (prop_id) {
    case PROP_HARDWARE:
      g_value_set_boolean (value, thiz->hardware);
      break;
    case PROP_ASYNC_DEPTH:
      g_value_set_uint (value, thiz->async_depth);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (thiz);
}

static void
gst_msdkdec_finalize (GObject * object)
{
  GstMsdkDec *thiz = GST_MSDKDEC (object);

  g_array_unref (thiz->surfaces);
  g_array_unref (thiz->tasks);
  g_ptr_array_unref (thiz->extra_params);
}

static void
gst_msdkdec_class_init (GstMsdkDecClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;
  GstVideoDecoderClass *decoder_class;

  gobject_class = G_OBJECT_CLASS (klass);
  element_class = GST_ELEMENT_CLASS (klass);
  decoder_class = GST_VIDEO_DECODER_CLASS (klass);

  gobject_class->set_property = gst_msdkdec_set_property;
  gobject_class->get_property = gst_msdkdec_get_property;
  gobject_class->finalize = gst_msdkdec_finalize;

  decoder_class->close = GST_DEBUG_FUNCPTR (gst_msdkdec_close);
  decoder_class->stop = GST_DEBUG_FUNCPTR (gst_msdkdec_stop);
  decoder_class->set_format = GST_DEBUG_FUNCPTR (gst_msdkdec_set_format);
  decoder_class->finish = GST_DEBUG_FUNCPTR (gst_msdkdec_finish);
  decoder_class->handle_frame = GST_DEBUG_FUNCPTR (gst_msdkdec_handle_frame);
  decoder_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_msdkdec_decide_allocation);
  decoder_class->flush = GST_DEBUG_FUNCPTR (gst_msdkdec_flush);
  decoder_class->drain = GST_DEBUG_FUNCPTR (gst_msdkdec_drain);

  g_object_class_install_property (gobject_class, PROP_HARDWARE,
      g_param_spec_boolean ("hardware", "Hardware", "Enable hardware decoders",
          PROP_HARDWARE_DEFAULT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_ASYNC_DEPTH,
      g_param_spec_uint ("async-depth", "Async Depth",
          "Depth of asynchronous pipeline",
          1, 20, PROP_ASYNC_DEPTH_DEFAULT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_static_pad_template (element_class, &src_factory);
}

static void
gst_msdkdec_init (GstMsdkDec * thiz)
{
  gst_video_info_init (&thiz->output_info);
  gst_video_info_init (&thiz->decode_pool_info);
  thiz->extra_params = g_ptr_array_new_with_free_func (g_free);
  thiz->surfaces = g_array_new (FALSE, TRUE, sizeof (MsdkSurface));
  g_array_set_clear_func (thiz->surfaces, clear_surface);
  thiz->tasks = g_array_new (FALSE, TRUE, sizeof (MsdkDecTask));
  thiz->hardware = PROP_HARDWARE_DEFAULT;
  thiz->async_depth = PROP_ASYNC_DEPTH_DEFAULT;
}
