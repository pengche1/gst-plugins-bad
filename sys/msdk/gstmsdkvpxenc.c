/* GStreamer Intel MSDK plugin
 * Copyright (c) 2016, Oblong Industries, Inc.
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
#include <mfxplugin.h>

#include "gstmsdkvpxenc.h"

GST_DEBUG_CATEGORY_EXTERN (gst_msdkvpxenc_debug);
#define GST_CAT_DEFAULT gst_msdkvpxenc_debug

#define gst_msdkvpxenc_parent_class parent_class
G_DEFINE_TYPE (GstMsdkVPXEnc, gst_msdkvpxenc, GST_TYPE_MSDKENC);

static gboolean
gst_msdkvpxenc_set_format (GstMsdkEnc * encoder)
{
  GstMsdkVPXEnc *thiz = GST_MSDKVPXENC (encoder);
  GstMsdkVPXEncClass *klass = GST_MSDKVPXENC_GET_CLASS (thiz);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstCaps *template_caps;
  GstCaps *allowed_caps = NULL;

  thiz->profile = 0;

  template_caps =
      gst_pad_template_get_caps (gst_element_class_get_pad_template
      (element_class, "sink"));
  allowed_caps = gst_pad_get_allowed_caps (GST_VIDEO_ENCODER_SRC_PAD (encoder));

  /* If downstream has ANY caps let encoder decide profile and level */
  if (allowed_caps == template_caps) {
    GST_INFO_OBJECT (thiz,
        "downstream has ANY caps, profile/level set to auto");
  } else if (allowed_caps) {
    GstStructure *s;
    const gchar *profile;

    if (gst_caps_is_empty (allowed_caps)) {
      gst_caps_unref (allowed_caps);
      gst_caps_unref (template_caps);
      return FALSE;
    }

    allowed_caps = gst_caps_make_writable (allowed_caps);
    allowed_caps = gst_caps_fixate (allowed_caps);
    s = gst_caps_get_structure (allowed_caps, 0);

    profile = gst_structure_get_string (s, "profile");
    if (profile)
      thiz->profile = g_ascii_strtoull (profile, NULL, 10) + 1;

    gst_caps_unref (allowed_caps);
  }

  gst_caps_unref (template_caps);

  return TRUE;
}

static GstCaps *
gst_msdkvpxenc_set_src_caps (GstMsdkEnc * encoder)
{
  GstCaps *caps;
  GstStructure *structure;
  gchar *profile = NULL;
  GstMsdkVPXEncClass *klass = GST_MSDKVPXENC_CLASS (encoder);

  caps = gst_caps_new_empty_simple (klass->media_type ());
  structure = gst_caps_get_structure (caps, 0);

  profile = g_strdup_printf ("%d", encoder->param.mfx.CodecProfile - 1);
  if (profile) {
    gst_structure_set (structure, "profile", G_TYPE_STRING, profile, NULL);
    g_free (profile);
  }

  return caps;
}

static void
gst_msdkvpxenc_class_init (GstMsdkVPXEncClass * klass)
{
  GstMsdkEncClass *encoder_class;

  encoder_class = GST_MSDKENC_CLASS (klass);

  encoder_class->set_format = gst_msdkvpxenc_set_format;
  encoder_class->set_src_caps = gst_msdkvpxenc_set_src_caps;
}

static void
gst_msdkvpxenc_init (GstMsdkVPXEnc * thiz)
{
}
