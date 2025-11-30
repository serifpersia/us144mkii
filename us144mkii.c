// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2025 Šerif Rami <ramiserifpersia@gmail.com>

#include "us144mkii.h"

MODULE_AUTHOR("Šerif Rami <ramiserifpersia@gmail.com>");
MODULE_DESCRIPTION("ALSA Driver for TASCAM US-144MKII");
MODULE_LICENSE("GPL");

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = { 1, [1 ...(SNDRV_CARDS - 1)] = 0 };
static int dev_idx;

static int tascam_probe(struct usb_interface *intf,
						const struct usb_device_id *usb_id);
static void tascam_disconnect(struct usb_interface *intf);
static int tascam_suspend(struct usb_interface *intf, pm_message_t message);
static int tascam_resume(struct usb_interface *intf);

void tascam_free_urbs(struct tascam_card *tascam)
{
	int i;

	usb_kill_anchored_urbs(&tascam->playback_anchor);
	usb_kill_anchored_urbs(&tascam->feedback_anchor);
	usb_kill_anchored_urbs(&tascam->capture_anchor);
	usb_kill_anchored_urbs(&tascam->midi_anchor);

	for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
		if (tascam->playback_urbs[i]) {
			usb_free_coherent(tascam->dev, tascam->playback_urb_alloc_size,
							  tascam->playback_urbs[i]->transfer_buffer,
					 tascam->playback_urbs[i]->transfer_dma);
			usb_free_urb(tascam->playback_urbs[i]);
			tascam->playback_urbs[i] = NULL;
		}
	}

	for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
		if (tascam->feedback_urbs[i]) {
			usb_free_coherent(tascam->dev, tascam->feedback_urb_alloc_size,
							  tascam->feedback_urbs[i]->transfer_buffer,
					 tascam->feedback_urbs[i]->transfer_dma);
			usb_free_urb(tascam->feedback_urbs[i]);
			tascam->feedback_urbs[i] = NULL;
		}
	}

	for (i = 0; i < NUM_CAPTURE_URBS; i++) {
		if (tascam->capture_urbs[i]) {
			usb_free_coherent(tascam->dev, CAPTURE_PACKET_SIZE,
							  tascam->capture_urbs[i]->transfer_buffer,
					 tascam->capture_urbs[i]->transfer_dma);
			usb_free_urb(tascam->capture_urbs[i]);
			tascam->capture_urbs[i] = NULL;
		}
	}

	if (tascam->midi_out_urb) {
		usb_free_coherent(tascam->dev, MIDI_PACKET_SIZE,
						  tascam->midi_out_buf,
					tascam->midi_out_urb->transfer_dma);
		usb_free_urb(tascam->midi_out_urb);
		tascam->midi_out_urb = NULL;
	}
	if (tascam->midi_in_urb) {
		usb_free_coherent(tascam->dev, MIDI_PACKET_SIZE,
						  tascam->midi_in_buf,
					tascam->midi_in_urb->transfer_dma);
		usb_free_urb(tascam->midi_in_urb);
		tascam->midi_in_urb = NULL;
	}
}

int tascam_alloc_urbs(struct tascam_card *tascam)
{
	int i;

	tascam->playback_urb_alloc_size = PLAYBACK_URB_PACKETS * (12 + 2) * BYTES_PER_FRAME;
	for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
		struct urb *urb = usb_alloc_urb(PLAYBACK_URB_PACKETS, GFP_KERNEL);
		if (!urb) return -ENOMEM;
		tascam->playback_urbs[i] = urb;
		urb->transfer_buffer = usb_alloc_coherent(tascam->dev,
												  tascam->playback_urb_alloc_size,
											GFP_KERNEL, &urb->transfer_dma);
		if (!urb->transfer_buffer) return -ENOMEM;
		urb->dev = tascam->dev;
		urb->pipe = usb_sndisocpipe(tascam->dev, EP_AUDIO_OUT);
		urb->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
		urb->interval = 1;
		urb->context = tascam;
		urb->complete = playback_urb_complete;
	}

	tascam->feedback_urb_alloc_size = FEEDBACK_URB_PACKETS * FEEDBACK_PACKET_SIZE;
	for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
		struct urb *urb = usb_alloc_urb(FEEDBACK_URB_PACKETS, GFP_KERNEL);
		if (!urb) return -ENOMEM;
		tascam->feedback_urbs[i] = urb;
		urb->transfer_buffer = usb_alloc_coherent(tascam->dev,
												  tascam->feedback_urb_alloc_size,
												  GFP_KERNEL, &urb->transfer_dma);
		if (!urb->transfer_buffer) return -ENOMEM;
		urb->dev = tascam->dev;
		urb->pipe = usb_rcvisocpipe(tascam->dev, EP_PLAYBACK_FEEDBACK);
		urb->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
		urb->interval = 4;
		urb->context = tascam;
		urb->complete = feedback_urb_complete;
	}

	for (i = 0; i < NUM_CAPTURE_URBS; i++) {
		struct urb *urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!urb) return -ENOMEM;
		tascam->capture_urbs[i] = urb;
		void *buf = usb_alloc_coherent(tascam->dev, CAPTURE_PACKET_SIZE,
									   GFP_KERNEL, &urb->transfer_dma);
		if (!buf) return -ENOMEM;
		usb_fill_bulk_urb(urb, tascam->dev,
						  usb_rcvbulkpipe(tascam->dev, EP_AUDIO_IN),
						  buf, CAPTURE_PACKET_SIZE,
					capture_urb_complete, tascam);
		urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	}

	return 0;
}

void tascam_stop_work_handler(struct work_struct *work)
{
	struct tascam_card *tascam = container_of(work, struct tascam_card, stop_work);

	usb_kill_anchored_urbs(&tascam->playback_anchor);
	usb_kill_anchored_urbs(&tascam->feedback_anchor);
	usb_kill_anchored_urbs(&tascam->capture_anchor);
	usb_kill_anchored_urbs(&tascam->midi_anchor);
	atomic_set(&tascam->active_urbs, 0);
}

static void tascam_card_private_free(struct snd_card *card)
{
	struct tascam_card *tascam = card->private_data;

	if (tascam) {
		tascam_free_urbs(tascam);
		if (tascam->dev) {
			usb_put_dev(tascam->dev);
			tascam->dev = NULL;
		}
	}
}

static int tascam_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct tascam_card *tascam = usb_get_intfdata(intf);
	if (!tascam) return 0;

	snd_pcm_suspend_all(tascam->pcm);
	cancel_work_sync(&tascam->stop_work);
	cancel_work_sync(&tascam->stop_pcm_work);

	usb_kill_anchored_urbs(&tascam->playback_anchor);
	usb_kill_anchored_urbs(&tascam->feedback_anchor);
	usb_kill_anchored_urbs(&tascam->capture_anchor);
	usb_kill_anchored_urbs(&tascam->midi_anchor);

	usb_control_msg(tascam->dev, usb_sndctrlpipe(tascam->dev, 0),
					VENDOR_REQ_DEEP_SLEEP, RT_H2D_VENDOR_DEV,
				 0x0000, 0x0000, NULL, 0, USB_CTRL_TIMEOUT_MS);
	return 0;
}

static int tascam_resume(struct usb_interface *intf)
{
	struct tascam_card *tascam = usb_get_intfdata(intf);
	int err;
	if (!tascam) return 0;

	err = usb_set_interface(tascam->dev, 0, 1);
	if (err < 0) return err;
	err = usb_set_interface(tascam->dev, 1, 1);
	if (err < 0) return err;

	if (tascam->current_rate > 0)
		us144mkii_configure_device_for_rate(tascam, tascam->current_rate);

	return 0;
}

static int tascam_probe(struct usb_interface *intf, const struct usb_device_id *usb_id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct snd_card *card;
	struct tascam_card *tascam;
	int err;
	char *handshake_buf __free(kfree) = NULL;

	if (intf->cur_altsetting->desc.bInterfaceNumber == 1)
		return 0;

	if (dev_idx >= SNDRV_CARDS) return -ENODEV;
	if (!enable[dev_idx]) return -ENOENT;

	handshake_buf = kmalloc(1, GFP_KERNEL);
	if (!handshake_buf) return -ENOMEM;

	err = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0),
						  VENDOR_REQ_MODE_CONTROL, RT_D2H_VENDOR_DEV,
					   MODE_VAL_HANDSHAKE_READ, 0x0000, handshake_buf, 1,
					   USB_CTRL_TIMEOUT_MS);
	if (err < 0) {
		dev_err(&dev->dev, "Handshake failed: %d\n", err);
		return err;
	}

	usb_set_interface(dev, 0, 1);
	usb_set_interface(dev, 1, 1);

	err = snd_card_new(&dev->dev, index[dev_idx], id[dev_idx], THIS_MODULE,
					   sizeof(struct tascam_card), &card);
	if (err < 0) return err;

	tascam = card->private_data;
	card->private_free = tascam_card_private_free;
	tascam->dev = usb_get_dev(dev);
	tascam->card = card;
	tascam->iface0 = intf;

	spin_lock_init(&tascam->lock);
	init_usb_anchor(&tascam->playback_anchor);
	init_usb_anchor(&tascam->feedback_anchor);
	init_usb_anchor(&tascam->capture_anchor);
	init_usb_anchor(&tascam->midi_anchor);

	INIT_WORK(&tascam->stop_work, tascam_stop_work_handler);
	INIT_WORK(&tascam->stop_pcm_work, tascam_stop_pcm_work_handler);

	strscpy(card->driver, DRIVER_NAME, sizeof(card->driver));
	if (le16_to_cpu(dev->descriptor.idProduct) == USB_PID_TASCAM_US144)
		strscpy(card->shortname, "US-144", sizeof(card->shortname));
	else
		strscpy(card->shortname, "US-144MKII", sizeof(card->shortname));

	snprintf(card->longname, sizeof(card->longname), "%s (%04x:%04x) at %s",
			 card->shortname, USB_VID_TASCAM, dev->descriptor.idProduct,
		  dev_name(&dev->dev));

	err = snd_pcm_new(card, "US144MKII PCM", 0, 1, 1, &tascam->pcm);
	if (err < 0) goto free_card;
	tascam->pcm->private_data = tascam;
	strscpy(tascam->pcm->name, "US144MKII PCM", sizeof(tascam->pcm->name));
	snd_pcm_set_ops(tascam->pcm, SNDRV_PCM_STREAM_PLAYBACK, &tascam_playback_ops);
	snd_pcm_set_ops(tascam->pcm, SNDRV_PCM_STREAM_CAPTURE, &tascam_capture_ops);

	snd_pcm_set_managed_buffer_all(tascam->pcm, SNDRV_DMA_TYPE_VMALLOC,
								   NULL, 0, 0);

	err = tascam_create_midi(tascam);
	if (err < 0) goto free_card;

	err = tascam_alloc_urbs(tascam);
	if (err < 0) goto free_card;

	if (us144mkii_configure_device_for_rate(tascam, 48000) < 0)
		dev_warn(&dev->dev, "Failed to initialize device at 48khz\n");
	else
		tascam->current_rate = 48000;

	err = snd_card_register(card);
	if (err < 0) goto free_card;

	usb_set_intfdata(intf, tascam);
	dev_idx++;
	return 0;

	free_card:
	snd_card_free(card);
	return err;
}

static void tascam_disconnect(struct usb_interface *intf)
{
	struct tascam_card *tascam = usb_get_intfdata(intf);
	if (!tascam) return;

	if (intf->cur_altsetting->desc.bInterfaceNumber == 0) {
		snd_card_disconnect(tascam->card);
		cancel_work_sync(&tascam->stop_work);
		cancel_work_sync(&tascam->stop_pcm_work);
		snd_card_free(tascam->card);
		dev_idx--;
	}
}

static const struct usb_device_id tascam_usb_ids[] = {
	{ USB_DEVICE(USB_VID_TASCAM, USB_PID_TASCAM_US144) },
	{ USB_DEVICE(USB_VID_TASCAM, USB_PID_TASCAM_US144MKII) },
	{ }
};
MODULE_DEVICE_TABLE(usb, tascam_usb_ids);

static struct usb_driver tascam_alsa_driver = {
	.name = DRIVER_NAME,
	.probe = tascam_probe,
	.disconnect = tascam_disconnect,
	.suspend = tascam_suspend,
	.resume = tascam_resume,
	.id_table = tascam_usb_ids,
};

module_usb_driver(tascam_alsa_driver);
