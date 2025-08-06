// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2025 Šerif Rami <ramiserifpersia@gmail.com>

#include "us144mkii.h"

/**
 * @brief Rate-to-Packet Fixing Data
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
<<<<<<< HEAD
static const unsigned int patterns_48khz[5][8] = {{5, 6, 6, 6, 6, 6, 6, 6},
                                                  {6, 6, 6, 6, 6, 6, 6, 6},
                                                  {6, 6, 6, 6, 6, 6, 6, 6},
                                                  {6, 6, 6, 7, 6, 6, 6, 6},
                                                  {7, 6, 6, 7, 6, 6, 7, 6}};
static const unsigned int patterns_96khz[5][8] = {
    {11, 12, 12, 12, 12, 12, 12, 12},
    {12, 12, 12, 12, 12, 12, 12, 12},
    {12, 12, 12, 12, 12, 12, 12, 12},
    {12, 12, 13, 12, 12, 12, 12, 12},
    {13, 12, 12, 13, 12, 12, 13, 12}};
static const unsigned int patterns_88khz[5][8] = {
    {10, 11, 11, 11, 11, 11, 11, 11},
    {11, 11, 11, 11, 11, 11, 11, 11},
    {11, 11, 11, 11, 11, 11, 11, 11},
    {11, 11, 12, 11, 11, 11, 11, 11},
    {12, 11, 11, 12, 11, 11, 12, 11}};
static const unsigned int patterns_44khz[5][8] = {{5, 5, 5, 5, 5, 5, 5, 6},
                                                  {5, 5, 5, 6, 5, 5, 5, 6},
                                                  {5, 5, 6, 5, 6, 5, 5, 6},
                                                  {5, 6, 5, 6, 5, 6, 5, 6},
                                                  {6, 6, 6, 6, 6, 6, 6, 5}};

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
=======
static const unsigned int patterns_48khz[5][8] = { { 5, 6, 6, 6, 6, 6, 6, 6 },
						   { 6, 6, 6, 6, 6, 6, 6, 6 },
						   { 6, 6, 6, 6, 6, 6, 6, 6 },
						   { 6, 6, 6, 7, 6, 6, 6, 6 },
						   { 7, 6, 6, 7, 6, 6, 7, 6 } };
static const unsigned int patterns_96khz[5][8] = {
	{ 11, 12, 12, 12, 12, 12, 12, 12 },
	{ 12, 12, 12, 12, 12, 12, 12, 12 },
	{ 12, 12, 12, 12, 12, 12, 12, 12 },
	{ 12, 12, 13, 12, 12, 12, 12, 12 },
	{ 13, 12, 12, 13, 12, 12, 13, 12 }
};
static const unsigned int patterns_88khz[5][8] = {
	{ 10, 11, 11, 11, 11, 11, 11, 11 },
	{ 11, 11, 11, 11, 11, 11, 11, 11 },
	{ 11, 11, 11, 11, 11, 11, 11, 11 },
	{ 11, 11, 12, 11, 11, 11, 11, 11 },
	{ 12, 11, 11, 12, 11, 11, 12, 11 }
};
static const unsigned int patterns_44khz[5][8] = { { 5, 5, 5, 5, 5, 5, 5, 6 },
						   { 5, 5, 5, 6, 5, 5, 5, 6 },
						   { 5, 5, 6, 5, 6, 5, 5, 6 },
						   { 5, 6, 5, 6, 5, 6, 5, 6 },
						   { 6, 6, 6, 6, 6, 6, 6, 5 } };

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
>>>>>>> f44b75094c078b0354fac280d769bc9a1bb6133b
};

/**
 * process_playback_routing_us144mkii() - Apply playback routing matrix
 * @tascam: The driver instance.
 * @src_buffer: Buffer containing 4 channels of S24_3LE audio from ALSA.
 * @dst_buffer: Buffer to be filled for the USB device.
 * @frames: Number of frames to process.
 */
void process_playback_routing_us144mkii(struct tascam_card *tascam,
<<<<<<< HEAD
                                        const u8 *src_buffer, u8 *dst_buffer,
                                        size_t frames) {
  size_t f;
  const u8 *src_12, *src_34;
  u8 *dst_line, *dst_digital;
=======
					const u8 *src_buffer, u8 *dst_buffer,
					size_t frames)
{
	size_t f;
	const u8 *src_12, *src_34;
	u8 *dst_line, *dst_digital;
>>>>>>> f44b75094c078b0354fac280d769bc9a1bb6133b

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
void process_capture_routing_us144mkii(struct tascam_card *tascam,
<<<<<<< HEAD
                                       const s32 *decoded_block,
                                       s32 *routed_block) {
  int f;
  const s32 *src_frame;
  s32 *dst_frame;
=======
				       const s32 *decoded_block,
				       s32 *routed_block)
{
	int f;
	const s32 *src_frame;
	s32 *dst_frame;
>>>>>>> f44b75094c078b0354fac280d769bc9a1bb6133b

  for (f = 0; f < FRAMES_PER_DECODE_BLOCK; f++) {
    src_frame = decoded_block + (f * DECODED_CHANNELS_PER_FRAME);
    dst_frame = routed_block + (f * DECODED_CHANNELS_PER_FRAME);

    /* ch1 and ch2 Source */
    if (tascam->capture_12_source == 0) { /* analog inputs */
      dst_frame[0] = src_frame[0];        /* Analog L */
      dst_frame[1] = src_frame[1];        /* Analog R */
    } else {                              /* digital inputs */
      dst_frame[0] = src_frame[2];        /* Digital L */
      dst_frame[1] = src_frame[3];        /* Digital R */
    }

    /* ch3 and ch4 Source */
    if (tascam->capture_34_source == 0) { /* analog inputs */
      dst_frame[2] = src_frame[0];        /* Analog L (Duplicate) */
      dst_frame[3] = src_frame[1];        /* Analog R (Duplicate) */
    } else {                              /* digital inputs */
      dst_frame[2] = src_frame[2];        /* Digital L */
      dst_frame[3] = src_frame[3];        /* Digital R */
    }
  }
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
int us144mkii_configure_device_for_rate(struct tascam_card *tascam, int rate) {
  struct usb_device *dev = tascam->dev;
  u8 *rate_payload_buf;
  u16 rate_vendor_wValue;
  int err = 0;
  const u8 *current_payload_src;

<<<<<<< HEAD
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
=======
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
>>>>>>> f44b75094c078b0354fac280d769bc9a1bb6133b

  rate_payload_buf = kmemdup(current_payload_src, 3, GFP_KERNEL);
  if (!rate_payload_buf)
    return -ENOMEM;

  dev_info(&dev->dev, "Configuring device for %d Hz\n", rate);

<<<<<<< HEAD
  err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_MODE_CONTROL,
                        RT_H2D_VENDOR_DEV, MODE_VAL_CONFIG, 0x0000, NULL, 0,
                        USB_CTRL_TIMEOUT_MS);
  if (err < 0)
    goto fail;
  err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), UAC_SET_CUR,
                        RT_H2D_CLASS_EP, UAC_SAMPLING_FREQ_CONTROL, EP_AUDIO_IN,
                        rate_payload_buf, 3, USB_CTRL_TIMEOUT_MS);
  if (err < 0)
    goto fail;
  err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), UAC_SET_CUR,
                        RT_H2D_CLASS_EP, UAC_SAMPLING_FREQ_CONTROL,
                        EP_AUDIO_OUT, rate_payload_buf, 3, USB_CTRL_TIMEOUT_MS);
  if (err < 0)
    goto fail;
  err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE,
                        RT_H2D_VENDOR_DEV, REG_ADDR_UNKNOWN_0D, REG_VAL_ENABLE,
                        NULL, 0, USB_CTRL_TIMEOUT_MS);
  if (err < 0)
    goto fail;
  err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE,
                        RT_H2D_VENDOR_DEV, REG_ADDR_UNKNOWN_0E, REG_VAL_ENABLE,
                        NULL, 0, USB_CTRL_TIMEOUT_MS);
  if (err < 0)
    goto fail;
  err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE,
                        RT_H2D_VENDOR_DEV, REG_ADDR_UNKNOWN_0F, REG_VAL_ENABLE,
                        NULL, 0, USB_CTRL_TIMEOUT_MS);
  if (err < 0)
    goto fail;
  err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE,
                        RT_H2D_VENDOR_DEV, rate_vendor_wValue, REG_VAL_ENABLE,
                        NULL, 0, USB_CTRL_TIMEOUT_MS);
  if (err < 0)
    goto fail;
  err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE,
                        RT_H2D_VENDOR_DEV, REG_ADDR_UNKNOWN_11, REG_VAL_ENABLE,
                        NULL, 0, USB_CTRL_TIMEOUT_MS);
  if (err < 0)
    goto fail;
  err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_MODE_CONTROL,
                        RT_H2D_VENDOR_DEV, MODE_VAL_STREAM_START, 0x0000, NULL,
                        0, USB_CTRL_TIMEOUT_MS);
  if (err < 0)
    goto fail;
=======
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
>>>>>>> f44b75094c078b0354fac280d769bc9a1bb6133b

  kfree(rate_payload_buf);
  return 0;

fail:
<<<<<<< HEAD
  dev_err(&dev->dev, "Device configuration failed at rate %d with error %d\n",
          rate, err);
  kfree(rate_payload_buf);
  return err;
=======
	dev_err(&dev->dev,
		"Device configuration failed at rate %d with error %d\n", rate,
		err);
	kfree(rate_payload_buf);
	return err;
>>>>>>> f44b75094c078b0354fac280d769bc9a1bb6133b
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
int tascam_pcm_hw_params(struct snd_pcm_substream *substream,
<<<<<<< HEAD
                         struct snd_pcm_hw_params *params) {
  struct tascam_card *tascam = snd_pcm_substream_chip(substream);
  int err;
  unsigned int rate = params_rate(params);
=======
			 struct snd_pcm_hw_params *params)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	int err;
	unsigned int rate = params_rate(params);
>>>>>>> f44b75094c078b0354fac280d769bc9a1bb6133b

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
int tascam_pcm_hw_free(struct snd_pcm_substream *substream) {
  return snd_pcm_lib_free_pages(substream);
}

/**
 * tascam_pcm_trigger() - Triggers the start or stop of PCM streams.
 * @substream: The ALSA PCM substream.
 * @cmd: The trigger command (e.g., SNDRV_PCM_TRIGGER_START,
 * SNDRV_PCM_TRIGGER_STOP).
 *
 * This function handles starting and stopping of playback and capture streams
 * by submitting or killing the associated URBs. It ensures that both streams
 * are started/stopped together.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int tascam_pcm_trigger(struct snd_pcm_substream *substream, int cmd) {
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

<<<<<<< HEAD
  if (do_start) {
    if (atomic_read(&tascam->active_urbs) > 0) {
      dev_WARN(tascam->card->dev, "Cannot start, URBs still active.\n");
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
    dev_err(tascam->card->dev, "Failed to submit URBs to start stream: %d\n",
            err);
    do_stop = true;
  }
=======
	if (do_start) {
		if (atomic_read(&tascam->active_urbs) > 0) {
			dev_WARN(tascam->card->dev,
				 "Cannot start, URBs still active.\n");
			return -EAGAIN;
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
				goto start_rollback;
			}
			atomic_inc(&tascam->active_urbs);
		}
		for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
			usb_get_urb(tascam->playback_urbs[i]);
			usb_anchor_urb(tascam->playback_urbs[i],
				       &tascam->playback_anchor);
			err = usb_submit_urb(tascam->playback_urbs[i],
					     GFP_ATOMIC);
			if (err < 0) {
				usb_unanchor_urb(tascam->playback_urbs[i]);
				usb_put_urb(tascam->playback_urbs[i]);
				goto start_rollback;
			}
			atomic_inc(&tascam->active_urbs);
		}
		for (i = 0; i < NUM_CAPTURE_URBS; i++) {
			usb_get_urb(tascam->capture_urbs[i]);
			usb_anchor_urb(tascam->capture_urbs[i],
				       &tascam->capture_anchor);
			err = usb_submit_urb(tascam->capture_urbs[i],
					     GFP_ATOMIC);
			if (err < 0) {
				usb_unanchor_urb(tascam->capture_urbs[i]);
				usb_put_urb(tascam->capture_urbs[i]);
				goto start_rollback;
			}
			atomic_inc(&tascam->active_urbs);
		}

		return 0;
start_rollback:
		dev_err(tascam->card->dev,
			"Failed to submit URBs to start stream: %d\n", err);
		do_stop = true;
	}
>>>>>>> f44b75094c078b0354fac280d769bc9a1bb6133b

  if (do_stop)
    schedule_work(&tascam->stop_work);

  return err;
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
int tascam_init_pcm(struct snd_pcm *pcm) {
  struct tascam_card *tascam = pcm->private_data;

<<<<<<< HEAD
  snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &tascam_playback_ops);
  snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &tascam_capture_ops);
  snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_CONTINUOUS,
                                        tascam->dev->dev.parent, 64 * 1024,
                                        tascam_pcm_hw.buffer_bytes_max);
=======
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &tascam_playback_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &tascam_capture_ops);
	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_CONTINUOUS,
					      tascam->dev->dev.parent,
					      64 * 1024,
					      tascam_pcm_hw.buffer_bytes_max);
>>>>>>> f44b75094c078b0354fac280d769bc9a1bb6133b

  INIT_WORK(&tascam->capture_work, tascam_capture_work_handler);

  return 0;
}
