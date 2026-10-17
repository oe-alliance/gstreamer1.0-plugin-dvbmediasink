/*
 * flv2mpeg4: H.263 and Sorenson Spark to MPEG-4 part 2 without decoding,
 * for hardware decoders that know MPEG-4 but neither of these. The
 * pictures are rewritten by the VIXY FLV converter in flv2mpeg4/.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <gst/gst.h>

#include "flv2mpeg4/flv2mpeg4.h"

GST_DEBUG_CATEGORY_STATIC(flv2mpeg4_debug);
#define GST_CAT_DEFAULT flv2mpeg4_debug

#define GST_TYPE_FLV2MPEG4 (gst_flv2mpeg4_get_type())
G_DECLARE_FINAL_TYPE(GstFlv2Mpeg4, gst_flv2mpeg4, GST, FLV2MPEG4, GstElement)

struct _GstFlv2Mpeg4
{
	GstElement element;
	GstPad *sinkpad, *srcpad;
	flv2mpeg4_CTX *ctx;
	gboolean itu;
	gint width, height;
	gint fps_n, fps_d;
	GstBuffer *vol;
	GByteArray *out;
	/* the segment waits for the caps of the first picture */
	GstEvent *segment;
};

G_DEFINE_TYPE(GstFlv2Mpeg4, gst_flv2mpeg4, GST_TYPE_ELEMENT)

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
	GST_STATIC_CAPS("video/x-flash-video, flvversion = (int) 1; video/x-h263, variant = (string) itu"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS,
	GST_STATIC_CAPS("video/mpeg, mpegversion = (int) 4, systemstream = (boolean) false, unpacked = (boolean) true, parsed = (boolean) true"));

static int write_packet(void *data, int keyframe, int pts, const uint8_t *buf, int size)
{
	GstFlv2Mpeg4 *self = data;
	g_byte_array_append(self->out, buf, size);
	return 0;
}

static int write_vol(void *data, int width, int height, int bitrate, const uint8_t *buf, int size)
{
	GstFlv2Mpeg4 *self = data;
	if (self->vol)
		gst_buffer_unref(self->vol);
	self->vol = gst_buffer_new_memdup(buf, size);
	return 0;
}

static void release(GstFlv2Mpeg4 *self)
{
	if (self->ctx)
		flv2mpeg4_release_ctx(&self->ctx);
	if (self->vol)
		gst_buffer_unref(self->vol);
	self->vol = NULL;
	self->width = self->height = 0;
}

/* a new converter and new caps whenever the coded size changes */
static gboolean setup(GstFlv2Mpeg4 *self, gint width, gint height)
{
	GstCaps *caps;
	gboolean ret;
	release(self);
	self->ctx = flv2mpeg4_init_ctx(self, width, height, write_packet, write_vol);
	flv2mpeg4_set_h263(self->ctx, self->itu);
	flv2mpeg4_prepare_extra_data(self->ctx);
	self->width = width;
	self->height = height;
	caps = gst_caps_new_simple("video/mpeg",
		"mpegversion", G_TYPE_INT, 4,
		"systemstream", G_TYPE_BOOLEAN, FALSE,
		"unpacked", G_TYPE_BOOLEAN, TRUE,
		"parsed", G_TYPE_BOOLEAN, TRUE,
		"width", G_TYPE_INT, width,
		"height", G_TYPE_INT, height,
		"codec_data", GST_TYPE_BUFFER, self->vol, NULL);
	if (self->fps_n > 0 && self->fps_d > 0)
		gst_caps_set_simple(caps, "framerate", GST_TYPE_FRACTION, self->fps_n, self->fps_d, NULL);
	GST_INFO_OBJECT(self, "%s %dx%d", self->itu ? "H.263" : "Sorenson Spark", width, height);
	ret = gst_pad_set_caps(self->srcpad, caps);
	gst_caps_unref(caps);
	if (ret && self->segment)
	{
		gst_pad_push_event(self->srcpad, self->segment);
		self->segment = NULL;
	}
	return ret;
}

static GstFlowReturn gst_flv2mpeg4_chain(GstPad *pad, GstObject *parent, GstBuffer *buffer)
{
	GstFlv2Mpeg4 *self = GST_FLV2MPEG4(parent);
	GstFlowReturn ret = GST_FLOW_OK;
	GstMapInfo map;
	GstBuffer *out;
	guint8 *padded;
	gint width = 0, height = 0;

	if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
	{
		gst_buffer_unref(buffer);
		return GST_FLOW_OK;
	}
	/* the bit reader reads 4 bytes ahead */
	padded = g_malloc0(map.size + 8);
	memcpy(padded, map.data, map.size);
	if (flv2mpeg4_picture_size(self->itu, padded, map.size, &width, &height) < 0 || width <= 0 || height <= 0)
	{
		/* H.263 options like UMV or PB frames are not supported */
		GST_WARNING_OBJECT(self, "dropping an unsupported picture of %" G_GSIZE_FORMAT " bytes", map.size);
		goto done;
	}
	if ((width != self->width || height != self->height) && !setup(self, width, height))
	{
		ret = GST_FLOW_NOT_NEGOTIATED;
		goto done;
	}
	g_byte_array_set_size(self->out, 0);
	if (flv2mpeg4_process_flv_packet(self->ctx, 0, padded, map.size, 0) < 0 || !self->out->len)
	{
		GST_WARNING_OBJECT(self, "dropping a picture the converter refused");
		goto done;
	}
	out = gst_buffer_new_memdup(self->out->data, self->out->len);
	gst_buffer_copy_into(out, buffer, GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS | GST_BUFFER_COPY_META, 0, -1);
	gst_buffer_unmap(buffer, &map);
	gst_buffer_unref(buffer);
	g_free(padded);
	return gst_pad_push(self->srcpad, out);
done:
	gst_buffer_unmap(buffer, &map);
	gst_buffer_unref(buffer);
	g_free(padded);
	return ret;
}

static gboolean gst_flv2mpeg4_sink_event(GstPad *pad, GstObject *parent, GstEvent *event)
{
	GstFlv2Mpeg4 *self = GST_FLV2MPEG4(parent);
	if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS)
	{
		GstCaps *caps;
		GstStructure *s;
		gst_event_parse_caps(event, &caps);
		s = gst_caps_get_structure(caps, 0);
		self->itu = gst_structure_has_name(s, "video/x-h263");
		if (!gst_structure_get_fraction(s, "framerate", &self->fps_n, &self->fps_d))
			self->fps_n = self->fps_d = 0;
		/* the source caps follow with the size of the first picture */
		release(self);
		gst_event_unref(event);
		return TRUE;
	}
	if (GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT && !gst_pad_has_current_caps(self->srcpad))
	{
		gst_event_replace(&self->segment, event);
		gst_event_unref(event);
		return TRUE;
	}
	return gst_pad_event_default(pad, parent, event);
}

static GstStateChangeReturn gst_flv2mpeg4_change_state(GstElement *element, GstStateChange transition)
{
	GstStateChangeReturn ret = GST_ELEMENT_CLASS(gst_flv2mpeg4_parent_class)->change_state(element, transition);
	if (transition == GST_STATE_CHANGE_PAUSED_TO_READY)
	{
		release(GST_FLV2MPEG4(element));
		gst_event_replace(&GST_FLV2MPEG4(element)->segment, NULL);
	}
	return ret;
}

static void gst_flv2mpeg4_finalize(GObject *object)
{
	GstFlv2Mpeg4 *self = GST_FLV2MPEG4(object);
	release(self);
	gst_event_replace(&self->segment, NULL);
	g_byte_array_unref(self->out);
	G_OBJECT_CLASS(gst_flv2mpeg4_parent_class)->finalize(object);
}

static void gst_flv2mpeg4_class_init(GstFlv2Mpeg4Class *klass)
{
	GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
	G_OBJECT_CLASS(klass)->finalize = gst_flv2mpeg4_finalize;
	element_class->change_state = GST_DEBUG_FUNCPTR(gst_flv2mpeg4_change_state);
	gst_element_class_add_static_pad_template(element_class, &sink_template);
	gst_element_class_add_static_pad_template(element_class, &src_template);
	gst_element_class_set_static_metadata(element_class, "H.263 to MPEG-4 converter",
		"Codec/Parser/Converter/Video",
		"Rewrites H.263 and Sorenson Spark pictures as MPEG-4 part 2 without decoding them",
		"openATV");
	GST_DEBUG_CATEGORY_INIT(flv2mpeg4_debug, "flv2mpeg4", 0, "H.263 to MPEG-4 converter");
}

static void gst_flv2mpeg4_init(GstFlv2Mpeg4 *self)
{
	self->out = g_byte_array_new();
	self->sinkpad = gst_pad_new_from_static_template(&sink_template, "sink");
	gst_pad_set_chain_function(self->sinkpad, GST_DEBUG_FUNCPTR(gst_flv2mpeg4_chain));
	gst_pad_set_event_function(self->sinkpad, GST_DEBUG_FUNCPTR(gst_flv2mpeg4_sink_event));
	gst_element_add_pad(GST_ELEMENT(self), self->sinkpad);
	self->srcpad = gst_pad_new_from_static_template(&src_template, "src");
	gst_pad_use_fixed_caps(self->srcpad);
	gst_element_add_pad(GST_ELEMENT(self), self->srcpad);
}

static gboolean plugin_init(GstPlugin *plugin)
{
	/* above the software decoders: a sink that takes H.263 or Spark itself
	 * gets the stream unchanged anyway */
	return gst_element_register(plugin, "flv2mpeg4", GST_RANK_PRIMARY, GST_TYPE_FLV2MPEG4);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, flv2mpeg4, "H.263 to MPEG-4 converter",
	plugin_init, VERSION, "LGPL", "GStreamer", "http://gstreamer.net/")
