// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2025 Šerif Rami <ramiserifpersia@gmail.com>

#include <linux/unaligned.h>
#include "us144mkii_pcm.h"

const struct snd_pcm_hardware tascam_capture_hw = {
	.info = (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
	SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_MMAP_VALID |
	SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME),
	.formats = SNDRV_PCM_FMTBIT_S32_LE,
	.rates = (SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
	SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000),
	.rate_min = 44100,
	.rate_max = 96000,
	.channels_min = NUM_CHANNELS,
	.channels_max = NUM_CHANNELS,
	.buffer_bytes_max = 1024 * 1024,
	.period_bytes_min = 32 * BYTES_PER_FRAME,
	.period_bytes_max = 1024 * BYTES_PER_FRAME,
	.periods_min = 2,
	.periods_max = 1024,
};

static int tascam_capture_open(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);

	substream->runtime->hw = tascam_capture_hw;

	if (tascam->dev_id == USB_PID_TASCAM_US122MKII) {
		substream->runtime->hw.channels_min = 2;
		substream->runtime->hw.channels_max = 2;
	}

	tascam->capture_substream = substream;
	atomic_set(&tascam->capture_active, 0);
	return 0;
}

static int tascam_capture_close(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);

	atomic_set(&tascam->capture_active, 0);
	usb_kill_anchored_urbs(&tascam->capture_anchor);
	tascam->capture_substream = NULL;
	return 0;
}

static int tascam_capture_prepare(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);

	usb_kill_anchored_urbs(&tascam->capture_anchor);

	tascam->driver_capture_pos = 0;
	tascam->capture_frames_processed = 0;
	tascam->last_cap_period_pos = 0;
	atomic_set(&tascam->implicit_fb_frames, 0);
	return 0;
}

static snd_pcm_uframes_t tascam_capture_pointer(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	unsigned long flags;
	u64 pos;
	snd_pcm_uframes_t buffer_size = substream->runtime->buffer_size;

	if (!atomic_read(&tascam->capture_active))
		return 0;
	spin_lock_irqsave(&tascam->lock, flags);
	pos = tascam->capture_frames_processed;
	spin_unlock_irqrestore(&tascam->lock, flags);

	return (snd_pcm_uframes_t)(pos % buffer_size);
}

static int tascam_capture_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	int i, ret = 0;
	bool start = false;
	bool stop = false;
	unsigned long flags;

	spin_lock_irqsave(&tascam->lock, flags);
	switch (cmd) {
		case SNDRV_PCM_TRIGGER_START:
		case SNDRV_PCM_TRIGGER_RESUME:
			if (!atomic_read(&tascam->capture_active)) {
				atomic_set(&tascam->capture_active, 1);
				start = true;
			}
			break;
		case SNDRV_PCM_TRIGGER_STOP:
		case SNDRV_PCM_TRIGGER_SUSPEND:
		case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
			atomic_set(&tascam->capture_active, 0);
			stop = true;
			break;
		default:
			ret = -EINVAL;
			break;
	}
	spin_unlock_irqrestore(&tascam->lock, flags);

	if (stop) {
		smp_mb();
		for (i = 0; i < NUM_CAPTURE_URBS; i++) {
			if (tascam->capture_urbs[i])
				usb_unlink_urb(tascam->capture_urbs[i]);
		}
	}

	if (start) {
		for (i = 0; i < NUM_CAPTURE_URBS; i++) {
			usb_anchor_urb(tascam->capture_urbs[i], &tascam->capture_anchor);
			if (usb_submit_urb(tascam->capture_urbs[i], GFP_ATOMIC) < 0) {
				usb_unanchor_urb(tascam->capture_urbs[i]);
				atomic_set(&tascam->capture_active, 0);
				smp_mb();
				for (int j = 0; j < i; j++)
					usb_unlink_urb(tascam->capture_urbs[j]);
				ret = -EIO;
				break;
			}
			atomic_inc(&tascam->active_urbs);
		}
	}
	return ret;
}

static inline void tascam_unpack_8bytes(const u8 *src, u8 *out_bit0, u8 *out_bit1)
{
	u64 val = get_unaligned_le64(src);
	u8 b0 = 0, b1 = 0;
	int i;

	for (i = 0; i < 8; i++) {
		b0 |= ((val >> (i * 8)) & 1) << (7 - i);
		b1 |= ((val >> (i * 8 + 1)) & 1) << (7 - i);
	}

	*out_bit0 = b0;
	*out_bit1 = b1;
}

static void tascam_decode_capture_chunk(const u8 *src, u32 *dst, int frames_to_decode)
{
	int i;
	u8 h[4], m[4], l[4];

	for (i = 0; i < frames_to_decode; i++) {
		const u8 *p_src_a = src + (i * 64);
		const u8 *p_src_b = src + (i * 64) + 32;

		tascam_unpack_8bytes(p_src_a, &h[0], &h[2]);
		tascam_unpack_8bytes(p_src_a + 8, &m[0], &m[2]);
		tascam_unpack_8bytes(p_src_a + 16, &l[0], &l[2]);

		tascam_unpack_8bytes(p_src_b, &h[1], &h[3]);
		tascam_unpack_8bytes(p_src_b + 8, &m[1], &m[3]);
		tascam_unpack_8bytes(p_src_b + 16, &l[1], &l[3]);

		*dst++ = (h[0] << 24) | (m[0] << 16) | (l[0] << 8);
		*dst++ = (h[1] << 24) | (m[1] << 16) | (l[1] << 8);
		*dst++ = (h[2] << 24) | (m[2] << 16) | (l[2] << 8);
		*dst++ = (h[3] << 24) | (m[3] << 16) | (l[3] << 8);
	}
}

static void tascam_decode_capture_chunk_122(const u8 *src, u32 *dst, int frames_to_decode)
{
	int i;

	for (i = 0; i < frames_to_decode; i++) {
		*dst++ = (src[2] << 24) | (src[1] << 16) | (src[0] << 8);
		*dst++ = (src[5] << 24) | (src[4] << 16) | (src[3] << 8);
		src += 6;
	}
}

void capture_urb_complete(struct urb *urb)
{
	struct tascam_card *tascam = urb->context;
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	unsigned long flags;
	int frames_received;
	snd_pcm_uframes_t write_pos;
	snd_pcm_uframes_t buffer_size, period_size;
	bool need_period_elapsed = false;

	if (!tascam)
		return;

	if (urb->status) {
		atomic_dec(&tascam->active_urbs);
		return;
	}

	substream = tascam->capture_substream;
	if (!substream || !substream->runtime) {
		atomic_dec(&tascam->active_urbs);
		return;
	}
	runtime = substream->runtime;
	if (!runtime->dma_area) {
		atomic_dec(&tascam->active_urbs);
		return;
	}

	buffer_size = runtime->buffer_size;
	period_size = runtime->period_size;

	if (urb->actual_length % 64 != 0)
		dev_warn_ratelimited(&tascam->dev->dev, "Unaligned capture packet size: %d\n", urb->actual_length);
	frames_received = urb->actual_length / 64;

	if (frames_received > 0) {
		spin_lock_irqsave(&tascam->lock, flags);

		if (!atomic_read(&tascam->capture_active)) {
			spin_unlock_irqrestore(&tascam->lock, flags);
			atomic_dec(&tascam->active_urbs);
			return;
		}

		write_pos = tascam->driver_capture_pos;

		u32 *dma_ptr = (u32 *)(runtime->dma_area + frames_to_bytes(runtime, write_pos));

		if (write_pos + frames_received <= buffer_size) {
			tascam_decode_capture_chunk(urb->transfer_buffer, dma_ptr, frames_received);
		} else {
			int part1 = buffer_size - write_pos;
			int part2 = frames_received - part1;

			tascam_decode_capture_chunk(urb->transfer_buffer, dma_ptr, part1);
			tascam_decode_capture_chunk(urb->transfer_buffer + (part1 * 64), (u32 *)runtime->dma_area, part2);
		}

		tascam->driver_capture_pos += frames_received;
		if (tascam->driver_capture_pos >= buffer_size)
			tascam->driver_capture_pos -= buffer_size;

		tascam->capture_frames_processed += frames_received;

		if (period_size > 0) {
			u64 current_period = div_u64(tascam->capture_frames_processed, period_size);

			if (current_period > tascam->last_cap_period_pos) {
				tascam->last_cap_period_pos = current_period;
				need_period_elapsed = true;
			}
		}
		spin_unlock_irqrestore(&tascam->lock, flags);

		if (need_period_elapsed)
			snd_pcm_period_elapsed(substream);
	}

	usb_anchor_urb(urb, &tascam->capture_anchor);
	if (usb_submit_urb(urb, GFP_ATOMIC) < 0) {
		usb_unanchor_urb(urb);
		atomic_dec(&tascam->active_urbs);
	}
}

void capture_urb_complete_122(struct urb *urb)
{
	struct tascam_card *tascam = urb->context;
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	unsigned long flags;
	int i, frames_in_urb = 0;
	snd_pcm_uframes_t write_pos;
	snd_pcm_uframes_t buffer_size, period_size;
	bool need_period_elapsed = false;

	if (!tascam)
		return;

	if (urb->status) {
		atomic_dec(&tascam->active_urbs);
		return;
	}

	substream = tascam->capture_substream;
	if (!substream || !substream->runtime) {
		atomic_dec(&tascam->active_urbs);
		return;
	}
	runtime = substream->runtime;
	buffer_size = runtime->buffer_size;
	period_size = runtime->period_size;

	spin_lock_irqsave(&tascam->lock, flags);

	if (!atomic_read(&tascam->capture_active)) {
		spin_unlock_irqrestore(&tascam->lock, flags);
		atomic_dec(&tascam->active_urbs);
		return;
	}

	write_pos = tascam->driver_capture_pos;

	for (i = 0; i < urb->number_of_packets; i++) {
		int len = urb->iso_frame_desc[i].actual_length;
		int frames = len / 6;
		u8 *src = urb->transfer_buffer + urb->iso_frame_desc[i].offset;

		if (frames > 0) {
			u32 *dst = (u32 *)(runtime->dma_area + frames_to_bytes(runtime, write_pos));

			if (write_pos + frames <= buffer_size) {
				tascam_decode_capture_chunk_122(src, dst, frames);
			} else {
				int part1 = buffer_size - write_pos;
				int part2 = frames - part1;
				tascam_decode_capture_chunk_122(src, dst, part1);
				tascam_decode_capture_chunk_122(src + (part1 * 6),
												(u32 *)runtime->dma_area, part2);
			}

			write_pos = (write_pos + frames) % buffer_size;
			frames_in_urb += frames;
		}

		urb->iso_frame_desc[i].length = US122_URB_ALLOC_SIZE;
	}

	tascam->driver_capture_pos = write_pos;
	tascam->capture_frames_processed += frames_in_urb;

	atomic_add(frames_in_urb, &tascam->implicit_fb_frames);

	if (period_size > 0) {
		u64 current_period = div_u64(tascam->capture_frames_processed, period_size);
		if (current_period > tascam->last_cap_period_pos) {
			tascam->last_cap_period_pos = current_period;
			need_period_elapsed = true;
		}
	}
	spin_unlock_irqrestore(&tascam->lock, flags);

	if (need_period_elapsed)
		snd_pcm_period_elapsed(substream);

	usb_anchor_urb(urb, &tascam->capture_anchor);
	if (usb_submit_urb(urb, GFP_ATOMIC) < 0) {
		usb_unanchor_urb(urb);
		atomic_dec(&tascam->active_urbs);
	}
}

const struct snd_pcm_ops tascam_capture_ops = {
	.open = tascam_capture_open,
	.close = tascam_capture_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = tascam_pcm_hw_params,
	.hw_free = NULL,
	.prepare = tascam_capture_prepare,
	.trigger = tascam_capture_trigger,
	.pointer = tascam_capture_pointer,
};
