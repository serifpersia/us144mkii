// SPDX-License-Identifier: GPL-2.0-only

#include "us144mkii.h"
#include "playback.h"

/**
 * process_playback_routing_us144mkii() - Apply playback routing matrix
 * @tascam: The driver instance.
 * @src_buffer: Buffer containing 4 channels of S24_3LE audio from ALSA.
 * @dst_buffer: Buffer to be filled for the USB device.
 * @frames: Number of frames to process.
 */
void process_playback_routing_us144mkii(struct tascam_card *tascam,
					       const u8 *src_buffer,
					       u8 *dst_buffer, size_t frames)
{
	size_t f;
	const u8 *src_12, *src_34;
	u8 *dst_line, *dst_digital;

	for (f = 0; f < frames; ++f) {
		src_12 = src_buffer + f * BYTES_PER_FRAME;
		src_34 = src_12 + (2 * BYTES_PER_SAMPLE);
		dst_line = dst_buffer + f * BYTES_PER_FRAME;
		dst_digital = dst_line + (2 * BYTES_PER_SAMPLE);

		/* LINE OUTPUTS (ch1/2 on device) */
		if (tascam->line_out_source == 0) /* "ch1 and ch2" */
			memcpy(dst_line, src_12, 2 * BYTES_PER_SAMPLE);
		else /* "ch3 and ch4" */
			memcpy(dst_line, src_34, 2 * BYTES_PER_SAMPLE);

		/* DIGITAL OUTPUTS (ch3/4 on device) */
		if (tascam->digital_out_source == 0) /* "ch1 and ch2" */
			memcpy(dst_digital, src_12, 2 * BYTES_PER_SAMPLE);
		else /* "ch3 and ch4" */
			memcpy(dst_digital, src_34, 2 * BYTES_PER_SAMPLE);
	}
}

/**
 * playback_urb_complete() - Completion handler for playback isochronous URBs.
 * @urb: the completed URB
 *
 * This function runs in interrupt context. It calculates the number of bytes
 * to send in the next set of packets based on the feedback-driven clock,
 * copies the audio data from the ALSA ring buffer (applying routing), and
 * resubmits the URB.
 */
void playback_urb_complete(struct urb *urb)
{
	struct tascam_card *tascam = urb->context;
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	unsigned long flags;
	u8 *src_buf, *dst_buf;
	size_t total_bytes_for_urb = 0;
	snd_pcm_uframes_t offset_frames;
	snd_pcm_uframes_t frames_to_copy;
	int ret, i;

	if (urb->status) {
		if (urb->status != -ENOENT && urb->status != -ECONNRESET && urb->status != -ESHUTDOWN &&
		    urb->status != -ENODEV)
			dev_err_ratelimited(tascam->card->dev, "Playback URB failed: %d\n", urb->status);
		goto out;
	}
	if (!tascam || !atomic_read(&tascam->playback_active))
		goto out;

	substream = tascam->playback_substream;
	if (!substream || !substream->runtime)
		goto out;
	runtime = substream->runtime;

	spin_lock_irqsave(&tascam->lock, flags);

	for (i = 0; i < urb->number_of_packets; i++) {
		unsigned int frames_for_packet;
		size_t bytes_for_packet;

		if (tascam->feedback_synced) {
			frames_for_packet = tascam->feedback_accumulator_pattern[tascam->feedback_pattern_out_idx];
			tascam->feedback_pattern_out_idx = (tascam->feedback_pattern_out_idx + 1) % FEEDBACK_ACCUMULATOR_SIZE;
		} else {
			frames_for_packet = runtime->rate / 8000;
		}
		bytes_for_packet = frames_for_packet * BYTES_PER_FRAME;

		urb->iso_frame_desc[i].offset = total_bytes_for_urb;
		urb->iso_frame_desc[i].length = bytes_for_packet;
		total_bytes_for_urb += bytes_for_packet;
	}
	urb->transfer_buffer_length = total_bytes_for_urb;

	offset_frames = tascam->driver_playback_pos;
	frames_to_copy = bytes_to_frames(runtime, total_bytes_for_urb);
	tascam->driver_playback_pos = (offset_frames + frames_to_copy) % runtime->buffer_size;

	spin_unlock_irqrestore(&tascam->lock, flags);

	if (total_bytes_for_urb > 0) {
		src_buf = runtime->dma_area + frames_to_bytes(runtime, offset_frames);
		dst_buf = tascam->playback_routing_buffer;

		/* Handle ring buffer wrap-around */
		if (offset_frames + frames_to_copy > runtime->buffer_size) {
			size_t first_chunk_bytes = frames_to_bytes(runtime, runtime->buffer_size - offset_frames);
			size_t second_chunk_bytes = total_bytes_for_urb - first_chunk_bytes;

			memcpy(dst_buf, src_buf, first_chunk_bytes);
			memcpy(dst_buf + first_chunk_bytes, runtime->dma_area, second_chunk_bytes);
		} else {
			memcpy(dst_buf, src_buf, total_bytes_for_urb);
		}

		/* Apply routing to the contiguous data in our routing buffer */
		process_playback_routing_us144mkii(tascam, dst_buf, dst_buf, frames_to_copy);
		memcpy(urb->transfer_buffer, dst_buf, total_bytes_for_urb);
	}

	urb->dev = tascam->dev;
	usb_get_urb(urb);
	usb_anchor_urb(urb, &tascam->playback_anchor);
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0) {
		dev_err_ratelimited(tascam->card->dev, "Failed to resubmit playback URB: %d\n", ret);
		usb_unanchor_urb(urb);
		usb_put_urb(urb);
	}
out:
	usb_put_urb(urb);
}

/**
 * feedback_urb_complete() - Completion handler for feedback isochronous URBs.
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
	struct snd_pcm_substream *playback_ss, *capture_ss;
	struct snd_pcm_runtime *playback_rt, *capture_rt;
	unsigned long flags;
	u64 total_frames_in_urb = 0;
	int ret, p;
	unsigned int old_in_idx, new_in_idx;
	bool playback_period_elapsed = false;
	bool capture_period_elapsed = false;

	if (urb->status) {
		if (urb->status != -ENOENT && urb->status != -ECONNRESET && urb->status != -ESHUTDOWN &&
		    urb->status != -ENODEV)
			dev_err_ratelimited(tascam->card->dev, "Feedback URB failed: %d\n", urb->status);
		goto out;
	}
	if (!tascam || !atomic_read(&tascam->playback_active))
		goto out;

	playback_ss = tascam->playback_substream;
	if (!playback_ss || !playback_ss->runtime)
		goto out;
	playback_rt = playback_ss->runtime;

	capture_ss = tascam->capture_substream;
	capture_rt = capture_ss ? capture_ss->runtime : NULL;

	spin_lock_irqsave(&tascam->lock, flags);

	if (tascam->feedback_urb_skip_count > 0) {
		tascam->feedback_urb_skip_count--;
		goto unlock_and_continue;
	}

	old_in_idx = tascam->feedback_pattern_in_idx;

	for (p = 0; p < urb->number_of_packets; p++) {
		u8 feedback_value = 0;
		const unsigned int *pattern;
		bool packet_ok = (urb->iso_frame_desc[p].status == 0 &&
				  urb->iso_frame_desc[p].actual_length >= 1);

		if (packet_ok)
			feedback_value = *((u8 *)urb->transfer_buffer + urb->iso_frame_desc[p].offset);

		if (packet_ok && feedback_value >= tascam->feedback_base_value &&
		    feedback_value <= tascam->feedback_max_value) {
			pattern = tascam->feedback_patterns[feedback_value - tascam->feedback_base_value];
			tascam->feedback_consecutive_errors = 0;
			int i;

			for (i = 0; i < 8; i++) {
				unsigned int in_idx = (tascam->feedback_pattern_in_idx + i) % FEEDBACK_ACCUMULATOR_SIZE;

				tascam->feedback_accumulator_pattern[in_idx] = pattern[i];
				total_frames_in_urb += pattern[i];
			}
		} else {
			unsigned int nominal_frames = playback_rt->rate / 8000;
			int i;

			if (tascam->feedback_synced) {
				tascam->feedback_consecutive_errors++;
				if (tascam->feedback_consecutive_errors > FEEDBACK_SYNC_LOSS_THRESHOLD) {
					dev_err(tascam->card->dev, "Fatal: Feedback sync lost. Stopping stream.\n");
					if (playback_ss)
						snd_pcm_stop(playback_ss, SNDRV_PCM_STATE_XRUN);
					if (capture_ss)
						snd_pcm_stop(capture_ss, SNDRV_PCM_STATE_XRUN);
					tascam->feedback_synced = false;
					goto unlock_and_continue;
				}
			}
			for (i = 0; i < 8; i++) {
				unsigned int in_idx = (tascam->feedback_pattern_in_idx + i) % FEEDBACK_ACCUMULATOR_SIZE;

				tascam->feedback_accumulator_pattern[in_idx] = nominal_frames;
				total_frames_in_urb += nominal_frames;
			}
		}
		tascam->feedback_pattern_in_idx = (tascam->feedback_pattern_in_idx + 8) % FEEDBACK_ACCUMULATOR_SIZE;
	}

	new_in_idx = tascam->feedback_pattern_in_idx;

	if (!tascam->feedback_synced) {
		unsigned int out_idx = tascam->feedback_pattern_out_idx;
		bool is_ahead = (new_in_idx - out_idx) % FEEDBACK_ACCUMULATOR_SIZE < (FEEDBACK_ACCUMULATOR_SIZE / 2);
		bool was_behind = (old_in_idx - out_idx) % FEEDBACK_ACCUMULATOR_SIZE >= (FEEDBACK_ACCUMULATOR_SIZE / 2);

		if (is_ahead && was_behind) {
			dev_dbg(tascam->card->dev, "Sync Acquired! (in: %u, out: %u)\n", new_in_idx, out_idx);
			tascam->feedback_synced = true;
			tascam->feedback_consecutive_errors = 0;
		}
	}

	if (total_frames_in_urb > 0) {
		tascam->playback_frames_consumed += total_frames_in_urb;
		if (atomic_read(&tascam->capture_active))
			tascam->capture_frames_processed += total_frames_in_urb;
	}

	if (playback_rt->period_size > 0) {
		u64 current_period = div_u64(tascam->playback_frames_consumed, playback_rt->period_size);

		if (current_period > tascam->last_period_pos) {
			tascam->last_period_pos = current_period;
			playback_period_elapsed = true;
		}
	}

	if (atomic_read(&tascam->capture_active) && capture_rt && capture_rt->period_size > 0) {
		u64 current_capture_period = div_u64(tascam->capture_frames_processed, capture_rt->period_size);

		if (current_capture_period > tascam->last_capture_period_pos) {
			tascam->last_capture_period_pos = current_capture_period;
			capture_period_elapsed = true;
		}
	}

unlock_and_continue:
	spin_unlock_irqrestore(&tascam->lock, flags);

	if (playback_period_elapsed)
		snd_pcm_period_elapsed(playback_ss);
	if (capture_period_elapsed)
		snd_pcm_period_elapsed(capture_ss);

	urb->dev = tascam->dev;
	usb_get_urb(urb);
	usb_anchor_urb(urb, &tascam->feedback_anchor);
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0) {
		dev_err_ratelimited(tascam->card->dev, "Failed to resubmit feedback URB: %d\n", ret);
		usb_unanchor_urb(urb);
		usb_put_urb(urb);
	}
out:
	usb_put_urb(urb);
}
