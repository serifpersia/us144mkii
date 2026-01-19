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
	.period_bytes_min = 576,
	.period_bytes_max = 1024 * 1024,
	.periods_min = 2,
	.periods_max = 1024,
};

static int tascam_playback_open(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);

	substream->runtime->hw = tascam_playback_hw;

	tascam->playback_substream = substream;
	atomic_set(&tascam->playback_active, 0);
	return 0;
}

static int tascam_playback_close(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);

	atomic_set(&tascam->playback_active, 0);

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
	size_t nominal_bytes = (runtime->rate / 8000) * PLAYBACK_FRAME_SIZE;

	usb_kill_anchored_urbs(&tascam->playback_anchor);
	usb_kill_anchored_urbs(&tascam->feedback_anchor);

	tascam->driver_playback_pos = 0;
	tascam->playback_frames_consumed = 0;
	tascam->last_pb_period_pos = 0;
	tascam->feedback_synced = false;
	tascam->running_ghost_playback = false;

	tascam->feedback_urb_skip_count = 4;

	tascam->phase_accum = 0;
	tascam->freq_q16 = div_u64(((u64)runtime->rate << 16), 8000);

	for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
		struct urb *f_urb = tascam->feedback_urbs[i];

		f_urb->number_of_packets = FEEDBACK_URB_PACKETS;
		f_urb->transfer_buffer_length = FEEDBACK_URB_PACKETS * FEEDBACK_PACKET_SIZE;
		for (u = 0; u < FEEDBACK_URB_PACKETS; u++) {
			f_urb->iso_frame_desc[u].offset = u * FEEDBACK_PACKET_SIZE;
			f_urb->iso_frame_desc[u].length = FEEDBACK_PACKET_SIZE;
		}
	}

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
	}
	return 0;
}

static snd_pcm_uframes_t tascam_playback_pointer(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	unsigned long flags;
	u64 pos;

	if (!atomic_read(&tascam->playback_active))
		return 0;

	spin_lock_irqsave(&tascam->lock, flags);
	pos = tascam->playback_frames_consumed;
	spin_unlock_irqrestore(&tascam->lock, flags);

	return (snd_pcm_uframes_t)(pos % substream->runtime->buffer_size);
}

static int tascam_playback_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	int i, ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&tascam->lock, flags);
	switch (cmd) {
		case SNDRV_PCM_TRIGGER_START:
		case SNDRV_PCM_TRIGGER_RESUME:
			if (!atomic_read(&tascam->playback_active)) {
				atomic_set(&tascam->playback_active, 1);
				tascam->running_ghost_playback = false;
				for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
					usb_anchor_urb(tascam->feedback_urbs[i], &tascam->feedback_anchor);
					if (usb_submit_urb(tascam->feedback_urbs[i], GFP_ATOMIC) < 0) {
						usb_unanchor_urb(tascam->feedback_urbs[i]);
					} else {
						atomic_inc(&tascam->active_urbs);
					}
				}
				for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
					usb_anchor_urb(tascam->playback_urbs[i], &tascam->playback_anchor);
					if (usb_submit_urb(tascam->playback_urbs[i], GFP_ATOMIC) < 0) {
						usb_unanchor_urb(tascam->playback_urbs[i]);
					} else {
						atomic_inc(&tascam->active_urbs);
					}
				}
			}
			break;
		case SNDRV_PCM_TRIGGER_STOP:
		case SNDRV_PCM_TRIGGER_SUSPEND:
		case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
			atomic_set(&tascam->playback_active, 0);
			break;
		default:
			ret = -EINVAL;
			break;
	}
	spin_unlock_irqrestore(&tascam->lock, flags);

	return ret;
}

/**
 * playback_urb_complete() - completion handler for playback isochronous URBs
 * @urb: the completed URB
 *
 * This function runs in interrupt context. It calculates the number of bytes
 * to send in the next set of packets based on the feedback-driven clock,
 * copies the audio data from the ALSA ring buffer, and resubmits the URB.
 */
void playback_urb_complete(struct urb *urb)
{
	struct tascam_card *tascam = urb->context;
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	size_t total_bytes_for_urb = 0;
	snd_pcm_uframes_t frames_to_copy;
	int i;
	unsigned long flags;
	bool need_period_elapsed = false;

	if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
		urb->status == -ESHUTDOWN || !tascam) {
		goto exit_clear;
		}

		if (!atomic_read(&tascam->playback_active) && !tascam->running_ghost_playback)
			goto exit_clear;

	spin_lock_irqsave(&tascam->lock, flags);
	substream = tascam->playback_substream;

	for (i = 0; i < urb->number_of_packets; i++) {
		unsigned int frames_for_packet;

		tascam->phase_accum += tascam->freq_q16;
		frames_for_packet = tascam->phase_accum >> 16;
		tascam->phase_accum &= 0xFFFF;

		if (frames_for_packet > MAX_FRAMES_PER_PACKET)
			frames_for_packet = MAX_FRAMES_PER_PACKET;

		urb->iso_frame_desc[i].offset = total_bytes_for_urb;
		urb->iso_frame_desc[i].length = frames_for_packet * PLAYBACK_FRAME_SIZE;
		total_bytes_for_urb += urb->iso_frame_desc[i].length;
	}
	urb->transfer_buffer_length = total_bytes_for_urb;

	if (total_bytes_for_urb == 0) {
		spin_unlock_irqrestore(&tascam->lock, flags);
		goto resubmit;
	}

	if (!substream) {
		memset(urb->transfer_buffer, 0, total_bytes_for_urb);
		spin_unlock_irqrestore(&tascam->lock, flags);
		goto resubmit;
	}

	runtime = substream->runtime;
	size_t playback_pos = tascam->driver_playback_pos;
	spin_unlock_irqrestore(&tascam->lock, flags);

	/* Copy from ALSA DMA buffer - outside of the main device lock */
	u8 *dst_buf = urb->transfer_buffer;
	size_t ptr_bytes = frames_to_bytes(runtime, playback_pos);
	frames_to_copy = bytes_to_frames(runtime, total_bytes_for_urb);

	if (playback_pos + frames_to_copy > runtime->buffer_size) {
		size_t part1 = runtime->buffer_size - playback_pos;
		size_t part1_bytes = frames_to_bytes(runtime, part1);

		memcpy(dst_buf, runtime->dma_area + ptr_bytes, part1_bytes);
		memcpy(dst_buf + part1_bytes, runtime->dma_area, total_bytes_for_urb - part1_bytes);
	} else {
		memcpy(dst_buf, runtime->dma_area + ptr_bytes, total_bytes_for_urb);
	}

	/* Re-acquire lock to update shared stream state */
	spin_lock_irqsave(&tascam->lock, flags);
	tascam->driver_playback_pos += frames_to_copy;
	if (tascam->driver_playback_pos >= runtime->buffer_size)
		tascam->driver_playback_pos -= runtime->buffer_size;

	tascam->playback_frames_consumed += frames_to_copy;

	if (runtime->period_size > 0 && 
		div_u64(tascam->playback_frames_consumed, runtime->period_size) > tascam->last_pb_period_pos) {
		tascam->last_pb_period_pos = div_u64(tascam->playback_frames_consumed, runtime->period_size);
		need_period_elapsed = true;
	}
	spin_unlock_irqrestore(&tascam->lock, flags);

resubmit:

	usb_anchor_urb(urb, &tascam->playback_anchor);
	if (usb_submit_urb(urb, GFP_ATOMIC) < 0) {
		goto exit_clear_locked;
	}

	if (need_period_elapsed && substream)
		snd_pcm_period_elapsed(substream);

	return;

exit_clear_locked:
	spin_lock_irqsave(&tascam->lock, flags);
exit_clear:
	usb_unanchor_urb(urb);
	spin_unlock_irqrestore(&tascam->lock, flags);
	atomic_dec(&tascam->active_urbs);
}

/**
 * feedback_urb_complete() - completion handler for feedback isochronous URBs
 * @urb: the completed URB
 *
 * This is the master clock for the driver. It runs in interrupt context.
 * It reads the feedback value from the device, which indicates how many
 * samples the device has consumed. This information is used to adjust the
 * playback rate and to advance the capture stream pointer, keeping both
 * streams in sync. It then calls snd_pcm_period_elapsed if necessary and
 * resubmits itself.
 */
void feedback_urb_complete(struct urb *urb)
{
	struct tascam_card *tascam = urb->context;
	unsigned long flags;
	int p;

	if (urb->status || !tascam || 
		(!atomic_read(&tascam->playback_active) && !tascam->running_ghost_playback)) {
		usb_unanchor_urb(urb);
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
		if (urb->iso_frame_desc[p].status == 0 && urb->iso_frame_desc[p].actual_length >= 1) {
			u8 *data = (u8 *)urb->transfer_buffer + urb->iso_frame_desc[p].offset;
			u32 sum_frames_3ms;
			u32 target_freq_q16;

			if (urb->iso_frame_desc[p].actual_length >= 3) {
				sum_frames_3ms = data[0] + data[1] + data[2];
			} else {
				sum_frames_3ms = data[0] * 3;
			}

			target_freq_q16 = (sum_frames_3ms << 16) / 24;

			tascam->freq_q16 = (tascam->freq_q16 * PLL_FILTER_OLD_WEIGHT +
			target_freq_q16 * PLL_FILTER_NEW_WEIGHT) / PLL_FILTER_DIVISOR;

			tascam->feedback_synced = true;
		}
	}
	spin_unlock_irqrestore(&tascam->lock, flags);

	resubmit:
	usb_anchor_urb(urb, &tascam->feedback_anchor);
	if (usb_submit_urb(urb, GFP_ATOMIC) < 0) {
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
