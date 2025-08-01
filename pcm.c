// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2025 serifpersia <ramiserifpersia@gmail.com>

#include "us144mkii.h"



/**
 * @brief Rate-to-Packet Fixing Data (Verified)
 *
 * These static arrays define the number of audio frames per USB isochronous
 * packet for various sample rates. This data is crucial for maintaining
 * audio synchronization and preventing xruns, as the device's feedback
 * mechanism indicates how many samples it has consumed.
 *
 * The patterns are indexed by a feedback value received from the device,
 * which helps the driver adjust the packet size dynamically to match the
 * device's consumption rate.
 */
static const unsigned int patterns_48khz[5][8] = {
	{5, 6, 6, 6, 6, 6, 6, 6},
	{6, 6, 6, 6, 6, 6, 6, 6},
	{6, 6, 6, 6, 6, 6, 6, 6},
	{6, 6, 6, 7, 6, 6, 6, 6},
	{7, 6, 6, 7, 6, 6, 7, 6}
};
static const unsigned int patterns_96khz[5][8] = {
	{11, 12, 12, 12, 12, 12, 12, 12},
	{12, 12, 12, 12, 12, 12, 12, 12},
	{12, 12, 12, 12, 12, 12, 12, 12},
	{12, 12, 13, 12, 12, 12, 12, 12},
	{13, 12, 12, 13, 12, 12, 13, 12}
};
static const unsigned int patterns_88khz[5][8] = {
	{10, 11, 11, 11, 11, 11, 11, 11},
	{11, 11, 11, 11, 11, 11, 11, 11},
	{11, 11, 11, 11, 11, 11, 11, 11},
	{11, 11, 12, 11, 11, 11, 11, 11},
	{12, 11, 11, 12, 11, 11, 12, 11}
};
static const unsigned int patterns_44khz[5][8] = {
	{5, 5, 5, 5, 5, 5, 5, 6},
	{5, 5, 5, 6, 5, 5, 5, 6},
	{5, 5, 6, 5, 6, 5, 5, 6},
	{5, 6, 5, 6, 5, 6, 5, 6},
	{6, 6, 6, 6, 6, 6, 6, 5}
};

const struct snd_pcm_hardware tascam_pcm_hw = {
	.info = (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
		 SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_MMAP_VALID |
		 SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME),
	.formats = SNDRV_PCM_FMTBIT_S24_3LE,
	.rates = (SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
		  SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000),
	.rate_min = 44100, .rate_max = 96000,
	.channels_min = NUM_CHANNELS,
	.channels_max = NUM_CHANNELS,
	.buffer_bytes_max = 1024 * 1024,
	.period_bytes_min = 48 * BYTES_PER_FRAME,
	.period_bytes_max = 1024 * BYTES_PER_FRAME,
	.periods_min = 2, .periods_max = 1024,
};

/**
 * process_playback_routing_us144mkii() - Apply playback routing matrix
 * @tascam: The driver instance.
 * @src_buffer: Buffer containing 4 channels of S24_3LE audio from ALSA.
 * @dst_buffer: Buffer to be filled for the USB device.
 * @frames: Number of frames to process.
 */
static void process_playback_routing_us144mkii(struct tascam_card *tascam,
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
 * process_capture_routing_us144mkii() - Apply capture routing matrix
 * @tascam: The driver instance.
 * @decoded_block: Buffer containing 4 channels of S32LE decoded audio.
 * @routed_block: Buffer to be filled for ALSA.
 */
static void process_capture_routing_us144mkii(struct tascam_card *tascam,
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

/**
 * tascam_playback_open() - Opens the PCM playback substream.
 * @substream: The ALSA PCM substream to open.
 *
 * This function sets the hardware parameters for the playback substream
 * and stores a reference to the substream in the driver's private data.
 *
 * Return: 0 on success.
 */
static int tascam_playback_open(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);

	substream->runtime->hw = tascam_pcm_hw;
	tascam->playback_substream = substream;
	atomic_set(&tascam->playback_active, 0);

	return 0;
}

/**
 * tascam_capture_open() - Opens the PCM capture substream.
 * @substream: The ALSA PCM substream to open.
 *
 * This function sets the hardware parameters for the capture substream
 * and stores a reference to the substream in the driver's private data.
 *
 * Return: 0 on success.
 */
static int tascam_capture_open(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);

	substream->runtime->hw = tascam_pcm_hw;
	tascam->capture_substream = substream;
	atomic_set(&tascam->capture_active, 0);

	return 0;
}

/**
 * tascam_playback_close() - Closes the PCM playback substream.
 * @substream: The ALSA PCM substream to close.
 *
 * This function clears the reference to the playback substream in the
 * driver's private data.
 *
 * Return: 0 on success.
 */
static int tascam_playback_close(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);

	tascam->playback_substream = NULL;

	return 0;
}

/**
 * tascam_capture_close() - Closes the PCM capture substream.
 * @substream: The ALSA PCM substream to close.
 *
 * This function clears the reference to the capture substream in the
 * driver's private data.
 *
 * Return: 0 on success.
 */
static int tascam_capture_close(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);

	tascam->capture_substream = NULL;

	return 0;
}

/**
 * us144mkii_configure_device_for_rate() - Set sample rate via USB control msgs
 * @tascam: the tascam_card instance
 * @rate: the target sample rate (e.g., 44100, 96000)
 *
 * This function sends a sequence of vendor-specific and UAC control messages
 * to configure the device hardware for the specified sample rate.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int us144mkii_configure_device_for_rate(struct tascam_card *tascam, int rate)
{
	struct usb_device *dev = tascam->dev;
	u8 *rate_payload_buf;
	u16 rate_vendor_wValue;
	int err = 0;
	const u8 *current_payload_src;

	static const u8 payload_44100[] = {0x44, 0xac, 0x00};
	static const u8 payload_48000[] = {0x80, 0xbb, 0x00};
	static const u8 payload_88200[] = {0x88, 0x58, 0x01};
	static const u8 payload_96000[] = {0x00, 0x77, 0x01};

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
		dev_err(&dev->dev, "Unsupported sample rate %d for configuration\n", rate);
		return -EINVAL;
	}

	rate_payload_buf = kmemdup(current_payload_src, 3, GFP_KERNEL);
	if (!rate_payload_buf)
		return -ENOMEM;

	dev_info(&dev->dev, "Configuring device for %d Hz\n", rate);

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_MODE_CONTROL, RT_H2D_VENDOR_DEV, MODE_VAL_CONFIG, 0x0000, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0)
		goto fail;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), UAC_SET_CUR, RT_H2D_CLASS_EP, UAC_SAMPLING_FREQ_CONTROL, EP_AUDIO_IN, rate_payload_buf, 3, USB_CTRL_TIMEOUT_MS);
	if (err < 0)
		goto fail;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), UAC_SET_CUR, RT_H2D_CLASS_EP, UAC_SAMPLING_FREQ_CONTROL, EP_AUDIO_OUT, rate_payload_buf, 3, USB_CTRL_TIMEOUT_MS);
	if (err < 0)
		goto fail;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, REG_ADDR_UNKNOWN_0D, REG_VAL_ENABLE, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0)
		goto fail;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, REG_ADDR_UNKNOWN_0E, REG_VAL_ENABLE, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0)
		goto fail;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, REG_ADDR_UNKNOWN_0F, REG_VAL_ENABLE, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0)
		goto fail;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, rate_vendor_wValue, REG_VAL_ENABLE, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0)
		goto fail;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, REG_ADDR_UNKNOWN_11, REG_VAL_ENABLE, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0)
		goto fail;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_MODE_CONTROL, RT_H2D_VENDOR_DEV, MODE_VAL_STREAM_START, 0x0000, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0)
		goto fail;

	kfree(rate_payload_buf);
	return 0;

fail:
	dev_err(&dev->dev, "Device configuration failed at rate %d with error %d\n", rate, err);
	kfree(rate_payload_buf);
	return err;
}

/**
 * tascam_pcm_hw_params() - Configures hardware parameters for PCM streams.
 * @substream: The ALSA PCM substream.
 * @params: The hardware parameters to apply.
 *
 * This function allocates pages for the PCM buffer and, for playback streams,
 * selects the appropriate feedback patterns based on the requested sample rate.
 * It also configures the device hardware for the selected sample rate if it
 * has changed.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
static int tascam_pcm_hw_params(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	int err;
	unsigned int rate = params_rate(params);

	err = snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(params));
	if (err < 0)
		return err;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		switch (rate) {
		case 44100:
			tascam->feedback_patterns = patterns_44khz;
			tascam->feedback_base_value = 43;
			tascam->feedback_max_value = 45;
			break;
		case 48000:
			tascam->feedback_patterns = patterns_48khz;
			tascam->feedback_base_value = 47;
			tascam->feedback_max_value = 49;
			break;
		case 88200:
			tascam->feedback_patterns = patterns_88khz;
			tascam->feedback_base_value = 87;
			tascam->feedback_max_value = 89;
			break;
		case 96000:
			tascam->feedback_patterns = patterns_96khz;
			tascam->feedback_base_value = 95;
			tascam->feedback_max_value = 97;
			break;
		default:
			return -EINVAL;
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

/**
 * tascam_pcm_hw_free() - Frees hardware parameters for PCM streams.
 * @substream: The ALSA PCM substream.
 *
 * This function frees the pages allocated for the PCM buffer.
 *
 * Return: 0 on success.
 */
static int tascam_pcm_hw_free(struct snd_pcm_substream *substream)
{
	return snd_pcm_lib_free_pages(substream);
}


/**
 * tascam_playback_prepare() - Prepares the PCM playback substream for use.
 * @substream: The ALSA PCM substream to prepare.
 *
 * This function initializes playback-related counters and flags, and configures
 * the playback URBs with appropriate packet sizes based on the nominal frame
 * rate and feedback accumulator.
 *
 * Return: 0 on success.
 */
static int tascam_playback_prepare(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int i, u;
	size_t nominal_frames_per_packet, nominal_bytes_per_packet;
	size_t total_bytes_in_urb;
	unsigned int feedback_packets;

	tascam->driver_playback_pos = 0;
	tascam->playback_frames_consumed = 0;
	tascam->last_period_pos = 0;
	tascam->feedback_pattern_in_idx = 0;
	tascam->feedback_pattern_out_idx = 0;
	tascam->feedback_synced = false;
	tascam->feedback_consecutive_errors = 0;
	tascam->feedback_urb_skip_count = NUM_FEEDBACK_URBS;

	nominal_frames_per_packet = runtime->rate / 8000;
	for (i = 0; i < FEEDBACK_ACCUMULATOR_SIZE; i++)
		tascam->feedback_accumulator_pattern[i] = nominal_frames_per_packet;

	feedback_packets = 1;

	for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
		struct urb *f_urb = tascam->feedback_urbs[i];
		int j;

		f_urb->number_of_packets = feedback_packets;
		f_urb->transfer_buffer_length = feedback_packets * FEEDBACK_PACKET_SIZE;
		for (j = 0; j < feedback_packets; j++) {
			f_urb->iso_frame_desc[j].offset = j * FEEDBACK_PACKET_SIZE;
			f_urb->iso_frame_desc[j].length = FEEDBACK_PACKET_SIZE;
		}
	}

	nominal_bytes_per_packet = nominal_frames_per_packet * BYTES_PER_FRAME;
	total_bytes_in_urb = nominal_bytes_per_packet * PLAYBACK_URB_PACKETS;

	for (u = 0; u < NUM_PLAYBACK_URBS; u++) {
		struct urb *urb = tascam->playback_urbs[u];

		memset(urb->transfer_buffer, 0, tascam->playback_urb_alloc_size);
		urb->transfer_buffer_length = total_bytes_in_urb;
		urb->number_of_packets = PLAYBACK_URB_PACKETS;
		for (i = 0; i < PLAYBACK_URB_PACKETS; i++) {
			urb->iso_frame_desc[i].offset = i * nominal_bytes_per_packet;
			urb->iso_frame_desc[i].length = nominal_bytes_per_packet;
		}
	}

	return 0;
}

/**
 * tascam_capture_prepare() - Prepares the PCM capture substream for use.
 * @substream: The ALSA PCM substream to prepare.
 *
 * This function initializes capture-related counters and ring buffer pointers.
 *
 * Return: 0 on success.
 */
static int tascam_capture_prepare(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);

	tascam->driver_capture_pos = 0;
	tascam->capture_frames_processed = 0;
	tascam->last_capture_period_pos = 0;
	tascam->capture_ring_buffer_read_ptr = 0;
	tascam->capture_ring_buffer_write_ptr = 0;

	return 0;
}

/**
 * tascam_pcm_trigger() - Triggers the start or stop of PCM streams.
 * @substream: The ALSA PCM substream.
 * @cmd: The trigger command (e.g., SNDRV_PCM_TRIGGER_START, SNDRV_PCM_TRIGGER_STOP).
 *
 * This function handles starting and stopping of playback and capture streams
 * by submitting or killing the associated URBs. It ensures that both streams
 * are started/stopped together.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
static int tascam_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	unsigned long flags;
	int err = 0;
	int i;
	bool do_start = false;
	bool do_stop = false;

	spin_lock_irqsave(&tascam->lock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		if (!atomic_read(&tascam->playback_active)) {
			atomic_set(&tascam->playback_active, 1);
			atomic_set(&tascam->capture_active, 1);
			do_start = true;
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		if (atomic_read(&tascam->playback_active)) {
			atomic_set(&tascam->playback_active, 0);
			atomic_set(&tascam->capture_active, 0);
			do_stop = true;
		}
		break;
	default:
		err = -EINVAL;
		break;
	}
	spin_unlock_irqrestore(&tascam->lock, flags);

	if (do_start) {
		if (atomic_read(&tascam->active_urbs) > 0) {
			dev_warn(tascam->card->dev, "Cannot start, URBs still active.\n");
			return -EAGAIN;
		}

		for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
			usb_get_urb(tascam->feedback_urbs[i]);
			usb_anchor_urb(tascam->feedback_urbs[i], &tascam->feedback_anchor);
			err = usb_submit_urb(tascam->feedback_urbs[i], GFP_ATOMIC);
			if (err < 0) {
				usb_unanchor_urb(tascam->feedback_urbs[i]);
				usb_put_urb(tascam->feedback_urbs[i]);
				goto start_rollback;
			}
			atomic_inc(&tascam->active_urbs);
		}
		for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
			usb_get_urb(tascam->playback_urbs[i]);
			usb_anchor_urb(tascam->playback_urbs[i], &tascam->playback_anchor);
			err = usb_submit_urb(tascam->playback_urbs[i], GFP_ATOMIC);
			if (err < 0) {
				usb_unanchor_urb(tascam->playback_urbs[i]);
				usb_put_urb(tascam->playback_urbs[i]);
				goto start_rollback;
			}
			atomic_inc(&tascam->active_urbs);
		}
		for (i = 0; i < NUM_CAPTURE_URBS; i++) {
			usb_get_urb(tascam->capture_urbs[i]);
			usb_anchor_urb(tascam->capture_urbs[i], &tascam->capture_anchor);
			err = usb_submit_urb(tascam->capture_urbs[i], GFP_ATOMIC);
			if (err < 0) {
				usb_unanchor_urb(tascam->capture_urbs[i]);
				usb_put_urb(tascam->capture_urbs[i]);
				goto start_rollback;
			}
			atomic_inc(&tascam->active_urbs);
		}

		return 0;
start_rollback:
		dev_err(tascam->card->dev, "Failed to submit URBs to start stream: %d\n", err);
		do_stop = true;
	}

	if (do_stop)
		schedule_work(&tascam->stop_work);

	return err;
}



/**
 * tascam_playback_pointer() - Returns the current playback pointer position.
 * @substream: The ALSA PCM substream.
 *
 * This function returns the current position of the playback pointer within
 * the ALSA ring buffer, in frames.
 *
 * Return: The current playback pointer position in frames.
 */
static snd_pcm_uframes_t tascam_playback_pointer(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	u64 pos;
	unsigned long flags;

	if (!atomic_read(&tascam->playback_active))
		return 0;

	spin_lock_irqsave(&tascam->lock, flags);
	pos = tascam->playback_frames_consumed;
	spin_unlock_irqrestore(&tascam->lock, flags);

	return runtime ? pos % runtime->buffer_size : 0;
}

static snd_pcm_uframes_t tascam_capture_pointer(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	u64 pos;
	unsigned long flags;

	if (!atomic_read(&tascam->capture_active))
		return 0;

	spin_lock_irqsave(&tascam->lock, flags);
	pos = tascam->capture_frames_processed;
	spin_unlock_irqrestore(&tascam->lock, flags);

	return runtime ? pos % runtime->buffer_size : 0;
}

/**
 * tascam_playback_ops - ALSA PCM operations for playback.
 *
 * This structure defines the callback functions for playback stream operations,
 * including open, close, ioctl, hardware parameters, hardware free, prepare,
 * trigger, and pointer.
 */
static struct snd_pcm_ops tascam_playback_ops = {
	.open = tascam_playback_open,
	.close = tascam_playback_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = tascam_pcm_hw_params,
	.hw_free = tascam_pcm_hw_free,
	.prepare = tascam_playback_prepare,
	.trigger = tascam_pcm_trigger,
	.pointer = tascam_playback_pointer,
};

/**
 * tascam_capture_ops - ALSA PCM operations for capture.
 *
 * This structure defines the callback functions for capture stream operations,
 * including open, close, ioctl, hardware parameters, hardware free, prepare,
 * trigger, and pointer.
 */
static struct snd_pcm_ops tascam_capture_ops = {
	.open = tascam_capture_open,
	.close = tascam_capture_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = tascam_pcm_hw_params,
	.hw_free = tascam_pcm_hw_free,
	.prepare = tascam_capture_prepare,
	.trigger = tascam_pcm_trigger,
	.pointer = tascam_capture_pointer,
};

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

/**
 * decode_tascam_capture_block() - Decodes a raw 512-byte block from the device.
 * @src_block: Pointer to the 512-byte raw source block.
 * @dst_block: Pointer to the destination buffer for decoded audio frames.
 *
 * The device sends audio data in a complex, multiplexed format. This function
 * demultiplexes the bits from the raw block into 8 frames of 4-channel,
 * 24-bit audio (stored in 32-bit containers).
 */
static void decode_tascam_capture_block(const u8 *src_block, s32 *dst_block)
{
	int frame, bit;

	memset(dst_block, 0, FRAMES_PER_DECODE_BLOCK * DECODED_CHANNELS_PER_FRAME * DECODED_SAMPLE_SIZE);

	for (frame = 0; frame < FRAMES_PER_DECODE_BLOCK; ++frame) {
		const u8 *p_src_frame_base = src_block + frame * 64;
		s32 *p_dst_frame = dst_block + frame * 4;

		s32 ch[4] = {0};

		for (bit = 0; bit < 24; ++bit) {
			u8 byte1 = p_src_frame_base[bit];
			u8 byte2 = p_src_frame_base[bit + 32];

			ch[0] = (ch[0] << 1) | (byte1 & 1);
			ch[2] = (ch[2] << 1) | ((byte1 >> 1) & 1);

			ch[1] = (ch[1] << 1) | (byte2 & 1);
			ch[3] = (ch[3] << 1) | ((byte2 >> 1) & 1);
		}

		/*
		 * The result is a 24-bit sample. Shift left by 8 to align it to
		 * the most significant bits of a 32-bit integer (S32_LE format).
		 */
		p_dst_frame[0] = ch[0] << 8;
		p_dst_frame[1] = ch[1] << 8;
		p_dst_frame[2] = ch[2] << 8;
		p_dst_frame[3] = ch[3] << 8;
	}
}

/**
 * tascam_capture_work_handler() - Deferred work for processing capture data.
 * @work: the work_struct instance
 *
 * This function runs in a kernel thread context, not an IRQ context. It reads
 * raw data from the capture ring buffer, decodes it, applies routing, and
 * copies the final audio data into the ALSA capture ring buffer. This offloads
 * * the CPU-intensive decoding from the time-sensitive URB completion handlers.
 */
static void tascam_capture_work_handler(struct work_struct *work)
{
	struct tascam_card *tascam = container_of(work, struct tascam_card, capture_work);
	struct snd_pcm_substream *substream = tascam->capture_substream;
	struct snd_pcm_runtime *runtime;
	unsigned long flags;
	u8 *raw_block = tascam->capture_decode_raw_block;
	s32 *decoded_block = tascam->capture_decode_dst_block;
	s32 *routed_block = tascam->capture_routing_buffer;

	if (!substream || !substream->runtime)
		return;
	runtime = substream->runtime;

	if (!raw_block || !decoded_block || !routed_block) {
		dev_err(tascam->card->dev, "Capture decode/routing buffers not allocated!\n");
		return;
	}

	while (atomic_read(&tascam->capture_active)) {
		size_t write_ptr, read_ptr, available_data;
		bool can_process;

		spin_lock_irqsave(&tascam->lock, flags);
		write_ptr = tascam->capture_ring_buffer_write_ptr;
		read_ptr = tascam->capture_ring_buffer_read_ptr;
		available_data = (write_ptr >= read_ptr) ? (write_ptr - read_ptr) : (CAPTURE_RING_BUFFER_SIZE - read_ptr + write_ptr);
		can_process = (available_data >= RAW_BYTES_PER_DECODE_BLOCK);

		if (can_process) {
			size_t i;

			for (i = 0; i < RAW_BYTES_PER_DECODE_BLOCK; i++)
				raw_block[i] = tascam->capture_ring_buffer[(read_ptr + i) % CAPTURE_RING_BUFFER_SIZE];
			tascam->capture_ring_buffer_read_ptr = (read_ptr + RAW_BYTES_PER_DECODE_BLOCK) % CAPTURE_RING_BUFFER_SIZE;
		}
		spin_unlock_irqrestore(&tascam->lock, flags);

		if (!can_process)
			break;

		decode_tascam_capture_block(raw_block, decoded_block);
		process_capture_routing_us144mkii(tascam, decoded_block, routed_block);

		spin_lock_irqsave(&tascam->lock, flags);
		if (atomic_read(&tascam->capture_active)) {
			int f;

			for (f = 0; f < FRAMES_PER_DECODE_BLOCK; ++f) {
				u8 *dst_frame_start = runtime->dma_area + frames_to_bytes(runtime, tascam->driver_capture_pos);
				s32 *routed_frame_start = routed_block + (f * NUM_CHANNELS);
				int c;

				for (c = 0; c < NUM_CHANNELS; c++) {
					u8 *dst_channel = dst_frame_start + (c * BYTES_PER_SAMPLE);
					s32 *src_channel_s32 = routed_frame_start + c;

					memcpy(dst_channel, ((char *)src_channel_s32) + 1, 3);
				}

				tascam->driver_capture_pos = (tascam->driver_capture_pos + 1) % runtime->buffer_size;
			}
		}
		spin_unlock_irqrestore(&tascam->lock, flags);
	}
}

/**
 * capture_urb_complete() - Completion handler for capture bulk URBs.
 * @urb: the completed URB
 *
 * This function runs in interrupt context. It copies the received raw data
 * into an intermediate ring buffer and then schedules the workqueue to process
 * it. It then resubmits the URB to receive more data.
 */
void capture_urb_complete(struct urb *urb)
{
	struct tascam_card *tascam = urb->context;
	int ret;
	unsigned long flags;

	if (urb->status) {
		if (urb->status != -ENOENT && urb->status != -ECONNRESET && urb->status != -ESHUTDOWN &&
		    urb->status != -ENODEV && urb->status != -EPROTO)
			dev_err_ratelimited(tascam->card->dev, "Capture URB failed: %d\n", urb->status);
		goto out;
	}
	if (!tascam || !atomic_read(&tascam->capture_active))
		goto out;

	if (urb->actual_length > 0) {
		size_t i;
		size_t write_ptr;

		spin_lock_irqsave(&tascam->lock, flags);
		write_ptr = tascam->capture_ring_buffer_write_ptr;
		for (i = 0; i < urb->actual_length; i++) {
			tascam->capture_ring_buffer[write_ptr] = ((u8 *)urb->transfer_buffer)[i];
			write_ptr = (write_ptr + 1) % CAPTURE_RING_BUFFER_SIZE;
		}
		tascam->capture_ring_buffer_write_ptr = write_ptr;
		spin_unlock_irqrestore(&tascam->lock, flags);

		schedule_work(&tascam->capture_work);
	}

	usb_get_urb(urb);
	usb_anchor_urb(urb, &tascam->capture_anchor);
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0) {
		dev_err_ratelimited(tascam->card->dev, "Failed to resubmit capture URB: %d\n", ret);
		usb_unanchor_urb(urb);
		usb_put_urb(urb);
	}
out:
	usb_put_urb(urb);
}

/**
 * tascam_init_pcm() - Initializes the ALSA PCM device.
 * @pcm: Pointer to the ALSA PCM device to initialize.
 *
 * This function sets up the PCM operations for playback and capture,
 * preallocates pages for the PCM buffer, and initializes the workqueue
 * for deferred capture processing.
 *
 * Return: 0 on success.
 */
int tascam_init_pcm(struct snd_pcm *pcm)
{
	struct tascam_card *tascam = pcm->private_data;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &tascam_playback_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &tascam_capture_ops);
	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_CONTINUOUS,
					      tascam->dev->dev.parent,
					      64 * 1024,
					      tascam_pcm_hw.buffer_bytes_max);

	INIT_WORK(&tascam->capture_work, tascam_capture_work_handler);

	return 0;
}
