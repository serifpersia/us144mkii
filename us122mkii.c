// SPDX-License-Identifier: GPL-2.0-only
/*
 * ALSA Driver for TASCAM US-122MKII (DEBUG VERSION)
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
MODULE_DESCRIPTION("TASCAM US-122MKII Debug Driver");
MODULE_LICENSE("GPL");

#define USB_VID_TASCAM          0x0644
#define USB_PID_US122MKII       0x8021

#define EP_CAPTURE              0x81
#define EP_PLAYBACK             0x02

#define FRAMES_PER_PACKET       6
#define BYTES_PER_FRAME         6
#define PACKET_SIZE             36
#define MAX_PACKET_SIZE         78

#define NUM_URBS                4
#define ISO_PACKETS_PER_URB     8

#define RT_H2D_CLASS_EP         (USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_ENDPOINT)
#define UAC_SET_CUR             0x01
#define UAC_SAMPLING_FREQ_CONTROL 0x0100

/* DEBUG MACRO */
#define DBG(fmt, args...) printk(KERN_INFO "TASCAM_DBG: " fmt, ##args)

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

	/* State Flags */
	int usb_stream_running;
	int capture_is_running;
	int playback_is_running;

	snd_pcm_uframes_t capture_hw_ptr;
	snd_pcm_uframes_t playback_hw_ptr;

	/* Debug counters to avoid log spam in IRQ */
	int irq_log_counter;
};

static int tascam_set_rate(struct tascam_us122mkii *tascam)
{
	/* CRITICAL FIX: Do not change rate if URBs are flying */
	if (tascam->usb_stream_running) {
		DBG("Skipping set_rate: Stream is ALREADY running.\n");
		return 0;
	}

	u8 rate_payload[3] = { 0x80, 0xbb, 0x00 }; /* 48000 Hz Little Endian */
	DBG("Sending USB Control Msg: Set Rate 48k\n");
	return usb_control_msg(tascam->dev, usb_sndctrlpipe(tascam->dev, 0),
						   UAC_SET_CUR, RT_H2D_CLASS_EP, UAC_SAMPLING_FREQ_CONTROL,
						EP_CAPTURE, rate_payload, 3, 1000);
}

/* --- URB Callbacks --- */

static void playback_complete(struct urb *urb)
{
	struct tascam_us122mkii *tascam = urb->context;
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	unsigned long flags;
	int i, f;
	u8 *dst;
	u32 *src;

	if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
		urb->status == -ESHUTDOWN) {
		DBG("Playback URB Stopped: %d\n", urb->status);
	return;
		}

		spin_lock_irqsave(&tascam->lock, flags);

		if (!tascam->usb_stream_running) {
			spin_unlock_irqrestore(&tascam->lock, flags);
			return;
		}

		substream = tascam->playback_substream;

		if (tascam->playback_is_running && substream) {
			runtime = substream->runtime;
			for (i = 0; i < ISO_PACKETS_PER_URB; i++) {
				int frames = FRAMES_PER_PACKET;
				int bytes = frames * BYTES_PER_FRAME;

				urb->iso_frame_desc[i].offset = i * MAX_PACKET_SIZE;
				urb->iso_frame_desc[i].length = bytes;
				urb->iso_frame_desc[i].status = 0;

				int pos_bytes = frames_to_bytes(runtime, tascam->playback_hw_ptr);
				int chunk_bytes = frames_to_bytes(runtime, frames);

				dst = urb->transfer_buffer + urb->iso_frame_desc[i].offset;
				src = (u32 *)(runtime->dma_area + pos_bytes);

				if (pos_bytes + chunk_bytes > frames_to_bytes(runtime, runtime->buffer_size))
					chunk_bytes = frames_to_bytes(runtime, runtime->buffer_size) - pos_bytes;

				int frames1 = bytes_to_frames(runtime, chunk_bytes);
				for (f = 0; f < frames1; f++) {
					dst[0] = (src[0] >> 8); dst[1] = (src[0] >> 16); dst[2] = (src[0] >> 24);
					dst[3] = (src[1] >> 8); dst[4] = (src[1] >> 16); dst[5] = (src[1] >> 24);
					dst += BYTES_PER_FRAME; src += 2;
				}
				if (chunk_bytes < bytes) {
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
		} else {
			/* SILENCE PUMP */
			for (i = 0; i < ISO_PACKETS_PER_URB; i++) {
				urb->iso_frame_desc[i].offset = i * MAX_PACKET_SIZE;
				urb->iso_frame_desc[i].length = PACKET_SIZE;
				urb->iso_frame_desc[i].status = 0;
				memset(urb->transfer_buffer + urb->iso_frame_desc[i].offset, 0, PACKET_SIZE);
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
	int has_data = 0;

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

			/* DEBUG: Check for incoming data presence */
			if (len > 0) has_data = 1;

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

		/* DEBUG: Every 1000 callbacks, print status */
		if (tascam->irq_log_counter++ > 1000) {
			tascam->irq_log_counter = 0;
			if (!has_data) DBG("Capturing... but URB len is 0!\n");
			// else DBG("Capturing... Data incoming OK.\n");
		}
	}

	/* Reset descriptors */
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

/* --- ALSA Operations --- */

static int pcm_open(struct snd_pcm_substream *substream)
{
	struct tascam_us122mkii *tascam = snd_pcm_substream_chip(substream);
	DBG("PCM Open: Stream %d\n", substream->stream);

	static const struct snd_pcm_hardware hw = {
		.info = (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_MMAP_VALID),
		.formats = SNDRV_PCM_FMTBIT_S32_LE,
		.rates = SNDRV_PCM_RATE_48000,
		.rate_min = 48000, .rate_max = 48000,
		.channels_min = 2, .channels_max = 2,
		.buffer_bytes_max = 128 * 1024,
		.period_bytes_min = 64, .period_bytes_max = 32768,
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
	DBG("PCM Close: Stream %d\n", substream->stream);

	spin_lock_irq(&tascam->lock);
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		tascam->capture_substream = NULL;
		tascam->capture_is_running = 0;
	} else {
		tascam->playback_substream = NULL;
		tascam->playback_is_running = 0;
	}

	if (!tascam->capture_substream && !tascam->playback_substream) {
		DBG("All streams closed. Stopping URBs.\n");
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
	DBG("PCM HW Params: Stream %d\n", substream->stream);
	return tascam_set_rate(tascam);
}

static int pcm_prepare(struct snd_pcm_substream *substream)
{
	struct tascam_us122mkii *tascam = snd_pcm_substream_chip(substream);
	DBG("PCM Prepare: Stream %d\n", substream->stream);

	spin_lock_irq(&tascam->lock);
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		tascam->capture_hw_ptr = 0;
	else
		tascam->playback_hw_ptr = 0;
	spin_unlock_irq(&tascam->lock);

	return 0;
}

static int pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct tascam_us122mkii *tascam = snd_pcm_substream_chip(substream);
	int i, err = 0;

	DBG("PCM Trigger: Cmd %d, Stream %d\n", cmd, substream->stream);

	spin_lock(&tascam->lock);

	switch (cmd) {
		case SNDRV_PCM_TRIGGER_START:
			if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
				tascam->capture_is_running = 1;
		else
			tascam->playback_is_running = 1;

		if (!tascam->usb_stream_running) {
			DBG("Trigger Start: Launching URBs...\n");
			tascam->usb_stream_running = 1;

			for (i = 0; i < NUM_URBS; i++) {
				usb_anchor_urb(tascam->pump_urbs[i], &tascam->pump_anchor);
				if ((err = usb_submit_urb(tascam->pump_urbs[i], GFP_ATOMIC)) < 0) {
					DBG("URB Submit Failed %d\n", err);
					tascam->usb_stream_running = 0;
					spin_unlock(&tascam->lock);
					return err;
				}
				usb_anchor_urb(tascam->capture_urbs[i], &tascam->capture_anchor);
				if ((err = usb_submit_urb(tascam->capture_urbs[i], GFP_ATOMIC)) < 0) {
					DBG("URB Submit Failed %d\n", err);
					tascam->usb_stream_running = 0;
					spin_unlock(&tascam->lock);
					return err;
				}
			}
		}
		break;

		case SNDRV_PCM_TRIGGER_STOP:
			DBG("Trigger Stop: Stream %d\n", substream->stream);
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

	DBG("Probe: Found TASCAM US-122MKII\n");

	if (intf->cur_altsetting->desc.bInterfaceNumber != 1) return -ENODEV;

	err = snd_card_new(&intf->dev, -1, "US122MKII", THIS_MODULE, sizeof(*tascam), &card);
	if (err < 0) return err;

	tascam = card->private_data;
	tascam->dev = dev;
	tascam->card = card;
	spin_lock_init(&tascam->lock);
	tascam->usb_stream_running = 0;
	tascam->irq_log_counter = 0;

	init_usb_anchor(&tascam->pump_anchor);
	init_usb_anchor(&tascam->capture_anchor);

	strcpy(card->driver, "us122mkii");
	strcpy(card->shortname, "TASCAM US-122MKII");
	sprintf(card->longname, "%s at bus %d device %d", card->shortname, dev->bus->busnum, dev->devnum);

	usb_set_interface(dev, 1, 1);
	tascam_set_rate(tascam);

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
			tascam->pump_urbs[i]->iso_frame_desc[j].length = PACKET_SIZE;
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

	DBG("Disconnect called.\n");

	if (!card) return;
	tascam = card->private_data;

	spin_lock_irq(&tascam->lock);
	tascam->usb_stream_running = 0;
	tascam->capture_is_running = 0;
	tascam->playback_is_running = 0;
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
	.name = "snd-usb-us122mkii-dbg",
	.probe = tascam_probe,
	.disconnect = tascam_disconnect,
	.id_table = tascam_ids,
};

module_usb_driver(tascam_driver);
