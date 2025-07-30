// SPDX-License-Identifier: GPL-2.0-only

#include "us144mkii.h"
#include "pcm.h"
#include "playback.h"
#include "capture.h"

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

static const struct snd_pcm_hardware tascam_pcm_hw = {
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
 * tascam_free_urbs() - Free all allocated URBs and associated buffers.
 * @tascam: the tascam_card instance
 *
 * This function kills, unlinks, and frees all playback, feedback, capture,
 * and MIDI URBs, along with their transfer buffers and the capture
 * ring/decode buffers.
 */
void tascam_free_urbs(struct tascam_card *tascam)
{
	int i;

	usb_kill_anchored_urbs(&tascam->playback_anchor);
	for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
		if (tascam->playback_urbs[i]) {
			usb_free_coherent(tascam->dev, tascam->playback_urb_alloc_size,
					  tascam->playback_urbs[i]->transfer_buffer,
					  tascam->playback_urbs[i]->transfer_dma);
			usb_free_urb(tascam->playback_urbs[i]);
			tascam->playback_urbs[i] = NULL;
		}
	}

	usb_kill_anchored_urbs(&tascam->feedback_anchor);
	for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
		if (tascam->feedback_urbs[i]) {
			usb_free_coherent(tascam->dev, tascam->feedback_urb_alloc_size,
					  tascam->feedback_urbs[i]->transfer_buffer,
					  tascam->feedback_urbs[i]->transfer_dma);
			usb_free_urb(tascam->feedback_urbs[i]);
			tascam->feedback_urbs[i] = NULL;
		}
	}

	usb_kill_anchored_urbs(&tascam->capture_anchor);
	for (i = 0; i < NUM_CAPTURE_URBS; i++) {
		if (tascam->capture_urbs[i]) {
			usb_free_coherent(tascam->dev, tascam->capture_urb_alloc_size,
					  tascam->capture_urbs[i]->transfer_buffer,
					  tascam->capture_urbs[i]->transfer_dma);
			usb_free_urb(tascam->capture_urbs[i]);
			tascam->capture_urbs[i] = NULL;
		}
	}

	usb_kill_anchored_urbs(&tascam->midi_in_anchor);
	for (i = 0; i < NUM_MIDI_IN_URBS; i++) {
		if (tascam->midi_in_urbs[i]) {
			usb_free_coherent(tascam->dev, MIDI_IN_BUF_SIZE,
					  tascam->midi_in_urbs[i]->transfer_buffer,
					  tascam->midi_in_urbs[i]->transfer_dma);
			usb_free_urb(tascam->midi_in_urbs[i]);
			tascam->midi_in_urbs[i] = NULL;
		}
	}

	usb_kill_anchored_urbs(&tascam->midi_out_anchor);
	for (i = 0; i < NUM_MIDI_OUT_URBS; i++) {
		if (tascam->midi_out_urbs[i]) {
			usb_free_coherent(tascam->dev, MIDI_OUT_BUF_SIZE,
					  tascam->midi_out_urbs[i]->transfer_buffer,
					  tascam->midi_out_urbs[i]->transfer_dma);
			usb_free_urb(tascam->midi_out_urbs[i]);
			tascam->midi_out_urbs[i] = NULL;
		}
	}

	kfree(tascam->playback_routing_buffer);
	tascam->playback_routing_buffer = NULL;
	kfree(tascam->capture_routing_buffer);
	tascam->capture_routing_buffer = NULL;
	kfree(tascam->capture_decode_dst_block);
	tascam->capture_decode_dst_block = NULL;
	kfree(tascam->capture_decode_raw_block);
	tascam->capture_decode_raw_block = NULL;
	kfree(tascam->capture_ring_buffer);
	tascam->capture_ring_buffer = NULL;
}

/**
 * tascam_alloc_urbs() - Allocate all URBs and associated buffers.
 * @tascam: the tascam_card instance
 *
 * This function allocates and initializes all URBs for playback, feedback,
 * capture, and MIDI, as well as the necessary buffers for data processing.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int tascam_alloc_urbs(struct tascam_card *tascam)
{
	int i;
	size_t max_packet_size;

	max_packet_size = ((96000 / 8000) + 2) * BYTES_PER_FRAME;
	tascam->playback_urb_alloc_size = max_packet_size * PLAYBACK_URB_PACKETS;

	for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
		struct urb *urb = usb_alloc_urb(PLAYBACK_URB_PACKETS, GFP_KERNEL);

		if (!urb)
			goto error;
		tascam->playback_urbs[i] = urb;

		urb->transfer_buffer = usb_alloc_coherent(tascam->dev, tascam->playback_urb_alloc_size,
					    GFP_KERNEL, &urb->transfer_dma);
		if (!urb->transfer_buffer)
			goto error;

		urb->dev = tascam->dev;
		urb->pipe = usb_sndisocpipe(tascam->dev, EP_AUDIO_OUT);
		urb->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
		urb->interval = 1;
		urb->context = tascam;
		urb->complete = playback_urb_complete;
	}

	tascam->feedback_urb_alloc_size = FEEDBACK_PACKET_SIZE * MAX_FEEDBACK_PACKETS;

	for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
		struct urb *f_urb = usb_alloc_urb(MAX_FEEDBACK_PACKETS, GFP_KERNEL);

		if (!f_urb)
			goto error;
		tascam->feedback_urbs[i] = f_urb;

		f_urb->transfer_buffer = usb_alloc_coherent(tascam->dev, tascam->feedback_urb_alloc_size,
					      GFP_KERNEL, &f_urb->transfer_dma);
		if (!f_urb->transfer_buffer)
			goto error;

		f_urb->dev = tascam->dev;
		f_urb->pipe = usb_rcvisocpipe(tascam->dev, EP_PLAYBACK_FEEDBACK);
		f_urb->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
		f_urb->interval = 4;
		f_urb->context = tascam;
		f_urb->complete = feedback_urb_complete;
	}

	tascam->capture_urb_alloc_size = CAPTURE_URB_SIZE;
	for (i = 0; i < NUM_CAPTURE_URBS; i++) {
		struct urb *c_urb = usb_alloc_urb(0, GFP_KERNEL);

		if (!c_urb)
			goto error;
		tascam->capture_urbs[i] = c_urb;

		c_urb->transfer_buffer = usb_alloc_coherent(tascam->dev, tascam->capture_urb_alloc_size,
					      GFP_KERNEL, &c_urb->transfer_dma);
		if (!c_urb->transfer_buffer)
			goto error;

		usb_fill_bulk_urb(c_urb, tascam->dev,
				  usb_rcvbulkpipe(tascam->dev, EP_AUDIO_IN),
				  c_urb->transfer_buffer,
				  tascam->capture_urb_alloc_size,
				  capture_urb_complete,
				  tascam);
		c_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	}

	/* MIDI URB and buffer allocation */
	for (i = 0; i < NUM_MIDI_IN_URBS; i++) {
		struct urb *m_urb = usb_alloc_urb(0, GFP_KERNEL);

		if (!m_urb)
			goto error;
		tascam->midi_in_urbs[i] = m_urb;
		m_urb->transfer_buffer = usb_alloc_coherent(tascam->dev,
			MIDI_IN_BUF_SIZE, GFP_KERNEL, &m_urb->transfer_dma);
		if (!m_urb->transfer_buffer)
			goto error;
		usb_fill_bulk_urb(m_urb, tascam->dev, usb_rcvbulkpipe(tascam->dev, EP_MIDI_IN),
				  m_urb->transfer_buffer, MIDI_IN_BUF_SIZE,
				  tascam_midi_in_urb_complete, tascam);
		m_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	}

	for (i = 0; i < NUM_MIDI_OUT_URBS; i++) {
		struct urb *m_urb = usb_alloc_urb(0, GFP_KERNEL);

		if (!m_urb)
			goto error;
		tascam->midi_out_urbs[i] = m_urb;
		m_urb->transfer_buffer = usb_alloc_coherent(tascam->dev,
			MIDI_OUT_BUF_SIZE, GFP_KERNEL, &m_urb->transfer_dma);
		if (!m_urb->transfer_buffer)
			goto error;
		usb_fill_bulk_urb(m_urb, tascam->dev,
				  usb_sndbulkpipe(tascam->dev, EP_MIDI_OUT),
				  m_urb->transfer_buffer, 0, /* length set later */
				  tascam_midi_out_urb_complete, tascam);
		m_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	}

	tascam->capture_ring_buffer = kmalloc(CAPTURE_RING_BUFFER_SIZE, GFP_KERNEL);
	if (!tascam->capture_ring_buffer)
		goto error;

	tascam->capture_decode_raw_block = kmalloc(RAW_BYTES_PER_DECODE_BLOCK, GFP_KERNEL);
	if (!tascam->capture_decode_raw_block)
		goto error;

	tascam->capture_decode_dst_block = kmalloc(FRAMES_PER_DECODE_BLOCK * DECODED_CHANNELS_PER_FRAME * DECODED_SAMPLE_SIZE, GFP_KERNEL);
	if (!tascam->capture_decode_dst_block)
		goto error;

	tascam->playback_routing_buffer = kmalloc(tascam->playback_urb_alloc_size, GFP_KERNEL);
	if (!tascam->playback_routing_buffer)
		goto error;

	tascam->capture_routing_buffer = kmalloc(FRAMES_PER_DECODE_BLOCK * DECODED_CHANNELS_PER_FRAME * DECODED_SAMPLE_SIZE, GFP_KERNEL);
	if (!tascam->capture_routing_buffer)
		goto error;

	return 0;

error:
	dev_err(tascam->card->dev, "Failed to allocate URBs\n");
	tascam_free_urbs(tascam);
	return -ENOMEM;
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

static int tascam_playback_open(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);

	substream->runtime->hw = tascam_pcm_hw;
	tascam->playback_substream = substream;
	atomic_set(&tascam->playback_active, 0);

	return 0;
}

static int tascam_capture_open(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);

	substream->runtime->hw = tascam_pcm_hw;
	tascam->capture_substream = substream;
	atomic_set(&tascam->capture_active, 0);

	return 0;
}

static int tascam_playback_close(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);

	tascam->playback_substream = NULL;

	return 0;
}

static int tascam_capture_close(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);

	tascam->capture_substream = NULL;

	return 0;
}

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

static int tascam_pcm_hw_free(struct snd_pcm_substream *substream)
{
	return snd_pcm_lib_free_pages(substream);
}


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

void tascam_stop_work_handler(struct work_struct *work)
{
	struct tascam_card *tascam = container_of(work, struct tascam_card, stop_work);

	usb_kill_anchored_urbs(&tascam->playback_anchor);
	usb_kill_anchored_urbs(&tascam->feedback_anchor);
	usb_kill_anchored_urbs(&tascam->capture_anchor);
	atomic_set(&tascam->active_urbs, 0);
	cancel_work_sync(&tascam->capture_work);
}

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

const struct snd_pcm_ops tascam_playback_ops = {
	.open = tascam_playback_open,
	.close = tascam_playback_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = tascam_pcm_hw_params,
	.hw_free = tascam_pcm_hw_free,
	.prepare = tascam_playback_prepare,
	.trigger = tascam_pcm_trigger,
	.pointer = tascam_playback_pointer,
};

const struct snd_pcm_ops tascam_capture_ops = {
	.open = tascam_capture_open,
	.close = tascam_capture_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = tascam_pcm_hw_params,
	.hw_free = tascam_pcm_hw_free,
	.prepare = tascam_capture_prepare,
	.trigger = tascam_pcm_trigger,
	.pointer = tascam_capture_pointer,
};

int tascam_create_pcm(struct tascam_card *tascam)
{
	int err;

	err = snd_pcm_new(tascam->card, "US144MKII", 0, 1, 1, &tascam->pcm);
	if (err < 0)
		return err;

	tascam->pcm->private_data = tascam;
	strscpy(tascam->pcm->name, "US144MKII", sizeof(tascam->pcm->name));

	snd_pcm_set_ops(tascam->pcm, SNDRV_PCM_STREAM_PLAYBACK, &tascam_playback_ops);
	snd_pcm_set_ops(tascam->pcm, SNDRV_PCM_STREAM_CAPTURE, &tascam_capture_ops);

	snd_pcm_lib_preallocate_pages_for_all(tascam->pcm, SNDRV_DMA_TYPE_CONTINUOUS,
					      tascam->dev->dev.parent,
					      64 * 1024, tascam_pcm_hw.buffer_bytes_max);

	return 0;
}
