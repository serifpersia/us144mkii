// SPDX-License-Identifier: GPL-2.0-only
/*
 * ALSA Driver for TASCAM US-122MKII
 * Architecture: Coupled Duplex Engine (Always-On Bidirectional)
 * Rate: Fixed 48kHz
 *
 * Copyright (c) 2026 Šerif Rami <ramiserifpersia@gmail.com>
 */

#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/initval.h>

MODULE_AUTHOR("Serif Rami");
MODULE_DESCRIPTION("TASCAM US-122MKII Driver (Coupled)");
MODULE_LICENSE("GPL");

#define USB_VID_TASCAM          0x0644
#define USB_PID_US122MKII       0x8021

#define EP_CAPTURE              0x81
#define EP_PLAYBACK             0x02

#define FRAMES_PER_PACKET       6
#define BYTES_PER_FRAME_USB     6
#define PACKET_SIZE             36
#define MAX_PACKET_SIZE         36

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

	struct urb *capture_urbs[NUM_URBS];
	struct urb *playback_urbs[NUM_URBS];

	struct usb_anchor anchor;

	spinlock_t lock;

	/*
	 * Engine State:
	 * stream_running: 1 if the USB transport (EP 0x81 + EP 0x02) is active.
	 * capture_active: 1 if ALSA wants capture data.
	 * playback_active: 1 if ALSA is providing playback data.
	 */
	atomic_t stream_running;
	int capture_active;
	int playback_active;

	snd_pcm_uframes_t capture_hw_ptr;
	snd_pcm_uframes_t playback_hw_ptr;
};

static int tascam_set_rate_48k(struct usb_device *dev)
{
	u8 *rate_payload;
	int err;

	rate_payload = kmalloc(3, GFP_KERNEL);
	if (!rate_payload)
		return -ENOMEM;

	/* Tascam 48kHz Command: 80 80 BB */
	rate_payload[0] = 0x80;
	rate_payload[1] = 0x80;
	rate_payload[2] = 0xbb;

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
						  UAC_SET_CUR, RT_H2D_CLASS_EP, UAC_SAMPLING_FREQ_CONTROL,
					   EP_CAPTURE, rate_payload, 3, 1000);

	kfree(rate_payload);
	return err;
}

/* --- Buffer Logic --- */

static inline void process_capture_data(struct tascam_us122mkii *tascam,
										struct snd_pcm_runtime *runtime,
										u8 *src, int len)
{
	u32 *dst;
	int i;
	int frames = len / BYTES_PER_FRAME_USB;
	int pos_bytes = frames_to_bytes(runtime, tascam->capture_hw_ptr);

	if (frames == 0) return;

	dst = (u32 *)(runtime->dma_area + pos_bytes);

	if (pos_bytes + frames_to_bytes(runtime, frames) > frames_to_bytes(runtime, runtime->buffer_size)) {
		int chunk1 = bytes_to_frames(runtime, frames_to_bytes(runtime, runtime->buffer_size) - pos_bytes);
		int chunk2 = frames - chunk1;

		for (i = 0; i < chunk1; i++) {
			dst[0] = (src[2] << 24) | (src[1] << 16) | (src[0] << 8);
			dst[1] = (src[5] << 24) | (src[4] << 16) | (src[3] << 8);
			src += BYTES_PER_FRAME_USB; dst += 2;
		}
		dst = (u32 *)runtime->dma_area;
		for (i = 0; i < chunk2; i++) {
			dst[0] = (src[2] << 24) | (src[1] << 16) | (src[0] << 8);
			dst[1] = (src[5] << 24) | (src[4] << 16) | (src[3] << 8);
			src += BYTES_PER_FRAME_USB; dst += 2;
		}
	} else {
		for (i = 0; i < frames; i++) {
			dst[0] = (src[2] << 24) | (src[1] << 16) | (src[0] << 8);
			dst[1] = (src[5] << 24) | (src[4] << 16) | (src[3] << 8);
			src += BYTES_PER_FRAME_USB; dst += 2;
		}
	}

	tascam->capture_hw_ptr += frames;
	if (tascam->capture_hw_ptr >= runtime->buffer_size)
		tascam->capture_hw_ptr -= runtime->buffer_size;
}

static inline void fill_playback_data(struct tascam_us122mkii *tascam,
									  struct snd_pcm_runtime *runtime,
									  u8 *dst)
{
	u32 *src;
	int i;
	int frames = FRAMES_PER_PACKET;
	int pos_bytes = frames_to_bytes(runtime, tascam->playback_hw_ptr);

	src = (u32 *)(runtime->dma_area + pos_bytes);

	if (pos_bytes + frames_to_bytes(runtime, frames) > frames_to_bytes(runtime, runtime->buffer_size)) {
		int chunk1 = bytes_to_frames(runtime, frames_to_bytes(runtime, runtime->buffer_size) - pos_bytes);
		int chunk2 = frames - chunk1;

		for (i = 0; i < chunk1; i++) {
			dst[0] = (src[0] >> 8); dst[1] = (src[0] >> 16); dst[2] = (src[0] >> 24);
			dst[3] = (src[1] >> 8); dst[4] = (src[1] >> 16); dst[5] = (src[1] >> 24);
			dst += BYTES_PER_FRAME_USB; src += 2;
		}
		src = (u32 *)runtime->dma_area;
		for (i = 0; i < chunk2; i++) {
			dst[0] = (src[0] >> 8); dst[1] = (src[0] >> 16); dst[2] = (src[0] >> 24);
			dst[3] = (src[1] >> 8); dst[4] = (src[1] >> 16); dst[5] = (src[1] >> 24);
			dst += BYTES_PER_FRAME_USB; src += 2;
		}
	} else {
		for (i = 0; i < frames; i++) {
			dst[0] = (src[0] >> 8); dst[1] = (src[0] >> 16); dst[2] = (src[0] >> 24);
			dst[3] = (src[1] >> 8); dst[4] = (src[1] >> 16); dst[5] = (src[1] >> 24);
			dst += BYTES_PER_FRAME_USB; src += 2;
		}
	}

	tascam->playback_hw_ptr += frames;
	if (tascam->playback_hw_ptr >= runtime->buffer_size)
		tascam->playback_hw_ptr -= runtime->buffer_size;
}

/* --- URB Callbacks --- */

static void playback_complete(struct urb *urb)
{
	struct tascam_us122mkii *tascam = urb->context;
	struct snd_pcm_runtime *runtime;
	unsigned long flags;
	int i;

	if (urb->status == -ENOENT || urb->status == -ECONNRESET || urb->status == -ESHUTDOWN)
		return;

	spin_lock_irqsave(&tascam->lock, flags);

	/* If the engine is supposed to stop, we don't resubmit. */
	if (!atomic_read(&tascam->stream_running)) {
		spin_unlock_irqrestore(&tascam->lock, flags);
		return;
	}

	/*
	 * LOGIC:
	 * If Playback Active: Fill with Audio.
	 * If Playback Inactive (but stream running for Capture): Fill with Silence.
	 */
	if (tascam->playback_active && tascam->playback_substream) {
		runtime = tascam->playback_substream->runtime;
		snd_pcm_period_elapsed(tascam->playback_substream);

		for (i = 0; i < ISO_PACKETS_PER_URB; i++) {
			urb->iso_frame_desc[i].offset = i * PACKET_SIZE;
			urb->iso_frame_desc[i].length = PACKET_SIZE;
			urb->iso_frame_desc[i].status = 0;
			fill_playback_data(tascam, runtime, urb->transfer_buffer + urb->iso_frame_desc[i].offset);
		}
	} else {
		/* Pump Silence */
		for (i = 0; i < ISO_PACKETS_PER_URB; i++) {
			urb->iso_frame_desc[i].offset = i * PACKET_SIZE;
			urb->iso_frame_desc[i].length = PACKET_SIZE;
			urb->iso_frame_desc[i].status = 0;
			memset(urb->transfer_buffer + urb->iso_frame_desc[i].offset, 0, PACKET_SIZE);
		}
	}

	spin_unlock_irqrestore(&tascam->lock, flags);
	usb_submit_urb(urb, GFP_ATOMIC);
}

static void capture_complete(struct urb *urb)
{
	struct tascam_us122mkii *tascam = urb->context;
	struct snd_pcm_runtime *runtime;
	unsigned long flags;
	int i;

	if (urb->status == -ENOENT || urb->status == -ECONNRESET || urb->status == -ESHUTDOWN)
		return;

	spin_lock_irqsave(&tascam->lock, flags);

	if (!atomic_read(&tascam->stream_running)) {
		spin_unlock_irqrestore(&tascam->lock, flags);
		return;
	}

	/*
	 * LOGIC:
	 * If Capture Active: Process Data.
	 * If Capture Inactive: Ignore Data (Drain).
	 */
	if (tascam->capture_active && tascam->capture_substream) {
		runtime = tascam->capture_substream->runtime;

		for (i = 0; i < ISO_PACKETS_PER_URB; i++) {
			int len = urb->iso_frame_desc[i].actual_length;
			if (len > 0)
				process_capture_data(tascam, runtime, urb->transfer_buffer + urb->iso_frame_desc[i].offset, len);
		}
		snd_pcm_period_elapsed(tascam->capture_substream);
	}

	/* Reset descriptors */
	for (i = 0; i < ISO_PACKETS_PER_URB; i++) {
		urb->iso_frame_desc[i].offset = i * PACKET_SIZE;
		urb->iso_frame_desc[i].length = PACKET_SIZE;
		urb->iso_frame_desc[i].status = 0;
	}

	spin_unlock_irqrestore(&tascam->lock, flags);
	usb_submit_urb(urb, GFP_ATOMIC);
}

/* --- ALSA Operations --- */

static int pcm_open(struct snd_pcm_substream *substream)
{
	struct tascam_us122mkii *tascam = snd_pcm_substream_chip(substream);
	static const struct snd_pcm_hardware hw = {
		.info = (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME),
		.formats = SNDRV_PCM_FMTBIT_S32_LE,
		.rates = SNDRV_PCM_RATE_48000,
		.rate_min = 48000, .rate_max = 48000,
		.channels_min = 2, .channels_max = 2,
		.buffer_bytes_max = 1024 * 1024,
		.period_bytes_min = 384,
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
	unsigned long flags;

	spin_lock_irqsave(&tascam->lock, flags);
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		tascam->capture_substream = NULL;
		tascam->capture_active = 0;
	} else {
		tascam->playback_substream = NULL;
		tascam->playback_active = 0;
	}

	/* If BOTH closed, Stop Engine */
	if (!tascam->capture_substream && !tascam->playback_substream) {
		atomic_set(&tascam->stream_running, 0);
		spin_unlock_irqrestore(&tascam->lock, flags);

		/* Kill all URBs */
		usb_kill_anchored_urbs(&tascam->anchor);
	} else {
		spin_unlock_irqrestore(&tascam->lock, flags);
	}

	return 0;
}

static int pcm_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	struct tascam_us122mkii *tascam = snd_pcm_substream_chip(substream);
	return tascam_set_rate_48k(tascam->dev);
}

static int pcm_prepare(struct snd_pcm_substream *substream)
{
	struct tascam_us122mkii *tascam = snd_pcm_substream_chip(substream);

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
	int i, j, err = 0;

	spin_lock(&tascam->lock);

	switch (cmd) {
		case SNDRV_PCM_TRIGGER_START:
		case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		case SNDRV_PCM_TRIGGER_RESUME:

			/* 1. Set Local Flag */
			if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
				tascam->capture_active = 1;
		else
			tascam->playback_active = 1;

		/* 2. Check if Engine is already running */
		if (atomic_read(&tascam->stream_running) == 0) {
			atomic_set(&tascam->stream_running, 1);

			/* START ENGINE: Submit BOTH Playback and Capture URBs */

			/* Prepare Initial Playback URBs (Audio or Silence) */
			for (i = 0; i < NUM_URBS; i++) {
				struct urb *u = tascam->playback_urbs[i];
				for (j = 0; j < ISO_PACKETS_PER_URB; j++) {
					u->iso_frame_desc[j].offset = j * PACKET_SIZE;
					u->iso_frame_desc[j].length = PACKET_SIZE;
					u->iso_frame_desc[j].status = 0;

					if (tascam->playback_active)
						fill_playback_data(tascam, tascam->playback_substream->runtime,
										   u->transfer_buffer + u->iso_frame_desc[j].offset);
						else
							memset(u->transfer_buffer + u->iso_frame_desc[j].offset, 0, PACKET_SIZE);
				}
				usb_anchor_urb(u, &tascam->anchor);
				if ((err = usb_submit_urb(u, GFP_ATOMIC)) < 0) goto error;
			}

			/* Prepare Initial Capture URBs */
			for (i = 0; i < NUM_URBS; i++) {
				usb_anchor_urb(tascam->capture_urbs[i], &tascam->anchor);
				if ((err = usb_submit_urb(tascam->capture_urbs[i], GFP_ATOMIC)) < 0) goto error;
			}
		}
		break;

		case SNDRV_PCM_TRIGGER_STOP:
		case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		case SNDRV_PCM_TRIGGER_SUSPEND:
			/*
			 * Just clear the specific flag.
			 * The callbacks will see this and switch to Silence (Playback) or Drain (Capture).
			 * We only kill URBs in pcm_close.
			 */
			if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
				tascam->capture_active = 0;
		else
			tascam->playback_active = 0;
		break;

		default:
			spin_unlock(&tascam->lock);
			return -EINVAL;
	}

	spin_unlock(&tascam->lock);
	return 0;

	error:
	atomic_set(&tascam->stream_running, 0);
	tascam->capture_active = 0;
	tascam->playback_active = 0;
	spin_unlock(&tascam->lock);
	return err;
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
	init_usb_anchor(&tascam->anchor);

	strcpy(card->driver, "us122mkii");
	strcpy(card->shortname, "TASCAM US-122MKII");
	sprintf(card->longname, "%s at bus %d device %d", card->shortname, dev->bus->busnum, dev->devnum);

	usb_set_interface(dev, 1, 1);
	tascam_set_rate_48k(dev);

	alloc_size = ISO_PACKETS_PER_URB * PACKET_SIZE;

	for (i = 0; i < NUM_URBS; i++) {
		/* Allocate Capture URBs */
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
			tascam->capture_urbs[i]->iso_frame_desc[j].offset = j * PACKET_SIZE;
			tascam->capture_urbs[i]->iso_frame_desc[j].length = PACKET_SIZE;
		}

		/* Allocate Playback URBs */
		tascam->playback_urbs[i] = usb_alloc_urb(ISO_PACKETS_PER_URB, GFP_KERNEL);
		tascam->playback_urbs[i]->transfer_buffer = usb_alloc_coherent(dev, alloc_size, GFP_KERNEL, &tascam->playback_urbs[i]->transfer_dma);
		tascam->playback_urbs[i]->dev = dev;
		tascam->playback_urbs[i]->context = tascam;
		tascam->playback_urbs[i]->pipe = usb_sndisocpipe(dev, EP_PLAYBACK);
		tascam->playback_urbs[i]->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
		tascam->playback_urbs[i]->interval = 1;
		tascam->playback_urbs[i]->complete = playback_complete;
		tascam->playback_urbs[i]->number_of_packets = ISO_PACKETS_PER_URB;

		for (j = 0; j < ISO_PACKETS_PER_URB; j++) {
			tascam->playback_urbs[i]->iso_frame_desc[j].offset = j * PACKET_SIZE;
			tascam->playback_urbs[i]->iso_frame_desc[j].length = PACKET_SIZE;
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
	atomic_set(&tascam->stream_running, 0);
	tascam->capture_active = 0;
	tascam->playback_active = 0;
	spin_unlock_irq(&tascam->lock);

	usb_kill_anchored_urbs(&tascam->anchor);
	snd_card_disconnect(card);

	for (i = 0; i < NUM_URBS; i++) {
		if (tascam->capture_urbs[i]) {
			usb_free_coherent(tascam->dev, ISO_PACKETS_PER_URB * PACKET_SIZE, tascam->capture_urbs[i]->transfer_buffer, tascam->capture_urbs[i]->transfer_dma);
			usb_free_urb(tascam->capture_urbs[i]);
		}
		if (tascam->playback_urbs[i]) {
			usb_free_coherent(tascam->dev, ISO_PACKETS_PER_URB * PACKET_SIZE, tascam->playback_urbs[i]->transfer_buffer, tascam->playback_urbs[i]->transfer_dma);
			usb_free_urb(tascam->playback_urbs[i]);
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
