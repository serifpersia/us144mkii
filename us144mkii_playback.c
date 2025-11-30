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
	.period_bytes_min = 64 * BYTES_PER_FRAME,
	.period_bytes_max = 1024 * BYTES_PER_FRAME,
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

	cancel_work_sync(&tascam->stop_pcm_work);
	tascam->playback_substream = NULL;
	return 0;
}

static int tascam_playback_prepare(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int i, u;
	u32 nominal_q16 = (runtime->rate << 16) / 8000;
	size_t nominal_bytes = (runtime->rate / 8000) * BYTES_PER_FRAME;

	usb_kill_anchored_urbs(&tascam->playback_anchor);
	usb_kill_anchored_urbs(&tascam->feedback_anchor);

	tascam->driver_playback_pos = 0;
	tascam->playback_frames_consumed = 0;
	tascam->last_pb_period_pos = 0;
	tascam->feedback_synced = false;
	tascam->feedback_urb_skip_count = NUM_FEEDBACK_URBS;
	tascam->phase_accum = 0;
	tascam->freq_q16 = nominal_q16;

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
		memset(urb->transfer_buffer, 0, tascam->playback_urb_alloc_size);
		urb->number_of_packets = PLAYBACK_URB_PACKETS;
		urb->transfer_buffer_length = PLAYBACK_URB_PACKETS * nominal_bytes;
		for (i = 0; i < PLAYBACK_URB_PACKETS; i++) {
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

	if (!atomic_read(&tascam->playback_active)) return 0;
	spin_lock_irqsave(&tascam->lock, flags);
	pos = tascam->playback_frames_consumed;
	spin_unlock_irqrestore(&tascam->lock, flags);
	return do_div(pos, substream->runtime->buffer_size);
}

static int tascam_playback_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	int i, ret = 0;
	bool start = false;
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

			for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
				if (tascam->feedback_urbs[i])
					usb_unlink_urb(tascam->feedback_urbs[i]);
			}
			for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
				if (tascam->playback_urbs[i])
					usb_unlink_urb(tascam->playback_urbs[i]);
			}
			break;
		default:
			ret = -EINVAL;
			break;
	}
	spin_unlock_irqrestore(&tascam->lock, flags);

	if (start) {
		for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
			usb_anchor_urb(tascam->feedback_urbs[i], &tascam->feedback_anchor);
			usb_submit_urb(tascam->feedback_urbs[i], GFP_ATOMIC);
			atomic_inc(&tascam->active_urbs);
		}
		for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
			usb_anchor_urb(tascam->playback_urbs[i], &tascam->playback_anchor);
			usb_submit_urb(tascam->playback_urbs[i], GFP_ATOMIC);
			atomic_inc(&tascam->active_urbs);
		}
	} else if (atomic_read(&tascam->playback_active) == 0) {
		schedule_work(&tascam->stop_work);
	}
	return ret;
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

	if (urb->status) {
		atomic_dec(&tascam->active_urbs);
		return;
	}
	if (!tascam || !atomic_read(&tascam->playback_active)) {
		atomic_dec(&tascam->active_urbs);
		return;
	}

	substream = tascam->playback_substream;
	if (!substream || !substream->runtime) {
		atomic_dec(&tascam->active_urbs);
		return;
	}
	runtime = substream->runtime;

	spin_lock_irqsave(&tascam->lock, flags);
	for (i = 0; i < urb->number_of_packets; i++) {
		unsigned int frames_for_packet;
		tascam->phase_accum += tascam->freq_q16;
		frames_for_packet = tascam->phase_accum >> 16;
		tascam->phase_accum &= 0xFFFF;
		urb->iso_frame_desc[i].offset = total_bytes_for_urb;
		urb->iso_frame_desc[i].length = frames_for_packet * BYTES_PER_FRAME;
		total_bytes_for_urb += urb->iso_frame_desc[i].length;
	}
	urb->transfer_buffer_length = total_bytes_for_urb;

	offset_frames = tascam->driver_playback_pos;
	frames_to_copy = bytes_to_frames(runtime, total_bytes_for_urb);
	tascam->driver_playback_pos = (offset_frames + frames_to_copy) % runtime->buffer_size;
	spin_unlock_irqrestore(&tascam->lock, flags);

	if (total_bytes_for_urb > 0) {
		u8 *dst_buf = urb->transfer_buffer;
		size_t ptr_bytes = frames_to_bytes(runtime, offset_frames);
		if (offset_frames + frames_to_copy > runtime->buffer_size) {
			size_t part1 = runtime->buffer_size - offset_frames;
			memcpy(dst_buf, runtime->dma_area + ptr_bytes, frames_to_bytes(runtime, part1));
			memcpy(dst_buf + frames_to_bytes(runtime, part1), runtime->dma_area, total_bytes_for_urb - frames_to_bytes(runtime, part1));
		} else {
			memcpy(dst_buf, runtime->dma_area + ptr_bytes, total_bytes_for_urb);
		}
	}

	spin_lock_irqsave(&tascam->lock, flags);
	tascam->playback_frames_consumed += frames_to_copy;

	if (runtime->period_size > 0) {
		u64 current_period = div_u64(tascam->playback_frames_consumed, runtime->period_size);
		if (current_period > tascam->last_pb_period_pos) {
			tascam->last_pb_period_pos = current_period;
			spin_unlock_irqrestore(&tascam->lock, flags);
			snd_pcm_period_elapsed(substream);
			spin_lock_irqsave(&tascam->lock, flags);
		}
	}
	spin_unlock_irqrestore(&tascam->lock, flags);

	if (usb_submit_urb(urb, GFP_ATOMIC) < 0)
		atomic_dec(&tascam->active_urbs);
}

void feedback_urb_complete(struct urb *urb)
{
	struct tascam_card *tascam = urb->context;
	int ret, p;
	unsigned long flags;

	if (urb->status) {
		atomic_dec(&tascam->active_urbs);
		return;
	}
	if (!tascam || !atomic_read(&tascam->playback_active)) {
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
