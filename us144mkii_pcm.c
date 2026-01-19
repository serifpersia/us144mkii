// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2025 Šerif Rami <ramiserifpersia@gmail.com>

#include "us144mkii_pcm.h"

struct rate_config {
	int rate;
	u8 data[3];
	u16 reg;
};

static const struct rate_config rate_map[] = {
	{ 44100, {0x44, 0xac, 0x00}, 0x1000 },
	{ 48000, {0x80, 0xbb, 0x00}, 0x1002 },
	{ 88200, {0x88, 0x58, 0x01}, 0x1008 },
	{ 96000, {0x00, 0x77, 0x01}, 0x100a },
};

static int tascam_write_regs(struct tascam_card *tascam, const u16 *regs, size_t count)
{
	int i, err = 0;
	struct usb_device *dev = tascam->dev;

	for (i = 0; i < count; i++) {
		err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
							  VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV,
						regs[i], REG_VAL_ENABLE, NULL, 0, USB_CTRL_TIMEOUT_MS);
		if (err < 0)
			return err;
	}
	return 0;
}

/**
 * us144mkii_configure_device_for_rate() - set sample rate via USB control msgs
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
	int i, err;
	const struct rate_config *cfg = NULL;
	u8 *payload;

	for (i = 0; i < ARRAY_SIZE(rate_map); i++) {
		if (rate_map[i].rate == rate) {
			cfg = &rate_map[i];
			break;
		}
	}
	if (!cfg)
		return -EINVAL;

	payload = kmemdup(cfg->data, 3, GFP_KERNEL);
	if (!payload)
		return -ENOMEM;

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_MODE_CONTROL,
						  RT_H2D_VENDOR_DEV, MODE_VAL_CONFIG, 0x0000, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0)
		goto out;

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), UAC_SET_CUR,
						  RT_H2D_CLASS_EP, UAC_SAMPLING_FREQ_CONTROL,
					   EP_AUDIO_IN, payload, 3, USB_CTRL_TIMEOUT_MS);
	if (err < 0)
		goto out;

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), UAC_SET_CUR,
						  RT_H2D_CLASS_EP, UAC_SAMPLING_FREQ_CONTROL,
					   EP_AUDIO_OUT, payload, 3, USB_CTRL_TIMEOUT_MS);
	if (err < 0)
		goto out;

	{
		const u16 regs_to_write[] = {
			0x0d04, 0x0e00, 0x0f00, cfg->reg, 0x110b
		};
		err = tascam_write_regs(tascam, regs_to_write, ARRAY_SIZE(regs_to_write));
		if (err < 0)
			goto out;
	}

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_MODE_CONTROL,
						  RT_H2D_VENDOR_DEV, MODE_VAL_STREAM_START, 0x0000, NULL, 0, USB_CTRL_TIMEOUT_MS);

	out:
	kfree(payload);
	return err;
}

/**
 * tascam_pcm_hw_params() - configure hardware parameters for PCM streams
 * @substream: the ALSA PCM substream
 * @params: the hardware parameters to apply
 *
 * This function allocates pages for the PCM buffer and configures the
 * device hardware for the selected sample rate if it has changed.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int tascam_pcm_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	unsigned int rate = params_rate(params);
	unsigned long flags;
	int err;

	spin_lock_irqsave(&tascam->lock, flags);
	if (tascam->current_rate == rate) {
		spin_unlock_irqrestore(&tascam->lock, flags);
		return 0;
	}

	if (atomic_read(&tascam->playback_active) || atomic_read(&tascam->capture_active)) {
		spin_unlock_irqrestore(&tascam->lock, flags);
		return -EBUSY;
	}
	spin_unlock_irqrestore(&tascam->lock, flags);

	usb_kill_anchored_urbs(&tascam->playback_anchor);
	usb_kill_anchored_urbs(&tascam->feedback_anchor);
	usb_kill_anchored_urbs(&tascam->capture_anchor);
	atomic_set(&tascam->active_urbs, 0);

	err = us144mkii_configure_device_for_rate(tascam, rate);
	if (err < 0) {
		spin_lock_irqsave(&tascam->lock, flags);
		tascam->current_rate = 0;
		spin_unlock_irqrestore(&tascam->lock, flags);
		return err;
	}

	spin_lock_irqsave(&tascam->lock, flags);
	tascam->current_rate = rate;
	spin_unlock_irqrestore(&tascam->lock, flags);

	return 0;
}

/**
 * tascam_stop_pcm_work_handler() - work handler to stop PCM streams
 * @work: pointer to the work_struct
 */
void tascam_stop_pcm_work_handler(struct work_struct *work)
{
	struct tascam_card *tascam = container_of(work, struct tascam_card, stop_pcm_work);

	if (tascam->dev && tascam->playback_substream)
		snd_pcm_stop(tascam->playback_substream, SNDRV_PCM_STATE_XRUN);
}
