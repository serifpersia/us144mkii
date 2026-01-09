// SPDX-License-Identifier: GPL-2.0-only
/*
 * ALSA Driver for TASCAM US-122MKII
 * Mode: Independent Streams (No Feedback Sync)
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
MODULE_DESCRIPTION("TASCAM US-122MKII Driver (Independent Streams)");
MODULE_LICENSE("GPL");

#define USB_VID_TASCAM          0x0644
#define USB_PID_US122MKII       0x8021

#define EP_CAPTURE              0x81
#define EP_PLAYBACK             0x02

/*
 * 48kHz Configuration:
 * 48000 Hz / 1000 URBs/sec = 48 frames per URB.
 * 8 Packets per URB -> 6 frames per packet.
 * 6 frames * 6 bytes (24-bit stereo) = 36 bytes per packet.
 */
#define FRAMES_PER_PACKET       6
#define BYTES_PER_FRAME_USB     6  /* 24-bit Little Endian Stereo */
#define PACKET_SIZE             36
#define MAX_PACKET_SIZE         36

/*
 * NUM_URBS = 4 (Quad Buffering)
 * Increases latency slightly (~4ms) but significantly ensures stability.
 */
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

	struct usb_anchor capture_anchor;
	struct usb_anchor playback_anchor;

	spinlock_t lock;

	/*
	 * Independent running flags.
	 * In this mode, capture and playback loops are separate.
	 */
	int capture_is_running;
	int playback_is_running;

	snd_pcm_uframes_t capture_hw_ptr;
	snd_pcm_uframes_t playback_hw_ptr;
};

/*
 * Safe Control Message (kmalloc)
 * Sets device to 48kHz.
 */
static int tascam_set_rate_48k(struct usb_device *dev)
{
	u8 *rate_payload;
	int err;

	rate_payload = kmalloc(3, GFP_KERNEL);
	if (!rate_payload)
		return -ENOMEM;

	/* 48000Hz -> 80 80 BB (Little Endian) */
	rate_payload[0] = 0x80;
	rate_payload[1] = 0x80;
	rate_payload[2] = 0xbb;

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
						  UAC_SET_CUR, RT_H2D_CLASS_EP, UAC_SAMPLING_FREQ_CONTROL,
					   EP_CAPTURE, rate_payload, 3, 1000);

	kfree(rate_payload);
	return err;
}

/* --- Data Conversion Helpers --- */

static inline void process_capture_data(struct tascam_us122mkii *tascam,
										struct snd_pcm_runtime *runtime,
										u8 *src, int len)
{
	u32 *dst;
	int frames = len / BYTES_PER_FRAME_USB;
	int pos_bytes = frames_to_bytes(runtime, tascam->capture_hw_ptr);
	int i;

	if (frames == 0) return;

	dst = (u32 *)(runtime->dma_area + pos_bytes);

	if (pos_bytes + frames_to_bytes(runtime, frames) > frames_to_bytes(runtime, runtime->buffer_size)) {
		int chunk1 = bytes_to_frames(runtime, frames_to_bytes(runtime, runtime->buffer_size) - pos_bytes);
		int chunk2 = frames - chunk1;

		/* 24-bit USB -> 32-bit ALSA */
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

/*
 * Fills a buffer with data from ALSA runtime.
 * Returns the number of bytes written.
 */
static inline void prepare_playback_data(struct tascam_us122mkii *tascam,
										 struct snd_pcm_runtime *runtime,
										 u8 *dst, int len)
{
	u32 *src;
	int frames = len / BYTES_PER_FRAME_USB;
	int pos_bytes = frames_to_bytes(runtime, tascam->playback_hw_ptr);
	int i;

	if (frames == 0) {
		memset(dst, 0, len);
		return;
	}

	src = (u32 *)(runtime->dma_area + pos_bytes);

	/* 32-bit ALSA -> 24-bit USB */
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

/* --- URB Callbacks (Independent) --- */

static void playback_complete(struct urb *urb)
{
	struct tascam_us122mkii *tascam = urb->context;
	struct snd_pcm_runtime *runtime;
	unsigned long flags;
	int i;

	if (urb->status == -ENOENT || urb->status == -ECONNRESET || urb->status == -ESHUTDOWN)
		return;

	spin_lock_irqsave(&tascam->lock, flags);

	if (!tascam->playback_is_running || !tascam->playback_substream) {
		spin_unlock_irqrestore(&tascam->lock, flags);
		return;
	}

	runtime = tascam->playback_substream->runtime;

	/* 1. Notify ALSA that a period has elapsed */
	snd_pcm_period_elapsed(tascam->playback_substream);

	/* 2. Prepare Data for the NEXT submission (Active Pumping) */
	for (i = 0; i < ISO_PACKETS_PER_URB; i++) {
		urb->iso_frame_desc[i].offset = i * MAX_PACKET_SIZE;
		urb->iso_frame_desc[i].length = MAX_PACKET_SIZE;
		urb->iso_frame_desc[i].status = 0;

		prepare_playback_data(tascam, runtime,
							  urb->transfer_buffer + urb->iso_frame_desc[i].offset,
						MAX_PACKET_SIZE);
	}

	spin_unlock_irqrestore(&tascam->lock, flags);

	/* 3. Resubmit immediately */
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

	if (!tascam->capture_is_running || !tascam->capture_substream) {
		spin_unlock_irqrestore(&tascam->lock, flags);
		return;
	}

	runtime = tascam->capture_substream->runtime;

	/* 1. Process Incoming Data */
	for (i = 0; i < ISO_PACKETS_PER_URB; i++) {
		int len = urb->iso_frame_desc[i].actual_length;
		if (len > 0) {
			process_capture_data(tascam, runtime,
								 urb->transfer_buffer + urb->iso_frame_desc[i].offset, len);
		}

		/* Reset descriptor for next run */
		urb->iso_frame_desc[i].offset = i * MAX_PACKET_SIZE;
		urb->iso_frame_desc[i].length = MAX_PACKET_SIZE;
		urb->iso_frame_desc[i].status = 0;
	}

	snd_pcm_period_elapsed(tascam->capture_substream);
	spin_unlock_irqrestore(&tascam->lock, flags);

	/* 2. Resubmit immediately */
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

	spin_lock_irq(&tascam->lock);
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		tascam->capture_substream = NULL;
		tascam->capture_is_running = 0;
		spin_unlock_irq(&tascam->lock);
		usb_kill_anchored_urbs(&tascam->capture_anchor);
	} else {
		tascam->playback_substream = NULL;
		tascam->playback_is_running = 0;
		spin_unlock_irq(&tascam->lock);
		usb_kill_anchored_urbs(&tascam->playback_anchor);
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
	struct snd_pcm_runtime *runtime = substream->runtime;
	int i, j, err = 0;

	spin_lock(&tascam->lock);

	switch (cmd) {
		case SNDRV_PCM_TRIGGER_START:
		case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		case SNDRV_PCM_TRIGGER_RESUME:

			if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
				if (tascam->capture_is_running) {
					spin_unlock(&tascam->lock);
					return 0;
				}
				tascam->capture_is_running = 1;

				/* Submit all Capture URBs */
				for (i = 0; i < NUM_URBS; i++) {
					usb_anchor_urb(tascam->capture_urbs[i], &tascam->capture_anchor);
					if ((err = usb_submit_urb(tascam->capture_urbs[i], GFP_ATOMIC)) < 0) {
						tascam->capture_is_running = 0;
						spin_unlock(&tascam->lock);
						return err;
					}
				}
			} else {
				if (tascam->playback_is_running) {
					spin_unlock(&tascam->lock);
					return 0;
				}
				tascam->playback_is_running = 1;

				/*
				 * Pre-fill Playback URBs to avoid initial silence/underrun.
				 * We fill buffers, set lengths, and submit.
				 */
				for (i = 0; i < NUM_URBS; i++) {
					struct urb *u = tascam->playback_urbs[i];
					for (j = 0; j < ISO_PACKETS_PER_URB; j++) {
						u->iso_frame_desc[j].offset = j * MAX_PACKET_SIZE;
						u->iso_frame_desc[j].length = MAX_PACKET_SIZE;
						u->iso_frame_desc[j].status = 0;
						prepare_playback_data(tascam, runtime,
											  u->transfer_buffer + u->iso_frame_desc[j].offset,
							MAX_PACKET_SIZE);
					}

					usb_anchor_urb(u, &tascam->playback_anchor);
					if ((err = usb_submit_urb(u, GFP_ATOMIC)) < 0) {
						tascam->playback_is_running = 0;
						spin_unlock(&tascam->lock);
						return err;
					}
				}
			}
			break;

		case SNDRV_PCM_TRIGGER_STOP:
		case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		case SNDRV_PCM_TRIGGER_SUSPEND:
			/*
			 * Just clear flags. Callbacks check flags and stop resubmitting.
			 * We do NOT kill URBs here (safe shutdown).
			 */
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

	init_usb_anchor(&tascam->capture_anchor);
	init_usb_anchor(&tascam->playback_anchor);

	strcpy(card->driver, "us122mkii");
	strcpy(card->shortname, "TASCAM US-122MKII");
	sprintf(card->longname, "%s at bus %d device %d", card->shortname, dev->bus->busnum, dev->devnum);

	usb_set_interface(dev, 1, 1);
	tascam_set_rate_48k(dev);

	alloc_size = ISO_PACKETS_PER_URB * MAX_PACKET_SIZE;

	for (i = 0; i < NUM_URBS; i++) {
		/* Capture Setup */
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

		/* Playback Setup */
		tascam->playback_urbs[i] = usb_alloc_urb(ISO_PACKETS_PER_URB, GFP_KERNEL);
		tascam->playback_urbs[i]->transfer_buffer = usb_alloc_coherent(dev, alloc_size, GFP_KERNEL, &tascam->playback_urbs[i]->transfer_dma);
		memset(tascam->playback_urbs[i]->transfer_buffer, 0, alloc_size);

		tascam->playback_urbs[i]->dev = dev;
		tascam->playback_urbs[i]->context = tascam;
		tascam->playback_urbs[i]->pipe = usb_sndisocpipe(dev, EP_PLAYBACK);
		tascam->playback_urbs[i]->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
		tascam->playback_urbs[i]->interval = 1;
		tascam->playback_urbs[i]->complete = playback_complete;
		tascam->playback_urbs[i]->number_of_packets = ISO_PACKETS_PER_URB;

		for (j = 0; j < ISO_PACKETS_PER_URB; j++) {
			tascam->playback_urbs[i]->iso_frame_desc[j].offset = j * MAX_PACKET_SIZE;
			tascam->playback_urbs[i]->iso_frame_desc[j].length = MAX_PACKET_SIZE;
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
	tascam->capture_is_running = 0;
	tascam->playback_is_running = 0;
	spin_unlock_irq(&tascam->lock);

	usb_kill_anchored_urbs(&tascam->capture_anchor);
	usb_kill_anchored_urbs(&tascam->playback_anchor);

	snd_card_disconnect(card);

	for (i = 0; i < NUM_URBS; i++) {
		if (tascam->capture_urbs[i]) {
			usb_free_coherent(tascam->dev, ISO_PACKETS_PER_URB * MAX_PACKET_SIZE, tascam->capture_urbs[i]->transfer_buffer, tascam->capture_urbs[i]->transfer_dma);
			usb_free_urb(tascam->capture_urbs[i]);
		}
		if (tascam->playback_urbs[i]) {
			usb_free_coherent(tascam->dev, ISO_PACKETS_PER_URB * MAX_PACKET_SIZE, tascam->playback_urbs[i]->transfer_buffer, tascam->playback_urbs[i]->transfer_dma);
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
