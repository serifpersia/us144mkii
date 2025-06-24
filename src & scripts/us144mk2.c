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
MODULE_VERSION("1.0");

#define DRIVER_NAME "us144mkii"

// --- Device and Endpoint Configuration ---
#define TASCAM_VID 0x0644
#define TASCAM_PID 0x8020

#define EP_AUDIO_OUT         0x02
#define EP_PLAYBACK_FEEDBACK 0x81
#define EP_CAPTURE_DATA      0x86

// --- USB Request Types ---
#define RT_H2D_CLASS_EP   0x22
#define RT_H2D_VENDOR_DEV 0x40
#define RT_D2H_VENDOR_DEV 0xc0

// --- UAC / Vendor Requests ---
#define UAC_SET_CUR 0x01
#define UAC_SAMPLING_FREQ_CONTROL 0x0100
#define VENDOR_REQ_REGISTER_WRITE 65
#define VENDOR_REQ_MODE_CONTROL   73

// --- Streaming Configuration ---
#define NUM_PLAYBACK_URBS 8
#define NUM_FEEDBACK_URBS 4
#define NUM_ISO_PACKETS 8

#define BYTES_PER_SAMPLE 3
#define DEVICE_CHANNELS 4
#define PLAYBACK_BYTES_PER_FRAME (DEVICE_CHANNELS * BYTES_PER_SAMPLE)

#define FEEDBACK_BYTES_PER_PACKET 3
#define MAX_SUPPORTED_RATE 96000
#define USB_CTRL_TIMEOUT_MS 1000

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
	int p_iso_packet_size;

	struct urb *feedback_urbs[NUM_FEEDBACK_URBS];
	size_t feedback_urb_alloc_size;
	int f_iso_packet_size;

	spinlock_t playback_lock;
	atomic_t playback_active;
	atomic_t feedback_active;
	snd_pcm_uframes_t playback_pos;
};

// --- Forward Declarations ---
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
	.channels_min = DEVICE_CHANNELS,
	.channels_max = DEVICE_CHANNELS,
	.buffer_bytes_max = 256 * 1024,
	.period_bytes_min = 256,
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

	dev_info(&dev->dev, "Configuring device for rate %d Hz\n", rate);

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
	if (err < 0) { dev_err(&dev->dev, "Set Initial Mode (0x0010) failed: %d\n", err); goto cleanup_buf; }

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), UAC_SET_CUR, RT_H2D_CLASS_EP, UAC_SAMPLING_FREQ_CONTROL, EP_CAPTURE_DATA, rate_payload_buf, 3, USB_CTRL_TIMEOUT_MS);
	if (err < 0) { dev_err(&dev->dev, "Set Rate on Capture EP (0x%02x) failed: %d\n", EP_CAPTURE_DATA, err); goto cleanup_buf; }
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), UAC_SET_CUR, RT_H2D_CLASS_EP, UAC_SAMPLING_FREQ_CONTROL, EP_AUDIO_OUT, rate_payload_buf, 3, USB_CTRL_TIMEOUT_MS);
	if (err < 0) { dev_err(&dev->dev, "Set Rate on Playback EP (0x%02x) failed: %d\n", EP_AUDIO_OUT, err); goto cleanup_buf; }

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, 0x0d04, 0x0101, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) { dev_err(&dev->dev, "Reg Write 0x0d04 failed: %d\n", err); goto cleanup_buf; }
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, 0x0e00, 0x0101, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) { dev_err(&dev->dev, "Reg Write 0x0e00 failed: %d\n", err); goto cleanup_buf; }
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, 0x0f00, 0x0101, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) { dev_err(&dev->dev, "Reg Write 0x0f00 failed: %d\n", err); goto cleanup_buf; }

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, rate_vendor_wValue, 0x0101, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) { dev_err(&dev->dev, "Rate-Dep Reg Write (0x%04x) failed: %d\n", rate_vendor_wValue, err); goto cleanup_buf; }

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, 0x110b, 0x0101, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) { dev_err(&dev->dev, "Reg Write 0x110b failed: %d\n", err); goto cleanup_buf; }

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_MODE_CONTROL, RT_H2D_VENDOR_DEV, 0x0030, 0x0000, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) { dev_err(&dev->dev, "Enable Streaming (0x0030) failed: %d\n", err); goto cleanup_buf; }

	dev_info(&dev->dev, "Device configuration for rate %d Hz completed.\n", rate);

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
	spin_lock_init(&tascam->playback_lock);
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
	dev_info(&intf->dev, "Interfaces 0 and 1 set to altsetting 1.\n");

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

	if (!tascam) {
		dev_warn(&intf->dev, "Disconnect called on interface with no private data.\n");
		return;
	}

	if (intf == tascam->iface0) {
		dev_info(&intf->dev, "Disconnecting TASCAM US-144MKII (iface0)...\n");
		atomic_set(&tascam->playback_active, 0);
		atomic_set(&tascam->feedback_active, 0);

		snd_card_disconnect(tascam->card);

		if (tascam->iface1) {
			dev_info(&intf->dev, "Releasing claimed interface 1.\n");
			usb_driver_release_interface(&tascam_alsa_driver, tascam->iface1);
			tascam->iface1 = NULL;
		}

		snd_card_free_when_closed(tascam->card);
		dev_info(&intf->dev, "TASCAM US-144MKII (iface0) disconnected and scheduled for freeing.\n");
	} else if (intf == tascam->iface1) {
		dev_info(&intf->dev, "Disconnecting TASCAM US-144MKII (iface1). Data already cleared by iface0 disconnect.\n");
	}
}


static void playback_urb_complete(struct urb *urb)
{
	struct tascam_card *tascam = urb->context;
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	unsigned long flags;
	int ret;

	if (urb->status == -ENOENT || urb->status == -ECONNRESET || urb->status == -ESHUTDOWN) {
		return;
	}

	if (!tascam || !atomic_read(&tascam->playback_active)) {
		return;
	}
	
	substream = tascam->playback_substream;
	if (!substream) return;
	runtime = substream->runtime;
	if (!runtime) return;

	if (urb->status != 0) {
		dev_warn_ratelimited(tascam->card->dev, "Playback URB completed with status %d\n", urb->status);
	}

	spin_lock_irqsave(&tascam->playback_lock, flags);

	for (int i = 0; i < NUM_ISO_PACKETS; ++i) {
		size_t dma_pos_bytes;
		size_t current_iso_packet_len = tascam->p_iso_packet_size;
		u8 *urb_packet_buffer = urb->transfer_buffer + (i * current_iso_packet_len);

		dma_pos_bytes = frames_to_bytes(runtime, tascam->playback_pos);

		if (dma_pos_bytes + current_iso_packet_len > runtime->dma_bytes) {
			size_t len1 = runtime->dma_bytes - dma_pos_bytes;
			memcpy(urb_packet_buffer, runtime->dma_area + dma_pos_bytes, len1);
			memcpy(urb_packet_buffer + len1, runtime->dma_area, current_iso_packet_len - len1);
		} else {
			memcpy(urb_packet_buffer, runtime->dma_area + dma_pos_bytes, current_iso_packet_len);
		}
		tascam->playback_pos += bytes_to_frames(runtime, current_iso_packet_len);
		if (tascam->playback_pos >= runtime->buffer_size) {
			tascam->playback_pos -= runtime->buffer_size;
		}
	}
	spin_unlock_irqrestore(&tascam->playback_lock, flags);

	if (atomic_read(&tascam->playback_active)) {
		urb->dev = tascam->dev;
		ret = usb_submit_urb(urb, GFP_ATOMIC);
		if (ret < 0) {
			dev_err_ratelimited(tascam->card->dev, "Failed to resubmit playback URB: %d\n", ret);
		}
	}
	snd_pcm_period_elapsed(substream);
}

static void feedback_urb_complete(struct urb *urb)
{
	struct tascam_card *tascam = urb->context;
	int ret;

	if (urb->status == -ENOENT || urb->status == -ECONNRESET || urb->status == -ESHUTDOWN) {
		return;
	}

	if (!tascam || !atomic_read(&tascam->feedback_active)) {
		return;
	}

	if (urb->status != 0) {
		dev_warn_ratelimited(tascam->card->dev, "Feedback URB completed with status %d\n", urb->status);
	}

	if (atomic_read(&tascam->feedback_active)) {
		urb->dev = tascam->dev;
		ret = usb_submit_urb(urb, GFP_ATOMIC);
		if (ret < 0) {
			dev_err_ratelimited(tascam->card->dev, "Failed to resubmit feedback URB: %d\n", ret);
		}
	}
}


static int tascam_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	unsigned long flags;
	int err = 0;
	int i;

	spin_lock_irqsave(&tascam->playback_lock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		if (atomic_read(&tascam->playback_active)) {
			err = -EBUSY;
			break;
		}
		tascam->playback_pos = 0;
		atomic_set(&tascam->playback_active, 1);
		atomic_set(&tascam->feedback_active, 1);
		dev_info(tascam->card->dev, "PCM TRIGGER START/RESUME\n");
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		if (!atomic_read(&tascam->playback_active)) {
			err = 0;
			break;
		}
		atomic_set(&tascam->playback_active, 0);
		atomic_set(&tascam->feedback_active, 0);
		dev_info(tascam->card->dev, "PCM TRIGGER STOP/SUSPEND/PAUSE\n");
		break;
	default:
		err = -EINVAL;
		break;
	}
	spin_unlock_irqrestore(&tascam->playback_lock, flags);

	if (err < 0) return err;

	if (cmd == SNDRV_PCM_TRIGGER_START || cmd == SNDRV_PCM_TRIGGER_RESUME) {
		for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
			err = usb_submit_urb(tascam->playback_urbs[i], GFP_ATOMIC);
			if (err < 0) {
				dev_err(tascam->card->dev, "Failed to submit playback URB %d: %d\n", i, err);
				atomic_set(&tascam->playback_active, 0);
				atomic_set(&tascam->feedback_active, 0);
				for (int j = 0; j < i; j++) usb_kill_urb(tascam->playback_urbs[j]);
				return err;
			}
		}
		for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
			err = usb_submit_urb(tascam->feedback_urbs[i], GFP_ATOMIC);
			if (err < 0) {
				dev_err(tascam->card->dev, "Failed to submit feedback URB %d: %d\n", i, err);
				atomic_set(&tascam->playback_active, 0);
				atomic_set(&tascam->feedback_active, 0);
				for (int j = 0; j < NUM_PLAYBACK_URBS; j++) usb_kill_urb(tascam->playback_urbs[j]);
				for (int j = 0; j < i; j++) usb_kill_urb(tascam->feedback_urbs[j]);
				return err;
			}
		}
	} else if (cmd == SNDRV_PCM_TRIGGER_STOP || cmd == SNDRV_PCM_TRIGGER_SUSPEND || cmd == SNDRV_PCM_TRIGGER_PAUSE_PUSH) {
		for (i = 0; i < NUM_PLAYBACK_URBS; i++)
			if (tascam->playback_urbs[i]) usb_kill_urb(tascam->playback_urbs[i]);
		for (i = 0; i < NUM_FEEDBACK_URBS; i++)
			if (tascam->feedback_urbs[i]) usb_kill_urb(tascam->feedback_urbs[i]);
	}
	return 0;
}

static snd_pcm_uframes_t tascam_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	return tascam->playback_pos;
}

static int tascam_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	size_t total_urb_transfer_bytes;
	size_t individual_iso_packet_size;
	int i, j;

	dev_info(tascam->card->dev, "PCM PREPARE: rate=%d, channels=%d, format=%d, period_size=%ld, periods=%d\n",
		 runtime->rate, runtime->channels, runtime->format, runtime->period_size, runtime->periods);

	total_urb_transfer_bytes = (runtime->rate / 1000) * PLAYBACK_BYTES_PER_FRAME;

	if (NUM_ISO_PACKETS == 0) {
		dev_err(tascam->card->dev, "NUM_ISO_PACKETS is zero, invalid configuration.\n");
		return -EINVAL;
	}
	individual_iso_packet_size = total_urb_transfer_bytes / NUM_ISO_PACKETS;

	if (individual_iso_packet_size == 0 && total_urb_transfer_bytes > 0) {
		dev_err(tascam->card->dev, "Zero individual ISO packet size. Rate %d, BPF %d, N_ISO %d\n",
			runtime->rate, PLAYBACK_BYTES_PER_FRAME, NUM_ISO_PACKETS);
		return -EINVAL;
	}
	tascam->p_iso_packet_size = individual_iso_packet_size;

	for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
		struct urb *urb = tascam->playback_urbs[i];
		if (!urb) continue;

		if (total_urb_transfer_bytes > tascam->playback_urb_alloc_size) {
			dev_err(tascam->card->dev, "Playback URB transfer size %zu > allocated %zu\n",
				total_urb_transfer_bytes, tascam->playback_urb_alloc_size);
			return -EINVAL;
		}
		memset(urb->transfer_buffer, 0, total_urb_transfer_bytes);
		urb->transfer_buffer_length = total_urb_transfer_bytes;
		urb->number_of_packets = NUM_ISO_PACKETS;

		for (j = 0; j < NUM_ISO_PACKETS; j++) {
			urb->iso_frame_desc[j].offset = j * tascam->p_iso_packet_size;
			urb->iso_frame_desc[j].length = tascam->p_iso_packet_size;
		}
	}

	tascam->f_iso_packet_size = FEEDBACK_BYTES_PER_PACKET;
	for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
		struct urb *f_urb = tascam->feedback_urbs[i];
		if (!f_urb) continue;
		f_urb->transfer_buffer_length = tascam->f_iso_packet_size * NUM_ISO_PACKETS;
		f_urb->number_of_packets = NUM_ISO_PACKETS;
		for (j = 0; j < NUM_ISO_PACKETS; j++) {
			f_urb->iso_frame_desc[j].offset = j * tascam->f_iso_packet_size;
			f_urb->iso_frame_desc[j].length = tascam->f_iso_packet_size;
		}
	}
	
	tascam->playback_pos = 0;
	return 0;
}

static int tascam_pcm_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	int err;

	dev_info(tascam->card->dev, "PCM HW_PARAMS: rate=%u, channels=%u, format=%d, period_bytes=%u, periods=%u\n",
		 params_rate(params), params_channels(params), params_format(params),
		 params_period_bytes(params), params_periods(params));

	if (params_channels(params) != DEVICE_CHANNELS) {
		dev_warn(tascam->card->dev, "Requested %d channels, but device is fixed at %d. This may cause issues.\n",
			 params_channels(params), DEVICE_CHANNELS);
	}

	err = us144mkii_configure_device_for_rate(tascam, params_rate(params));
	if (err < 0) {
		dev_err(tascam->card->dev, "Failed to set hardware rate to %u: %d\n", params_rate(params), err);
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
	int i;

	dev_info(tascam->card->dev, "PCM OPEN\n");

	runtime->hw = tascam_pcm_hw;

	tascam->playback_substream = substream;
	atomic_set(&tascam->playback_active, 0);
	atomic_set(&tascam->feedback_active, 0);

	tascam->playback_urb_alloc_size = (MAX_SUPPORTED_RATE / 1000) * PLAYBACK_BYTES_PER_FRAME;
	if (tascam->playback_urb_alloc_size == 0) {
		dev_err(tascam->card->dev, "Calculated playback_urb_alloc_size is zero.\n");
		return -EINVAL;
	}

	for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
		struct urb *urb = usb_alloc_urb(NUM_ISO_PACKETS, GFP_KERNEL);
		if (!urb) goto error_free_playback_urbs;
		tascam->playback_urbs[i] = urb;

		urb->transfer_buffer = usb_alloc_coherent(tascam->dev, tascam->playback_urb_alloc_size,
						    GFP_KERNEL, &urb->transfer_dma);
		if (!urb->transfer_buffer) {
			goto error_free_playback_urbs;
		}
		urb->dev = tascam->dev;
		urb->pipe = usb_sndisocpipe(tascam->dev, EP_AUDIO_OUT);
		urb->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
		urb->interval = 1;
		urb->context = tascam;
		urb->complete = playback_urb_complete;
	}

	tascam->f_iso_packet_size = FEEDBACK_BYTES_PER_PACKET;
	tascam->feedback_urb_alloc_size = tascam->f_iso_packet_size * NUM_ISO_PACKETS;
	if (tascam->feedback_urb_alloc_size == 0) {
		dev_err(tascam->card->dev, "Calculated feedback_urb_alloc_size is zero.\n");
		goto error_free_playback_urbs;
	}

	for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
		struct urb *f_urb = usb_alloc_urb(NUM_ISO_PACKETS, GFP_KERNEL);
		if (!f_urb) goto error_free_feedback_urbs;
		tascam->feedback_urbs[i] = f_urb;

		f_urb->transfer_buffer = usb_alloc_coherent(tascam->dev, tascam->feedback_urb_alloc_size,
						      GFP_KERNEL, &f_urb->transfer_dma);
		if (!f_urb->transfer_buffer) {
			goto error_free_feedback_urbs;
		}
		f_urb->dev = tascam->dev;
		f_urb->pipe = usb_rcvisocpipe(tascam->dev, EP_PLAYBACK_FEEDBACK);
		f_urb->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
		f_urb->interval = 1;
		f_urb->context = tascam;
		f_urb->complete = feedback_urb_complete;
	}

	return 0;

error_free_feedback_urbs:
	for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
		if (tascam->feedback_urbs[i]) {
			if (tascam->feedback_urbs[i]->transfer_buffer) {
				usb_free_coherent(tascam->dev, tascam->feedback_urb_alloc_size,
						  tascam->feedback_urbs[i]->transfer_buffer,
						  tascam->feedback_urbs[i]->transfer_dma);
			}
			usb_free_urb(tascam->feedback_urbs[i]);
			tascam->feedback_urbs[i] = NULL;
		}
	}
error_free_playback_urbs:
	for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
		if (tascam->playback_urbs[i]) {
			if (tascam->playback_urbs[i]->transfer_buffer) {
				usb_free_coherent(tascam->dev, tascam->playback_urb_alloc_size,
						  tascam->playback_urbs[i]->transfer_buffer,
						  tascam->playback_urbs[i]->transfer_dma);
			}
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

	dev_info(tascam->card->dev, "PCM CLOSE\n");

	atomic_set(&tascam->playback_active, 0);
	atomic_set(&tascam->feedback_active, 0);

	for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
		if (tascam->playback_urbs[i]) {
			usb_kill_urb(tascam->playback_urbs[i]);
			if (tascam->playback_urbs[i]->transfer_buffer) {
				usb_free_coherent(tascam->dev, tascam->playback_urb_alloc_size,
						  tascam->playback_urbs[i]->transfer_buffer,
						  tascam->playback_urbs[i]->transfer_dma);
			}
			usb_free_urb(tascam->playback_urbs[i]);
			tascam->playback_urbs[i] = NULL;
		}
	}

	for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
		if (tascam->feedback_urbs[i]) {
			usb_kill_urb(tascam->feedback_urbs[i]);
			if (tascam->feedback_urbs[i]->transfer_buffer) {
				usb_free_coherent(tascam->dev, tascam->feedback_urb_alloc_size,
						  tascam->feedback_urbs[i]->transfer_buffer,
						  tascam->feedback_urbs[i]->transfer_dma);
			}
			usb_free_urb(tascam->feedback_urbs[i]);
			tascam->feedback_urbs[i] = NULL;
		}
	}
	tascam->playback_substream = NULL;
	return 0;
}

// --- USB Driver Registration ---
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
