// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2025 Šerif Rami <ramiserifpersia@gmail.com>

#include "us144mkii_pcm.h"

const struct snd_pcm_hardware tascam_playback_hw = {
	.info = (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
	SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_MMAP_VALID |
	SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME),
	.formats = SNDRV_PCM_FMTBIT_S24_3LE,
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

static int tascam_playback_open(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);

	substream->runtime->hw = tascam_playback_hw;

	if (tascam->dev_id == USB_PID_TASCAM_US122MKII) {
		substream->runtime->hw.channels_min = 2;
		substream->runtime->hw.channels_max = 2;
	}

	tascam->playback_substream = substream;
	atomic_set(&tascam->playback_active, 0);
	return 0;
}

static int tascam_playback_close(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);

	atomic_set(&tascam->playback_active, 0);
	cancel_work_sync(&tascam->stop_pcm_work);
	usb_kill_anchored_urbs(&tascam->playback_anchor);
	usb_kill_anchored_urbs(&tascam->feedback_anchor);
	tascam->playback_substream = NULL;
	return 0;
}

static int tascam_playback_prepare(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int i, u;
	size_t nominal_bytes = (runtime->rate / 8000) * BYTES_PER_FRAME;

	usb_kill_anchored_urbs(&tascam->playback_anchor);
	usb_kill_anchored_urbs(&tascam->feedback_anchor);

	tascam->driver_playback_pos = 0;
	tascam->playback_frames_consumed = 0;
	tascam->last_pb_period_pos = 0;
	tascam->feedback_synced = false;
	tascam->feedback_urb_skip_count = NUM_FEEDBACK_URBS;
	tascam->phase_accum = 0;
	tascam->freq_q16 = (runtime->rate << 16) / 8000;

	atomic_set(&tascam->implicit_fb_frames, 0);

	if (tascam->dev_id != USB_PID_TASCAM_US122MKII) {
		for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
			struct urb *f_urb = tascam->feedback_urbs[i];

			f_urb->number_of_packets = FEEDBACK_URB_PACKETS;
			f_urb->transfer_buffer_length = FEEDBACK_URB_PACKETS * FEEDBACK_PACKET_SIZE;
			for (u = 0; u < FEEDBACK_URB_PACKETS; u++) {
				f_urb->iso_frame_desc[u].offset = u * FEEDBACK_PACKET_SIZE;
				f_urb->iso_frame_desc[u].length = FEEDBACK_PACKET_SIZE;
			}
		}
	}

	for (u = 0; u < NUM_PLAYBACK_URBS; u++) {
		struct urb *urb = tascam->playback_urbs[u];
		int num_packets = PLAYBACK_URB_PACKETS;

		if (tascam->dev_id == USB_PID_TASCAM_US122MKII) {
			num_packets = US122_ISO_PACKETS;
			nominal_bytes = (runtime->rate / 8000) * US122_BYTES_PER_FRAME;
		}

		urb->number_of_packets = num_packets;
		urb->transfer_buffer_length = num_packets * nominal_bytes;
		for (i = 0; i < num_packets; i++) {
			urb->iso_frame_desc[i].offset = i * nominal_bytes;
			urb->iso_frame_desc[i].length = nominal_bytes;
		}
	}
	return 0;
}

static snd_pcm_uframes_t tascam_playback_pointer(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	unsigned long flags;
	u64 pos;
	snd_pcm_uframes_t buffer_size = substream->runtime->buffer_size;

	if (!atomic_read(&tascam->playback_active))
		return 0;
	spin_lock_irqsave(&tascam->lock, flags);
	pos = tascam->playback_frames_consumed;
	spin_unlock_irqrestore(&tascam->lock, flags);

	return (snd_pcm_uframes_t)(pos % buffer_size);
}

static int tascam_playback_trigger(struct snd_pcm_substream *substream, int cmd)
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
			if (!atomic_read(&tascam->playback_active)) {
				atomic_set(&tascam->playback_active, 1);
				start = true;
			}
			break;
		case SNDRV_PCM_TRIGGER_STOP:
		case SNDRV_PCM_TRIGGER_SUSPEND:
		case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
			atomic_set(&tascam->playback_active, 0);
			stop = true;
			break;
		default:
			ret = -EINVAL;
			break;
	}
	spin_unlock_irqrestore(&tascam->lock, flags);

	if (stop) {
		smp_mb();
		if (tascam->dev_id != USB_PID_TASCAM_US122MKII) {
			for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
				if (tascam->feedback_urbs[i])
					usb_unlink_urb(tascam->feedback_urbs[i]);
			}
		}
		for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
			if (tascam->playback_urbs[i])
				usb_unlink_urb(tascam->playback_urbs[i]);
		}
	}

	if (start) {
		if (tascam->dev_id != USB_PID_TASCAM_US122MKII) {
			for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
				usb_anchor_urb(tascam->feedback_urbs[i], &tascam->feedback_anchor);
				if (usb_submit_urb(tascam->feedback_urbs[i], GFP_ATOMIC) < 0) {
					usb_unanchor_urb(tascam->feedback_urbs[i]);
					atomic_set(&tascam->playback_active, 0);
					smp_mb();
					for (int j = 0; j < i; j++)
						usb_unlink_urb(tascam->feedback_urbs[j]);
					ret = -EIO;
					goto error;
				}
				atomic_inc(&tascam->active_urbs);
			}
		}
		for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
			usb_anchor_urb(tascam->playback_urbs[i], &tascam->playback_anchor);
			if (usb_submit_urb(tascam->playback_urbs[i], GFP_ATOMIC) < 0) {
				usb_unanchor_urb(tascam->playback_urbs[i]);
				atomic_set(&tascam->playback_active, 0);
				smp_mb();
				if (tascam->dev_id != USB_PID_TASCAM_US122MKII) {
					for (int j = 0; j < NUM_FEEDBACK_URBS; j++)
						usb_unlink_urb(tascam->feedback_urbs[j]);
				}
				for (int j = 0; j < i; j++)
					usb_unlink_urb(tascam->playback_urbs[j]);
				ret = -EIO;
				goto error;
			}
			atomic_inc(&tascam->active_urbs);
		}
	} else if (stop && ret == 0) {
		schedule_work(&tascam->stop_work);
	}

	error:
	return ret;
}

static void tascam_encode_playback_122(const u8 *src, u8 *dst, int frames)
{
	memcpy(dst, src, frames * US122_BYTES_PER_FRAME);
}

void playback_urb_complete(struct urb *urb)
{
	struct tascam_card *tascam = urb->context;
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	size_t total_bytes_for_urb = 0;
	snd_pcm_uframes_t offset_frames;
	snd_pcm_uframes_t frames_to_copy;
	int i;
	unsigned long flags;
	bool need_period_elapsed = false;
	snd_pcm_uframes_t buffer_size, period_size;

	if (!tascam)
		return;

	if (urb->status) {
		atomic_dec(&tascam->active_urbs);
		return;
	}

	substream = tascam->playback_substream;
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

	spin_lock_irqsave(&tascam->lock, flags);

	if (!atomic_read(&tascam->playback_active)) {
		spin_unlock_irqrestore(&tascam->lock, flags);
		atomic_dec(&tascam->active_urbs);
		return;
	}

	if (tascam->dev_id == USB_PID_TASCAM_US122MKII) {
		int frames_accumulated = atomic_xchg(&tascam->implicit_fb_frames, 0);
		int frames_per_packet;
		int remainder;

		if (frames_accumulated == 0)
			frames_accumulated = (runtime->rate * urb->number_of_packets) / 8000;

		frames_per_packet = frames_accumulated / urb->number_of_packets;
		remainder = frames_accumulated % urb->number_of_packets;

		for (i = 0; i < urb->number_of_packets; i++) {
			int f = frames_per_packet + (i < remainder ? 1 : 0);
			urb->iso_frame_desc[i].offset = total_bytes_for_urb;
			urb->iso_frame_desc[i].length = f * US122_BYTES_PER_FRAME;
			total_bytes_for_urb += urb->iso_frame_desc[i].length;
		}
	} else {
		for (i = 0; i < urb->number_of_packets; i++) {
			unsigned int frames_for_packet;

			tascam->phase_accum += tascam->freq_q16;
			frames_for_packet = tascam->phase_accum >> 16;
			tascam->phase_accum &= 0xFFFF;
			urb->iso_frame_desc[i].offset = total_bytes_for_urb;
			urb->iso_frame_desc[i].length = frames_for_packet * BYTES_PER_FRAME;
			total_bytes_for_urb += urb->iso_frame_desc[i].length;
		}
	}
	urb->transfer_buffer_length = total_bytes_for_urb;

	offset_frames = tascam->driver_playback_pos;

	if (tascam->dev_id == USB_PID_TASCAM_US122MKII) {
		frames_to_copy = total_bytes_for_urb / US122_BYTES_PER_FRAME;
	} else {
		frames_to_copy = bytes_to_frames(runtime, total_bytes_for_urb);
	}

	tascam->driver_playback_pos = (offset_frames + frames_to_copy) % buffer_size;

	if (total_bytes_for_urb > 0) {
		u8 *dst_buf = urb->transfer_buffer;
		size_t ptr_bytes = frames_to_bytes(runtime, offset_frames);

		if (offset_frames + frames_to_copy > buffer_size) {
			size_t part1 = buffer_size - offset_frames;
			size_t part1_bytes = frames_to_bytes(runtime, part1);

			if (tascam->dev_id == USB_PID_TASCAM_US122MKII) {
				tascam_encode_playback_122(runtime->dma_area + ptr_bytes, dst_buf, part1);
				tascam_encode_playback_122(runtime->dma_area,
										   dst_buf + (part1 * US122_BYTES_PER_FRAME),
										   frames_to_copy - part1);
			} else {
				memcpy(dst_buf, runtime->dma_area + ptr_bytes, part1_bytes);
				memcpy(dst_buf + part1_bytes, runtime->dma_area, total_bytes_for_urb - part1_bytes);
			}
		} else {
			if (tascam->dev_id == USB_PID_TASCAM_US122MKII) {
				tascam_encode_playback_122(runtime->dma_area + ptr_bytes, dst_buf, frames_to_copy);
			} else {
				memcpy(dst_buf, runtime->dma_area + ptr_bytes, total_bytes_for_urb);
			}
		}
	}

	tascam->playback_frames_consumed += frames_to_copy;

	if (period_size > 0) {
		u64 current_period = div_u64(tascam->playback_frames_consumed, period_size);

		if (current_period > tascam->last_pb_period_pos) {
			tascam->last_pb_period_pos = current_period;
			need_period_elapsed = true;
		}
	}
	spin_unlock_irqrestore(&tascam->lock, flags);

	if (need_period_elapsed)
		snd_pcm_period_elapsed(substream);

	usb_anchor_urb(urb, &tascam->playback_anchor);
	if (usb_submit_urb(urb, GFP_ATOMIC) < 0) {
		usb_unanchor_urb(urb);
		atomic_dec(&tascam->active_urbs);
	}
}

void feedback_urb_complete(struct urb *urb)
{
	struct tascam_card *tascam = urb->context;
	int ret, p;
	unsigned long flags;

	if (!tascam)
		return;

	if (urb->status) {
		atomic_dec(&tascam->active_urbs);
		return;
	}
	if (!atomic_read(&tascam->playback_active)) {
		atomic_dec(&tascam->active_urbs);
		return;
	}

	spin_lock_irqsave(&tascam->lock, flags);
	if (tascam->feedback_urb_skip_count > 0) {
		tascam->feedback_urb_skip_count--;
		spin_unlock_irqrestore(&tascam->lock, flags);
		goto resubmit;
	}

	for (p = 0; p < urb->number_of_packets; p++) {
		if (urb->iso_frame_desc[p].status == 0 && urb->iso_frame_desc[p].actual_length >= 3) {
			u8 *data = (u8 *)urb->transfer_buffer + urb->iso_frame_desc[p].offset;
			u32 sum = data[0] + data[1] + data[2];
			u32 target_freq_q16 = ((sum * 65536) / 3) / 8;

			tascam->freq_q16 = (tascam->freq_q16 * PLL_FILTER_OLD_WEIGHT + target_freq_q16 * PLL_FILTER_NEW_WEIGHT) / PLL_FILTER_DIVISOR;
			tascam->feedback_synced = true;
		}
	}
	spin_unlock_irqrestore(&tascam->lock, flags);

	resubmit:
	usb_anchor_urb(urb, &tascam->feedback_anchor);
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0) {
		usb_unanchor_urb(urb);
		atomic_dec(&tascam->active_urbs);
	}
}

const struct snd_pcm_ops tascam_playback_ops = {
	.open = tascam_playback_open,
	.close = tascam_playback_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = tascam_pcm_hw_params,
	.hw_free = NULL,
	.prepare = tascam_playback_prepare,
	.trigger = tascam_playback_trigger,
	.pointer = tascam_playback_pointer,
};
