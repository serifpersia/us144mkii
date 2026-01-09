// SPDX-License-Identifier: GPL-2.0-only
/*
 * ALSA Driver for TASCAM US-122MKII
 *
 * Copyright (c) 2026 Šerif Rami
 */

#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/initval.h>

MODULE_AUTHOR("Serif Rami");
MODULE_DESCRIPTION("TASCAM US-122MKII Driver");
MODULE_LICENSE("GPL");

#define USB_VID_TASCAM          0x0644
#define USB_PID_US122MKII       0x8021

#define EP_CAPTURE              0x81
#define EP_PLAYBACK             0x02

#define BYTES_PER_FRAME         6
#define MAX_PACKET_SIZE         78
#define PACKET_SIZE_BASE        36

#define NUM_URBS                4
#define ISO_PACKETS_PER_URB     8

#define RT_H2D_CLASS_EP         (USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_ENDPOINT)
#define UAC_SET_CUR             0x01
#define UAC_SAMPLING_FREQ_CONTROL 0x0100

struct tascam_us122mkii {
	struct usb_device *dev;
	struct snd_card *card;
	struct snd_pcm *pcm;

	struct snd_pcm_substream *capture_substream;
	struct snd_pcm_substream *playback_substream;

	struct urb *pump_urbs[NUM_URBS];
	struct urb *capture_urbs[NUM_URBS];

	struct usb_anchor pump_anchor;
	struct usb_anchor capture_anchor;

	spinlock_t lock;

	int usb_stream_running;
	int capture_is_running;
	int playback_is_running;

	unsigned int current_rate;
	unsigned int accum;

	snd_pcm_uframes_t capture_hw_ptr;
	snd_pcm_uframes_t playback_hw_ptr;
};

static int tascam_set_rate(struct tascam_us122mkii *tascam, unsigned int rate)
{
	int err;
	u8 payload[3];

	if (tascam->usb_stream_running) {
		if (tascam->current_rate == rate) return 0;
		printk(KERN_WARNING "TASCAM: Attempt to change rate to %d while running! Ignored.\n", rate);
		return -EBUSY;
	}

	switch (rate) {
		case 44100:
			payload[0] = 0x44; payload[1] = 0xAC; payload[2] = 0x00;
			break;
		case 48000:
			payload[0] = 0x80; payload[1] = 0xBB; payload[2] = 0x00;
			break;
		case 88200:
			payload[0] = 0x88; payload[1] = 0x58; payload[2] = 0x01;
			break;
		case 96000:
			payload[0] = 0x00; payload[1] = 0x77; payload[2] = 0x01;
			break;
		default:
			return -EINVAL;
	}

	err = usb_control_msg(tascam->dev, usb_sndctrlpipe(tascam->dev, 0),
						  UAC_SET_CUR, RT_H2D_CLASS_EP, UAC_SAMPLING_FREQ_CONTROL,
						  EP_CAPTURE, payload, 3, 1000);

	if (err < 0) {
		printk(KERN_ERR "TASCAM: Failed to set rate %d (Err %d)\n", rate, err);
		return err;
	}

	printk(KERN_INFO "TASCAM: Rate set to %d Hz\n", rate);
	tascam->current_rate = rate;
	tascam->accum = 0;
	return 0;
}

static void playback_complete(struct urb *urb)
{
	struct tascam_us122mkii *tascam = urb->context;
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	unsigned long flags;
	int i, f;
	u8 *dst;
	u32 *src;
	unsigned int frames, bytes;

	if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
		urb->status == -ESHUTDOWN)
		return;

	spin_lock_irqsave(&tascam->lock, flags);

	if (!tascam->usb_stream_running) {
		spin_unlock_irqrestore(&tascam->lock, flags);
		return;
	}

	substream = tascam->playback_substream;
	runtime = (substream) ? substream->runtime : NULL;

	for (i = 0; i < ISO_PACKETS_PER_URB; i++) {

		tascam->accum += tascam->current_rate;
		frames = tascam->accum / 8000;
		tascam->accum %= 8000;

		bytes = frames * BYTES_PER_FRAME;

		urb->iso_frame_desc[i].offset = i * MAX_PACKET_SIZE;
		urb->iso_frame_desc[i].length = bytes;
		urb->iso_frame_desc[i].status = 0;

		dst = urb->transfer_buffer + urb->iso_frame_desc[i].offset;

		memset(dst, 0, bytes);

		if (tascam->playback_is_running && runtime) {
			int pos_bytes = frames_to_bytes(runtime, tascam->playback_hw_ptr);
			int chunk_bytes = frames_to_bytes(runtime, frames);

			src = (u32 *)(runtime->dma_area + pos_bytes);

			if (pos_bytes + chunk_bytes > frames_to_bytes(runtime, runtime->buffer_size))
				chunk_bytes = frames_to_bytes(runtime, runtime->buffer_size) - pos_bytes;

			int frames1 = bytes_to_frames(runtime, chunk_bytes);
			for (f = 0; f < frames1; f++) {
				dst[0] = (src[0] >> 8); dst[1] = (src[0] >> 16); dst[2] = (src[0] >> 24);
				dst[3] = (src[1] >> 8); dst[4] = (src[1] >> 16); dst[5] = (src[1] >> 24);
				dst += BYTES_PER_FRAME; src += 2;
			}

			if (chunk_bytes < frames_to_bytes(runtime, frames)) {
				src = (u32 *)runtime->dma_area;
				int frames2 = frames - frames1;
				for (f = 0; f < frames2; f++) {
					dst[0] = (src[0] >> 8); dst[1] = (src[0] >> 16); dst[2] = (src[0] >> 24);
					dst[3] = (src[1] >> 8); dst[4] = (src[1] >> 16); dst[5] = (src[1] >> 24);
					dst += BYTES_PER_FRAME; src += 2;
				}
			}

			tascam->playback_hw_ptr += frames;
			if (tascam->playback_hw_ptr >= runtime->buffer_size)
				tascam->playback_hw_ptr -= runtime->buffer_size;
		}
	}

	spin_unlock_irqrestore(&tascam->lock, flags);

	if (tascam->playback_is_running && substream)
		snd_pcm_period_elapsed(substream);

	usb_submit_urb(urb, GFP_ATOMIC);
}

static void capture_complete(struct urb *urb)
{
	struct tascam_us122mkii *tascam = urb->context;
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	unsigned long flags;
	int i, f;
	u8 *src;
	u32 *dst;

	if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
		urb->status == -ESHUTDOWN)
		return;

	spin_lock_irqsave(&tascam->lock, flags);

	if (!tascam->usb_stream_running) {
		spin_unlock_irqrestore(&tascam->lock, flags);
		return;
	}

	substream = tascam->capture_substream;

	if (tascam->capture_is_running && substream) {
		runtime = substream->runtime;

		for (i = 0; i < ISO_PACKETS_PER_URB; i++) {
			int len = urb->iso_frame_desc[i].actual_length;
			if (len < BYTES_PER_FRAME) continue;

			int frames = len / BYTES_PER_FRAME;
			int bytes = frames_to_bytes(runtime, frames);
			int pos_bytes = frames_to_bytes(runtime, tascam->capture_hw_ptr);

			src = urb->transfer_buffer + urb->iso_frame_desc[i].offset;
			dst = (u32 *)(runtime->dma_area + pos_bytes);

			int chunk = bytes;
			if (pos_bytes + chunk > frames_to_bytes(runtime, runtime->buffer_size))
				chunk = frames_to_bytes(runtime, runtime->buffer_size) - pos_bytes;

			int frames1 = bytes_to_frames(runtime, chunk);
			for (f = 0; f < frames1; f++) {
				dst[0] = (src[2] << 24) | (src[1] << 16) | (src[0] << 8);
				dst[1] = (src[5] << 24) | (src[4] << 16) | (src[3] << 8);
				src += BYTES_PER_FRAME; dst += 2;
			}

			if (chunk < bytes) {
				dst = (u32 *)runtime->dma_area;
				int frames2 = frames - frames1;
				for (f = 0; f < frames2; f++) {
					dst[0] = (src[2] << 24) | (src[1] << 16) | (src[0] << 8);
					dst[1] = (src[5] << 24) | (src[4] << 16) | (src[3] << 8);
					src += BYTES_PER_FRAME; dst += 2;
				}
			}

			tascam->capture_hw_ptr += frames;
			if (tascam->capture_hw_ptr >= runtime->buffer_size)
				tascam->capture_hw_ptr -= runtime->buffer_size;
		}
	}

	for (i = 0; i < ISO_PACKETS_PER_URB; i++) {
		urb->iso_frame_desc[i].offset = i * MAX_PACKET_SIZE;
		urb->iso_frame_desc[i].length = MAX_PACKET_SIZE;
		urb->iso_frame_desc[i].status = 0;
	}

	spin_unlock_irqrestore(&tascam->lock, flags);

	if (tascam->capture_is_running && substream)
		snd_pcm_period_elapsed(substream);

	usb_submit_urb(urb, GFP_ATOMIC);
}

static int pcm_open(struct snd_pcm_substream *substream)
{
	struct tascam_us122mkii *tascam = snd_pcm_substream_chip(substream);

	static const struct snd_pcm_hardware hw = {
		.info = (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_MMAP_VALID),
		.formats = SNDRV_PCM_FMTBIT_S32_LE,
		.rates = (SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
		SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000),
		.rate_min = 44100, .rate_max = 96000,
		.channels_min = 2, .channels_max = 2,
		.buffer_bytes_max = 1024 * 1024,
		.period_bytes_min = 64,
		.period_bytes_max = 1024 * 1024,
		.periods_min = 2, .periods_max = 1024,
	};

	substream->runtime->hw = hw;

	spin_lock_irq(&tascam->lock);
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		tascam->capture_substream = substream;
	else
		tascam->playback_substream = substream;
	spin_unlock_irq(&tascam->lock);

	return 0;
}

static int pcm_close(struct snd_pcm_substream *substream)
{
	struct tascam_us122mkii *tascam = snd_pcm_substream_chip(substream);

	spin_lock_irq(&tascam->lock);
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		tascam->capture_substream = NULL;
		tascam->capture_is_running = 0;
	} else {
		tascam->playback_substream = NULL;
		tascam->playback_is_running = 0;
	}

	if (!tascam->capture_substream && !tascam->playback_substream) {
		tascam->usb_stream_running = 0;
		spin_unlock_irq(&tascam->lock);
		usb_kill_anchored_urbs(&tascam->pump_anchor);
		usb_kill_anchored_urbs(&tascam->capture_anchor);
	} else {
		spin_unlock_irq(&tascam->lock);
	}
	return 0;
}

static int pcm_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	struct tascam_us122mkii *tascam = snd_pcm_substream_chip(substream);
	return tascam_set_rate(tascam, params_rate(params));
}

static int pcm_prepare(struct snd_pcm_substream *substream)
{
	struct tascam_us122mkii *tascam = snd_pcm_substream_chip(substream);
	spin_lock_irq(&tascam->lock);
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		tascam->capture_hw_ptr = 0;
	else {
		tascam->playback_hw_ptr = 0;
		if (!tascam->usb_stream_running) tascam->accum = 0;
	}
	spin_unlock_irq(&tascam->lock);
	return 0;
}

static int pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct tascam_us122mkii *tascam = snd_pcm_substream_chip(substream);
	int i, err = 0;

	spin_lock(&tascam->lock);

	switch (cmd) {
		case SNDRV_PCM_TRIGGER_START:
			if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
				tascam->capture_is_running = 1;
		else
			tascam->playback_is_running = 1;

		if (!tascam->usb_stream_running) {
			tascam->usb_stream_running = 1;
			for (i = 0; i < NUM_URBS; i++) {
				usb_anchor_urb(tascam->pump_urbs[i], &tascam->pump_anchor);
				if ((err = usb_submit_urb(tascam->pump_urbs[i], GFP_ATOMIC)) < 0) {
					tascam->usb_stream_running = 0;
					spin_unlock(&tascam->lock);
					return err;
				}
				usb_anchor_urb(tascam->capture_urbs[i], &tascam->capture_anchor);
				if ((err = usb_submit_urb(tascam->capture_urbs[i], GFP_ATOMIC)) < 0) {
					tascam->usb_stream_running = 0;
					spin_unlock(&tascam->lock);
					return err;
				}
			}
		}
		break;

		case SNDRV_PCM_TRIGGER_STOP:
			if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
				tascam->capture_is_running = 0;
		else
			tascam->playback_is_running = 0;
		break;

		default:
			spin_unlock(&tascam->lock);
			return -EINVAL;
	}

	spin_unlock(&tascam->lock);
	return 0;
}

static snd_pcm_uframes_t pcm_pointer(struct snd_pcm_substream *substream)
{
	struct tascam_us122mkii *tascam = snd_pcm_substream_chip(substream);
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		return tascam->capture_hw_ptr;
	else
		return tascam->playback_hw_ptr;
}

static const struct snd_pcm_ops pcm_ops = {
	.open = pcm_open,
	.close = pcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = pcm_hw_params,
	.prepare = pcm_prepare,
	.trigger = pcm_trigger,
	.pointer = pcm_pointer,
};

static int tascam_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct snd_card *card;
	struct tascam_us122mkii *tascam;
	int err, i, j, alloc_size;

	if (intf->cur_altsetting->desc.bInterfaceNumber != 1) return -ENODEV;

	err = snd_card_new(&intf->dev, -1, "US122MKII", THIS_MODULE, sizeof(*tascam), &card);
	if (err < 0) return err;

	tascam = card->private_data;
	tascam->dev = dev;
	tascam->card = card;
	spin_lock_init(&tascam->lock);
	tascam->usb_stream_running = 0;
	tascam->current_rate = 0;

	init_usb_anchor(&tascam->pump_anchor);
	init_usb_anchor(&tascam->capture_anchor);

	strcpy(card->driver, "us122mkii");
	strcpy(card->shortname, "TASCAM US-122MKII");
	sprintf(card->longname, "%s at bus %d device %d", card->shortname, dev->bus->busnum, dev->devnum);

	usb_set_interface(dev, 1, 1);
	tascam_set_rate(tascam, 48000);

	alloc_size = ISO_PACKETS_PER_URB * MAX_PACKET_SIZE;

	for (i = 0; i < NUM_URBS; i++) {
		tascam->pump_urbs[i] = usb_alloc_urb(ISO_PACKETS_PER_URB, GFP_KERNEL);
		tascam->pump_urbs[i]->transfer_buffer = usb_alloc_coherent(dev, alloc_size, GFP_KERNEL, &tascam->pump_urbs[i]->transfer_dma);
		memset(tascam->pump_urbs[i]->transfer_buffer, 0, alloc_size);

		tascam->pump_urbs[i]->dev = dev;
		tascam->pump_urbs[i]->context = tascam;
		tascam->pump_urbs[i]->pipe = usb_sndisocpipe(dev, EP_PLAYBACK);
		tascam->pump_urbs[i]->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
		tascam->pump_urbs[i]->interval = 1;
		tascam->pump_urbs[i]->complete = playback_complete;
		tascam->pump_urbs[i]->number_of_packets = ISO_PACKETS_PER_URB;

		for (j = 0; j < ISO_PACKETS_PER_URB; j++) {
			tascam->pump_urbs[i]->iso_frame_desc[j].offset = j * MAX_PACKET_SIZE;
			tascam->pump_urbs[i]->iso_frame_desc[j].length = 0;
		}

		tascam->capture_urbs[i] = usb_alloc_urb(ISO_PACKETS_PER_URB, GFP_KERNEL);
		tascam->capture_urbs[i]->transfer_buffer = usb_alloc_coherent(dev, alloc_size, GFP_KERNEL, &tascam->capture_urbs[i]->transfer_dma);

		tascam->capture_urbs[i]->dev = dev;
		tascam->capture_urbs[i]->context = tascam;
		tascam->capture_urbs[i]->pipe = usb_rcvisocpipe(dev, EP_CAPTURE);
		tascam->capture_urbs[i]->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
		tascam->capture_urbs[i]->interval = 1;
		tascam->capture_urbs[i]->complete = capture_complete;
		tascam->capture_urbs[i]->number_of_packets = ISO_PACKETS_PER_URB;

		for (j = 0; j < ISO_PACKETS_PER_URB; j++) {
			tascam->capture_urbs[i]->iso_frame_desc[j].offset = j * MAX_PACKET_SIZE;
			tascam->capture_urbs[i]->iso_frame_desc[j].length = MAX_PACKET_SIZE;
		}
	}

	err = snd_pcm_new(card, "US122MKII", 0, 1, 1, &tascam->pcm);
	if (err < 0) goto cleanup;

	tascam->pcm->private_data = tascam;
	snd_pcm_set_ops(tascam->pcm, SNDRV_PCM_STREAM_PLAYBACK, &pcm_ops);
	snd_pcm_set_ops(tascam->pcm, SNDRV_PCM_STREAM_CAPTURE, &pcm_ops);
	snd_pcm_set_managed_buffer_all(tascam->pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);

	err = snd_card_register(card);
	if (err < 0) goto cleanup;

	usb_set_intfdata(intf, card);
	return 0;

	cleanup:
	snd_card_free(card);
	return err;
}

static void tascam_disconnect(struct usb_interface *intf)
{
	struct snd_card *card = usb_get_intfdata(intf);
	struct tascam_us122mkii *tascam;
	int i;

	if (!card) return;
	tascam = card->private_data;

	spin_lock_irq(&tascam->lock);
	tascam->usb_stream_running = 0;
	spin_unlock_irq(&tascam->lock);

	usb_kill_anchored_urbs(&tascam->pump_anchor);
	usb_kill_anchored_urbs(&tascam->capture_anchor);

	snd_card_disconnect(card);

	for (i = 0; i < NUM_URBS; i++) {
		if (tascam->pump_urbs[i]) {
			usb_free_coherent(tascam->dev, ISO_PACKETS_PER_URB * MAX_PACKET_SIZE, tascam->pump_urbs[i]->transfer_buffer, tascam->pump_urbs[i]->transfer_dma);
			usb_free_urb(tascam->pump_urbs[i]);
		}
		if (tascam->capture_urbs[i]) {
			usb_free_coherent(tascam->dev, ISO_PACKETS_PER_URB * MAX_PACKET_SIZE, tascam->capture_urbs[i]->transfer_buffer, tascam->capture_urbs[i]->transfer_dma);
			usb_free_urb(tascam->capture_urbs[i]);
		}
	}
	snd_card_free(card);
}

static const struct usb_device_id tascam_ids[] = {
	{ USB_DEVICE(USB_VID_TASCAM, USB_PID_US122MKII) },
	{}
};
MODULE_DEVICE_TABLE(usb, tascam_ids);

static struct usb_driver tascam_driver = {
	.name = "snd-usb-us122mkii",
	.probe = tascam_probe,
	.disconnect = tascam_disconnect,
	.id_table = tascam_ids,
};

module_usb_driver(tascam_driver);
