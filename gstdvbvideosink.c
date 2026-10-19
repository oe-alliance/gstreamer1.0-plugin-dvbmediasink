/*
 * GStreamer DVB Media Sink
 *
 * Copyright 2011 <slashdev@gmx.net>
 *
 * based on code by:
 * Copyright 2006 Felix Domke <tmbinc@elitedvb.net>
 * Copyright 2005 Thomas Vander Stichele <thomas@apestaart.org>
 * Copyright 2005 Ronald S. Bultje <rbultje@ronald.bitfreak.net>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Alternatively, the contents of this file may be used under the
 * GNU Lesser General Public License Version 2.1 (the "LGPL"), in
 * which case the following provisions apply instead of the ones
 * mentioned above:
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:element-plugin
 *
 * <refsect2>
 * <title>Example launch line</title>
 * <para>
 * <programlisting>
 * gst-launch -v -m audiotestsrc ! plugin ! fakesink silent=TRUE
 * </programlisting>
 * </para>
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef __sh__
#include <linux/dvb/stm_ioctls.h>
#endif

#include <gst/gst.h>
#include <gst/base/gstbasesink.h>

#include "common.h"
#include "gstdvbvideosink.h"
#include "gstdvbsink-marshal.h"

#ifndef VIDEO_SET_CODEC_DATA
typedef struct video_codec_data
{
	int length;
	guint8 *data;
} video_codec_data_t;
#define VIDEO_SET_CODEC_DATA _IOW('o', 80, video_codec_data_t)
#endif

#ifdef DREAMBOX
static int readMpegProc(const char *str, int decoder)
{
	int val = -1;
	char tmp[64];
	sprintf(tmp, "/proc/stb/vmpeg/%d/%s", decoder, str);
	FILE *f = fopen(tmp, "r");
	if(f)
	{
		fscanf(f, "%x", &val);
		fclose(f);
	}
	return val;
}

static int readApiSize(int fd, int *xres, int *yres, int *aspect)
{
	video_size_t size;
	if(!ioctl(fd, VIDEO_GET_SIZE, &size))
	{
		*xres = size.w;
		*yres = size.h;
		*aspect = size.aspect_ratio == 0 ? 2 : 3;  // convert dvb api to etsi
		return 0;
	}
	return -1;
}

static int readApiFrameRate(int fd, int *framerate)
{
	unsigned int frate;
	if(!ioctl(fd, VIDEO_GET_FRAME_RATE, &frate))
	{
		*framerate = frate;
		return 0;
	}
	return -1;
}
#endif

GST_DEBUG_CATEGORY_STATIC (dvbvideosink_debug);
#define GST_CAT_DEFAULT dvbvideosink_debug

#ifdef DREAMNEXTGEN
/* The Dream decoder of dreamone/dreamtwo refuses PES writes. It takes the
 * decoder setup by VIDEO_SET_DEC_SYSINFO and whole access units by
 * VIDEO_SET_FRAME. */
struct dream_dec_sysinfo
{
	uint32_t format;
	uint32_t width;
	uint32_t height;
	uint32_t rate; /* frame duration in 96 kHz ticks */
	uint32_t extra;
	uint32_t status;
	uint32_t ratio;
	void *param;
	uint64_t ratio64;
};

struct dream_video_frame
{
	uint64_t pts; /* 90 kHz */
	ssize_t bytes[8];
	const uint8_t *data[8];
	int is_phys_addr[8];
};

G_STATIC_ASSERT(sizeof(struct dream_dec_sysinfo) == 48);
G_STATIC_ASSERT(sizeof(struct dream_video_frame) == 168);

#define DREAM_VIDEO_SET_FRAME _IOWR('o', 64, struct dream_video_frame)
#define DREAM_VIDEO_SET_DEC_SYSINFO _IOWR('o', 65, struct dream_dec_sysinfo)
#define DREAM_EVENT_PTS_VALID 0x20
#define DREAM_PTS_MASK 0x1ffffffffULL
#define DREAM_PTS_NONE G_MAXUINT64
/* a write which waits long for room ends in a driver write timeout, after
 * which the driver drops every frame until a flush: write at most this far
 * ahead of the frame on screen */
#define DREAM_LEAD (2 * 90000)
#define DREAM_STALL_MS 1200

typedef struct
{
	GByteArray *data;
	guint64 pts;
	guint offset;
} dream_frame_t;

static guint64 dream_tsync(const char *name)
{
	char path[64], buf[32];
	guint64 value = 0;
	snprintf(path, sizeof(path), "/sys/class/tsync/%s", name);
	FILE *f = fopen(path, "r");
	if (f)
	{
		if (fgets(buf, sizeof(buf), f))
			value = g_ascii_strtoull(buf, NULL, 16);
		fclose(f);
	}
	return value;
}

static int dream_read_int(const char *path)
{
	int value = -1;
	FILE *f = fopen(path, "r");
	if (f)
	{
		if (fscanf(f, "%d", &value) != 1)
			value = -1;
		fclose(f);
	}
	return value;
}

static gboolean dream_codec(GstDVBVideoSink *self, int *streamtype, uint32_t *format)
{
	switch (self->codec_type)
	{
	case CT_MPEG1:
	case CT_MPEG2:
		*streamtype = 0;
		*format = 0;
		return TRUE;
	case CT_H264:
		*streamtype = 1;
		*format = 4;
		return TRUE;
	case CT_H265:
		*streamtype = 22;
		*format = 15;
		return TRUE;
	case CT_MPEG4_PART2:
		/* H.263 comes as MPEG-4 from the flv2mpeg4 element */
		if (self->stream_type == STREAMTYPE_H263)
			return FALSE;
		*streamtype = 4;
		*format = 3;
		return TRUE;
	case CT_DIVX4:
		*streamtype = 4;
		*format = 2;
		return TRUE;
	case CT_DIVX311:
		*streamtype = 4;
		*format = 1;
		return TRUE;
	case CT_VP9:
		*streamtype = 23;
		*format = 16;
		return TRUE;
	case CT_VC1:
		*streamtype = 3;
		*format = 11;
		return TRUE;
	case CT_VC1_SM:
		*streamtype = 3;
		*format = 10;
		return TRUE;
	case CT_MJPEG:
		*streamtype = 5;
		*format = 5;
		return TRUE;
	case CT_REAL:
		/* the format of RealVideo 3, dream_setup takes 9 for version 4 */
		*streamtype = 31;
		*format = 8;
		return TRUE;
	case CT_AVS:
		*streamtype = 7;
		*format = 13;
		return TRUE;
	default:
		/* set_caps leaves the codec type of DivX 5/6 alone */
		if (self->stream_type != STREAMTYPE_DIVX5)
			return FALSE;
		*streamtype = 4;
		*format = 3;
		return TRUE;
	}
}

/* codecs whose access units the Dream decoder takes in its own framing */
static gboolean dream_raw_codec(GstDVBVideoSink *self)
{
	return self->codec_type == CT_VP9 || self->codec_type == CT_DIVX311 || self->codec_type == CT_VC1 || self->codec_type == CT_VC1_SM ||
		self->codec_type == CT_MJPEG || self->codec_type == CT_REAL;
}

static gboolean dream_setup(GstDVBVideoSink *self, GstStructure *structure)
{
	struct dream_dec_sysinfo sysinfo;
	int streamtype, numerator, denominator, width = 0, height = 0;
	uint32_t format;
	const GValue *codec_data = gst_structure_get_value(structure, "codec_data");
	const gchar *wmv = gst_structure_get_string(structure, "format");
	if (self->fd < 0 || !dream_codec(self, &streamtype, &format))
		return FALSE;
	/* the decoder knows WMV9 and VC-1 advanced profile only */
	if ((self->codec_type == CT_VC1 && g_strcmp0(wmv, "WVC1")) ||
		(self->codec_type == CT_VC1_SM && g_strcmp0(wmv, "WMV3") && g_strcmp0(wmv, "WVC3")))
		return FALSE;
	gst_structure_get_int(structure, "width", &width);
	gst_structure_get_int(structure, "height", &height);
	if (self->headers)
		gst_buffer_unref(self->headers);
	self->headers = codec_data && GST_VALUE_HOLDS_BUFFER(codec_data) ? gst_buffer_ref(gst_value_get_buffer(codec_data)) : NULL;
	self->width = width;
	self->height = height;
	if (self->codec_type == CT_VC1_SM && (!self->headers || gst_buffer_get_size(self->headers) < 4))
		return FALSE;
	if (self->codec_type == CT_DIVX311 && (width <= 0 || height <= 0 || width > 4095 || height > 4095))
		return FALSE;
	if (self->codec_type == CT_REAL)
	{
		int version = 0;
		gst_structure_get_int(structure, "rmversion", &version);
		if (version != 3 && version != 4)
			return FALSE;
		format = version == 3 ? 8 : 9;
	}
	memset(&sysinfo, 0, sizeof(sysinfo));
	sysinfo.format = format;
	sysinfo.width = width;
	sysinfo.height = height;
	if (gst_structure_get_fraction(structure, "framerate", &numerator, &denominator) && numerator > 0 && denominator > 0)
		sysinfo.rate = gst_util_uint64_scale(96000, denominator, numerator);
	else
		sysinfo.rate = 3200;
	/* param stays NULL, other values wedge the decoder */
	if (ioctl(self->fd, DREAM_VIDEO_SET_DEC_SYSINFO, &sysinfo) < 0)
	{
		GST_ERROR_OBJECT(self, "VIDEO_SET_DEC_SYSINFO format %u failed: %s", format, g_strerror(errno));
		return FALSE;
	}
	GST_INFO_OBJECT(self, "decoder format %u streamtype %d %ux%u rate %u", format, streamtype, sysinfo.width, sysinfo.height, sysinfo.rate);
	return ioctl(self->fd, VIDEO_SET_STREAMTYPE, streamtype) >= 0;
}

static void dream_post(GstDVBVideoSink *self, GstStructure *s)
{
	gst_element_post_message(GST_ELEMENT(self), gst_message_new_element(GST_OBJECT(self), s));
}

/* the driver sends no size or frame rate events */
static void dream_post_info(GstDVBVideoSink *self)
{
	int width = dream_read_int("/sys/class/video/frame_width");
	int height = dream_read_int("/sys/class/video/frame_height");
	int framerate = dream_read_int("/proc/stb/vmpeg/0/frame_rate");
	int progressive = dream_read_int("/proc/stb/vmpeg/0/progressive");
	int aspect = 2, w, h;
	if (readApiSize(self->fd, &w, &h, &aspect) < 0)
		aspect = 2;
	if (width > 0 && height > 0 && (width != self->info_width || height != self->info_height || aspect != self->info_aspect))
	{
		self->info_width = width;
		self->info_height = height;
		self->info_aspect = aspect;
		dream_post(self, gst_structure_new("eventSizeChanged",
			"aspect_ratio", G_TYPE_INT, aspect,
			"width", G_TYPE_INT, width,
			"height", G_TYPE_INT, height, NULL));
	}
	/* the scaled PTS of a trick mode give a frame rate of their own, it
	 * would switch the video mode */
	if (framerate > 0 && framerate != self->info_framerate && self->rate == 1.0)
	{
		self->info_framerate = framerate;
		dream_post(self, gst_structure_new("eventFrameRateChanged",
			"frame_rate", G_TYPE_INT, framerate, NULL));
	}
	if ((progressive == 0 || progressive == 1) && progressive != self->info_progressive)
	{
		self->info_progressive = progressive;
		dream_post(self, gst_structure_new("eventProgressiveChanged",
			"progressive", G_TYPE_INT, progressive, NULL));
	}
}

static void dream_read_events(GstDVBVideoSink *self)
{
	int i;
	for (i = 0; i < 16; i++)
	{
		struct video_event evt;
		/* an empty event queue answers with type 0, not with an error */
		if (ioctl(self->fd, VIDEO_GET_EVENT, &evt) < 0 || !evt.type)
			break;
		if (evt.type == DREAM_EVENT_PTS_VALID)
			dream_post_info(self);
	}
}

static void dream_read_commands(GstDVBVideoSink *self)
{
	gchar command;
	while (read(self->unlockfd[0], &command, 1) > 0)
		;
}

static void dream_frame_free(gpointer data)
{
	dream_frame_t *entry = data;
	g_byte_array_unref(entry->data);
	g_free(entry);
}

/* the 33 bit distance from base to pts */
static gint64 dream_pts_diff(guint64 pts, guint64 base)
{
	gint64 diff = (gint64)((pts - base) & DREAM_PTS_MASK);
	return diff >= (1LL << 32) ? diff - (1LL << 33) : diff;
}

/* A trick mode plays at the decoder's normal pace, the PTS are scaled by the
 * rate from the first one; backwards they are mirrored and keep rising. */
static guint64 dream_trick_pts(GstDVBVideoSink *self, guint64 pts)
{
	if (self->rate == 1.0 || pts == DREAM_PTS_NONE)
		return pts;
	if (self->trick_base == DREAM_PTS_NONE)
		self->trick_base = pts;
	return (self->trick_base + (gint64)(dream_pts_diff(pts, self->trick_base) / self->rate)) & DREAM_PTS_MASK;
}

static void dream_flush(GstDVBVideoSink *self)
{
	g_queue_clear_full(&self->frames, dream_frame_free);
	if (self->frame)
	{
		g_byte_array_unref(self->frame);
		self->frame = NULL;
	}
	self->flush_vpts = dream_tsync("pts_video");
	self->start_pts = DREAM_PTS_NONE;
	/* after a seek the decoder starts at a keyframe */
	self->wait_keyframe = TRUE;
	self->trick_base = DREAM_PTS_NONE;
}

/* collect the PES writes of one access unit, drop the PES header itself */
static void dream_collect(GstDVBVideoSink *self, GstBuffer *buffer, size_t start, size_t end)
{
	GstMapInfo map;
	if (end <= start || !gst_buffer_map(buffer, &map, GST_MAP_READ))
		return;
	if (!self->frame)
	{
		self->frame = g_byte_array_new();
		self->frame_pts = DREAM_PTS_NONE;
	}
	if (buffer == self->pesheader_buffer && start == 0 && end >= 9)
	{
		guint8 *pes = map.data;
		if ((pes[7] & 0x80) && self->frame_pts == DREAM_PTS_NONE)
			self->frame_pts = ((guint64)(pes[9] & 0x0e) << 29 | pes[10] << 22 | (pes[11] & 0xfe) << 14 | pes[12] << 7 | pes[13] >> 1) & DREAM_PTS_MASK;
		/* codec headers and start codes follow the PES header */
		start = 9 + pes[8];
	}
	if (end > start)
		g_byte_array_append(self->frame, map.data + start, end - start);
	gst_buffer_unmap(buffer, &map);
}

/* Hold the frame until it is at most DREAM_LEAD ahead of the frame on screen.
 * Returns FALSE when a pause, unlock or flush interrupts the wait. */
static gboolean dream_wait_lead(GstDVBVideoSink *self, guint64 pts)
{
	guint64 last_vpts = 0;
	int still_ms = 0;
	struct pollfd pfd;
	pfd.fd = self->unlockfd[0];
	pfd.events = POLLIN;
	while (!self->flushing && !self->paused && !self->unlocking)
	{
		guint64 vpts = dream_tsync("pts_video");
		/* until the first picture shows, the lead counts from the first frame */
		guint64 ref = vpts == self->flush_vpts ? self->start_pts : vpts;
		gint32 lead, behind;
		if (pts == DREAM_PTS_NONE || ref == DREAM_PTS_NONE)
			return TRUE;
		/* after a jump the audio may set the clock ahead of the first picture,
		 * which then waits for the frames up to the clock */
		behind = (gint32)((guint32)dream_tsync("pts_pcrscr") - (guint32)ref);
		if (behind > 0 && behind < 10 * 90000)
			ref += behind;
		lead = (gint32)((guint32)pts - (guint32)ref);
		/* far off is a jump, the decoder sorts it out */
		if (lead <= DREAM_LEAD || lead > 10 * 90000)
			return TRUE;
		still_ms = vpts == last_vpts ? still_ms + 10 : 0;
		if (still_ms > DREAM_STALL_MS)
			return TRUE;
		last_vpts = vpts;
		if (poll(&pfd, 1, 10) > 0)
			dream_read_commands(self);
	}
	return FALSE;
}

/* 1: written, 0: interrupted, keep the frame, -1: error */
static int dream_write(GstDVBVideoSink *self, dream_frame_t *entry)
{
	struct dream_video_frame frame;
	struct pollfd pfd[3];
	guint64 last_vpts = 0, last_pcr = 0;
	gint64 since = g_get_monotonic_time();
	int nfds = 2;

	if (!dream_wait_lead(self, entry->pts))
		return 0;

	pfd[0].fd = self->unlockfd[0];
	pfd[0].events = POLLIN;
	pfd[1].fd = self->fd;
	pfd[1].events = POLLIN;
	if (self->poll_fd >= 0)
	{
		pfd[2].fd = self->poll_fd;
		pfd[2].events = POLLIN;
		nfds = 3;
	}
	memset(&frame, 0, sizeof(frame));
	while (entry->offset < entry->data->len)
	{
		int ret;
		if (self->flushing || self->paused || self->unlocking)
			return 0;
		/* the driver takes large frames in parts, the rest goes without a PTS */
		frame.pts = entry->offset ? DREAM_PTS_NONE : entry->pts;
		frame.bytes[0] = entry->data->len - entry->offset;
		frame.data[0] = entry->data->data + entry->offset;
		ret = ioctl(self->fd, DREAM_VIDEO_SET_FRAME, &frame);
		if (ret > frame.bytes[0])
		{
			GST_ERROR_OBJECT(self, "VIDEO_SET_FRAME took %d of %zd bytes", ret, frame.bytes[0]);
			return -1;
		}
		if (ret > 0)
		{
			entry->offset += ret;
			continue;
		}
		if (ret < 0 && errno != EAGAIN && errno != EINTR)
		{
			/* a decoder the driver failed to start refuses every frame: drop
			 * it, the audio plays on */
			if (!self->frames_refused++)
				GST_WARNING_OBJECT(self, "VIDEO_SET_FRAME of %u bytes failed: %s, dropping the video", entry->data->len, g_strerror(errno));
			return 1;
		}
		/* wait for the decoder, a dropped frame breaks the reference chain */
		if (poll(pfd, nfds, 50) > 0 && (pfd[0].revents & POLLIN))
			dream_read_commands(self);
		dream_read_events(self);
		{
			/* the PTS stands still while the clock runs: the decoder hangs */
			guint64 vpts = dream_tsync("pts_video"), pcr = dream_tsync("pts_pcrscr");
			gint64 now = g_get_monotonic_time();
			if (vpts != last_vpts)
			{
				last_vpts = vpts;
				last_pcr = pcr;
				since = now;
			}
			else if (now - since > DREAM_STALL_MS * 1000 && (gint64)(pcr - last_pcr) > DREAM_STALL_MS * 90)
			{
				GST_WARNING_OBJECT(self, "decoder stalled, video PTS stood still for %" G_GINT64_FORMAT " ms, clearing the video buffer", (now - since) / 1000);
				ioctl(self->fd, VIDEO_CLEAR_BUFFER);
				/* the cleared decoder takes nothing before a keyframe with
				 * the codec headers */
				self->must_send_header = TRUE;
				self->wait_keyframe = TRUE;
				self->flush_vpts = dream_tsync("pts_video");
				self->start_pts = DREAM_PTS_NONE;
				return 1;
			}
		}
	}
	if (self->start_pts == DREAM_PTS_NONE)
		self->start_pts = entry->pts;
	if (++self->frames_written % 25 == 1)
		dream_post_info(self);
	return 1;
}

/* write the collected access unit after the frames held back by a pause */
static int dream_write_frame(GstDVBVideoSink *self)
{
	if (self->frame)
	{
		if (self->frame->len)
		{
			dream_frame_t *entry = g_new0(dream_frame_t, 1);
			entry->data = self->frame;
			entry->pts = dream_trick_pts(self, self->frame_pts);
			g_queue_push_tail(&self->frames, entry);
		}
		else
			g_byte_array_unref(self->frame);
		self->frame = NULL;
	}
	while (!g_queue_is_empty(&self->frames))
	{
		int ret;
		if (self->flushing)
		{
			g_queue_clear_full(&self->frames, dream_frame_free);
			break;
		}
		ret = dream_write(self, g_queue_peek_head(&self->frames));
		if (ret <= 0)
			return ret;
		dream_frame_free(g_queue_pop_head(&self->frames));
		if (self->wait_keyframe)
			g_queue_clear_full(&self->frames, dream_frame_free);
	}
	return 0;
}

/* an empty frame makes the decoder show the frames it still holds */
static void dream_write_eos(GstDVBVideoSink *self)
{
	struct dream_video_frame frame;
	int i;
	memset(&frame, 0, sizeof(frame));
	for (i = 0; i < 20 && !self->flushing; i++)
	{
		if (ioctl(self->fd, DREAM_VIDEO_SET_FRAME, &frame) >= 0 || (errno != EAGAIN && errno != EINTR))
			break;
		gst_sleepms(50);
	}
}

static void dream_append_be16(GByteArray *frame, guint value)
{
	guint8 b[2] = { value >> 8, value };
	g_byte_array_append(frame, b, 2);
}

static void dream_append_be32(GByteArray *frame, guint32 value)
{
	guint8 b[4] = { value >> 24, value >> 16, value >> 8, value };
	g_byte_array_append(frame, b, 4);
}

/* MJPEG decoder setup: SOI and the default Huffman tables of ITU-T T.81 Annex K */
static const guint8 dream_jpeg_header[] = {
	0xff, 0xd8, 0xff, 0xc4, 0x01, 0xa2, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
	0x09, 0x0a, 0x0b, 0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
	0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00, 0x00, 0x01,
	0x7d, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61,
	0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1,
	0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27,
	0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
	0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
	0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
	0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6,
	0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4,
	0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1,
	0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
	0xf8, 0xf9, 0xfa, 0x11, 0x00, 0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04,
	0x00, 0x01, 0x02, 0x77, 0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41,
	0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09,
	0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17,
	0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
	0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64,
	0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83,
	0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
	0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8,
	0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6,
	0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4,
	0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

/* VP9: every frame of a superframe as its own AMLV packet, without the index */
static gboolean dream_frame_vp9(GByteArray *frame, const guint8 *data, gsize size)
{
	gsize sizes[8], count = 1, index_size = 0, total = 0, pos, i, b;
	guint8 marker = data[size - 1];
	sizes[0] = size;
	if ((marker & 0xe0) == 0xc0)
	{
		guint length_bytes = ((marker >> 3) & 3) + 1;
		count = (marker & 7) + 1;
		index_size = 2 + count * length_bytes;
		if (index_size > size || data[size - index_size] != marker)
			return FALSE;
		pos = size - index_size + 1;
		for (i = 0; i < count; i++)
		{
			sizes[i] = 0;
			for (b = 0; b < length_bytes; b++)
				sizes[i] |= (gsize)data[pos++] << (8 * b);
			if (!sizes[i] || sizes[i] > size - index_size - total)
				return FALSE;
			total += sizes[i];
		}
		if (total != size - index_size)
			return FALSE;
	}
	for (i = 0, pos = 0; i < count; pos += sizes[i], i++)
	{
		dream_append_be32(frame, sizes[i] + 4);
		dream_append_be32(frame, ~(guint32)(sizes[i] + 4));
		dream_append_be32(frame, 1);
		g_byte_array_append(frame, (const guint8 *)"AMLV", 4);
		g_byte_array_append(frame, data + pos, sizes[i]);
	}
	return TRUE;
}

/* WMV9 packet header: 24 bit length split by 0x88 escapes, checksum twice */
static void dream_wmv_packet(GByteArray *frame, guint8 type, gsize length)
{
	guint8 h[22] = { 0, 0, 1, type, 0, length >> 16, 0x88, length >> 8, length, 0x88, 0xff, 0xff, 0x88, 0xff, 0xff, 0x88 };
	guint i, sum = 0;
	for (i = 4; i < 16; i++)
		sum += h[i];
	h[16] = h[19] = sum >> 8;
	h[17] = h[20] = sum;
	h[18] = h[21] = 0x88;
	g_byte_array_append(frame, h, sizeof(h));
}

static GstFlowReturn dream_render_raw(GstDVBVideoSink *self, GstBuffer *buffer)
{
	GstMapInfo map, headers;
	GByteArray *frame;
	gboolean ok = TRUE;
	if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
		return GST_FLOW_OK;
	if (!map.size)
	{
		gst_buffer_unmap(buffer, &map);
		return GST_FLOW_OK;
	}
	headers.data = NULL;
	headers.size = 0;
	if (self->headers)
		gst_buffer_map(self->headers, &headers, GST_MAP_READ);
	frame = g_byte_array_sized_new(map.size + headers.size + 64);
	switch (self->codec_type)
	{
	case CT_VP9:
		ok = dream_frame_vp9(frame, map.data, map.size);
		break;
	case CT_DIVX311:
		if (self->must_send_header)
		{
			guint32 dimensions = self->width << 12 | self->height;
			guint8 h[6] = { 0x20, dimensions >> 16, dimensions >> 8, dimensions, 0, 0 };
			dream_append_be32(frame, 1);
			g_byte_array_append(frame, h, sizeof(h));
		}
		g_byte_array_append(frame, (const guint8 *)"\x00\x00\x00\x01\xb6" "DIVX3.11", 13);
		dream_append_be32(frame, map.size);
		g_byte_array_append(frame, map.data, map.size);
		break;
	case CT_VC1_SM:
		/* the sequence packet goes with every frame */
		ok = map.size <= 0xffffff && headers.size + 4 <= 0xffffff;
		if (ok)
		{
			dream_wmv_packet(frame, 0x10, headers.size + 4);
			dream_append_be16(frame, self->width);
			dream_append_be16(frame, self->height);
			g_byte_array_append(frame, headers.data, headers.size);
			dream_wmv_packet(frame, 0x0d, map.size);
			g_byte_array_append(frame, map.data, map.size);
		}
		break;
	case CT_MJPEG:
		ok = map.size >= 2 && map.data[0] == 0xff && map.data[1] == 0xd8;
		if (ok && self->must_send_header)
			g_byte_array_append(frame, dream_jpeg_header, sizeof(dream_jpeg_header));
		g_byte_array_append(frame, map.data, map.size);
		break;
	case CT_REAL:
		if (self->must_send_header && headers.size)
			g_byte_array_append(frame, headers.data, headers.size);
		g_byte_array_append(frame, map.data, map.size);
		break;
	default: /* CT_VC1 */
	{
		gboolean startcode = map.size >= 3 && !memcmp(map.data, "\x00\x00\x01", 3);
		if (self->must_send_header && headers.size)
		{
			/* without the ASF binding byte in front of the sequence header */
			gsize skip = headers.size > 3 && memcmp(headers.data, "\x00\x00\x01", 3) ? 1 : 0;
			g_byte_array_append(frame, headers.data + skip, headers.size - skip);
		}
		else if (self->must_send_header && !startcode)
			ok = FALSE;
		if (ok && !startcode)
			g_byte_array_append(frame, (const guint8 *)"\x00\x00\x01\x0d", 4);
		g_byte_array_append(frame, map.data, map.size);
		break;
	}
	}
	if (self->headers)
		gst_buffer_unmap(self->headers, &headers);
	if (!ok)
	{
		GST_WARNING_OBJECT(self, "dropping a malformed frame of %" G_GSIZE_FORMAT " bytes", map.size);
		g_byte_array_unref(frame);
		gst_buffer_unmap(buffer, &map);
		return GST_FLOW_OK;
	}
	self->must_send_header = FALSE;
	self->frame = frame;
	self->frame_pts = DREAM_PTS_NONE;
	if (GST_BUFFER_PTS_IS_VALID(buffer) || (self->use_dts && GST_BUFFER_DTS_IS_VALID(buffer)))
	{
		GstClockTime ts = GST_BUFFER_PTS_IS_VALID(buffer) ? GST_BUFFER_PTS(buffer) : GST_BUFFER_DTS(buffer);
		self->frame_pts = gst_util_uint64_scale(ts, 9, 100000) & DREAM_PTS_MASK;
		self->pts_written = TRUE;
	}
	gst_buffer_unmap(buffer, &map);
	if (dream_write_frame(self) < 0)
	{
		GST_ELEMENT_ERROR(self, RESOURCE, WRITE, (NULL), ("video write: %s", g_strerror(errno)));
		return GST_FLOW_ERROR;
	}
	return GST_FLOW_OK;
}
#endif

#define VIDEO_CAPS \
  "width = (int) [ 16, 4096 ], " \
  "height = (int) [ 16, 4096 ] "

#define MPEG4V2_LIMITED_CAPS \
  "width = (int) [ 16, 800 ], " \
  "height = (int) [ 16, 600 ], " \
  "framerate = (fraction) [ 0, MAX ]"

enum
{
	PROP_0,
	PROP_SYNC,
	PROP_ASYNC,
	PROP_SYNC_E2PLAYER,
	PROP_ASYNC_E2PLAYER,
	PROP_RENDER_DELAY,
	PROP_LAST
};

enum
{
	SIGNAL_GET_DECODER_TIME,
	SIGNAL_GET_VIDEO_CODEC,
	LAST_SIGNAL
};

static guint gst_dvb_videosink_signals[LAST_SIGNAL] = { 0 };

static GstStaticPadTemplate sink_factory =
GST_STATIC_PAD_TEMPLATE (
	"sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS (
#ifdef HAVE_MPEG4
	"video/mpeg, "
		"mpegversion = (int) 4, "
		"unpacked = (boolean) true, "
		VIDEO_CAPS "; "
#endif
	"video/mpeg, "
		"mpegversion = (int) { 1, 2 }, "
		VIDEO_CAPS "; "
#ifdef HAVE_H265
	"video/x-h265, "
		VIDEO_CAPS "; "
	"video/x-h265, "
		"alignment = (string) nal; "
#endif
#ifdef HAVE_H264
	"video/x-h264, "
		VIDEO_CAPS "; "
	"video/x-h264, "
		"parsed = (boolean) true, "
		"alignment = (string) nal; "
#endif
#ifdef HAVE_H263
	"video/x-h263, "
		VIDEO_CAPS "; "
#endif
#ifdef HAVE_MPEG4V2
	"video/x-msmpeg, "
#ifdef HAVE_LIMITED_MPEG4V2
		MPEG4V2_LIMITED_CAPS
#else
		VIDEO_CAPS 
#endif
		", msmpegversion = (int) 43; "
	"video/x-divx, "
#ifdef HAVE_LIMITED_MPEG4V2
		MPEG4V2_LIMITED_CAPS
#else
		VIDEO_CAPS 
#endif
		", divxversion = (int) 3;"
	"video/x-xvid, "
#ifdef HAVE_LIMITED_MPEG4V2
		MPEG4V2_LIMITED_CAPS
#else
		VIDEO_CAPS 
#endif
		"; "
	"video/x-3ivx, "
#ifdef HAVE_LIMITED_MPEG4V2
		MPEG4V2_LIMITED_CAPS
#else
		VIDEO_CAPS 
#endif
		"; "
#endif
#ifdef HAVE_WMV
	"video/x-wmv, "
		VIDEO_CAPS ", wmvversion = (int) 3; "
#endif
#ifdef HAVE_SPARK
	"video/x-flash-video, "
		VIDEO_CAPS "; "
#endif
#ifdef HAVE_VB6
	"video/x-vp6, "
		VIDEO_CAPS "; "
	"video/x-vp6-flash, "
		VIDEO_CAPS "; "
#endif
#ifdef HAVE_VB8
	"video/x-vp8, "
		VIDEO_CAPS "; "
#endif
#ifdef HAVE_VB9
	"video/x-vp9, "
		VIDEO_CAPS "; "
#endif
#ifdef DREAMNEXTGEN
	/* demuxers often leave out the size */
	"image/jpeg; "
	"video/x-pn-realvideo, "
		VIDEO_CAPS ", rmversion = (int) { 3, 4 }; "
	"video/x-cavs; "
#endif
	)
);

#define VIDEO_ENCODING_UNKNOWN  0xFF

unsigned int streamtype_to_encoding(unsigned int streamtype)
{
#ifdef VIDEO_SET_ENCODING
	switch(streamtype)
	{
	case STREAMTYPE_MPEG2:
		return VIDEO_ENCODING_AUTO;
	case STREAMTYPE_MPEG4_H264:
		return VIDEO_ENCODING_H264;
	case STREAMTYPE_H263:
		return VIDEO_ENCODING_H263;
	case STREAMTYPE_MPEG4_Part2:
		return VIDEO_ENCODING_MPEG4P2;
	case STREAMTYPE_MPEG1:
		return VIDEO_ENCODING_AUTO;
	case STREAMTYPE_XVID:
		return VIDEO_ENCODING_MPEG4P2;
	case STREAMTYPE_DIVX311:
		return VIDEO_ENCODING_MPEG4P2;
	case STREAMTYPE_DIVX4:
		return VIDEO_ENCODING_MPEG4P2;
	case STREAMTYPE_DIVX5:
		return VIDEO_ENCODING_MPEG4P2;
	case STREAMTYPE_VC1:
		return VIDEO_ENCODING_VC1;
	case STREAMTYPE_VC1_SM:
		return VIDEO_ENCODING_WMV;
	default:
		return VIDEO_ENCODING_UNKNOWN;
	}
#endif
	return VIDEO_ENCODING_UNKNOWN;
}

static void gst_dvbvideosink_init(GstDVBVideoSink *self);
static void gst_dvbvideosink_dispose(GObject *obj);
static void gst_dvbvideosink_reset(GObject *obj);
static void gst_dvbvideosink_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_dvbvideosink_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec);

#define DEBUG_INIT \
	GST_DEBUG_CATEGORY_INIT(dvbvideosink_debug, "dvbvideosink", 0, "dvbvideosink element");

static GstBaseSinkClass *parent_class = NULL;
G_DEFINE_TYPE_WITH_CODE(GstDVBVideoSink, gst_dvbvideosink, GST_TYPE_BASE_SINK, DEBUG_INIT);

static gboolean gst_dvbvideosink_start (GstBaseSink * sink);
static gboolean gst_dvbvideosink_stop (GstBaseSink * sink);
static gboolean gst_dvbvideosink_event (GstBaseSink * sink, GstEvent * event);
static GstFlowReturn gst_dvbvideosink_render (GstBaseSink * sink, GstBuffer * buffer);
static gboolean gst_dvbvideosink_set_caps (GstBaseSink * sink, GstCaps * caps);
static gboolean gst_dvbvideosink_unlock (GstBaseSink * basesink);
static gboolean gst_dvbvideosink_unlock_stop (GstBaseSink * basesink);
static GstStateChangeReturn gst_dvbvideosink_change_state (GstElement * element, GstStateChange transition);
static gint64 gst_dvbvideosink_get_decoder_time (GstDVBVideoSink *self);
static gint64 gst_dvbvideosink_get_video_codec (GstDVBVideoSink *self);

/* initialize the plugin's class */
static void gst_dvbvideosink_class_init(GstDVBVideoSinkClass *self)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (self);
	GstBaseSinkClass *gstbasesink_class = GST_BASE_SINK_CLASS (self);
	GstElementClass *element_class = GST_ELEMENT_CLASS (self);

	parent_class = g_type_class_peek_parent(self);
	gobject_class->finalize = gst_dvbvideosink_reset;
	gobject_class->dispose = gst_dvbvideosink_dispose;

	gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&sink_factory));
	gst_element_class_set_static_metadata(element_class,
		"DVB video sink",
		"Generic/DVBVideoSink",
		"Outputs PES into a linuxtv dvb video device",
		"PLi team");

	gobject_class->set_property = gst_dvbvideosink_set_property;
	gobject_class->get_property = gst_dvbvideosink_get_property;

	g_object_class_install_property (gobject_class, PROP_SYNC,
			g_param_spec_boolean ("sync", "Sync", "Sync on the clock", FALSE,
					G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (gobject_class, PROP_ASYNC,
			g_param_spec_boolean ("async", "Async", "preroll", FALSE,
					G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (gobject_class, PROP_SYNC_E2PLAYER,
			g_param_spec_boolean ("e2-sync", "E2-Sync", "Sync on the clock", FALSE,
					G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (gobject_class, PROP_ASYNC_E2PLAYER,
			g_param_spec_boolean ("e2-async", "E2-Async", "preroll", FALSE,
					G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (gobject_class, PROP_RENDER_DELAY,
			g_param_spec_uint64 ("render-delay", "Renderdelay", "Render-delay increase latency",
					0, G_MAXUINT64, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	gstbasesink_class->start = GST_DEBUG_FUNCPTR (gst_dvbvideosink_start);
	gstbasesink_class->stop = GST_DEBUG_FUNCPTR (gst_dvbvideosink_stop);
	gstbasesink_class->render = GST_DEBUG_FUNCPTR (gst_dvbvideosink_render);
	gstbasesink_class->event = GST_DEBUG_FUNCPTR (gst_dvbvideosink_event);
	gstbasesink_class->unlock = GST_DEBUG_FUNCPTR (gst_dvbvideosink_unlock);
	gstbasesink_class->unlock_stop = GST_DEBUG_FUNCPTR (gst_dvbvideosink_unlock_stop);
	gstbasesink_class->set_caps = GST_DEBUG_FUNCPTR (gst_dvbvideosink_set_caps);

	element_class->change_state = GST_DEBUG_FUNCPTR (gst_dvbvideosink_change_state);

	gst_dvb_videosink_signals[SIGNAL_GET_DECODER_TIME] =
		g_signal_new ("get-decoder-time",
		G_TYPE_FROM_CLASS (self),
		G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
		G_STRUCT_OFFSET (GstDVBVideoSinkClass, get_decoder_time),
		NULL, NULL, gst_dvbsink_marshal_INT64__VOID, G_TYPE_INT64, 0);

	self->get_decoder_time = gst_dvbvideosink_get_decoder_time;

	gst_dvb_videosink_signals[SIGNAL_GET_VIDEO_CODEC] =
		g_signal_new ("get-video-codec",
		G_TYPE_FROM_CLASS (self),
		G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
		G_STRUCT_OFFSET (GstDVBVideoSinkClass, get_video_codec),
		NULL, NULL, gst_dvbsink_marshal_INT64__VOID, G_TYPE_INT64, 0);

	self->get_video_codec = gst_dvbvideosink_get_video_codec;
}

#define H264_BUFFER_SIZE (64*1024+2048)

/* initialize the new element
 * instantiate pads and add them to element
 * set functions
 * initialize structure
 */
static void gst_dvbvideosink_init(GstDVBVideoSink *self)
{
	self->must_send_header = TRUE;
	self->h264_nal_len_size = 0;
	self->h264_initial_audelim_written = FALSE;
	self->pesheader_buffer = NULL;
	self->codec_data = NULL;
	self->codec_type = CT_UNKNOWN;
	self->stream_type = STREAMTYPE_UNKNOWN;
	self->use_dts = FALSE;
	self->paused = self->playing = self->unlocking = self->flushing = self->first_paused = FALSE;
	self->pts_written = self->using_dts_downmix = self->synchronized = self->pass_eos = FALSE;
	self->lastpts = 0;
	self->timestamp_offset = 0;
	self->queue = NULL;
	self->fd = -1;
	self->unlockfd[0] = self->unlockfd[1] = -1;
	self->saved_fallback_framerate[0] = 0;
	self->rate = 1.0;
	self->wmv_asf = FALSE;
#ifdef VIDEO_SET_ENCODING
	self->use_set_encoding = TRUE;
#else
	self->use_set_encoding = FALSE;
#endif
#ifdef DREAMNEXTGEN
	self->poll_fd = -1;
	self->frame = NULL;
	self->headers = NULL;
	g_queue_init(&self->frames);
#endif
	gst_base_sink_set_sync(GST_BASE_SINK(self), FALSE);
	gst_base_sink_set_async_enabled(GST_BASE_SINK(self), FALSE);
}

static void gst_dvbvideosink_dispose(GObject *obj)
{
	G_OBJECT_CLASS(parent_class)->dispose(obj);
	GST_DEBUG("GstDVBVideoSink DISPOSED");
}

static void gst_dvbvideosink_reset(GObject *obj)
{
	G_OBJECT_CLASS(parent_class)->finalize(obj);
	GST_DEBUG("GstDVBVideoSink RESET");
}

static void gst_dvbvideosink_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec)
{
	GstDVBVideoSink *self = GST_DVBVIDEOSINK (object);

	switch (prop_id)
	{
		/* sink should only work with sync turned off, ignore all attempts to change it *
		 * looks like no element tries to change this setting so leave it allowed for now *
		 * subject to changes in future Most stb do support sync settings *
		 * exception on this are the old dreamboxes and vuplus boxes and maybe some other ol ones */
		case PROP_SYNC:
			GST_INFO_OBJECT(self, "ignoring attempt to change 'sync' to %s by unknown element", g_value_get_boolean(value) ? "TRUE" : "FALSE");
			break;
		case PROP_ASYNC:
			GST_INFO_OBJECT(self, "ignoring attempt to change by 'async' to %s by unknown element", g_value_get_boolean(value) ? "TRUE" : "FALSE");
			break;
		case PROP_SYNC_E2PLAYER:
			gst_base_sink_set_sync(GST_BASE_SINK(object), g_value_get_boolean(value));
			GST_DEBUG_OBJECT(self, "CHANGE sync setting to %s", g_value_get_boolean(value) ? "TRUE" : "FALSE");
			if (gst_base_sink_get_sync(GST_BASE_SINK(object)))
			{
				GST_INFO_OBJECT(self, "Gstreamer sync succesfully set to TRUE by e2Player");
				self->synchronized = TRUE;
			}
			else
			{
				GST_INFO_OBJECT(self, "Gstreamer sync succesfully set to FALSE by e2Player");
				self->synchronized = FALSE;
			}
			break;
		case PROP_ASYNC_E2PLAYER:
			gst_base_sink_set_async_enabled(GST_BASE_SINK(object), g_value_get_boolean(value));
			GST_DEBUG_OBJECT(self, "CHANGE async setting to %s  source ", g_value_get_boolean(value) ? "TRUE" : "FALSE");
			if (gst_base_sink_is_async_enabled(GST_BASE_SINK(object)))
			{
				GST_INFO_OBJECT(self, "Gstreamer async succesfully set to TRUE by e2Player");
			}
			else
			{
				GST_INFO_OBJECT(self, "Gstreamer async succesfully set to FALSE by e2Player");
			}
			break;
		case PROP_RENDER_DELAY:
			gst_base_sink_set_render_delay(GST_BASE_SINK(object), g_value_get_uint64(value));
			GST_DEBUG_OBJECT(self, "Change renderdelay to  = %" G_GUINT64_FORMAT , g_value_get_uint64(value));
			if (gst_base_sink_get_render_delay(GST_BASE_SINK(object)) == g_value_get_uint64(value))
				GST_INFO_OBJECT(self, "Renderdelay changed to  %" G_GUINT64_FORMAT , g_value_get_uint64(value));
			else
				GST_WARNING_OBJECT(self, "Renderdelay change to  %" G_GUINT64_FORMAT " FAILURE", g_value_get_uint64(value));
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}

static void gst_dvbvideosink_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec)
{
	GstDVBVideoSink *self = GST_DVBVIDEOSINK (object);

	switch (prop_id)
	{
		case PROP_SYNC:
			g_value_set_boolean(value, gst_base_sink_get_sync(GST_BASE_SINK(object)));
			GST_INFO_OBJECT(self, "Requested by other element SYNC VALUE = %s", g_value_get_boolean(value) ? "TRUE" : "FALSE");
			break;
		case PROP_ASYNC:
			g_value_set_boolean(value, gst_base_sink_is_async_enabled(GST_BASE_SINK(object)));
			GST_INFO_OBJECT(self, "Requested by other element ASYNC VALUE = %s", g_value_get_boolean(value) ? "TRUE" : "FALSE");
			break;
		case PROP_RENDER_DELAY:
			g_value_set_uint64(value, gst_base_sink_get_render_delay(GST_BASE_SINK(object)));
			GST_INFO_OBJECT(self, "Requested by other element RENDER DELAY = %" G_GUINT64_FORMAT , g_value_get_uint64(value));
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}

static gint64 gst_dvbvideosink_get_decoder_time(GstDVBVideoSink *self)
{
	gint64 cur = 0;
	gint res = -1;
	if (self->fd < 0 || !self->playing || !self->pts_written)
		return GST_CLOCK_TIME_NONE;

	res = ioctl(self->fd, VIDEO_GET_PTS, &cur);
	if (cur && res >= 0)
	{
		self->lastpts = cur;
	}
	else
	{
		cur = self->lastpts;
	}
#ifdef DREAMNEXTGEN
	/* the position in the stream, not the scaled PTS of a trick mode; the
	 * picture of before the seek has none in it */
	if (self->rate != 1.0 && (self->trick_base == DREAM_PTS_NONE || dream_tsync("pts_video") == self->flush_vpts))
		return GST_CLOCK_TIME_NONE;
	if (self->rate != 1.0)
		cur = (gint64)((self->trick_base + (gint64)(dream_pts_diff(cur, self->trick_base) * self->rate)) & DREAM_PTS_MASK);
	cur = gst_util_uint64_scale(cur & DREAM_PTS_MASK, 100000, 9);
#else
	cur *= 11111;
#endif
	// timestamp_offset is a gstreamer nanoseconds var
	cur -= self->timestamp_offset;

	return cur;
}

static gint64 gst_dvbvideosink_get_video_codec(GstDVBVideoSink *self)
{
	// returns a superset of streamtypes available in lib/dvb/decoder.h
	return self->stream_type;
}

static gboolean gst_dvbvideosink_unlock(GstBaseSink *basesink)
{
	GstDVBVideoSink *self = GST_DVBVIDEOSINK (basesink);
	self->unlocking = TRUE;
	/* wakeup the poll */
	write(self->unlockfd[1], "\x01", 1);
	GST_DEBUG_OBJECT(basesink, "unlock");
	return TRUE;
}

static gboolean gst_dvbvideosink_unlock_stop(GstBaseSink *basesink)
{
	GstDVBVideoSink *self = GST_DVBVIDEOSINK (basesink);
	self->unlocking = FALSE;
	GST_DEBUG_OBJECT(basesink, "unlock_stop");
	return TRUE;
}

static gboolean gst_dvbvideosink_event(GstBaseSink *sink, GstEvent *event)
{
	GstDVBVideoSink *self = GST_DVBVIDEOSINK (sink);
	GST_DEBUG_OBJECT (self, "EVENT %s", gst_event_type_get_name(GST_EVENT_TYPE (event)));
	int ret = TRUE, wait = FALSE;

	switch (GST_EVENT_TYPE (event))
	{
	case GST_EVENT_FLUSH_START:
		GST_INFO_OBJECT (self,"GST_EVENT_FLUSH_START");
		if(self->flushed && !self->playing && self->using_dts_downmix && !self->paused)
		{ 
			self->playing = TRUE;
			self->ok_to_write = 1;
		}
		self->flushed = FALSE;
		self->flushing = TRUE;
		/* wakeup the poll */
		write(self->unlockfd[1], "\x01", 1);
		break;
	case GST_EVENT_FLUSH_STOP:
		GST_INFO_OBJECT (self,"GST_EVENT_FLUSH_STOP");
		if (self->fd >= 0) ioctl(self->fd, VIDEO_CLEAR_BUFFER);
		GST_OBJECT_LOCK(self);
		self->must_send_header = TRUE;
		while (self->queue)
		{
			queue_pop(&self->queue);
		}
#ifdef DREAMNEXTGEN
		dream_flush(self);
#endif
		self->flushing = FALSE;
		GST_OBJECT_UNLOCK(self);
		/* flush while media is playing requires a delay before rendering */
		if (self->using_dts_downmix && !self->paused)
		{
			self->ok_to_write = 0;
			self->playing = FALSE;
		}
		self->flushed = TRUE;
		break;
	case GST_EVENT_STREAM_GROUP_DONE:
		GST_INFO_OBJECT (self,"GST_EVENT_STREAM_GROUP_DONE");
		self->pass_eos = TRUE;
		break;
	case GST_EVENT_EOS:
	{
		GST_INFO_OBJECT (self, "GST_EVENT_EOS");
#ifdef VIDEO_FLUSH
		if (self->fd >= 0) ioctl(self->fd, VIDEO_FLUSH, 1/*NONBLOCK*/); //Notify the player that no addionional data will be injected
#endif
		struct pollfd pfd[2];
		pfd[0].fd = self->unlockfd[0];
		pfd[0].events = POLLIN;
		pfd[1].fd = self->fd;
		pfd[1].events = POLLIN;
#ifdef DREAMNEXTGEN
		/* readable means a pending event here, not an empty buffer: wait
		 * for the PTS on screen to stand still */
		pfd[1].fd = -1;
		if (self->fd >= 0)
		{
			dream_read_commands(self);
			dream_write_eos(self);
		}
#endif
		int x = 0;
		int retval = 0;
		gint64 previous_pts = 0;
		gint64 current_pts = 0;
		gboolean first_loop_done = FALSE;
		GST_BASE_SINK_PREROLL_UNLOCK(sink);
 		while (1)
		{
				retval = poll(pfd, 2, 250);

			if (retval < 0)
			{
				GST_INFO_OBJECT(self,"poll in EVENT_EOS");
				ret = FALSE;
				break;
			}
			else if (pfd[0].revents & POLLIN)
			{
				GST_INFO_OBJECT(self, "wait EOS aborted!! media is not ended");
				wait = TRUE;
				ret = FALSE;
				break;
			}
			/* video must first wait up on right playposition before sending eos by driver or position 
			 * on fast boxes like the 4 K's the eos is send by short media (below buffer video-mem buffer lenght) to early
			 * The video did not had the time to start playing.
			 * If the media is long enough which means media lenght bigger then drivers video-mem this issue does not occur
			*/
			else if (pfd[1].revents & POLLIN && first_loop_done)
			{
				GST_INFO_OBJECT(self, "got buffer empty from driver!");
				break;
			}
			else if (sink->flushing)
			{
				GST_INFO_OBJECT(self, "wait EOS flushing!!");
				wait = TRUE;
				ret = FALSE;
				break;
			}
			else
			{
				
				/* max 500 ms needed for 4K stb's for the first loop detection.
				 * note streamed live media may have an eternal position of 0
				 * We only will react on empty buffer event for streamed media which remains at zero
				 * Like usual this is not the case for all live streamed media */
				current_pts = gst_dvbvideosink_get_decoder_time(self);

				if(current_pts > 0 || x >= 1)
				{
					if(previous_pts == current_pts && current_pts > 0)
					{
						GST_INFO_OBJECT(self,"Media ended push eos to basesink current_pts %" G_GINT64_FORMAT " previous_pts %" G_GINT64_FORMAT,
							current_pts, previous_pts);
						break;
					}
					else
					{
						if(previous_pts == 0 && x < 1)
						{
							gst_sleepms(500);
						}						
						else
							first_loop_done = TRUE;
						GST_DEBUG_OBJECT(self,"poll out current_pts %" G_GINT64_FORMAT " previous_pts %" G_GINT64_FORMAT,
							current_pts, previous_pts);
						previous_pts = current_pts;
						if(x < 1)
							x++;
					}
				}
				else if (x < 1)
				{
					gst_sleepms(500);
					x++;
				}
				else
					first_loop_done = TRUE;
			}
		}
		GST_BASE_SINK_PREROLL_LOCK(sink);
		break;
	}
	case GST_EVENT_SEGMENT:
	{
		const GstSegment *segment;
		GstFormat format;
		gdouble rate;
		guint64 start, end, pos;
		gint64 start_dvb;
		gst_event_parse_segment(event, &segment);
		format = segment->format;
		rate = segment->rate;
		start = segment->start;
		end = segment->stop;
		pos = segment->position;
		start_dvb = start / 11111LL;
		GST_INFO_OBJECT(self, "SEGMENT rate=%f format=%d start=%"G_GUINT64_FORMAT " pos=%"G_GUINT64_FORMAT, rate, format, start, pos);
		GST_INFO_OBJECT(self, "SEGMENT DVB TIMESTAMP=%"G_GINT64_FORMAT " HEXFORMAT %#"G_GINT64_MODIFIER "x", start_dvb, start_dvb);
		if (format == GST_FORMAT_TIME)
		{
			self->timestamp_offset = start - pos;
#ifdef DREAMNEXTGEN
			/* a backwards segment starts at its stop, the PTS need no offset */
			if (rate < 0.0)
				self->timestamp_offset = 0;
			/* the decoder's own trick mode starts only for some streams,
			 * dream_trick_pts scales the PTS instead */
			if (rate != self->rate)
			{
				self->trick_base = DREAM_PTS_NONE;
				self->rate = rate;
			}
#else
			if (rate != self->rate)
			{
				int skip = 0, repeat = 0;
				if (rate > 1.0)
				{
					skip = (int)rate;
				}
				else if (rate < 1.0)
				{
					repeat = 1.0 / rate;
				}
				ioctl(self->fd, VIDEO_SLOWMOTION, repeat);
				ioctl(self->fd, VIDEO_FAST_FORWARD, skip);
				ioctl(self->fd, VIDEO_CONTINUE);
				self->rate = rate;
			}
#endif
		}
		break;
	}
	case GST_EVENT_CAPS:
	{
		GstCaps *caps;
		gst_event_parse_caps(event, &caps);
		if (caps)
		{
			GST_INFO_OBJECT(self,"CAP %"GST_PTR_FORMAT, caps);
		}
		else
			ret = FALSE;
		break;
	}
	case GST_EVENT_TAG:
	{
		GstTagList *taglist;
		gst_event_parse_tag(event, &taglist);
		GST_DEBUG_OBJECT(self,"TAG %"GST_PTR_FORMAT, taglist);
		if(self->codec_type == CT_VC1 || self->codec_type == CT_H264)
		{
			gchar *cont_val = NULL;
			gboolean have_cont_tag = gst_tag_list_get_string (taglist, GST_TAG_CONTAINER_FORMAT, &cont_val);
			if(have_cont_tag && cont_val)
			{
				if(!strncmp(cont_val, "ASF", 3) && self->codec_type == CT_VC1)
				{
					GST_INFO_OBJECT(self,"SET wmv_asf to TRUE");
					self->wmv_asf = TRUE;
				}
				if(!strncmp(cont_val, "Matroska", 8) && self->codec_type == CT_H264)
				{
					self->use_dts = TRUE;
					//special case on Matroska media only it may happen that the pts is missing.
					//that is cause media was made with some camera's,webcam or some older recording devices
					//who did not add a pts since the dts is only present on p frames and in that case pts=dts
					//This error is only present on h264 codec since that is the codec those devices are using when encoding.
					GST_INFO_OBJECT(self,"SET use dts from buffer to trough");
				}
			}
			if(have_cont_tag)
				g_free(cont_val);
		}
		break;
	}
	default:
		break;
	}
	if (ret)
		ret = GST_BASE_SINK_CLASS(parent_class)->event(sink, event);
	else if (!wait)
		gst_event_unref(event);

	return ret;
}

static int video_write(GstBaseSink *sink, GstDVBVideoSink *self, GstBuffer *buffer, size_t start, size_t end)
{
#ifdef DREAMNEXTGEN
	/* render writes the collected access unit when it is complete */
	dream_collect(self, buffer, start, end);
	return 0;
#endif
	size_t written = start;
	size_t len = end;
	struct pollfd pfd[2];
	guint8 *data;
	int retval = 0;
	GstMapInfo map;
	gst_buffer_map(buffer, &map, GST_MAP_READ);
	data = map.data;

	pfd[0].fd = self->unlockfd[0];
	pfd[0].events = POLLIN;
	pfd[1].fd = self->fd;
	pfd[1].events = POLLOUT | POLLPRI;

	do
	{
		if (self->flushing)
		{
			GST_INFO_OBJECT(self, "flushing, skip %d bytes", len - written);
			break;
		}
		else if (self->paused || self->unlocking)
		{
			GST_OBJECT_LOCK(self);
			queue_push(&self->queue, buffer, written, end);
			GST_OBJECT_UNLOCK(self);
			GST_TRACE_OBJECT(self, "pushed %d bytes to queue", len - written);
			break;
		}
		else
		{
			GST_TRACE_OBJECT (self, "going into poll, have %d bytes to write", len - written);
		}
		if (poll(pfd, 2, -1) < 0)
		{
			if (errno == EINTR) continue;
			retval = -1;
			break;
		}
		if (pfd[0].revents & POLLIN)
		{
			/* read all stop commands */
			while (1)
			{
				gchar command;
				int res = read(self->unlockfd[0], &command, 1);
				if (res < 0)
				{
					GST_DEBUG_OBJECT (self, "no more commands");
					/* no more commands */
					break;
				}
			}
		}
		if (pfd[1].revents & POLLPRI)
		{
			GstStructure *s;
			GstMessage *msg;
			struct video_event evt;
			if (ioctl(self->fd, VIDEO_GET_EVENT, &evt) < 0)
			{
				g_warning("failed to ioctl VIDEO_GET_EVENT!");
			}
			else
			{
				GST_INFO_OBJECT (self, "VIDEO_EVENT %d", evt.type);
				if (evt.type == VIDEO_EVENT_SIZE_CHANGED) {
					s = gst_structure_new ("eventSizeChanged",
						"aspect_ratio", G_TYPE_INT, evt.u.size.aspect_ratio == 0 ? 2 : 3,
						"width", G_TYPE_INT, evt.u.size.w,
						"height", G_TYPE_INT, evt.u.size.h, NULL);
					msg = gst_message_new_element (GST_OBJECT(sink), s);
					gst_element_post_message (GST_ELEMENT(sink), msg);
				}
				else if (evt.type == VIDEO_EVENT_FRAME_RATE_CHANGED)
				{
					s = gst_structure_new ("eventFrameRateChanged",
						"frame_rate", G_TYPE_INT, evt.u.frame_rate, NULL);
					msg = gst_message_new_element (GST_OBJECT(sink), s);
					gst_element_post_message (GST_ELEMENT(sink), msg);
				}
				else if (evt.type == 16 /*VIDEO_EVENT_PROGRESSIVE_CHANGED*/)
				{
					s = gst_structure_new ("eventProgressiveChanged",
						"progressive", G_TYPE_INT, evt.u.frame_rate, NULL);
					msg = gst_message_new_element (GST_OBJECT(sink), s);
					gst_element_post_message (GST_ELEMENT(sink), msg);
				}
				else if (evt.type == 17 /*VIDEO_EVENT_GAMMA_CHANGED*/)
				{
					s = gst_structure_new ("eventGammaChanged",
						"gamma", G_TYPE_INT, evt.u.frame_rate, NULL);
					msg = gst_message_new_element (GST_OBJECT(sink), s);
					gst_element_post_message (GST_ELEMENT(sink), msg);
				}
				else
				{
					g_warning ("unhandled DVBAPI Video Event %d", evt.type);
				}
			}
		}
		if (pfd[1].revents & POLLOUT)
		{
			int wr = 0;
			size_t queuestart, queueend;
			GstBuffer *queuebuffer;
			GST_OBJECT_LOCK(self);
			if (queue_front(&self->queue, &queuebuffer, &queuestart, &queueend) >= 0)
			{
				guint8 *queuedata;
				GstMapInfo queuemap;
				gst_buffer_map(queuebuffer, &queuemap, GST_MAP_READ);
				queuedata = queuemap.data;
				wr = write(self->fd, queuedata + queuestart, queueend - queuestart);
				gst_buffer_unmap(queuebuffer, &queuemap);
				if (wr < 0)
				{
					switch (errno)
					{
						case EINTR:
						case EAGAIN:
							break;
						default:
							GST_OBJECT_UNLOCK(self);
							retval = -3;
							break;
					}
					if (retval < 0) break;
				}
				else if (wr >= queueend - queuestart)
				{
					queue_pop(&self->queue);
					GST_TRACE_OBJECT (self, "written %d queue bytes... pop entry", wr);
				}
				else
				{
					self->queue->start += wr;
					GST_TRACE_OBJECT (self, "written %d queue bytes... update offset", wr);
				}
				GST_OBJECT_UNLOCK(self);
				continue;
			}
			GST_OBJECT_UNLOCK(self);
			wr = write(self->fd, data + written, len - written);
			if (wr < 0)
			{
				switch (errno)
				{
					case EINTR:
					case EAGAIN:
						continue;
					default:
						retval = -3;
						break;
				}
				if (retval < 0) break;
			}
			written += wr;
		}
	} while (written < len);

	gst_buffer_unmap(buffer, &map);
	return retval;
}

static GstFlowReturn gst_dvbvideosink_render(GstBaseSink *sink, GstBuffer *buffer)
{
	GstDVBVideoSink *self = GST_DVBVIDEOSINK(sink);
	guint8 *pes_header;
	gsize pes_header_len = 0;
	gsize data_len;
	guint8 *data, *original_data;
	guint8 *codec_data = NULL;
	gsize codec_data_size = 0;
	gsize payload_len = 0;
	GstBuffer *tmpbuf = NULL;
	GstFlowReturn ret = GST_FLOW_OK;

	if (self->fd < 0)
	{
		return GST_FLOW_OK;
	}
	gint i = 0;
	/* WAIT 1 seconds after flush needed for enigma2 to be ready*/
	while (self->ok_to_write == 0)
	{
			self->flushed = FALSE;
			self->ok_to_write = 1;
			self->playing = TRUE;
			gst_sleepms(1200);
			GST_INFO_OBJECT(self,"RESUME PLAY AFTER FLUSH + 1,2 SECOND");
	}
#ifdef DREAMNEXTGEN
	if (self->wait_keyframe)
	{
		if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT))
			return GST_FLOW_OK;
		self->wait_keyframe = FALSE;
	}
	if (dream_raw_codec(self))
		return dream_render_raw(self, buffer);
#endif
	GstMapInfo map, pesheadermap, codecdatamap;
	gst_buffer_map(buffer, &map, GST_MAP_READ);
	original_data = data = map.data;
	data_len = map.size;
	gst_buffer_map(self->pesheader_buffer, &pesheadermap, GST_MAP_WRITE);
	pes_header = pesheadermap.data;
	if (self->codec_data)
	{
		gst_buffer_map(self->codec_data, &codecdatamap, GST_MAP_READ);
		codec_data = codecdatamap.data;
		codec_data_size = codecdatamap.size;
	}

	if (self->codec_type == CT_H264 && !self->h264_initial_audelim_written)
	{
		int i = 0;
		while( data[i] == 0 && i != data_len)
		{
			GST_TRACE_OBJECT(self, "data[%d] = %d", i , data[i]); 
			i++;
		}
		if (i > 1 && data[i] == 1)
		{
			int au_type = data[i+1] & 0x1f;
			char au_str[64];
			switch(au_type)
			{
				case 1: strcpy(au_str, "SLICE"); break;
				case 5: strcpy(au_str, "IDR"); break;
				case 6: strcpy(au_str, "SEI"); break;
				case 7: strcpy(au_str, "SPS"); break;
				case 8: strcpy(au_str, "PPS"); break;
				case 9: strcpy(au_str, "AU_DELIM"); break;
				default:
					strcpy(au_str, "UNK");
					break;
			}
			GST_DEBUG_OBJECT(self, "AU_TYPE = %s [%d]", au_str, au_type);
			if (au_type == 9)
			{
				if (!GST_BUFFER_PTS_IS_VALID(buffer))
				{
					GST_DEBUG_OBJECT(self, "writing missing pts to AU_DELIM");
					GST_BUFFER_PTS(buffer) = 0;
				}
				self->h264_initial_audelim_written = TRUE;
			}
		}
		else
			GST_TRACE_OBJECT(self, "data[%d] = %d :(", i, data[i]);
	}

	pes_header[0] = 0;
	pes_header[1] = 0;
	pes_header[2] = 1;
	pes_header[3] = 0xE0;
	// two unused bytes holding the remaining pes_header lenght.
	// here for video it can be zero which means header can have any lenght.
	pes_header[6] = 0x81;
	pes_header[7] = 0; /* no pts */
	pes_header[8] = 0;
	pes_header_len = 9;

	if (self->codec_type == CT_VC1 || self->codec_type == CT_VC1_SM)
	{
		if (!(GST_BUFFER_FLAGS(buffer) & GST_BUFFER_FLAG_DELTA_UNIT))
		{
			pes_header[6] = 0x80;
		}
	}

	if (GST_BUFFER_PTS_IS_VALID(buffer) || (self->use_dts && GST_BUFFER_DTS_IS_VALID(buffer)))
	{
		pes_header[7] = 0x80; /* pts */
		pes_header[8] = 5; /* pts size */
		pes_header_len += 5;
		pes_set_pts(GST_BUFFER_PTS_IS_VALID(buffer) ? GST_BUFFER_PTS(buffer) : GST_BUFFER_DTS(buffer), pes_header);

		if (self->codec_data)
		{
			if (self->must_send_header)
			{
				if (self->codec_type != CT_MPEG1 && self->codec_type != CT_MPEG2 && (self->codec_type != CT_DIVX4 || data[3] == 0x00))
				{
					if (self->codec_type == CT_DIVX311)
					{
						video_write(sink, self, self->codec_data, 0, codec_data_size);
					}
					else
					{
						memcpy(pes_header + pes_header_len, codec_data, codec_data_size);
						pes_header_len += codec_data_size;
					}
					self->must_send_header = FALSE;
				}
			}
			if (self->codec_type == CT_H264 || self->codec_type == CT_H265)
			{
				unsigned int pos = 0;
				if (self->h264_nal_len_size >= 3)
				{
					/* we need to write to the buffer */
					gst_buffer_unmap(buffer, &map);
					if (!gst_buffer_is_writable(buffer))
					{
						/* buffer is not writable, create a new buffer to which we can write */
						buffer = tmpbuf = gst_buffer_copy(buffer);
					}
					gst_buffer_map(buffer, &map, GST_MAP_READ | GST_MAP_WRITE);
					original_data = data = map.data;
					data_len = map.size;
					while (1)
					{
						unsigned int pack_len = 0;
						int i;
						for (i = 0; i < self->h264_nal_len_size; i++, pos++)
						{
							pack_len <<= 8;
							pack_len += data[pos];
							/* replace the lenght field with \x00..\x00\x01 */
							data[pos] = (i == self->h264_nal_len_size - 1) ? 1 : 0;
						}
						if ((pos + pack_len) >= data_len) break;
						pos += pack_len;
					}
				}
				else
				{
					/* length field too small to insert \x00\x00\x01, so we need to copy everything into a second buffer */
					unsigned char *dest;
					unsigned int dest_pos = 0;
					/* TODO: predict needed size, based on data_len and h264_nal_len_size, and number of frames */
					tmpbuf = gst_buffer_new_and_alloc(H264_BUFFER_SIZE);
					GstMapInfo tmpmap;
					gst_buffer_map(tmpbuf, &tmpmap, GST_MAP_READ | GST_MAP_WRITE);
					dest = tmpmap.data;
					while (1)
					{
						unsigned int pack_len = 0;
						int i;
						for (i = 0; i < self->h264_nal_len_size; i++, pos++)
						{
							pack_len <<= 8;
							pack_len += data[pos];
						}
						memcpy(dest + dest_pos, "\x00\x00\x01", 3);
						dest_pos += 3;
						memcpy(dest + dest_pos, data + pos, pack_len);
						dest_pos += pack_len;
						if ((pos + pack_len) >= data_len) break;
						pos += pack_len;
					}
					/* switch to the h264 buffer, where we copied the original render buffer contents */
					gst_buffer_unmap(buffer, &map);
					gst_buffer_unmap(tmpbuf, &tmpmap);
					buffer = tmpbuf;
					gst_buffer_map(buffer, &map, GST_MAP_READ);
					original_data = data = map.data;
					data_len = dest_pos;
				}
			}
			else if (self->codec_type == CT_MPEG4_PART2)
			{
				if (memcmp(data, "\x00\x00\x01", 3))
				{
					memcpy(pes_header + pes_header_len, "\x00\x00\x01", 3);
					pes_header_len += 3;
				}
			}
			else if (self->codec_type == CT_DIVX311)
			{
				if (memcmp(data, "\x00\x00\x01\xb6", 4))
				{
					memcpy(pes_header + pes_header_len, "\x00\x00\x01\xb6", 4);
					pes_header_len += 4;
				}
			}
		}
		else if (self->codec_type == CT_VP9 || self->codec_type == CT_VP8 || self->codec_type == CT_VP6 || self->codec_type == CT_SPARK) {
#if defined(VUPLUS)
			if (self->codec_type == CT_VP9)
			{
				uint32_t vp9_pts = (GST_TIME_AS_USECONDS(GST_BUFFER_PTS_IS_VALID(buffer) ? GST_BUFFER_PTS(buffer) : GST_BUFFER_DTS(buffer)) *45) / 1000;
				memcpy(&pes_header[9], &vp9_pts, sizeof(vp9_pts));
			}
#endif
			uint32_t len = data_len + 4 + 6;
			memcpy(pes_header+pes_header_len, "BCMV", 4);
			pes_header_len += 4;
			if (self->codec_type == CT_VP6)
				++len;
			pes_header[pes_header_len++] = (len & 0xFF000000) >> 24;
			pes_header[pes_header_len++] = (len & 0x00FF0000) >> 16;
			pes_header[pes_header_len++] = (len & 0x0000FF00) >> 8;
			pes_header[pes_header_len++] = (len & 0x000000FF) >> 0;
			pes_header[pes_header_len++] = 0;
#if defined(VUPLUS)
			pes_header[pes_header_len++] = 0;
#else
			pes_header[pes_header_len++] = self->codec_type == CT_VP9 ? 1 : 0;
#endif
			if (self->codec_type == CT_VP6)
				pes_header[pes_header_len++] = 0;
		}
	}

	payload_len = data_len + pes_header_len - 6;

	if (self->codec_type == CT_MPEG2 || self->codec_type == CT_MPEG1)
	{
		if (!self->codec_data && data_len > 3 && !memcmp(data, "\x00\x00\x01\xb3", 4))
		{
			gboolean ok = TRUE;
			unsigned int pos = 4;
			unsigned int sheader_data_len = 0;
			while (pos < data_len && ok)
			{
				if (pos >= data_len) break;
				pos += 7;
				if (pos >=data_len) break;
				sheader_data_len = 12;
				if (data[pos] & 2)
				{ // intra matrix
					pos += 64;
					if (pos >=data_len) break;
					sheader_data_len += 64;
				}
				if (data[pos] & 1)
				{ // non intra matrix
					pos += 64;
					if (pos >=data_len) break;
					sheader_data_len += 64;
				}
				pos += 1;
				if (pos + 3 >=data_len) break;
				if (!memcmp(&data[pos], "\x00\x00\x01\xb5", 4))
				{
					// extended start code
					pos += 3;
					sheader_data_len += 3;
					do
					{
						pos += 1;
						++sheader_data_len;
						if (pos + 2 > data_len)
						{
							ok = FALSE;
							break;
						}
					} while (memcmp(&data[pos], "\x00\x00\x01", 3));
					if (!ok) break;
				}
				if (pos + 3 >= data_len) break;
				if (!memcmp(&data[pos], "\x00\x00\x01\xb2", 4))
				{
					// private data
					pos += 3;
					sheader_data_len += 3;
					do
					{
						pos += 1;
						++sheader_data_len;
						if (pos + 2 > data_len)
						{
							ok = FALSE;
							break;
						}
					} while (memcmp(&data[pos], "\x00\x00\x01", 3));
					if (!ok) break;
				}
				self->codec_data = gst_buffer_new_and_alloc(sheader_data_len);
				if (self->codec_data)
				{
					gst_buffer_map(self->codec_data, &codecdatamap, GST_MAP_READ | GST_MAP_WRITE);
					codec_data = codecdatamap.data;
					codec_data_size = codecdatamap.size;
					memcpy(codec_data, data + pos - sheader_data_len, sheader_data_len);
				}
				self->must_send_header = FALSE;
				break;
			}
		}
		else if (self->codec_data && self->must_send_header)
		{
			int pos = 0;
			while (pos <= data_len - 4)
			{
				if (memcmp(&data[pos], "\x00\x00\x01\xb8", 4)) /* find group start code */
				{
					pos++;
					continue;
				}
				payload_len += codec_data_size;
				pes_set_payload_size(payload_len, pes_header);
				if (video_write(sink, self, self->pesheader_buffer, 0, pes_header_len) < 0) goto error;
				if (video_write(sink, self, buffer, data - original_data, (data - original_data) + pos) < 0) goto error;
				if (video_write(sink, self, self->codec_data, 0, codec_data_size) < 0) goto error;
				if (video_write(sink, self, buffer, data - original_data + pos, (data - original_data) + data_len - pos) < 0) goto error;
				self->must_send_header = FALSE;
				goto ok;
			}
		}
	}
#if defined(VUPLUS) || defined(GIGABLUEMIPS)
 	else if (self->codec_type == CT_VC1 || self->codec_type == CT_VC1_SM)
#else
	else if ((self->wmv_asf && self->codec_type == CT_VC1) || self->codec_type == CT_VC1_SM)
#endif
	{
		memcpy(pes_header + pes_header_len, "\x00\x00\x01\x0d", 4);
		pes_header_len += 4;
		payload_len += 4;
	}

#if defined(VUPLUS) || defined(HISILICON) || defined(OSMIO4K)
	pes_set_payload_size(payload_len, pes_header);
	if (video_write(sink, self, self->pesheader_buffer, 0, pes_header_len) < 0) goto error;
	if (video_write(sink, self, buffer, data - original_data, (data - original_data) + data_len) < 0) goto error;
#else
	if (self->codec_type == CT_VP9)
	{
		if (payload_len > 0x8008)
			payload_len = 0x8008;

		int offs = data - original_data;
		int bytes = payload_len - 10 - 8;

		pes_set_payload_size(payload_len, pes_header);

		if (video_write(sink, self, self->pesheader_buffer, 0, pes_header_len) < 0) goto error;
		if (video_write(sink, self, buffer, offs, offs + bytes) < 0) goto error;

		offs += bytes;

		while (bytes < data_len)
		{
			int left = data_len - bytes;
			int wr = 0x8000;
			if (wr > left)
				wr = left;

			gst_buffer_unmap(self->pesheader_buffer, &pesheadermap);
			gst_buffer_map(self->pesheader_buffer, &pesheadermap, GST_MAP_WRITE);
			pes_header = pesheadermap.data;

			//pes_header[0] = 0x00;
			//pes_header[1] = 0x00;
			//pes_header[2] = 0x01;
			//pes_header[3] = 0xE0;
			pes_header[6] = 0x81;
			pes_header[7] = 0x00;
			pes_header[8] = 0x00;
			pes_header_len = 9;

			pes_set_payload_size(wr + 3, pes_header);

			if (video_write(sink, self, self->pesheader_buffer, 0, pes_header_len) < 0) goto error;
			if (video_write(sink, self, buffer, offs, offs + wr) < 0) goto error;

			bytes += wr;
			offs += wr;
		}

		gst_buffer_unmap(self->pesheader_buffer, &pesheadermap);
		gst_buffer_map(self->pesheader_buffer, &pesheadermap, GST_MAP_WRITE);
		pes_header = pesheadermap.data;

		//pes_header[0] = 0x00;
		//pes_header[1] = 0x00;
		//pes_header[2] = 0x01;
		//pes_header[3] = 0xE0;
		pes_header[4] = 0x00;
		pes_header[5] = 0xB2;
		pes_header[6] = 0x81;
		pes_header[7] = 0x01;
		pes_header[8] = 0x14;
		pes_header[9] = 0x80;
		pes_header[10] = 'B';
		pes_header[11] = 'R';
		pes_header[12] = 'C';
		pes_header[13] = 'M';
		memset(pes_header+14, 0, 170);
		pes_header[26] = 0xFF;
		pes_header[27] = 0xFF;
		pes_header[28] = 0xFF;
		pes_header[29] = 0xFF;
		pes_header[33] = 0x85;

		if (video_write(sink, self, self->pesheader_buffer, 0, 184) < 0) goto error;
	}
	else
	{
		pes_set_payload_size(payload_len, pes_header);
		if (video_write(sink, self, self->pesheader_buffer, 0, pes_header_len) < 0) goto error;
		if (video_write(sink, self, buffer, data - original_data, (data - original_data) + data_len) < 0) goto error;
	}

#endif
	if (GST_BUFFER_PTS_IS_VALID(buffer) || (self->use_dts && GST_BUFFER_DTS_IS_VALID(buffer)))
	{
		self->pts_written = TRUE;
	}

ok:
#ifdef DREAMNEXTGEN
	if (dream_write_frame(self) < 0)
		goto error;
#endif
	gst_buffer_unmap(buffer, &map);
	gst_buffer_unmap(self->pesheader_buffer, &pesheadermap);
	if (self->codec_data)
	{
		gst_buffer_unmap(self->codec_data, &codecdatamap);
	}
	if (tmpbuf)
	{
		gst_buffer_unref(tmpbuf);
		tmpbuf = NULL;
	}

	return GST_FLOW_OK;
error:
	gst_buffer_unmap(buffer, &map);
	gst_buffer_unmap(self->pesheader_buffer, &pesheadermap);
	if (self->codec_data)
	{
		gst_buffer_unmap(self->codec_data, &codecdatamap);
	}
	if (tmpbuf)
	{
		gst_buffer_unref(tmpbuf);
		tmpbuf = NULL;
	}
	{
		GST_ELEMENT_ERROR(self, RESOURCE, READ, (NULL),
				("video write: %s", g_strerror (errno)));
		GST_WARNING_OBJECT (self, "Video write error");
		return ret == GST_FLOW_OK ? GST_FLOW_ERROR : ret;
	}
}

static gboolean gst_dvbvideosink_set_caps(GstBaseSink *basesink, GstCaps *caps)
{
	GstDVBVideoSink *self = GST_DVBVIDEOSINK (basesink);
	GstStructure *structure = gst_caps_get_structure (caps, 0);
	const char *mimetype = gst_structure_get_name (structure);
	t_stream_type prev_stream_type = self->stream_type;
	self->stream_type = STREAMTYPE_UNKNOWN;
	gboolean was_playing = self->playing;

	GST_INFO_OBJECT (self, "caps = %" GST_PTR_FORMAT, caps);

	if (self->codec_data)
	{
		gst_buffer_unref(self->codec_data);
		self->codec_data = NULL;
	}

	GST_DEBUG_OBJECT(self, "set_caps %" GST_PTR_FORMAT, caps);

	if (!strcmp (mimetype, "video/mpeg"))
	{
		gint mpegversion;
		gst_structure_get_int (structure, "mpegversion", &mpegversion);
		switch (mpegversion)
		{
			case 1:
				self->stream_type = STREAMTYPE_MPEG1;
				self->codec_type = CT_MPEG1;
				GST_INFO_OBJECT (self, "MIMETYPE video/mpeg1 -> STREAMTYPE_MPEG1");
			break;
			case 2:
				self->stream_type = STREAMTYPE_MPEG2;
				self->codec_type = CT_MPEG2;
				GST_INFO_OBJECT (self, "MIMETYPE video/mpeg2 -> STREAMTYPE_MPEG2");
			break;
			case 4:
			{
				self->stream_type = STREAMTYPE_MPEG4_Part2;
				const GValue *codec_data = gst_structure_get_value(structure, "codec_data");
				if (codec_data)
				{
					GST_INFO_OBJECT (self, "MPEG4 have codec data");
					self->codec_data = gst_value_get_buffer(codec_data);
					gst_buffer_ref (self->codec_data);
				}
				self->codec_type = CT_MPEG4_PART2;
				GST_INFO_OBJECT (self, "MIMETYPE video/mpeg4 -> STREAMTYPE_MPEG4_Part2");
			}
			break;
			default:
				GST_ELEMENT_ERROR (self, STREAM, FORMAT, (NULL), ("unhandled mpeg version %i", mpegversion));
			break;
		}
	}
	else if (!strcmp (mimetype, "video/x-3ivx"))
	{
		const GValue *codec_data = gst_structure_get_value(structure, "codec_data");
		if (codec_data)
		{
			GST_INFO_OBJECT (self, "have 3ivx codec... handle as CT_MPEG4_PART2");
			self->codec_data = gst_value_get_buffer(codec_data);
			gst_buffer_ref (self->codec_data);
		}
		self->stream_type = STREAMTYPE_MPEG4_Part2;
		self->codec_type = CT_MPEG4_PART2;
		GST_INFO_OBJECT (self, "MIMETYPE video/x-3ivx -> STREAMTYPE_MPEG4_Part2");
	}
	else if (!strcmp (mimetype, "video/x-h264"))
	{
		const GValue *cd_data = gst_structure_get_value(structure, "codec_data");
		self->stream_type = STREAMTYPE_MPEG4_H264;
		self->codec_type = CT_H264;
		if (cd_data)
		{
			unsigned char tmp[2048];
			unsigned int tmp_len = 0;
			GstBuffer *codec_data = gst_value_get_buffer(cd_data);
			guint8 *data;
			gsize cd_len;
			unsigned int cd_pos = 0;
			GstMapInfo codecdatamap;
			gst_buffer_map(codec_data, &codecdatamap, GST_MAP_READ);
			data = codecdatamap.data;
			cd_len = codecdatamap.size;
			GST_INFO_OBJECT (self, "H264 have codec data..!");
			if (cd_len > 7 && data[0] == 1)
			{
				unsigned short len = (data[6] << 8) | data[7];
				if (cd_len >= (len + 8))
				{
					unsigned int i=0;
					uint8_t profile_num[] = { 66, 77, 88, 100 };
					uint8_t profile_cmp[2] = { 0x67, 0x00 };
					const char *profile_str[] = { "baseline", "main", "extended", "high" };
					memcpy(tmp, "\x00\x00\x00\x01", 4);
					tmp_len += 4;
					memcpy(tmp + tmp_len, data + 8, len);
					for (i = 0; i < 4; ++i)
					{
						profile_cmp[1] = profile_num[i];
						if (!memcmp(tmp+tmp_len, profile_cmp, 2))
						{
							uint8_t level_org = tmp[tmp_len + 3];
							/* not for the Amlogic decoder, it sizes its buffers by the real level */
#ifndef DREAMNEXTGEN
							if (level_org > 0x29)
							{
								GST_INFO_OBJECT (self, "H264 %s profile@%d.%d patched down to 4.1!", profile_str[i], level_org / 10 , level_org % 10);
								tmp[tmp_len+3] = 0x29; // level 4.1
							}
							else
#endif
							{
								GST_INFO_OBJECT (self, "H264 %s profile@%d.%d", profile_str[i], level_org / 10 , level_org % 10);
							}
							break;
						}
					}
					tmp_len += len;
					cd_pos = 8 + len;
					if (cd_len > (cd_pos + 2))
					{
						len = (data[cd_pos + 1] << 8) | data[cd_pos + 2];
						cd_pos += 3;
						if ((cd_len >= (cd_pos+len)) && (tmp_len+len+4 < 2048))
						{
							memcpy(tmp+tmp_len, "\x00\x00\x00\x01", 4);
							tmp_len += 4;
							memcpy(tmp+tmp_len, data+cd_pos, len);
							tmp_len += len;
							self->codec_data = gst_buffer_new_and_alloc(tmp_len);
							gst_buffer_fill(self->codec_data, 0, tmp, tmp_len);
							self->h264_nal_len_size = (data[4] & 0x03) + 1;
						}
						else
						{
							GST_WARNING_OBJECT (self, "codec_data too short(4)");
						}
					}
					else
					{
						GST_WARNING_OBJECT (self, "codec_data too short(3)");
					}
				}
				else
				{
					GST_WARNING_OBJECT (self, "codec_data too short(2)");
				}
			}
			else if (cd_len <= 7)
			{
				GST_WARNING_OBJECT (self, "codec_data too short(1)");
			}
			else
			{
				GST_WARNING_OBJECT (self, "wrong avcC version %d!", data[0]);
			}
			gst_buffer_unmap(codec_data, &codecdatamap);
		}
		else
		{
			self->h264_nal_len_size = 0;
		}
		GST_INFO_OBJECT (self, "MIMETYPE video/x-h264 -> STREAMTYPE_MPEG4_H264");
	}
	else if (!strcmp (mimetype, "video/x-h265"))
	{
		const GValue *cd_data = gst_structure_get_value(structure, "codec_data");
		self->stream_type = STREAMTYPE_MPEG4_H265;
		self->codec_type = CT_H265;
		if (cd_data)
		{
			unsigned char tmp[2048];
			unsigned int tmp_len = 0;
			GstBuffer *codec_data = gst_value_get_buffer(cd_data);
			guint8 *data;
			gsize cd_len;
			unsigned int cd_pos = 0;
			GstMapInfo codecdatamap;
			gst_buffer_map(codec_data, &codecdatamap, GST_MAP_READ);
			data = codecdatamap.data;
			cd_len = codecdatamap.size;
			GST_INFO_OBJECT (self, "H265 have codec data..!");

			if (cd_len > 3 && (data[0] || data[1] || data[2] > 1)) {
				if (cd_len > 22) {
					int i;
					if (data[0] != 0) {
						GST_ELEMENT_WARNING (self, STREAM, DECODE, ("Unsupported extra data version %d, decoding may fail", data[0]), (NULL));
					}
					self->h264_nal_len_size = (data[21] & 3) + 1;
					int num_param_sets = data[22];
					int pos = 23;
					for (i = 0; i < num_param_sets; i++) {
						int j;
						if (pos + 3 > cd_len) {
							GST_ELEMENT_ERROR (self, STREAM, DECODE, ("Buffer underrun in extra header (%d >= %ld)", pos + 3, cd_len), (NULL));
							break;
						}
						// ignore flags + NAL type (1 byte)
						int nal_type = data[pos] & 0x3f;
						int nal_count = data[pos + 1] << 8 | data[pos + 2];
						pos += 3;
						for (j = 0; j < nal_count; j++) {
							if (pos + 2 > cd_len) {
								GST_ELEMENT_ERROR (self, STREAM, DECODE, ("Buffer underrun in extra nal header (%d >= %ld)", pos + 2, cd_len), (NULL));
								break;
							}
							int nal_size = data[pos] << 8 | data[pos + 1];
							pos += 2;
							if (pos + nal_size > cd_len) {
								GST_ELEMENT_ERROR (self, STREAM, DECODE, ("Buffer underrun in extra nal (%d >= %ld)", pos + 2 + nal_size, cd_len), (NULL));
								break;
							}
							if ((nal_type == 0x20 || nal_type == 0x21 || nal_type == 0x22) && ((tmp_len + 4 + nal_size) < 2048 )) // use only VPS, SPS, PPS nals
							{
								memcpy(tmp+tmp_len, "\x00\x00\x00\x01", 4);
								tmp_len += 4;
								memcpy(tmp + tmp_len, data + pos, nal_size);
								tmp_len += nal_size;
							}
							else if ((tmp_len + 4 + nal_size) >= 2048)
							{
								GST_ELEMENT_WARNING (self, STREAM, DECODE, ("Ignoring nal as tmp buffer is too small tmp_len + nal = %d", tmp_len + 4 + nal_size), (NULL));
							}
							pos += nal_size;
						}
					}
				}
				GST_DEBUG ("Assuming packetized data (%d bytes length)", self->h264_nal_len_size);
				{
					self->codec_data = gst_buffer_new_and_alloc(tmp_len);
					gst_buffer_fill(self->codec_data, 0, tmp, tmp_len);
				}
			}
			gst_buffer_unmap(codec_data, &codecdatamap);
		}
		else
		{
			self->h264_nal_len_size = 0;
		}
		GST_INFO_OBJECT (self, "MIMETYPE video/x-h265 -> STREAMTYPE_MPEG4_H265");
	}
	else if (!strcmp (mimetype, "video/x-h263"))
	{
		self->stream_type = STREAMTYPE_H263;
		self->codec_type = CT_MPEG4_PART2;
		GST_INFO_OBJECT (self, "MIMETYPE video/x-h263 -> STREAMTYPE_H263");
	}
	else if (!strcmp (mimetype, "video/x-xvid"))
	{
		self->stream_type = STREAMTYPE_XVID;
		self->codec_type = CT_MPEG4_PART2;
		GST_INFO_OBJECT (self, "MIMETYPE video/x-xvid -> STREAMTYPE_XVID");
	}
	else if (!strcmp (mimetype, "video/x-divx") || !strcmp (mimetype, "video/x-msmpeg"))
	{
		gint divxversion = -1;
		if (!gst_structure_get_int(structure, "divxversion", &divxversion))
		{
			gst_structure_get_int(structure, "msmpegversion", &divxversion);
		}
		switch (divxversion)
		{
			case 3:
			case 43:
			{
				#define B_GET_BITS(w,e,b)  (((w)>>(b))&(((unsigned)(-1))>>((sizeof(unsigned))*8-(e+1-b))))
				#define B_SET_BITS(name,v,e,b)  (((unsigned)(v))<<(b))
				static const guint8 brcm_divx311_sequence_header[] =
				{
					0x00, 0x00, 0x01, 0xE0, 0x00, 0x34, 0x80, 0x80, // PES HEADER
					0x05, 0x2F, 0xFF, 0xFF, 0xFF, 0xFF, 
					0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x20, /* 0 .. 7 */
					0x08, 0xC8, 0x0D, 0x40, 0x00, 0x53, 0x88, 0x40, /* 8 .. 15 */
					0x0C, 0x40, 0x01, 0x90, 0x00, 0x97, 0x53, 0x0A, /* 16 .. 24 */
					0x00, 0x00, 0x00, 0x00,
					0x30, 0x7F, 0x00, 0x00, 0x01, 0xB2, 0x44, 0x69, /* 0 .. 7 */
					0x76, 0x58, 0x33, 0x31, 0x31, 0x41, 0x4E, 0x44  /* 8 .. 15 */
				};
				self->codec_data = gst_buffer_new_and_alloc(63);
				guint8 *data;
				gint height, width;
				GstMapInfo map;
				gst_buffer_map(self->codec_data, &map, GST_MAP_WRITE);
				data = map.data;
				gst_structure_get_int (structure, "height", &height);
				gst_structure_get_int (structure, "width", &width);
				memcpy(data, brcm_divx311_sequence_header, 63);
				data += 38;
				data[0] = B_GET_BITS(width,11,4);
				data[1] = B_SET_BITS("width [3..0]", B_GET_BITS(width,3,0), 7, 4) |
					B_SET_BITS("'10'", 0x02, 3, 2) |
					B_SET_BITS("height [11..10]", B_GET_BITS(height,11,10), 1, 0);
				data[2] = B_GET_BITS(height,9,2);
				data[3]= B_SET_BITS("height [1.0]", B_GET_BITS(height,1,0), 7, 6) |
					B_SET_BITS("'100000'", 0x20, 5, 0);
				self->stream_type = STREAMTYPE_DIVX311;
				self->codec_type = CT_DIVX311;
				GST_INFO_OBJECT (self, "MIMETYPE video/x-divx vers. 3 -> STREAMTYPE_DIVX311");
				gst_buffer_unmap(self->codec_data, &map);
			}
			break;
			case 4:
				self->stream_type = STREAMTYPE_DIVX4;
				self->codec_type = CT_DIVX4;
				self->codec_data = gst_buffer_new_and_alloc(12);
				gst_buffer_fill(self->codec_data, 0, "\x00\x00\x01\xb2\x44\x69\x76\x58\x34\x41\x4e\x44", 12);
				GST_INFO_OBJECT (self, "MIMETYPE video/x-divx vers. 4 -> STREAMTYPE_DIVX4");
			break;
			case 6:
			case 5:
				self->stream_type = STREAMTYPE_DIVX5;
				GST_INFO_OBJECT (self, "MIMETYPE video/x-divx vers. %d -> STREAMTYPE_DIVX5", divxversion);
			break;
			default:
				GST_ELEMENT_ERROR (self, STREAM, FORMAT, (NULL), ("unhandled divx version %i", divxversion));
			break;
		}
	}
	else if (!strcmp (mimetype, "video/x-wmv"))
	{
		guint32 fourcc = 0;
		const gchar *value = gst_structure_get_string(structure, "format");
		if (value)
		{
			fourcc = GST_STR_FOURCC(value);
		}
		if (fourcc == GST_MAKE_FOURCC('W', 'V', 'C', '1') || fourcc == GST_MAKE_FOURCC('W', 'M', 'V', 'A'))
		{
			self->stream_type = STREAMTYPE_VC1;
			self->codec_type = CT_VC1;
			GST_INFO_OBJECT (self, "MIMETYPE video/x-wmv %s -> STREAMTYPE_VC1", value);
		}
		else
		{
			self->stream_type = STREAMTYPE_VC1_SM;
			self->codec_type = CT_VC1_SM;
			GST_INFO_OBJECT (self, "MIMETYPE video/x-wmv %s -> STREAMTYPE_VC1_SM", value);
		}
	}
	else if (!strcmp (mimetype, "video/x-vp6") || !strcmp (mimetype, "video/x-vp6-flash"))
	{
		self->stream_type = STREAMTYPE_VB6;
		self->codec_type = CT_VP6;
		GST_INFO_OBJECT (self, "MIMETYPE %s -> VIDEO_SET_STREAMTYPE, STREAMTYPE_VB6", mimetype);
	}
	else if (!strcmp (mimetype, "video/x-vp8"))
	{
		self->stream_type = STREAMTYPE_VB8;
		self->codec_type = CT_VP8;
		GST_INFO_OBJECT (self, "MIMETYPE video/x-vp8 -> VIDEO_SET_STREAMTYPE, STREAMTYPE_VB8");
	}
	else if (!strcmp (mimetype, "video/x-vp9"))
	{
		self->stream_type = STREAMTYPE_VB9;
		self->codec_type = CT_VP9;
		GST_INFO_OBJECT (self, "MIMETYPE video/x-vp9 -> VIDEO_SET_STREAMTYPE, STREAMTYPE_VB9");
	}
	else if (!strcmp (mimetype, "video/x-flash-video"))
	{
		self->stream_type = STREAMTYPE_SPARK;
		self->codec_type = CT_SPARK;
		GST_INFO_OBJECT (self, "MIMETYPE video/x-flash-video -> VIDEO_SET_STREAMTYPE, STREAMTYPE_SPARK");
	}
#ifdef DREAMNEXTGEN
	else if (!strcmp (mimetype, "image/jpeg"))
	{
		self->stream_type = STREAMTYPE_MJPEG;
		self->codec_type = CT_MJPEG;
		GST_INFO_OBJECT (self, "MIMETYPE image/jpeg -> STREAMTYPE_MJPEG");
	}
	else if (!strcmp (mimetype, "video/x-pn-realvideo"))
	{
		self->stream_type = STREAMTYPE_REAL;
		self->codec_type = CT_REAL;
		GST_INFO_OBJECT (self, "MIMETYPE video/x-pn-realvideo -> STREAMTYPE_REAL");
	}
	else if (!strcmp (mimetype, "video/x-cavs"))
	{
		self->stream_type = STREAMTYPE_AVS;
		self->codec_type = CT_AVS;
		GST_INFO_OBJECT (self, "MIMETYPE video/x-cavs -> STREAMTYPE_AVS");
	}
#endif

	/* Adaptive streams can replace the parameter sets without changing codec.
	 * Send the new AVC/HEVC headers before the next timestamped access unit. */
	if ((self->codec_type == CT_H264 || self->codec_type == CT_H265) &&
		self->codec_data && gst_buffer_get_size(self->codec_data))
		self->must_send_header = TRUE;

	if (self->stream_type != STREAMTYPE_UNKNOWN)
	{
		gint numerator, denominator;
		if (gst_structure_get_fraction (structure, "framerate", &numerator, &denominator))
		{
			FILE *f = fopen("/proc/stb/vmpeg/0/fallback_framerate", "w");
			if (f)
			{
				int valid_framerates[] = { 23976, 24000, 25000, 29970, 30000, 50000, 59940, 60000 };
				int framerate = (int)(((double)numerator * 1000) / denominator);
				int diff = 60000;
				int best = 0;
				int i = 0;
				for (; i < 7; ++i)
				{
					int ndiff = abs(framerate - valid_framerates[i]);
					if (ndiff < diff)
					{
						diff = ndiff;
						best = i;
					}
				}
				fprintf(f, "%d", valid_framerates[best]);
				fclose(f);
			}
		}
		if (self->playing && self->stream_type != prev_stream_type)
		{
			if (self->fd >= 0)
			{
				ioctl(self->fd, VIDEO_STOP, 0);
				//if(ioctl(self->fd, VIDEO_CLEAR_BUFFER) >= 0)
					//GST_INFO_OBJECT(self, "new streamtype VIDEO BUFFER FLUSHED");
			}
			self->playing = FALSE;
		}
#if defined(DREAMNEXTGEN)
		if (!self->playing && !dream_setup(self, structure))
		{
			GST_ELEMENT_ERROR(self, STREAM, CODEC_NOT_FOUND, (NULL), ("hardware decoder can't handle streamtype %i", self->stream_type));
		}
#elif defined(VIDEO_SET_ENCODING)
		if (self->use_set_encoding)
		{
			unsigned int encoding = streamtype_to_encoding(self->stream_type);
			if (!self->playing && (self->fd < 0 || ioctl(self->fd, VIDEO_SET_ENCODING, encoding) < 0))
			{
				GST_ELEMENT_ERROR(self, STREAM, DECODE, (NULL), ("hardware decoder can't be set to encoding %i", encoding));
			}
		}
		else
		{
			if (!self->playing && (self->fd < 0 || ioctl(self->fd, VIDEO_SET_STREAMTYPE, self->stream_type) < 0))
			{
				GST_ELEMENT_ERROR(self, STREAM, CODEC_NOT_FOUND, (NULL), ("hardware decoder can't handle streamtype %i", self->stream_type));
			}
		}
#else
		if (!self->playing && (self->fd < 0 || ioctl(self->fd, VIDEO_SET_STREAMTYPE, self->stream_type) < 0))
		{
			GST_ELEMENT_ERROR(self, STREAM, CODEC_NOT_FOUND, (NULL), ("hardware decoder can't handle streamtype %i", self->stream_type));
		}
#endif
		if (self->fd >= 0) 
		{
			if (self->codec_type == CT_VC1)
			{
				const GValue *codec_data = gst_structure_get_value(structure, "codec_data");
				if (codec_data)
				{
					guint8 *codec_data_pointer;
					gint codec_size;
					guint8 *data;
					video_codec_data_t videocodecdata;
					GstMapInfo codecdatamap;
					gst_buffer_map(gst_value_get_buffer(codec_data), &codecdatamap, GST_MAP_READ);
					codec_data_pointer = codecdatamap.data;
					codec_size = codecdatamap.size;
#if defined(DREAMBOX) || defined(DAGS)
					GstMapInfo map;
					self->codec_data = gst_buffer_new_and_alloc(8 + codec_size);
					gst_buffer_map(self->codec_data, &map, GST_MAP_WRITE);
					data = map.data;
					data += 8;
					if (codec_data && codec_size) memcpy(data , codec_data_pointer, codec_size);
#else
					videocodecdata.length = 8 + codec_size;
					data = videocodecdata.data = (guint8*)g_malloc(videocodecdata.length);
					memset(data, 0, videocodecdata.length);
					data += 8;
					memcpy(data, codec_data_pointer, codec_size);
					ioctl(self->fd, VIDEO_SET_CODEC_DATA, &videocodecdata);
					g_free(videocodecdata.data);
#endif
					gst_buffer_unmap(gst_value_get_buffer(codec_data), &codecdatamap);
#if defined(DREAMBOX) || defined(DAGS)
					gst_buffer_unmap(self->codec_data, &map);
#endif
				}
			}
			else if (self->codec_type == CT_VC1_SM)
			{
				const GValue *codec_data = gst_structure_get_value(structure, "codec_data");
				if (codec_data)
				{
					guint8 *codec_data_pointer;
					gint codec_size;
					guint8 *data;
					video_codec_data_t videocodecdata;
					gint width, height;
					GstMapInfo codecdatamap;
					gst_buffer_map(gst_value_get_buffer(codec_data), &codecdatamap, GST_MAP_READ);
					codec_data_pointer = codecdatamap.data;
					codec_size = codecdatamap.size;
					if (codec_size > 4) codec_size = 4;
					gst_structure_get_int(structure, "width", &width);
					gst_structure_get_int(structure, "height", &height);
#if defined(DREAMBOX) || defined(DAGS)
					GstMapInfo map;
					self->codec_data = gst_buffer_new_and_alloc(18 + codec_size);
					gst_buffer_map(self->codec_data, &map, GST_MAP_WRITE);
					data = map.data;
					/* pes header */
					*(data++) = 0x00;
					*(data++) = 0x00;
					*(data++) = 0x01;
					*(data++) = 0x0f;
					/* width */
					*(data++) = (width >> 8) & 0xff;
					*(data++) = width & 0xff;
					/* height */
					*(data++) = (height >> 8) & 0xff;
					*(data++) = height & 0xff;
					if (codec_data && codec_size) memcpy(data, codec_data_pointer, codec_size);
#else
					videocodecdata.length = 33;
					data = videocodecdata.data = (guint8*)g_malloc(videocodecdata.length);
					memset(data, 0, videocodecdata.length);
					data += 18;
					/* width */
					*(data++) = (width >> 8) & 0xff;
					*(data++) = width & 0xff;
					/* height */
					*(data++) = (height >> 8) & 0xff;
					*(data++) = height & 0xff;
					if (codec_data && codec_size) memcpy(data, codec_data_pointer, codec_size);
					ioctl(self->fd, VIDEO_SET_CODEC_DATA, &videocodecdata);
					g_free(videocodecdata.data);
#endif
					gst_buffer_unmap(gst_value_get_buffer(codec_data), &codecdatamap);
#if defined(DREAMBOX) || defined(DAGS)
					gst_buffer_unmap(self->codec_data, &map);
#endif
				}
			}
			if (!self->playing)
			{
				ioctl(self->fd, VIDEO_PLAY);
#ifdef DREAMNEXTGEN
				/* a codec change while playing */
				if (!self->paused)
					ioctl(self->fd, VIDEO_CONTINUE);
#endif
			}
		}
		self->playing = TRUE;
	}
	else
	{
		GST_ELEMENT_ERROR (self, STREAM, TYPE_NOT_FOUND, (NULL), ("unimplemented stream type %s", mimetype));
	}

	return TRUE;
}

static gboolean gst_dvbvideosink_start(GstBaseSink *basesink)
{
	GstDVBVideoSink *self = GST_DVBVIDEOSINK(basesink);

	GST_DEBUG_OBJECT(self, "start");
	FILE *f;

	if (socketpair(PF_UNIX, SOCK_STREAM, 0, self->unlockfd) < 0)
	{
		perror("socketpair");
		goto error;
	}

	fcntl(self->unlockfd[0], F_SETFL, O_NONBLOCK);
	fcntl(self->unlockfd[1], F_SETFL, O_NONBLOCK);

	self->pesheader_buffer = gst_buffer_new_and_alloc(2048);

	f = fopen("/proc/stb/vmpeg/0/fallback_framerate", "r");
	if (f)
	{
		fgets(self->saved_fallback_framerate, sizeof(self->saved_fallback_framerate), f);
		fclose(f);
		f = NULL;
	}

	self->fd = open("/dev/dvb/adapter0/video0", O_RDWR | O_NONBLOCK);
#ifdef DREAMNEXTGEN
	self->poll_fd = open("/dev/amvideo_poll", O_RDWR | O_NONBLOCK);
	f = fopen("/sys/class/video/disable_video", "w");
	if (f)
	{
		fputs("0", f);
		fclose(f);
		f = NULL;
	}
	if (self->fd >= 0)
	{
		/* the state of the previous user */
		ioctl(self->fd, VIDEO_STOP, 0);
		ioctl(self->fd, VIDEO_CLEAR_BUFFER);
	}
	self->flush_vpts = dream_tsync("pts_video");
	self->start_pts = DREAM_PTS_NONE;
	self->wait_keyframe = FALSE;
	self->trick_base = DREAM_PTS_NONE;
	self->frames_written = self->frames_refused = 0;
	self->info_width = self->info_height = self->info_aspect = self->info_framerate = self->info_progressive = -1;
#endif

	self->pts_written = FALSE;
	self->lastpts = 0;

	return TRUE;
error:
	{
		GST_ELEMENT_ERROR (self, RESOURCE, OPEN_READ_WRITE, (NULL),
				GST_ERROR_SYSTEM);
		return FALSE;
	}
}

static gboolean gst_dvbvideosink_stop(GstBaseSink *basesink)
{
	GstDVBVideoSink *self = GST_DVBVIDEOSINK(basesink);
	FILE *f = NULL;
	GST_INFO_OBJECT(self, "stop");
	if (self->fd >= 0)
	{
		if (self->playing)
			ioctl(self->fd, VIDEO_STOP);
		if (self->rate != 1.0)
		{
			ioctl(self->fd, VIDEO_SLOWMOTION, 0);
			ioctl(self->fd, VIDEO_FAST_FORWARD, 0);
			self->rate = 1.0;
		}
		ioctl(self->fd, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_DEMUX);
		if(ioctl(self->fd, VIDEO_CLEAR_BUFFER) >= 0)
			GST_INFO_OBJECT(self, "STOP VIDEO BUFFER FLUSHED");
		close(self->fd);
	}

	if (self->codec_data)
		gst_buffer_unref(self->codec_data);
	if (self->pesheader_buffer)
		gst_buffer_unref(self->pesheader_buffer);
	while (self->queue)
	{
		queue_pop(&self->queue);
	}
#ifdef DREAMNEXTGEN
	dream_flush(self);
	if (self->poll_fd >= 0)
		close(self->poll_fd);
	self->poll_fd = -1;
	if (self->headers)
		gst_buffer_unref(self->headers);
	self->headers = NULL;
#endif

	f = fopen("/proc/stb/vmpeg/0/fallback_framerate", "w");
	if (f)
	{
		fputs(self->saved_fallback_framerate, f);
		fclose(f);
		f = NULL;
	}

	/* close write end first */
	if (self->unlockfd[1] >= 0)
		close(self->unlockfd[1]);
	if (self->unlockfd[0] >= 0)
		close(self->unlockfd[0]);
	self->must_send_header = TRUE;
	self->h264_nal_len_size = 0;
	self->h264_initial_audelim_written = FALSE;
	self->pesheader_buffer = NULL;
	self->codec_data = NULL;
	self->codec_type = CT_UNKNOWN;
	self->stream_type = STREAMTYPE_UNKNOWN;
	self->use_dts = FALSE;
	self->paused = self->playing = self->unlocking = self->flushing = self->first_paused = FALSE;
	self->pts_written = self->using_dts_downmix = self->synchronized = self->pass_eos = FALSE;
	self->lastpts = 0;
	self->timestamp_offset = 0;
	self->queue = NULL;
	self->fd = -1;
	self->unlockfd[0] = self->unlockfd[1] = -1;
	self->saved_fallback_framerate[0] = 0;
	self->rate = 1.0;
	self->wmv_asf = FALSE;
#ifdef VIDEO_SET_ENCODING
	self->use_set_encoding = TRUE;
#else
	self->use_set_encoding = FALSE;
#endif
	gst_base_sink_set_sync(GST_BASE_SINK(self), FALSE);
	gst_base_sink_set_async_enabled(GST_BASE_SINK(self), FALSE);
	GST_INFO_OBJECT(self, "STOP MEDIA COMPLETED");
	return TRUE;
}

static GstStateChangeReturn gst_dvbvideosink_change_state(GstElement *element, GstStateChange transition)
{
	GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
	GstDVBVideoSink *self = GST_DVBVIDEOSINK (element);
	FILE *f;

	switch (transition)
	{
	case GST_STATE_CHANGE_NULL_TO_READY:
		GST_INFO_OBJECT (self,"GST_STATE_CHANGE_NULL_TO_READY");
// special debug added to check correct machinebuild during development phase
#ifdef DREAMBOX
		GST_INFO_OBJECT(self,"BUILD FOR DREAMBOX");
#endif
#ifdef VUPLUS
		GST_INFO_OBJECT(self,"BUILD FOR VUPLUS");
#endif
#ifdef TYPE2
		GST_INFO_OBJECT(self,"BUILD FOR TYPE2 by-passes");
#endif
/* 	This debug added to check that sink was build for right boxtype
	In openatv the DVBMEDIASINK_CONFIG will in future be based on ${MACHINE}
	Depending on that other defines and or specific machine code can be set.
	Some extra defines will be added to configur.ac file and then be used to limit
	code lines or change some codelines at compile time, then the final build dvbmediasink
	can be kept small and small into memory also, cause some older stb'so have very few memory.
	But machine is also added as a defined var containing the stb box group for mediasink , this can then be used :
	in a if (!strcmp(machine, "<stbgroup>")) where stbgroup comes from ${MACHINE} at compile time.
	example for a dreambox 8000 it will be dm8000 for vuplus duo2 it will be vuduo2 for mutant51 it will be hd51. */
#ifdef machine
		GST_INFO_OBJECT(self,"BUILD FOR STB BOXTYPE %s", machine);
#endif
		self->ok_to_write = 1;
		break;
	case GST_STATE_CHANGE_READY_TO_PAUSED:
		GST_INFO_OBJECT (self,"GST_STATE_CHANGE_READY_TO_PAUSED");
		self->paused = TRUE;
		self->first_paused = TRUE;
		if (self->fd >= 0)
		{
			/* the Dream decoder knows the picture only after the first frames,
			 * dream_post_info reports it then */
#if defined(DREAMBOX) && !defined(DREAMNEXTGEN)
			GstStructure *s;
			GstMessage *msg;
			int aspect = -1, width = -1, height = -1, framerate = -1, progressive = -1;

			progressive = readMpegProc("progressive", 0);

			if(readApiSize(self->fd, &width, &height, &aspect) == -1)
			{
				aspect = readMpegProc("aspect", 0);
				width = readMpegProc("xres", 0);
				height = readMpegProc("yres", 0);
			}
			else
			{
				aspect = aspect == 0 ? 2 : 3; // dvb api to etsi
			}

			if(readApiFrameRate(self->fd, &framerate) == -1)
			{
				framerate = readMpegProc("framerate", 0);
			}

			s = gst_structure_new ("eventSizeAvail",
				"aspect_ratio", G_TYPE_INT, aspect == 0 ? 2 : 3,
				"width", G_TYPE_INT, width,
				"height", G_TYPE_INT, height, NULL);
			msg = gst_message_new_element (GST_OBJECT (element), s);
			gst_element_post_message (GST_ELEMENT (element), msg);

			s = gst_structure_new ("eventFrameRateAvail",
				"frame_rate", G_TYPE_INT, framerate, NULL);
			msg = gst_message_new_element (GST_OBJECT (element), s);
			gst_element_post_message (GST_ELEMENT (element), msg);

			s = gst_structure_new ("eventProgressiveAvail",
				"progressive", G_TYPE_INT, progressive, NULL);
			msg = gst_message_new_element (GST_OBJECT (element), s);
			gst_element_post_message (GST_ELEMENT (element), msg);
#endif
			ioctl(self->fd, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_MEMORY);
			ioctl(self->fd, VIDEO_FREEZE);
		}
		if(get_downmix_ready())
			self->using_dts_downmix = TRUE;
		break;
	case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
		GST_INFO_OBJECT (self,"GST_STATE_CHANGE_PAUSED_TO_PLAYING");
		if (self->fd >= 0)
			ioctl(self->fd, VIDEO_CONTINUE);
		self->paused = FALSE;
		break;
	default:
		break;
	}

	ret = GST_ELEMENT_CLASS(parent_class)->change_state(element, transition);

	switch (transition)
	{
	case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
		GST_INFO_OBJECT (self,"GST_STATE_CHANGE_PLAYING_TO_PAUSED");
		self->paused = TRUE;
		if (self->fd >= 0) ioctl(self->fd, VIDEO_FREEZE);
		/* wakeup the poll */
		write(self->unlockfd[1], "\x01", 1);
		break;
	case GST_STATE_CHANGE_PAUSED_TO_READY:
		GST_INFO_OBJECT (self,"GST_STATE_CHANGE_PAUSED_TO_READY");
		break;
	case GST_STATE_CHANGE_READY_TO_NULL:
		GST_INFO_OBJECT(self, "GST_STATE_CHANGE_READY_TO_NULL");
		break;
	default:
		break;
	}

	return ret;
}

/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and pad templates
 * register the features
 *
 * exchange the string 'plugin' with your elemnt name
 */
static gboolean plugin_init (GstPlugin *plugin)
{
	gst_debug_set_colored(GST_DEBUG_COLOR_MODE_OFF);
	return gst_element_register (plugin, "dvbvideosink",
						 GST_RANK_PRIMARY,
						 GST_TYPE_DVBVIDEOSINK);
}

/* this is the structure that gstreamer looks for to register plugins
 *
 * exchange the strings 'plugin' and 'Template plugin' with you plugin name and
 * description
 */
GST_PLUGIN_DEFINE (
	GST_VERSION_MAJOR,
	GST_VERSION_MINOR,
	dvbvideosink,
	"DVB Video Output",
	plugin_init,
	VERSION,
	"LGPL",
	"GStreamer",
	"http://gstreamer.net/"
)
