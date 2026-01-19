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
	.channels_min = 4,
	.channels_max = 4,
	.buffer_bytes_max = 1024 * 1024,
	.period_bytes_min = 768,
	.period_bytes_max = 1024 * 1024,
	.periods_min = 2,
	.periods_max = 1024,
};

static int tascam_capture_open(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	substream->runtime->hw = tascam_capture_hw;
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
	return 0;
}

static int tascam_capture_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	int i, ret = 0;
	bool start = false, stop = false;
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
	}
	spin_unlock_irqrestore(&tascam->lock, flags);

	if (start) {
		us144mkii_maybe_start_stream(tascam);
		spin_lock_irqsave(&tascam->lock, flags);
		for (i = 0; i < NUM_CAPTURE_URBS; i++) {
			usb_anchor_urb(tascam->capture_urbs[i], &tascam->capture_anchor);
			if (usb_submit_urb(tascam->capture_urbs[i], GFP_ATOMIC) < 0) {
				usb_unanchor_urb(tascam->capture_urbs[i]);
				ret = -EIO;
				break;
			}
			atomic_inc(&tascam->active_urbs);
		}
		spin_unlock_irqrestore(&tascam->lock, flags);

		if (ret < 0)
			us144mkii_maybe_stop_stream(tascam);
	}

	if (stop) {
		spin_lock_irqsave(&tascam->lock, flags);
		for (i = 0; i < NUM_CAPTURE_URBS; i++) {
			if (tascam->capture_urbs[i])
				usb_unlink_urb(tascam->capture_urbs[i]);
		}
		spin_unlock_irqrestore(&tascam->lock, flags);

		us144mkii_maybe_stop_stream(tascam);
	}

	return ret;
}

static void tascam_decode_capture_chunk(const u8 *src, u32 *dst, int frames)
{
	int i;
	u8 h[4], m[4], l[4];
	u64 x, t;

	/*
	 * Unpack using bit-reversal and butterfly transposition
	 * to handle the device's specific bit layout.
	 */
	for (i = 0; i < frames; i++) {
		const u8 *sa = src + (i * 64), *sb = sa + 32;

		#define UNPACK(s, o0, o1) do { \
		x = __builtin_bswap64(get_unaligned_le64(s)); \
		t = (x ^ (x >> 7)) & 0x00AA00AA00AA00AAULL; x = x ^ t ^ (t << 7); \
		t = (x ^ (x >> 14)) & 0x0000CCCC0000CCCCULL; x = x ^ t ^ (t << 14); \
		t = (x ^ (x >> 28)) & 0x00000000F0F0F0F0ULL; x = x ^ t ^ (t << 28); \
		*o0 = (u8)x; *o1 = (u8)(x >> 8); \
		} while (0)

		UNPACK(sa, &h[0], &h[2]); UNPACK(sa + 8, &m[0], &m[2]); UNPACK(sa + 16, &l[0], &l[2]);
		UNPACK(sb, &h[1], &h[3]); UNPACK(sb + 8, &m[1], &m[3]); UNPACK(sb + 16, &l[1], &l[3]);

		put_unaligned_le32((h[0] << 24) | (m[0] << 16) | (l[0] << 8), dst++);
		put_unaligned_le32((h[1] << 24) | (m[1] << 16) | (l[1] << 8), dst++);
		put_unaligned_le32((h[2] << 24) | (m[2] << 16) | (l[2] << 8), dst++);
		put_unaligned_le32((h[3] << 24) | (m[3] << 16) | (l[3] << 8), dst++);
	}
}

/**
 * capture_urb_complete() - completion handler for capture URBs
 * @urb: the completed URB
 *
 * Decodes audio data, updates ring buffer, and handles period elapsed.
 */
void capture_urb_complete(struct urb *urb)
{
	struct tascam_card *tascam = urb->context;
	struct snd_pcm_runtime *runtime;
	int frames, part1;
	unsigned long flags;
	bool need_period_elapsed = false;

	if (urb->status || !tascam || !tascam->dev)
		goto exit;

	if (!tascam->capture_substream || !tascam->capture_substream->runtime)
		goto exit;

	runtime = tascam->capture_substream->runtime;
	frames = urb->actual_length / 64;

	if (frames > 0) {
		spin_lock_irqsave(&tascam->lock, flags);

		if (!atomic_read(&tascam->capture_active)) {
			spin_unlock_irqrestore(&tascam->lock, flags);
			goto exit;
		}

		u32 *dma = (u32 *)(runtime->dma_area + frames_to_bytes(runtime, tascam->driver_capture_pos));

		if (tascam->driver_capture_pos + frames <= runtime->buffer_size) {
			tascam_decode_capture_chunk(urb->transfer_buffer, dma, frames);
		} else {
			part1 = runtime->buffer_size - tascam->driver_capture_pos;
			tascam_decode_capture_chunk(urb->transfer_buffer, dma, part1);
			tascam_decode_capture_chunk(urb->transfer_buffer + (part1 * 64),
										(u32 *)runtime->dma_area, frames - part1);
		}

		tascam->driver_capture_pos = (tascam->driver_capture_pos + frames) % runtime->buffer_size;
		tascam->capture_frames_processed += frames;

		if (div_u64(tascam->capture_frames_processed, runtime->period_size) > tascam->last_cap_period_pos) {
			tascam->last_cap_period_pos = div_u64(tascam->capture_frames_processed, runtime->period_size);
			need_period_elapsed = true;
		}
		spin_unlock_irqrestore(&tascam->lock, flags);
	}

	usb_anchor_urb(urb, &tascam->capture_anchor);
	if (usb_submit_urb(urb, GFP_ATOMIC) < 0) {
		usb_unanchor_urb(urb);
		atomic_dec(&tascam->active_urbs);
		return;
	}

	if (need_period_elapsed)
		snd_pcm_period_elapsed(tascam->capture_substream);

	return;

	exit:
	usb_unanchor_urb(urb);
	atomic_dec(&tascam->active_urbs);
}

static snd_pcm_uframes_t tascam_capture_pointer(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	unsigned long flags;
	snd_pcm_uframes_t ptr;

	spin_lock_irqsave(&tascam->lock, flags);
	ptr = tascam->driver_capture_pos;
	spin_unlock_irqrestore(&tascam->lock, flags);

	return ptr;
}

const struct snd_pcm_ops tascam_capture_ops = {
	.open = tascam_capture_open,
	.close = tascam_capture_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = tascam_pcm_hw_params,
	.prepare = tascam_capture_prepare,
	.trigger = tascam_capture_trigger,
	.pointer = tascam_capture_pointer,
};
