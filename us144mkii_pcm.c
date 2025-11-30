// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2025 Šerif Rami <ramiserifpersia@gmail.com>

#include "us144mkii_pcm.h"

static int tascam_write_regs(struct tascam_card *tascam, const u16 *regs, size_t count)
{
	int i, err = 0;
	struct usb_device *dev = tascam->dev;
	for (i = 0; i < count; i++) {
		err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
							  VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV,
							  regs[i], REG_VAL_ENABLE, NULL, 0, USB_CTRL_TIMEOUT_MS);
		if (err < 0) return err;
	}
	return 0;
}

int us144mkii_configure_device_for_rate(struct tascam_card *tascam, int rate)
{
	struct usb_device *dev = tascam->dev;
	u8 *rate_payload_buf;
	int err = 0;
	const u8 *current_payload_src;
	u16 rate_reg;

	static const u8 payload_44100[] = { 0x44, 0xac, 0x00 };
	static const u8 payload_48000[] = { 0x80, 0xbb, 0x00 };
	static const u8 payload_88200[] = { 0x88, 0x58, 0x01 };
	static const u8 payload_96000[] = { 0x00, 0x77, 0x01 };

	switch (rate) {
		case 44100: current_payload_src = payload_44100; rate_reg = REG_ADDR_RATE_44100; break;
		case 48000: current_payload_src = payload_48000; rate_reg = REG_ADDR_RATE_48000; break;
		case 88200: current_payload_src = payload_88200; rate_reg = REG_ADDR_RATE_88200; break;
		case 96000: current_payload_src = payload_96000; rate_reg = REG_ADDR_RATE_96000; break;
		default: return -EINVAL;
	}

	rate_payload_buf = kmemdup(current_payload_src, 3, GFP_KERNEL);
	if (!rate_payload_buf) return -ENOMEM;

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
						  VENDOR_REQ_MODE_CONTROL, RT_H2D_VENDOR_DEV,
					   MODE_VAL_CONFIG, 0x0000, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) goto fail;

	usb_control_msg(dev, usb_sndctrlpipe(dev, 0), UAC_SET_CUR,
					RT_H2D_CLASS_EP, UAC_SAMPLING_FREQ_CONTROL,
				 EP_AUDIO_IN, rate_payload_buf, 3, USB_CTRL_TIMEOUT_MS);
	usb_control_msg(dev, usb_sndctrlpipe(dev, 0), UAC_SET_CUR,
					RT_H2D_CLASS_EP, UAC_SAMPLING_FREQ_CONTROL,
				 EP_AUDIO_OUT, rate_payload_buf, 3, USB_CTRL_TIMEOUT_MS);

	{
		const u16 regs_to_write[] = {
			REG_ADDR_UNKNOWN_0D, REG_ADDR_UNKNOWN_0E,
			REG_ADDR_UNKNOWN_0F, rate_reg, REG_ADDR_UNKNOWN_11
		};
		err = tascam_write_regs(tascam, regs_to_write, ARRAY_SIZE(regs_to_write));
		if (err < 0) goto fail;
	}

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
						  VENDOR_REQ_MODE_CONTROL, RT_H2D_VENDOR_DEV,
					   MODE_VAL_STREAM_START, 0x0000, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) goto fail;

	kfree(rate_payload_buf);
	return 0;
	fail:
	kfree(rate_payload_buf);
	return err;
}

int tascam_pcm_hw_params(struct snd_pcm_substream *substream,
						 struct snd_pcm_hw_params *params)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	unsigned int rate = params_rate(params);
	int err;

	if (tascam->current_rate != rate) {

		usb_kill_anchored_urbs(&tascam->playback_anchor);
		usb_kill_anchored_urbs(&tascam->feedback_anchor);
		usb_kill_anchored_urbs(&tascam->capture_anchor);

		atomic_set(&tascam->active_urbs, 0);

		err = us144mkii_configure_device_for_rate(tascam, rate);
		if (err < 0) {
			tascam->current_rate = 0;
			return err;
		}
		tascam->current_rate = rate;
	}
	return 0;
}

void tascam_stop_pcm_work_handler(struct work_struct *work)
{
	struct tascam_card *tascam = container_of(work, struct tascam_card, stop_pcm_work);
	if (tascam->playback_substream)
		snd_pcm_stop(tascam->playback_substream, SNDRV_PCM_STATE_XRUN);
}
