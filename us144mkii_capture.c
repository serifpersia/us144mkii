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

	if (stop) {
		if (tascam->running_ghost_playback) {
			tascam->running_ghost_playback = false;

			for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
				if (tascam->playback_urbs[i])
					usb_unlink_urb(tascam->playback_urbs[i]);
			}
			for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
				if (tascam->feedback_urbs[i])
					usb_unlink_urb(tascam->feedback_urbs[i]);
			}
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

		if (ret == 0 && !atomic_read(&tascam->playback_active)) {
			int u;
			tascam->running_ghost_playback = true;
			tascam->phase_accum = 0;
			tascam->freq_q16 = div_u64(((u64)tascam->current_rate << 16), 8000);
			tascam->feedback_urb_skip_count = 4;
			tascam->feedback_synced = false;

			for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
				struct urb *f_urb = tascam->feedback_urbs[i];
				f_urb->number_of_packets = FEEDBACK_URB_PACKETS;
				f_urb->transfer_buffer_length = FEEDBACK_URB_PACKETS * FEEDBACK_PACKET_SIZE;
				for (u = 0; u < FEEDBACK_URB_PACKETS; u++) {
					f_urb->iso_frame_desc[u].offset = u * FEEDBACK_PACKET_SIZE;
					f_urb->iso_frame_desc[u].length = FEEDBACK_PACKET_SIZE;
				}
				usb_anchor_urb(f_urb, &tascam->feedback_anchor);
				if (usb_submit_urb(f_urb, GFP_ATOMIC) < 0) {
					usb_unanchor_urb(f_urb);
				} else {
					atomic_inc(&tascam->active_urbs);
				}
			}

			size_t nominal_bytes = (tascam->current_rate / 8000) * PLAYBACK_FRAME_SIZE;
			for (u = 0; u < NUM_PLAYBACK_URBS; u++) {
				struct urb *urb = tascam->playback_urbs[u];
				size_t total_bytes = 0;
				urb->number_of_packets = PLAYBACK_URB_PACKETS;
				for (i = 0; i < PLAYBACK_URB_PACKETS; i++) {
					urb->iso_frame_desc[i].offset = i * nominal_bytes;
					urb->iso_frame_desc[i].length = nominal_bytes;
					total_bytes += nominal_bytes;
				}
				urb->transfer_buffer_length = total_bytes;
				memset(urb->transfer_buffer, 0, total_bytes);

				usb_anchor_urb(urb, &tascam->playback_anchor);
				if (usb_submit_urb(urb, GFP_ATOMIC) < 0) {
					usb_unanchor_urb(urb);
				} else {
					atomic_inc(&tascam->active_urbs);
				}
			}
		}
	}
	spin_unlock_irqrestore(&tascam->lock, flags);
	return ret;
}

static inline void tascam_unpack_8bytes(const u8 *src, u8 *out_bit0, u8 *out_bit1)
{
	/* The hardware sends bits in a layout that requires both transposition
	 * and bit-reversal within the result. swab64() + Butterfly Transpose
	 * achieves exactly the same mapping as the original bit-by-bit loop.
	 */
	u64 x = get_unaligned_le64(src);
	u64 t;

	/* Stage 0: Reverse byte order to handle the hardware's MSB-first nature */
	x = __builtin_bswap64(x);

	/* 8x8 Bit Transposition (Butterfly) */
	t = (x ^ (x >> 7)) & 0x00AA00AA00AA00AAULL;
	x = x ^ t ^ (t << 7);

	t = (x ^ (x >> 14)) & 0x0000CCCC0000CCCCULL;
	x = x ^ t ^ (t << 14);

	t = (x ^ (x >> 28)) & 0x00000000F0F0F0F0ULL;
	x = x ^ t ^ (t << 28);

	/* Extract the untangled bits for the first two sample planes */
	*out_bit0 = (u8)(x >> 0);
	*out_bit1 = (u8)(x >> 8);
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

		put_unaligned_le32((h[0] << 24) | (m[0] << 16) | (l[0] << 8), dst++);
		put_unaligned_le32((h[1] << 24) | (m[1] << 16) | (l[1] << 8), dst++);
		put_unaligned_le32((h[2] << 24) | (m[2] << 16) | (l[2] << 8), dst++);
		put_unaligned_le32((h[3] << 24) | (m[3] << 16) | (l[3] << 8), dst++);
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

	if (!tascam->dev) {
		usb_unanchor_urb(urb);
		atomic_dec(&tascam->active_urbs);
		return;
	}

	if (urb->status) {
		usb_unanchor_urb(urb);
		atomic_dec(&tascam->active_urbs);
		return;
	}

	substream = tascam->capture_substream;
	if (!substream || !substream->runtime) {
		usb_unanchor_urb(urb);
		atomic_dec(&tascam->active_urbs);
		return;
	}
	runtime = substream->runtime;
	if (!runtime->dma_area) {
		usb_unanchor_urb(urb);
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
	}

	usb_anchor_urb(urb, &tascam->capture_anchor);
	if (usb_submit_urb(urb, GFP_ATOMIC) < 0) {
		usb_unanchor_urb(urb);
		atomic_dec(&tascam->active_urbs);
		spin_unlock_irqrestore(&tascam->lock, flags);
		return;
	}

	spin_unlock_irqrestore(&tascam->lock, flags);

	if (need_period_elapsed && substream)
		snd_pcm_period_elapsed(substream);
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
