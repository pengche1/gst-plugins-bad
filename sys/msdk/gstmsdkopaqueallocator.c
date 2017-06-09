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

#include <string.h>
#include <gst/gstmeta.h>

#include "gstmsdkopaqueallocator.h"

#define GST_MSDK_OPAQUE_MEMORY_TYPE "MsdkOpaqueMemory"

GType
gst_msdk_opaque_meta_api_get_type (void)
{
  static volatile GType type = 0;
  static const gchar *tags[] = { NULL };

  if (g_once_init_enter (&type)) {
    GType _type = gst_meta_api_type_register ("GstMsdkOpaqueMetaAPI", tags);
    g_once_init_leave (&type, _type);
  }

  return type;
}

G_DEFINE_TYPE (GstMsdkOpaqueAllocator, gst_msdk_opaque_allocator,
    GST_TYPE_ALLOCATOR);

static GstMemory *
gst_msdk_opaque_allocator_alloc (GstAllocator * gallocator, gsize size,
    GstAllocationParams * params)
{
  GstMsdkOpaqueAllocator *allocator = GST_MSDK_OPAQUE_ALLOCATOR (gallocator);
  GstMsdkOpaqueMemory *mem;

  if (allocator->num_allocated >= allocator->ext_buffer.Out.NumSurface)
    return NULL;

  mem = g_slice_new0 (GstMsdkOpaqueMemory);
  gst_memory_init (GST_MEMORY_CAST (mem), params->flags, gallocator, NULL, 1, 1,
      0, 1);
  mem->surface = allocator->ext_buffer.Out.Surfaces[allocator->num_allocated++];

  return GST_MEMORY_CAST (mem);
}

static void
gst_msdk_opaque_allocator_free (GstAllocator * allocator, GstMemory * memory)
{
  GstMsdkOpaqueMemory *mem = (GstMsdkOpaqueMemory *) memory;

  g_slice_free (GstMsdkOpaqueMemory, mem);
}

static GstMemory *
gst_msdk_opaque_allocator_mem_copy (GstMemory * mem, gssize offset, gssize size)
{
  return NULL;
}

static GstMemory *
gst_msdk_opaque_allocator_mem_share (GstMemory * mem, gssize offset,
    gssize size)
{
  return NULL;
}

static gboolean
gst_msdk_opaque_allocator_mem_is_span (GstMemory * mem1, GstMemory * mem2,
    gsize * offset)
{
  return FALSE;
}

static gpointer
gst_msdk_opaque_allocator_mem_map_full (GstMemory * mem, GstMapInfo * info,
    gsize maxsize)
{
  return NULL;
}

static void
gst_msdk_opaque_allocator_mem_unmap_full (GstMemory * mem, GstMapInfo * info)
{
  return;
}

static void
gst_msdk_opaque_allocator_class_init (GstMsdkOpaqueAllocatorClass * klass)
{
  GstAllocatorClass *allocator_class = GST_ALLOCATOR_CLASS (klass);

  allocator_class->alloc = gst_msdk_opaque_allocator_alloc;
  allocator_class->free = gst_msdk_opaque_allocator_free;
}

static void
gst_msdk_opaque_allocator_init (GstMsdkOpaqueAllocator * self)
{
  GstAllocator *allocator = GST_ALLOCATOR (self);

  allocator->mem_copy = gst_msdk_opaque_allocator_mem_copy;
  allocator->mem_share = gst_msdk_opaque_allocator_mem_share;
  allocator->mem_is_span = gst_msdk_opaque_allocator_mem_is_span;
  allocator->mem_map_full = gst_msdk_opaque_allocator_mem_map_full;
  allocator->mem_unmap_full = gst_msdk_opaque_allocator_mem_unmap_full;
}

gboolean
gst_is_msdk_opaque_memory (GstMemory * mem)
{
  return gst_memory_is_type (mem, GST_MSDK_OPAQUE_MEMORY_TYPE);
}

GstMsdkOpaqueAllocator *
gst_msdk_opaque_allocator_new (mfxFrameSurface1 ** surfaces,
    mfxU16 num_surfaces, mfxU16 surface_type)
{
  GstMsdkOpaqueAllocator *allocator;
  allocator = g_object_new (GST_TYPE_MSDK_OPAQUE_ALLOCATOR, NULL);

  allocator->ext_buffer.Header.BufferId = MFX_EXTBUFF_OPAQUE_SURFACE_ALLOCATION;
  allocator->ext_buffer.Header.BufferSz = sizeof (allocator->ext_buffer);
  allocator->ext_buffer.Out.Surfaces = surfaces;
  allocator->ext_buffer.Out.Type = surface_type;
  allocator->ext_buffer.Out.NumSurface = num_surfaces;
  memcpy (&allocator->ext_buffer.In, &allocator->ext_buffer.Out,
      sizeof (allocator->ext_buffer.In));

  return allocator;
}

mfxExtBuffer *
gst_msdk_opaque_allocator_ext_buffer (GstMsdkOpaqueAllocator * allocator)
{
  return &allocator->ext_buffer.Header;
}
