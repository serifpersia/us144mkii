// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2025 Šerif Rami <ramiserifpersia@gmail.com>

#include "us144mkii.h"

/**
 * fpo_init_pattern() - Generates a packet distribution pattern.
 * @size: The number of elements in the pattern array (e.g., 8).
 * @pattern_array: Pointer to the array to be populated.
 * @initial_value: The base value to initialize each element with.
 * @target_sum: The desired sum of all elements in the final array.
 *
 * This function initializes an array with a base value and then iteratively
 * adjusts the elements to match a target sum, distributing the difference
 * as evenly as possible.
 */
static void fpo_init_pattern(unsigned int size, unsigned int *pattern_array,
	unsigned int initial_value, int target_sum)
{
	int diff, i;

	if (!size)
		return;

	for (i = 0; i < size; ++i)
		pattern_array[i] = initial_value;

	diff = target_sum - (size * initial_value);
	for (i = 0; i < abs(diff); ++i) {
		if (diff > 0)
			pattern_array[i]++;
		else
			pattern_array[i]--;
	}
}

const struct snd_pcm_hardware tascam_pcm_hw = {
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
	.period_bytes_min = 48 * BYTES_PER_FRAME,
	.period_bytes_max = 1024 * BYTES_PER_FRAME,
	.periods_min = 2,
	.periods_max = 1024,
};

void process_playback_routing_us144mkii(struct tascam_card *tascam,
					const u8 *src_buffer, u8 *dst_buffer,
					size_t frames)
{
	size_t f;

	/*
	 * If no routing is needed and the operation is in-place, we can
	 * skip everything.
	 */
	if (tascam->line_out_source == 0 && tascam->digital_out_source == 1 &&
	    src_buffer == dst_buffer)
		return;

	for (f = 0; f < frames; ++f) {
		const u8 *frame_base = src_buffer + f * BYTES_PER_FRAME;
		u8 *dst_frame_base = dst_buffer + f * BYTES_PER_FRAME;

		/*
		 * Use a temporary buffer for the source frame to prevent data
		 * corruption during in-place routing operations.
		 */
		u8 temp_src_frame[BYTES_PER_FRAME];
		const u8 *src_12;
		const u8 *src_34;

		memcpy(temp_src_frame, frame_base, BYTES_PER_FRAME);
		src_12 = temp_src_frame;
		src_34 = temp_src_frame + (2 * BYTES_PER_SAMPLE);

		/* LINE OUTPUTS (ch1/2 on device) */
		if (tascam->line_out_source == 0) /* "Playback 1-2" */
			memcpy(dst_frame_base, src_12, 2 * BYTES_PER_SAMPLE);
		else /* "Playback 3-4" */
			memcpy(dst_frame_base, src_34, 2 * BYTES_PER_SAMPLE);

		/* DIGITAL OUTPUTS (ch3/4 on device) */
		if (tascam->digital_out_source == 0) /* "Playback 1-2" */
			memcpy(dst_frame_base + (2 * BYTES_PER_SAMPLE), src_12,
			       2 * BYTES_PER_SAMPLE);
		else /* "Playback 3-4" */
			memcpy(dst_frame_base + (2 * BYTES_PER_SAMPLE), src_34,
			       2 * BYTES_PER_SAMPLE);
	}
}

void process_capture_routing_us144mkii(struct tascam_card *tascam,
				       const s32 *decoded_block,
				       s32 *routed_block)
{
	int f;
	const s32 *src_frame;
	s32 *dst_frame;

	for (f = 0; f < FRAMES_PER_DECODE_BLOCK; f++) {
		src_frame = decoded_block + (f * DECODED_CHANNELS_PER_FRAME);
		dst_frame = routed_block + (f * DECODED_CHANNELS_PER_FRAME);

		/* ch1 and ch2 Source */
		if (tascam->capture_12_source == 0) { /* analog inputs */
			dst_frame[0] = src_frame[0]; /* Analog L */
			dst_frame[1] = src_frame[1]; /* Analog R */
		} else { /* digital inputs */
			dst_frame[0] = src_frame[2]; /* Digital L */
			dst_frame[1] = src_frame[3]; /* Digital R */
		}

		/* ch3 and ch4 Source */
		if (tascam->capture_34_source == 0) { /* analog inputs */
			dst_frame[2] = src_frame[0]; /* Analog L (Duplicate) */
			dst_frame[3] = src_frame[1]; /* Analog R (Duplicate) */
		} else { /* digital inputs */
			dst_frame[2] = src_frame[2]; /* Digital L */
			dst_frame[3] = src_frame[3]; /* Digital R */
		}
	}
}

int us144mkii_configure_device_for_rate(struct tascam_card *tascam, int rate)
{
	struct usb_device *dev = tascam->dev;
	u8 *rate_payload_buf __free(kfree) = NULL;
	u16 rate_vendor_wValue;
	int err = 0;
	const u8 *current_payload_src;

	static const u8 payload_44100[] = { 0x44, 0xac, 0x00 };
	static const u8 payload_48000[] = { 0x80, 0xbb, 0x00 };
	static const u8 payload_88200[] = { 0x88, 0x58, 0x01 };
	static const u8 payload_96000[] = { 0x00, 0x77, 0x01 };

	switch (rate) {
	case 44100:
		current_payload_src = payload_44100;
		rate_vendor_wValue = REG_ADDR_RATE_44100;
		break;
	case 48000:
		current_payload_src = payload_48000;
		rate_vendor_wValue = REG_ADDR_RATE_48000;
		break;
	case 88200:
		current_payload_src = payload_88200;
		rate_vendor_wValue = REG_ADDR_RATE_88200;
		break;
	case 96000:
		current_payload_src = payload_96000;
		rate_vendor_wValue = REG_ADDR_RATE_96000;
		break;
	default:
		dev_err(&dev->dev,
			"Unsupported sample rate %d for configuration\n", rate);
		return -EINVAL;
	}

	rate_payload_buf = kmemdup(current_payload_src, 3, GFP_KERNEL);
	if (!rate_payload_buf)
		return -ENOMEM;

	dev_info(&dev->dev, "Configuring device for %d Hz\n", rate);

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
			      VENDOR_REQ_MODE_CONTROL, RT_H2D_VENDOR_DEV,
			      MODE_VAL_CONFIG, 0x0000, NULL, 0,
			      USB_CTRL_TIMEOUT_MS);
	if (err < 0)
		goto fail;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), UAC_SET_CUR,
			      RT_H2D_CLASS_EP, UAC_SAMPLING_FREQ_CONTROL,
			      EP_AUDIO_IN, rate_payload_buf, 3,
			      USB_CTRL_TIMEOUT_MS);
	if (err < 0)
		goto fail;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), UAC_SET_CUR,
			      RT_H2D_CLASS_EP, UAC_SAMPLING_FREQ_CONTROL,
			      EP_AUDIO_OUT, rate_payload_buf, 3,
			      USB_CTRL_TIMEOUT_MS);
	if (err < 0)
		goto fail;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
			      VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV,
			      REG_ADDR_UNKNOWN_0D, REG_VAL_ENABLE, NULL, 0,
			      USB_CTRL_TIMEOUT_MS);
	if (err < 0)
		goto fail;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
			      VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV,
			      REG_ADDR_UNKNOWN_0E, REG_VAL_ENABLE, NULL, 0,
			      USB_CTRL_TIMEOUT_MS);
	if (err < 0)
		goto fail;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
			      VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV,
			      REG_ADDR_UNKNOWN_0F, REG_VAL_ENABLE, NULL, 0,
			      USB_CTRL_TIMEOUT_MS);
	if (err < 0)
		goto fail;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
			      VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV,
			      rate_vendor_wValue, REG_VAL_ENABLE, NULL, 0,
			      USB_CTRL_TIMEOUT_MS);
	if (err < 0)
		goto fail;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
			      VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV,
			      REG_ADDR_UNKNOWN_11, REG_VAL_ENABLE, NULL, 0,
			      USB_CTRL_TIMEOUT_MS);
	if (err < 0)
		goto fail;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
			      VENDOR_REQ_MODE_CONTROL, RT_H2D_VENDOR_DEV,
			      MODE_VAL_STREAM_START, 0x0000, NULL, 0,
			      USB_CTRL_TIMEOUT_MS);
	if (err < 0)
		goto fail;
	return 0;

fail:
	dev_err(&dev->dev,
		"Device configuration failed at rate %d with error %d\n", rate,
		err);
	return err;
}

int tascam_pcm_hw_params(struct snd_pcm_substream *substream,
					 struct snd_pcm_hw_params *params)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	int err;
	unsigned int rate = params_rate(params);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		tascam->fpo.sample_rate_khz = rate / 1000;
		tascam->fpo.base_feedback_value = tascam->fpo.sample_rate_khz;
		tascam->fpo.feedback_offset = 2;
		tascam->fpo.current_index = 0;
		tascam->fpo.previous_index = 0;
		tascam->fpo.sync_locked = false;

		unsigned int initial_value = tascam->fpo.sample_rate_khz / 8;

		for (int i = 0; i < 5; i++) {
			int target_sum = tascam->fpo.sample_rate_khz -
					 tascam->fpo.feedback_offset + i;
			fpo_init_pattern(8, tascam->fpo.full_frame_patterns[i],
					       initial_value, target_sum);
		}
	}

	if (tascam->current_rate != rate) {
		err = us144mkii_configure_device_for_rate(tascam, rate);
		if (err < 0) {
			tascam->current_rate = 0;
			return err;
		}
		tascam->current_rate = rate;
	}

	return 0;
}

int tascam_pcm_hw_free(struct snd_pcm_substream *substream)
{
	return 0;
}

int tascam_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	int err = 0;
	int i;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			if (atomic_xchg(&tascam->playback_active, 1) == 0) {
				for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
					usb_get_urb(tascam->playback_urbs[i]);
					usb_anchor_urb(tascam->playback_urbs[i], &tascam->playback_anchor);
					err = usb_submit_urb(tascam->playback_urbs[i], GFP_ATOMIC);
					if (err < 0) {
						usb_unanchor_urb(tascam->playback_urbs[i]);
						usb_put_urb(tascam->playback_urbs[i]);
						break;
					}
					atomic_inc(&tascam->active_urbs);
				}
				for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
					usb_get_urb(tascam->feedback_urbs[i]);
					usb_anchor_urb(tascam->feedback_urbs[i],
							&tascam->feedback_anchor);
					err = usb_submit_urb(tascam->feedback_urbs[i],
								GFP_ATOMIC);
					if (err < 0) {
						usb_unanchor_urb(tascam->feedback_urbs[i]);
						usb_put_urb(tascam->feedback_urbs[i]);
						atomic_dec(&tascam->active_urbs);
						break;
					}
					atomic_inc(&tascam->active_urbs);
				}
			}
		} else {
			if (atomic_xchg(&tascam->capture_active, 1) == 0) {
				for (i = 0; i < NUM_CAPTURE_URBS; i++) {
					usb_get_urb(tascam->capture_urbs[i]);
					usb_anchor_urb(tascam->capture_urbs[i], &tascam->capture_anchor);
					err = usb_submit_urb(tascam->capture_urbs[i], GFP_ATOMIC);
					if (err < 0) {
						usb_unanchor_urb(tascam->capture_urbs[i]);
						usb_put_urb(tascam->capture_urbs[i]);
						break;
					}
					atomic_inc(&tascam->active_urbs);
				}
			}
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			if (atomic_xchg(&tascam->playback_active, 0) == 1) {
				usb_kill_anchored_urbs(&tascam->playback_anchor);
				usb_kill_anchored_urbs(&tascam->feedback_anchor);
				schedule_work(&tascam->stop_work);
			}
		} else {
			if (atomic_xchg(&tascam->capture_active, 0) == 1) {
				usb_kill_anchored_urbs(&tascam->capture_anchor);
				schedule_work(&tascam->stop_work);
			}
		}
		break;
	default:
		err = -EINVAL;
		break;
	}

	return err;
}
int tascam_init_pcm(struct snd_pcm *pcm)
{
	struct tascam_card *tascam = pcm->private_data;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &tascam_playback_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &tascam_capture_ops);

	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_CONTINUOUS,
				       tascam->dev->dev.parent, 64 * 1024,
				       tascam_pcm_hw.buffer_bytes_max);

	return 0;
}
