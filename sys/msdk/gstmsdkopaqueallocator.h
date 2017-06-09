/* GStreamer Intel MSDK plugin
 * Copyright © 2017, Intel Corporation
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

#ifndef __GST_MSDKOPAQUEALLOCATOR_H__
#define __GST_MSDKOPAQUEALLOCATOR_H__

G_BEGIN_DECLS

#include <gst/gstallocator.h>
#include <mfxvideo.h>

#define GST_TYPE_MSDK_OPAQUE_ALLOCATOR                  (gst_msdk_opaque_allocator_get_type ())
#define GST_MSDK_OPAQUE_ALLOCATOR(obj)                  (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_MSDK_OPAQUE_ALLOCATOR, GstMsdkOpaqueAllocator))
#define GST_IS_MSDK_OPAQUE_ALLOCATOR(obj)               (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_MSDK_OPAQUE_ALLOCATOR))
#define GST_MSDK_OPAQUE_ALLOCATOR_CLASS(klass)          (G_TYPE_CHECK_CLASS_CAST ((klass), GST_TYPE_MSDK_OPAQUE_ALLOCATOR, GstMsdkOpaqueAllocatorClass))
#define GST_IS_MSDK_OPAQUE_ALLOCATOR_CLASS(klass)       (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_MSDK_OPAQUE_ALLOCATOR))
#define GST_MSDK_OPAQUE_ALLOCATOR_GET_CLASS(obj)        (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_MSDK_OPAQUE_ALLOCATOR, GstMsdkOpaqueAllocatorClass))

/* This meta can be added to an allocation query to indicate that the
 * downstream element supports Intel Media SDK opaque surfaces. The
 * corresponding parameter structure in the allocation query must
 * contain these parameter fields:
 *
 * "gst.msdk.opaque.num_surfaces" : uint - Number of surfaces that
 * must be allocated for this element.
 * "gst.msdk.opaque.type" : uint - Type bitfield of surfaces that this
 * element requires.
 */
#define GST_MSDK_OPAQUE_META_API_TYPE (gst_msdk_opaque_meta_api_get_type())

typedef struct _GstMsdkOpaqueMemory           GstMsdkOpaqueMemory;
typedef struct _GstMsdkOpaqueAllocator        GstMsdkOpaqueAllocator;
typedef struct _GstMsdkOpaqueAllocatorClass   GstMsdkOpaqueAllocatorClass;

struct _GstMsdkOpaqueMemory
{
  GstMemory mem;
  mfxFrameSurface1 *surface;
};

struct _GstMsdkOpaqueAllocator
{
  GstAllocator parent;

  mfxExtOpaqueSurfaceAlloc ext_buffer;
  gint num_allocated;
};

struct _GstMsdkOpaqueAllocatorClass
{
  GstAllocatorClass parent_class;
};

gboolean gst_is_msdk_opaque_memory (GstMemory * mem);
GType gst_msdk_opaque_allocator_get_type (void);
GType gst_msdk_opaque_meta_api_get_type (void);

GstMsdkOpaqueAllocator * gst_msdk_opaque_allocator_new (mfxFrameSurface1 ** surfaces, mfxU16 num_surfaces, mfxU16 surface_type);
mfxExtBuffer * gst_msdk_opaque_allocator_ext_buffer (GstMsdkOpaqueAllocator * allocator);

G_END_DECLS

#endif /* __GST_MSDKOPAQUEALLOCATOR_H__ */
