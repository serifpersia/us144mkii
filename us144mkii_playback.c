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
	.channels_min = 4,
	.channels_max = 4,
	.buffer_bytes_max = 1024 * 1024,
	.period_bytes_min = 576,
	.period_bytes_max = 1024 * 1024,
	.periods_min = 2,
	.periods_max = 1024,
};

static int submit_urbs(struct tascam_card *tascam, struct urb **urbs, int count, struct usb_anchor *anchor)
{
	int i;
	for (i = 0; i < count; i++) {
		usb_anchor_urb(urbs[i], anchor);
		if (usb_submit_urb(urbs[i], GFP_ATOMIC) < 0) {
			usb_unanchor_urb(urbs[i]);
			return -EIO;
		}
		atomic_inc(&tascam->active_urbs);
	}
	return 0;
}

static void prepare_urb_descriptors(struct tascam_card *tascam)
{
	int i, u;
	size_t nominal_bytes = (tascam->current_rate / 8000) * PLAYBACK_FRAME_SIZE;

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
		urb->number_of_packets = PLAYBACK_URB_PACKETS;
		for (i = 0; i < PLAYBACK_URB_PACKETS; i++) {
			urb->iso_frame_desc[i].offset = i * nominal_bytes;
			urb->iso_frame_desc[i].length = nominal_bytes;
		}
		urb->transfer_buffer_length = PLAYBACK_URB_PACKETS * nominal_bytes;
		memset(urb->transfer_buffer, 0, urb->transfer_buffer_length);
	}
}

/**
 * us144mkii_maybe_start_stream() - Start implicit playback for capture/MIDI
 * @tascam: the tascam_card instance
 *
 * Increments reference count and starts the ghost playback stream if no
 * real playback is active.
 */
void us144mkii_maybe_start_stream(struct tascam_card *tascam)
{
	unsigned long flags;

	atomic_inc(&tascam->stream_refs);

	spin_lock_irqsave(&tascam->lock, flags);
	if (!atomic_read(&tascam->playback_active) && !tascam->running_ghost_playback) {
		tascam->running_ghost_playback = true;
		tascam->phase_accum = 0;
		tascam->freq_q16 = div_u64(((u64)tascam->current_rate << 16), 8000);
		tascam->feedback_urb_skip_count = 4;
		tascam->feedback_synced = false;

		prepare_urb_descriptors(tascam);

		submit_urbs(tascam, tascam->feedback_urbs, NUM_FEEDBACK_URBS, &tascam->feedback_anchor);
		submit_urbs(tascam, tascam->playback_urbs, NUM_PLAYBACK_URBS, &tascam->playback_anchor);
	}
	spin_unlock_irqrestore(&tascam->lock, flags);
}

/**
 * us144mkii_maybe_stop_stream() - Stop implicit playback
 * @tascam: the tascam_card instance
 *
 * Decrements reference count and stops the ghost playback stream if it
 * reaches zero and no real playback is active.
 */
void us144mkii_maybe_stop_stream(struct tascam_card *tascam)
{
	unsigned long flags;
	int i;

	if (atomic_dec_return(&tascam->stream_refs) > 0)
		return;

	spin_lock_irqsave(&tascam->lock, flags);
	if (!atomic_read(&tascam->playback_active) && tascam->running_ghost_playback) {
		tascam->running_ghost_playback = false;
		for (i = 0; i < NUM_PLAYBACK_URBS; i++)
			usb_unlink_urb(tascam->playback_urbs[i]);
		for (i = 0; i < NUM_FEEDBACK_URBS; i++)
			usb_unlink_urb(tascam->feedback_urbs[i]);
	}
	spin_unlock_irqrestore(&tascam->lock, flags);
}

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

	usb_kill_anchored_urbs(&tascam->playback_anchor);
	usb_kill_anchored_urbs(&tascam->feedback_anchor);

	tascam->driver_playback_pos = 0;
	tascam->playback_frames_consumed = 0;
	tascam->last_pb_period_pos = 0;
	tascam->feedback_synced = false;
	tascam->running_ghost_playback = false;
	tascam->feedback_urb_skip_count = 4;
	tascam->phase_accum = 0;
	tascam->freq_q16 = div_u64(((u64)tascam->current_rate << 16), 8000);

	prepare_urb_descriptors(tascam);
	return 0;
}

static int tascam_playback_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&tascam->lock, flags);
	switch (cmd) {
		case SNDRV_PCM_TRIGGER_START:
		case SNDRV_PCM_TRIGGER_RESUME:
			if (!atomic_read(&tascam->playback_active)) {
				atomic_set(&tascam->playback_active, 1);
				/* If ghost playback is running, just takeover flag */
				if (tascam->running_ghost_playback) {
					tascam->running_ghost_playback = false;
				} else {
					submit_urbs(tascam, tascam->feedback_urbs, NUM_FEEDBACK_URBS, &tascam->feedback_anchor);
					submit_urbs(tascam, tascam->playback_urbs, NUM_PLAYBACK_URBS, &tascam->playback_anchor);
				}
			}
			break;
		case SNDRV_PCM_TRIGGER_STOP:
		case SNDRV_PCM_TRIGGER_SUSPEND:
		case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
			atomic_set(&tascam->playback_active, 0);
			/* Fall back to ghost playback if capture/midi active */
			if (atomic_read(&tascam->stream_refs) > 0)
				tascam->running_ghost_playback = true;
		break;
		default:
			ret = -EINVAL;
			break;
	}
	spin_unlock_irqrestore(&tascam->lock, flags);
	return ret;
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

/**
 * playback_urb_complete() - completion handler for playback isochronous URBs
 * @urb: the completed URB
 *
 * This function runs in interrupt context. It calculates the number of bytes
 * to send in the next set of packets based on the feedback-driven clock,
 * copies the audio data from the ALSA ring buffer (or zero for ghost stream),
 * and resubmits the URB.
 */
void playback_urb_complete(struct urb *urb)
{
	struct tascam_card *tascam = urb->context;
	struct snd_pcm_runtime *runtime;
	size_t total_bytes = 0;
	size_t ptr_bytes, part1_bytes;
	unsigned int frames, i;
	unsigned long flags;
	bool need_period_elapsed = false;

	if (urb->status || !tascam ||
	    (!atomic_read(&tascam->playback_active) &&
	     !tascam->running_ghost_playback)) {
		if (tascam) {
			usb_unanchor_urb(urb);
			atomic_dec(&tascam->active_urbs);
		}
		return;
	}

	spin_lock_irqsave(&tascam->lock, flags);

	for (i = 0; i < urb->number_of_packets; i++) {
		tascam->phase_accum += tascam->freq_q16;
		frames = min((tascam->phase_accum >> 16), (u32)MAX_FRAMES_PER_PACKET);
		tascam->phase_accum &= 0xFFFF;
		urb->iso_frame_desc[i].offset = total_bytes;
		urb->iso_frame_desc[i].length = frames * PLAYBACK_FRAME_SIZE;
		total_bytes += frames * PLAYBACK_FRAME_SIZE;
	}
	urb->transfer_buffer_length = total_bytes;

	/* Ghost Playback: Send Silence */
	if (!atomic_read(&tascam->playback_active)) {
		if (tascam->running_ghost_playback) {
			memset(urb->transfer_buffer, 0, total_bytes);
			spin_unlock_irqrestore(&tascam->lock, flags);
			goto resubmit;
		}
		spin_unlock_irqrestore(&tascam->lock, flags);
		usb_unanchor_urb(urb);
		atomic_dec(&tascam->active_urbs);
		return;
	}

	if (!tascam->playback_substream) {
		memset(urb->transfer_buffer, 0, total_bytes);
		spin_unlock_irqrestore(&tascam->lock, flags);
		goto resubmit;
	}

	runtime = tascam->playback_substream->runtime;
	ptr_bytes = frames_to_bytes(runtime, tascam->driver_playback_pos);
	spin_unlock_irqrestore(&tascam->lock, flags);

	/* Copy from ALSA Buffer */
	if (total_bytes + ptr_bytes > frames_to_bytes(runtime, runtime->buffer_size)) {
		part1_bytes = frames_to_bytes(runtime, runtime->buffer_size) - ptr_bytes;
		memcpy(urb->transfer_buffer, runtime->dma_area + ptr_bytes, part1_bytes);
		memcpy(urb->transfer_buffer + part1_bytes, runtime->dma_area, total_bytes - part1_bytes);
	} else {
		memcpy(urb->transfer_buffer, runtime->dma_area + ptr_bytes, total_bytes);
	}

	spin_lock_irqsave(&tascam->lock, flags);
	tascam->driver_playback_pos += bytes_to_frames(runtime, total_bytes);
	if (tascam->driver_playback_pos >= runtime->buffer_size)
		tascam->driver_playback_pos -= runtime->buffer_size;

	tascam->playback_frames_consumed += bytes_to_frames(runtime, total_bytes);
	if (div_u64(tascam->playback_frames_consumed, runtime->period_size) > tascam->last_pb_period_pos) {
		tascam->last_pb_period_pos = div_u64(tascam->playback_frames_consumed, runtime->period_size);
		need_period_elapsed = true;
	}
	spin_unlock_irqrestore(&tascam->lock, flags);

resubmit:
	usb_anchor_urb(urb, &tascam->playback_anchor);
	if (usb_submit_urb(urb, GFP_ATOMIC) < 0)
		goto exit_unanchor;

	if (need_period_elapsed && tascam->playback_substream)
		snd_pcm_period_elapsed(tascam->playback_substream);
	return;

exit_unanchor:
	usb_unanchor_urb(urb);
	atomic_dec(&tascam->active_urbs);
}

/**
 * feedback_urb_complete() - completion handler for feedback isochronous URBs
 * @urb: the completed URB
 *
 * Updates the PLL based on the number of samples consumed by the device.
 */
void feedback_urb_complete(struct urb *urb)
{
	struct tascam_card *tascam = urb->context;
	unsigned long flags;
	int p;

	if (urb->status || !tascam || (!atomic_read(&tascam->playback_active) && !tascam->running_ghost_playback)) {
		usb_unanchor_urb(urb);
		atomic_dec(&tascam->active_urbs);
		return;
	}

	spin_lock_irqsave(&tascam->lock, flags);

	if (tascam->feedback_urb_skip_count > 0) {
		tascam->feedback_urb_skip_count--;
	} else {
		for (p = 0; p < urb->number_of_packets; p++) {
			if (urb->iso_frame_desc[p].actual_length > 0) {
				u8 *d = urb->transfer_buffer + urb->iso_frame_desc[p].offset;
				u32 val = (urb->iso_frame_desc[p].actual_length >= 3) ? (d[0] + d[1] + d[2]) : (d[0] * 3);
				u32 target = (val << 16) / 24;
				tascam->freq_q16 = (tascam->freq_q16 * PLL_FILTER_OLD_WEIGHT +
				target * PLL_FILTER_NEW_WEIGHT) / PLL_FILTER_DIVISOR;
				tascam->feedback_synced = true;
			}
		}
	}
	spin_unlock_irqrestore(&tascam->lock, flags);

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
	.prepare = tascam_playback_prepare,
	.trigger = tascam_playback_trigger,
	.pointer = tascam_playback_pointer,
};
