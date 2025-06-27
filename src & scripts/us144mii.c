// SPDX-License-Identifier: GPL-2.0
// (c) 2025 serifpersia <ramiserifpersia@gmail.com>

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/delay.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

MODULE_AUTHOR("serifpersia");
MODULE_DESCRIPTION("ALSA Driver for TASCAM US-144MKII");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.4");

#define DRIVER_NAME "us144mkii"

/* --- LATENCY CONTROL PANEL --- */
/*
 * Set the desired latency profile by its index.
 * This setting is static and applies to all sample rates.
 *
 * Cheatsheet:
 * Index | Profile | Feedback Interval
 * ------|---------|--------------------
 *   0   | Lowest  | ~5ms
 *   1   | Low     | ~1ms
 *   2   | Normal  | ~2ms
 *   3   | High    | ~5ms
 *   4   | Highest | ~5ms
 */
#define LATENCY_PROFILE_INDEX 2 /* Default to Normal */

/* --- Internal Latency Configuration - Do not edit --- */
static const unsigned int latency_profile_packets[] = { 5, 1, 2, 5, 5 };

#if LATENCY_PROFILE_INDEX < 0 || LATENCY_PROFILE_INDEX >= 5
#error "Invalid LATENCY_PROFILE_INDEX. Please choose a value from 0 to 4."
#endif

#define CURRENT_LATENCY_PACKETS latency_profile_packets[LATENCY_PROFILE_INDEX]


#define TASCAM_VID 0x0644
#define TASCAM_PID 0x8020

#define EP_AUDIO_OUT         0x02
#define EP_PLAYBACK_FEEDBACK 0x81
#define EP_CAPTURE_DATA      0x86

#define RT_H2D_CLASS_EP   0x22
#define RT_H2D_VENDOR_DEV 0x40
#define RT_D2H_VENDOR_DEV 0xc0

#define UAC_SET_CUR 0x01
#define UAC_SAMPLING_FREQ_CONTROL 0x0100
#define VENDOR_REQ_REGISTER_WRITE 65
#define VENDOR_REQ_MODE_CONTROL   73

#define NUM_PLAYBACK_URBS 8
#define NUM_FEEDBACK_URBS 4

#define BYTES_PER_SAMPLE 3
#define DEVICE_CHANNELS 4
#define DEVICE_BYTES_PER_FRAME (DEVICE_CHANNELS * BYTES_PER_SAMPLE)

#define MAX_SUPPORTED_RATE 96000
#define USB_CTRL_TIMEOUT_MS 1000

#define PLAYBACK_URB_ISO_PACKETS 40
#define FEEDBACK_PACKET_SIZE 3
#define FEEDBACK_ACCUMULATOR_SIZE 128

static const unsigned int patterns_48khz[5][8] = {
	{5, 6, 6, 6, 5, 6, 6, 6}, {5, 6, 6, 6, 6, 6, 6, 6},
	{6, 6, 6, 6, 6, 6, 6, 6}, {7, 6, 6, 6, 6, 6, 6, 6},
	{7, 6, 6, 6, 7, 6, 6, 6}
};

static const unsigned int patterns_96khz[5][8] = {
	{11, 12, 12, 12, 11, 12, 12, 12}, {11, 12, 12, 12, 12, 12, 12, 12},
	{12, 12, 12, 12, 12, 12, 12, 12}, {13, 12, 12, 12, 12, 12, 12, 12},
	{13, 12, 12, 12, 13, 12, 12, 12}
};

static const unsigned int patterns_88khz[5][8] = {
	{10, 11, 11, 11, 10, 11, 11, 11}, {10, 11, 11, 11, 11, 11, 11, 11},
	{11, 11, 11, 11, 11, 11, 11, 11}, {12, 11, 11, 11, 11, 11, 11, 11},
	{12, 11, 11, 11, 12, 11, 11, 11}
};

static const unsigned int patterns_44khz[5][8] = {
	{5, 5, 5, 6, 5, 5, 5, 6}, {5, 5, 6, 5, 5, 6, 5, 6},
	{5, 6, 5, 6, 5, 6, 5, 6}, {6, 5, 6, 6, 5, 6, 5, 6},
	{6, 6, 6, 5, 6, 6, 6, 5}
};

static struct usb_driver tascam_alsa_driver;

struct tascam_card {
	struct usb_device *dev;
	struct usb_interface *iface0;
	struct usb_interface *iface1;
	struct snd_card *card;
	struct snd_pcm *pcm;

	struct snd_pcm_substream *playback_substream;
	struct urb *playback_urbs[NUM_PLAYBACK_URBS];
	size_t playback_urb_alloc_size;

	struct urb *feedback_urbs[NUM_FEEDBACK_URBS];
	size_t feedback_urb_alloc_size;

	spinlock_t lock;
	atomic_t playback_active;
	atomic_t feedback_active;

	unsigned int feedback_accumulator_pattern[FEEDBACK_ACCUMULATOR_SIZE];
	unsigned int feedback_pattern_out_idx;
	unsigned int feedback_pattern_in_idx;

	snd_pcm_uframes_t driver_playback_pos;
	snd_pcm_uframes_t playback_frames_consumed;

	const unsigned int (*feedback_patterns)[8];
	unsigned int feedback_base_value;
	unsigned int feedback_max_value;
	unsigned int feedback_urb_skip_count;
};

static int tascam_pcm_open(struct snd_pcm_substream *substream);
static int tascam_pcm_close(struct snd_pcm_substream *substream);
static int tascam_pcm_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params);
static int tascam_pcm_hw_free(struct snd_pcm_substream *substream);
static int tascam_pcm_prepare(struct snd_pcm_substream *substream);
static int tascam_pcm_trigger(struct snd_pcm_substream *substream, int cmd);
static snd_pcm_uframes_t tascam_pcm_pointer(struct snd_pcm_substream *substream);

static void playback_urb_complete(struct urb *urb);
static void feedback_urb_complete(struct urb *urb);
static int us144mkii_configure_device_for_rate(struct tascam_card *tascam, int rate);


// TODO: Fix DAW crashing when using this device
static const struct snd_pcm_hardware tascam_pcm_hw = {
	.info = (SNDRV_PCM_INFO_MMAP |
		 SNDRV_PCM_INFO_INTERLEAVED |
		 SNDRV_PCM_INFO_BLOCK_TRANSFER |
		 SNDRV_PCM_INFO_MMAP_VALID |
		 SNDRV_PCM_INFO_PAUSE |
		 SNDRV_PCM_INFO_RESUME),
	.formats = SNDRV_PCM_FMTBIT_S24_3LE,
	.rates = (SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
		  SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000),
	.rate_min = 44100,
	.rate_max = 96000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = 1024 * 1024,
	.period_bytes_min = 64,
	.period_bytes_max = 64 * 1024,
	.periods_min = 2,
	.periods_max = 1024,
};

static struct snd_pcm_ops tascam_playback_ops = {
	.open =		tascam_pcm_open,
	.close =	tascam_pcm_close,
	.ioctl =	snd_pcm_lib_ioctl,
	.hw_params =	tascam_pcm_hw_params,
	.hw_free =	tascam_pcm_hw_free,
	.prepare =	tascam_pcm_prepare,
	.trigger =	tascam_pcm_trigger,
	.pointer =	tascam_pcm_pointer,
};

static int tascam_capture_open_stub(struct snd_pcm_substream *substream) { return -ENODEV; }
static int tascam_capture_close_stub(struct snd_pcm_substream *substream) { return 0; }
static struct snd_pcm_ops tascam_capture_ops = {
	.open =		tascam_capture_open_stub,
	.close =	tascam_capture_close_stub,
};

static void tascam_card_private_free(struct snd_card *card)
{
	struct tascam_card *tascam = card->private_data;
	if (tascam && tascam->dev) {
		usb_put_dev(tascam->dev);
		tascam->dev = NULL;
	}
}

static int us144mkii_configure_device_for_rate(struct tascam_card *tascam, int rate)
{
	struct usb_device *dev = tascam->dev;
	u8 *rate_payload_buf;
	u16 rate_vendor_wValue;
	int err = 0;

	static const u8 payload_44100[] = {0x44, 0xac, 0x00};
	static const u8 payload_48000[] = {0x80, 0xbb, 0x00};
	static const u8 payload_88200[] = {0x88, 0x58, 0x01};
	static const u8 payload_96000[] = {0x00, 0x77, 0x01};
	const u8 *current_payload_src;

	rate_payload_buf = kmalloc(3, GFP_KERNEL);
	if (!rate_payload_buf) return -ENOMEM;

	switch (rate) {
	case 44100: current_payload_src = payload_44100; rate_vendor_wValue = 0x1000; break;
	case 48000: current_payload_src = payload_48000; rate_vendor_wValue = 0x1002; break;
	case 88200: current_payload_src = payload_88200; rate_vendor_wValue = 0x1008; break;
	case 96000: current_payload_src = payload_96000; rate_vendor_wValue = 0x100a; break;
	default:
		dev_err(&dev->dev, "Unsupported sample rate %d for configuration\n", rate);
		kfree(rate_payload_buf);
		return -EINVAL;
	}

	memcpy(rate_payload_buf, current_payload_src, 3);

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_MODE_CONTROL, RT_H2D_VENDOR_DEV, 0x0010, 0x0000, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) { goto cleanup_buf; }

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), UAC_SET_CUR, RT_H2D_CLASS_EP, UAC_SAMPLING_FREQ_CONTROL, EP_CAPTURE_DATA, rate_payload_buf, 3, USB_CTRL_TIMEOUT_MS);
	if (err < 0) { goto cleanup_buf; }
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), UAC_SET_CUR, RT_H2D_CLASS_EP, UAC_SAMPLING_FREQ_CONTROL, EP_AUDIO_OUT, rate_payload_buf, 3, USB_CTRL_TIMEOUT_MS);
	if (err < 0) { goto cleanup_buf; }

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, 0x0d04, 0x0101, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) { goto cleanup_buf; }
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, 0x0e00, 0x0101, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) { goto cleanup_buf; }
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, 0x0f00, 0x0101, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) { goto cleanup_buf; }

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, rate_vendor_wValue, 0x0101, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) { goto cleanup_buf; }

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, 0x110b, 0x0101, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) { goto cleanup_buf; }

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_MODE_CONTROL, RT_H2D_VENDOR_DEV, 0x0030, 0x0000, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) { goto cleanup_buf; }

cleanup_buf:
	kfree(rate_payload_buf);
	return err;
}

static int tascam_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct tascam_card *tascam;
	struct snd_card *card;
	struct snd_pcm *pcm;
	int err;
	u8 *handshake_buf;

	if (intf->cur_altsetting->desc.bInterfaceNumber != 0)
		return -ENODEV;

	err = snd_card_new(&intf->dev, -1, "US144MKII", THIS_MODULE,
			   sizeof(struct tascam_card), &card);
	if (err < 0) {
		dev_err(&intf->dev, "Failed to create snd_card: %d\n", err);
		return err;
	}

	tascam = card->private_data;
	tascam->card = card;
	tascam->dev = usb_get_dev(dev);
	tascam->iface0 = intf;
	card->private_free = tascam_card_private_free;
	usb_set_intfdata(intf, tascam);
	spin_lock_init(&tascam->lock);
	atomic_set(&tascam->playback_active, 0);
	atomic_set(&tascam->feedback_active, 0);

	strscpy(card->driver, DRIVER_NAME, sizeof(card->driver));
	strscpy(card->shortname, "TASCAM US-144MKII", sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname), "%s (VID:%04X PID:%04X) at %s IF%d",
		 "TASCAM US-144MKII",
		 le16_to_cpu(dev->descriptor.idVendor),
		 le16_to_cpu(dev->descriptor.idProduct),
		 dev->bus->bus_name,
		 intf->cur_altsetting->desc.bInterfaceNumber);

	tascam->iface1 = usb_ifnum_to_if(dev, 1);
	if (!tascam->iface1) {
		dev_err(&intf->dev, "Interface 1 not found.\n");
		err = -ENODEV;
		goto free_card_obj;
	}
	err = usb_driver_claim_interface(&tascam_alsa_driver, tascam->iface1, tascam);
	if (err < 0) {
		dev_err(&intf->dev, "Could not claim interface 1: %d\n", err);
		tascam->iface1 = NULL;
		goto free_card_obj;
	}

	err = usb_set_interface(dev, 0, 1);
	if (err < 0) { dev_err(&intf->dev, "Set Alt Setting on Intf 0 failed: %d\n", err); goto release_iface1_and_free_card; }
	err = usb_set_interface(dev, 1, 1);
	if (err < 0) { dev_err(&intf->dev, "Set Alt Setting on Intf 1 failed: %d\n", err); goto release_iface1_and_free_card; }

	handshake_buf = kmalloc(1, GFP_KERNEL);
	if (!handshake_buf) { err = -ENOMEM; goto release_iface1_and_free_card; }
	err = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0), VENDOR_REQ_MODE_CONTROL,
			      RT_D2H_VENDOR_DEV, 0x0000, 0x0000,
			      handshake_buf, 1, USB_CTRL_TIMEOUT_MS);
	if (err < 0) {
		dev_warn(&intf->dev, "Handshake read failed: %d (continuing)\n", err);
	} else if (err == 1 && handshake_buf[0] == 0x12) {
		dev_info(&intf->dev, "Handshake successful (response 0x12).\n");
	} else {
		dev_warn(&intf->dev, "Handshake: expected 0x12, got 0x%02x (len %d) (continuing)\n", handshake_buf[0], err);
	}
	kfree(handshake_buf);
	handshake_buf = NULL;

	err = us144mkii_configure_device_for_rate(tascam, 48000);
	if (err < 0) {
		dev_err(&intf->dev, "Initial device configuration for 48kHz failed: %d\n", err);
		goto release_iface1_and_free_card;
	}

	err = snd_pcm_new(card, "US144MKII PCM", 0, 1, 1, &pcm);
	if (err < 0) {
		dev_err(&intf->dev, "Failed to create snd_pcm: %d\n", err);
		goto release_iface1_and_free_card;
	}
	tascam->pcm = pcm;
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &tascam_playback_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &tascam_capture_ops);
	pcm->private_data = tascam;
	strscpy(pcm->name, "US-144MKII Audio", sizeof(pcm->name));

	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_CONTINUOUS,
					      usb_ifnum_to_if(dev,0)->dev.parent,
					      64 * 1024,
					      tascam_pcm_hw.buffer_bytes_max);

	err = snd_card_register(card);
	if (err < 0) {
		dev_err(&intf->dev, "Failed to register snd_card: %d\n", err);
		goto release_iface1_and_free_card;
	}

	dev_info(&intf->dev, "%s: TASCAM US-144MKII ALSA driver initialized.\n", card->longname);
	return 0;

release_iface1_and_free_card:
	if (tascam->iface1) {
		usb_driver_release_interface(&tascam_alsa_driver, tascam->iface1);
		tascam->iface1 = NULL;
	}
free_card_obj:
	snd_card_free(card);
	return err;
}

static void tascam_disconnect(struct usb_interface *intf)
{
	struct tascam_card *tascam = usb_get_intfdata(intf);

	if (!tascam)
		return;

	if (intf == tascam->iface0) {
		atomic_set(&tascam->playback_active, 0);
		atomic_set(&tascam->feedback_active, 0);

		snd_card_disconnect(tascam->card);

		if (tascam->iface1) {
			usb_driver_release_interface(&tascam_alsa_driver, tascam->iface1);
			tascam->iface1 = NULL;
		}

		snd_card_free_when_closed(tascam->card);
	}
}

static void playback_urb_complete(struct urb *urb)
{
	struct tascam_card *tascam = urb->context;
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	unsigned long flags;
	int ret, i, f;
	char *urb_buf_ptr = urb->transfer_buffer;
	size_t alsa_frame_bytes;

	if (urb->status == -ENOENT || urb->status == -ECONNRESET || urb->status == -ESHUTDOWN)
		return;

	if (!tascam || !atomic_read(&tascam->playback_active))
		return;

	substream = tascam->playback_substream;
	if (!substream)
		return;
	runtime = substream->runtime;
	if (!runtime)
		return;

	alsa_frame_bytes = runtime->frame_bits / 8;

	spin_lock_irqsave(&tascam->lock, flags);

	for (i = 0; i < PLAYBACK_URB_ISO_PACKETS; i++) {
		unsigned int frames_for_packet;
		size_t bytes_for_packet;

		frames_for_packet = tascam->feedback_accumulator_pattern[tascam->feedback_pattern_out_idx];
		tascam->feedback_pattern_out_idx = (tascam->feedback_pattern_out_idx + 1) % FEEDBACK_ACCUMULATOR_SIZE;

		for (f = 0; f < frames_for_packet; f++) {
			size_t alsa_pos_bytes = frames_to_bytes(runtime, tascam->driver_playback_pos);
			char *alsa_frame_ptr = runtime->dma_area + alsa_pos_bytes;

			memcpy(urb_buf_ptr, alsa_frame_ptr, alsa_frame_bytes);
			memset(urb_buf_ptr + alsa_frame_bytes, 0, alsa_frame_bytes);

			urb_buf_ptr += DEVICE_BYTES_PER_FRAME;
			tascam->driver_playback_pos++;
			if (tascam->driver_playback_pos >= runtime->buffer_size)
				tascam->driver_playback_pos = 0;
		}

		bytes_for_packet = frames_for_packet * DEVICE_BYTES_PER_FRAME;
		urb->iso_frame_desc[i].offset = (urb_buf_ptr - (char *)urb->transfer_buffer) - bytes_for_packet;
		urb->iso_frame_desc[i].length = bytes_for_packet;
	}

	spin_unlock_irqrestore(&tascam->lock, flags);

	urb->transfer_buffer_length = urb_buf_ptr - (char *)urb->transfer_buffer;

	if (atomic_read(&tascam->playback_active)) {
		urb->dev = tascam->dev;
		ret = usb_submit_urb(urb, GFP_ATOMIC);
		if (ret < 0)
			dev_err_ratelimited(tascam->card->dev, "Failed to resubmit playback URB: %d\n", ret);
	}
}

static void feedback_urb_complete(struct urb *urb)
{
	struct tascam_card *tascam = urb->context;
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	unsigned long flags;
	int ret, i, p;
	unsigned int frames_consumed_in_urb = 0;

	if (urb->status == -ENOENT || urb->status == -ECONNRESET || urb->status == -ESHUTDOWN)
		return;

	if (!tascam || !atomic_read(&tascam->feedback_active))
		return;

	substream = tascam->playback_substream;
	if (!substream)
		return;
	runtime = substream->runtime;
	if (!runtime)
		return;

	spin_lock_irqsave(&tascam->lock, flags);
	if (tascam->feedback_urb_skip_count > 0) {
		tascam->feedback_urb_skip_count--;
		spin_unlock_irqrestore(&tascam->lock, flags);
		goto resubmit;
	}
	spin_unlock_irqrestore(&tascam->lock, flags);

	if (urb->status != 0)
		goto resubmit;

	spin_lock_irqsave(&tascam->lock, flags);
	for (p = 0; p < urb->number_of_packets; p++) {
		u8 feedback_value;
		const unsigned int *pattern;
		int pattern_index;
		unsigned int frames_this_ms = 0;

		if (urb->iso_frame_desc[p].actual_length < 1)
			continue;

		feedback_value = *((u8 *)urb->transfer_buffer + urb->iso_frame_desc[p].offset);

		if (feedback_value >= tascam->feedback_base_value &&
		    feedback_value <= tascam->feedback_max_value) {
			pattern_index = feedback_value - tascam->feedback_base_value;
			pattern = tascam->feedback_patterns[pattern_index];
		} else {
			pattern_index = 2;
			pattern = tascam->feedback_patterns[pattern_index];
		}

		for (i = 0; i < 8; i++) {
			unsigned int in_idx = (tascam->feedback_pattern_in_idx + i) % FEEDBACK_ACCUMULATOR_SIZE;
			tascam->feedback_accumulator_pattern[in_idx] = pattern[i];
			frames_this_ms += pattern[i];
		}
		tascam->feedback_pattern_in_idx = (tascam->feedback_pattern_in_idx + 8) % FEEDBACK_ACCUMULATOR_SIZE;
		frames_consumed_in_urb += frames_this_ms;
	}

	tascam->playback_frames_consumed += frames_consumed_in_urb;
	if (tascam->playback_frames_consumed >= runtime->buffer_size)
		tascam->playback_frames_consumed %= runtime->buffer_size;

	spin_unlock_irqrestore(&tascam->lock, flags);

	snd_pcm_period_elapsed(substream);

resubmit:
	if (atomic_read(&tascam->feedback_active)) {
		urb->dev = tascam->dev;
		ret = usb_submit_urb(urb, GFP_ATOMIC);
		if (ret < 0)
			dev_err_ratelimited(tascam->card->dev, "Failed to resubmit feedback URB: %d\n", ret);
	}
}

static int tascam_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	unsigned long flags;
	int err = 0;
	int i;
	bool start_streaming = false;

	spin_lock_irqsave(&tascam->lock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		if (!atomic_read(&tascam->playback_active)) {
			atomic_set(&tascam->playback_active, 1);
			atomic_set(&tascam->feedback_active, 1);
			start_streaming = true;
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		atomic_set(&tascam->playback_active, 0);
		atomic_set(&tascam->feedback_active, 0);
		break;
	default:
		err = -EINVAL;
		break;
	}
	spin_unlock_irqrestore(&tascam->lock, flags);

	if (err < 0)
		return err;

	if (start_streaming) {
		for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
			err = usb_submit_urb(tascam->playback_urbs[i], GFP_ATOMIC);
			if (err < 0)
				goto stop_and_kill;
		}
		for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
			err = usb_submit_urb(tascam->feedback_urbs[i], GFP_ATOMIC);
			if (err < 0)
				goto stop_and_kill;
		}
	} else {
stop_and_kill:
		if (start_streaming) {
			atomic_set(&tascam->playback_active, 0);
			atomic_set(&tascam->feedback_active, 0);
		}
		for (i = 0; i < NUM_PLAYBACK_URBS; i++)
			usb_kill_urb(tascam->playback_urbs[i]);
		for (i = 0; i < NUM_FEEDBACK_URBS; i++)
			usb_kill_urb(tascam->feedback_urbs[i]);
	}
	return err;
}

static snd_pcm_uframes_t tascam_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	return tascam->playback_frames_consumed;
}

static int tascam_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned long flags;
	int i, u;
	size_t nominal_frames_per_packet;
	size_t nominal_bytes_per_packet;
	size_t total_bytes_in_urb;

	spin_lock_irqsave(&tascam->lock, flags);

	tascam->driver_playback_pos = 0;
	tascam->playback_frames_consumed = 0;
	tascam->feedback_pattern_in_idx = 0;
	tascam->feedback_pattern_out_idx = 0;
	tascam->feedback_urb_skip_count = NUM_FEEDBACK_URBS * 2;

	nominal_frames_per_packet = runtime->rate / 8000;
	for (i = 0; i < FEEDBACK_ACCUMULATOR_SIZE; i++)
		tascam->feedback_accumulator_pattern[i] = nominal_frames_per_packet;

	nominal_bytes_per_packet = nominal_frames_per_packet * DEVICE_BYTES_PER_FRAME;
	total_bytes_in_urb = nominal_bytes_per_packet * PLAYBACK_URB_ISO_PACKETS;

	if (total_bytes_in_urb > tascam->playback_urb_alloc_size) {
		dev_err(tascam->card->dev, "Prepare: Nominal URB size %zu > allocated %zu\n",
			total_bytes_in_urb, tascam->playback_urb_alloc_size);
		spin_unlock_irqrestore(&tascam->lock, flags);
		return -EINVAL;
	}

	for (u = 0; u < NUM_PLAYBACK_URBS; u++) {
		struct urb *urb = tascam->playback_urbs[u];

		memset(urb->transfer_buffer, 0, total_bytes_in_urb);
		urb->transfer_buffer_length = total_bytes_in_urb;

		for (i = 0; i < PLAYBACK_URB_ISO_PACKETS; i++) {
			urb->iso_frame_desc[i].offset = i * nominal_bytes_per_packet;
			urb->iso_frame_desc[i].length = nominal_bytes_per_packet;
		}
	}

	spin_unlock_irqrestore(&tascam->lock, flags);

	return 0;
}

static int tascam_pcm_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	int err;
	unsigned int rate = params_rate(params);

	switch (rate) {
	case 44100:
		tascam->feedback_patterns = patterns_44khz;
		tascam->feedback_base_value = 42;
		tascam->feedback_max_value = 46;
		break;
	case 48000:
		tascam->feedback_patterns = patterns_48khz;
		tascam->feedback_base_value = 46;
		tascam->feedback_max_value = 50;
		break;
	case 88200:
		tascam->feedback_patterns = patterns_88khz;
		tascam->feedback_base_value = 86;
		tascam->feedback_max_value = 90;
		break;
	case 96000:
		tascam->feedback_patterns = patterns_96khz;
		tascam->feedback_base_value = 94;
		tascam->feedback_max_value = 98;
		break;
	default:
		dev_err(tascam->card->dev, "Unsupported sample rate for feedback: %u\n", rate);
		return -EINVAL;
	}

	err = us144mkii_configure_device_for_rate(tascam, rate);
	if (err < 0) {
		dev_err(tascam->card->dev, "Failed to set hardware rate to %u: %d\n", rate, err);
		return err;
	}

	return snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(params));
}

static int tascam_pcm_hw_free(struct snd_pcm_substream *substream)
{
	return snd_pcm_lib_free_pages(substream);
}

static int tascam_pcm_open(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int i, j;
	size_t max_frames_per_packet, max_packet_size;

	runtime->hw = tascam_pcm_hw;

	tascam->playback_substream = substream;
	atomic_set(&tascam->playback_active, 0);
	atomic_set(&tascam->feedback_active, 0);

	max_frames_per_packet = (MAX_SUPPORTED_RATE / 8000) + 2;
	max_packet_size = max_frames_per_packet * DEVICE_BYTES_PER_FRAME;
	tascam->playback_urb_alloc_size = max_packet_size * PLAYBACK_URB_ISO_PACKETS;

	if (tascam->playback_urb_alloc_size == 0) {
		dev_err(tascam->card->dev, "Calculated playback_urb_alloc_size is zero.\n");
		return -EINVAL;
	}

	for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
		struct urb *urb = usb_alloc_urb(PLAYBACK_URB_ISO_PACKETS, GFP_KERNEL);
		if (!urb)
			goto error_free_urbs;
		tascam->playback_urbs[i] = urb;

		urb->transfer_buffer = usb_alloc_coherent(tascam->dev, tascam->playback_urb_alloc_size,
						    GFP_KERNEL, &urb->transfer_dma);
		if (!urb->transfer_buffer) {
			goto error_free_urbs;
		}
		urb->dev = tascam->dev;
		urb->pipe = usb_sndisocpipe(tascam->dev, EP_AUDIO_OUT);
		urb->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
		urb->interval = 1;
		urb->context = tascam;
		urb->complete = playback_urb_complete;
		urb->number_of_packets = PLAYBACK_URB_ISO_PACKETS;
	}

	tascam->feedback_urb_alloc_size = FEEDBACK_PACKET_SIZE * CURRENT_LATENCY_PACKETS;

	for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
		struct urb *f_urb = usb_alloc_urb(CURRENT_LATENCY_PACKETS, GFP_KERNEL);
		if (!f_urb)
			goto error_free_urbs;
		tascam->feedback_urbs[i] = f_urb;

		f_urb->transfer_buffer = usb_alloc_coherent(tascam->dev, tascam->feedback_urb_alloc_size,
						      GFP_KERNEL, &f_urb->transfer_dma);
		if (!f_urb->transfer_buffer)
			goto error_free_urbs;

		f_urb->dev = tascam->dev;
		f_urb->pipe = usb_rcvisocpipe(tascam->dev, EP_PLAYBACK_FEEDBACK);
		f_urb->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
		f_urb->interval = 4;
		f_urb->context = tascam;
		f_urb->complete = feedback_urb_complete;
		f_urb->number_of_packets = CURRENT_LATENCY_PACKETS;
		f_urb->transfer_buffer_length = tascam->feedback_urb_alloc_size;
		for (j = 0; j < CURRENT_LATENCY_PACKETS; j++) {
			f_urb->iso_frame_desc[j].offset = j * FEEDBACK_PACKET_SIZE;
			f_urb->iso_frame_desc[j].length = FEEDBACK_PACKET_SIZE;
		}
	}

	return 0;

error_free_urbs:
	for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
		if (tascam->feedback_urbs[i]) {
			usb_free_coherent(tascam->dev, tascam->feedback_urb_alloc_size,
					  tascam->feedback_urbs[i]->transfer_buffer,
					  tascam->feedback_urbs[i]->transfer_dma);
			usb_free_urb(tascam->feedback_urbs[i]);
			tascam->feedback_urbs[i] = NULL;
		}
	}
	for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
		if (tascam->playback_urbs[i]) {
			usb_free_coherent(tascam->dev, tascam->playback_urb_alloc_size,
					  tascam->playback_urbs[i]->transfer_buffer,
					  tascam->playback_urbs[i]->transfer_dma);
			usb_free_urb(tascam->playback_urbs[i]);
			tascam->playback_urbs[i] = NULL;
		}
	}
	return -ENOMEM;
}

static int tascam_pcm_close(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	int i;

	atomic_set(&tascam->playback_active, 0);
	atomic_set(&tascam->feedback_active, 0);

	for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
		if (tascam->playback_urbs[i]) {
			usb_kill_urb(tascam->playback_urbs[i]);
			usb_free_coherent(tascam->dev, tascam->playback_urb_alloc_size,
					  tascam->playback_urbs[i]->transfer_buffer,
					  tascam->playback_urbs[i]->transfer_dma);
			usb_free_urb(tascam->playback_urbs[i]);
			tascam->playback_urbs[i] = NULL;
		}
	}

	for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
		if (tascam->feedback_urbs[i]) {
			usb_kill_urb(tascam->feedback_urbs[i]);
			usb_free_coherent(tascam->dev, tascam->feedback_urb_alloc_size,
					  tascam->feedback_urbs[i]->transfer_buffer,
					  tascam->feedback_urbs[i]->transfer_dma);
			usb_free_urb(tascam->feedback_urbs[i]);
			tascam->feedback_urbs[i] = NULL;
		}
	}
	tascam->playback_substream = NULL;
	return 0;
}

static const struct usb_device_id tascam_id_table[] = {
	{ USB_DEVICE(TASCAM_VID, TASCAM_PID) },
	{ }
};
MODULE_DEVICE_TABLE(usb, tascam_id_table);

static struct usb_driver tascam_alsa_driver = {
	.name =		DRIVER_NAME,
	.probe =	tascam_probe,
	.disconnect =	tascam_disconnect,
	.id_table =	tascam_id_table,
};

module_usb_driver(tascam_alsa_driver);
