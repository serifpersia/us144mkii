// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2025 serifpersia <ramiserifpersia@gmail.com>
/*
 * ALSA Driver for TASCAM US-144MKII Audio Interface
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/printk.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>
#include <sound/control.h>

MODULE_AUTHOR("serifpersia <ramiserifpersia@gmail.com>");
MODULE_DESCRIPTION("ALSA Driver for TASCAM US-144MKII");
MODULE_LICENSE("GPL");

#define DRIVER_NAME "us144mkii"
#define DRIVER_VERSION "1.0"

/*
 * TODO:
 * - Implement audio input capture.
 * - Implement MIDI IN/OUT.
 */

/* --- Module Parameters --- */
static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = {1, [1 ... (SNDRV_CARDS - 1)] = 0};
static int dev_idx;

/* --- USB Device Identification --- */
#define USB_VID_TASCAM				0x0644
#define USB_PID_TASCAM_US144MKII	0x8020

/* --- USB Endpoints (Alternate Setting 1) --- */
#define EP_PLAYBACK_FEEDBACK		0x81
#define EP_AUDIO_OUT				0x02
#define EP_MIDI_IN					0x83
#define EP_MIDI_OUT					0x04
#define EP_AUDIO_IN					0x86

/* --- USB Control Message Protocol --- */
#define RT_H2D_CLASS_EP		(USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_ENDPOINT)
#define RT_D2H_CLASS_EP     (USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_ENDPOINT)
#define RT_H2D_VENDOR_DEV	(USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE)
#define RT_D2H_VENDOR_DEV	(USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE)
#define UAC_SET_CUR					0x01
#define UAC_GET_CUR					0x81
#define UAC_SAMPLING_FREQ_CONTROL 	0x0100
#define VENDOR_REQ_REGISTER_WRITE	0x41
#define VENDOR_REQ_MODE_CONTROL		0x49
#define MODE_VAL_HANDSHAKE_READ		0x0000
#define MODE_VAL_CONFIG				0x0010
#define MODE_VAL_STREAM_START		0x0030
#define REG_ADDR_UNKNOWN_0D			0x0d04
#define REG_ADDR_UNKNOWN_0E			0x0e00
#define REG_ADDR_UNKNOWN_0F			0x0f00
#define REG_ADDR_RATE_44100			0x1000
#define REG_ADDR_RATE_48000			0x1002
#define REG_ADDR_RATE_88200			0x1008
#define REG_ADDR_RATE_96000			0x100a
#define REG_ADDR_UNKNOWN_11			0x110b
#define REG_VAL_ENABLE				0x0101

/* --- URB Configuration --- */
#define NUM_PLAYBACK_URBS			8
#define PLAYBACK_URB_PACKETS		4
#define NUM_FEEDBACK_URBS			4
#define MAX_FEEDBACK_PACKETS		5
#define FEEDBACK_PACKET_SIZE		3
#define USB_CTRL_TIMEOUT_MS			1000

/* --- Audio Format Configuration --- */
#define BYTES_PER_SAMPLE			3
#define NUM_CHANNELS				4
#define BYTES_PER_FRAME		(NUM_CHANNELS * BYTES_PER_SAMPLE)
#define FEEDBACK_ACCUMULATOR_SIZE 	128

/* --- Main Driver Data Structure --- */
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
	int current_rate;
	unsigned int latency_profile;
	unsigned int playback_routing;

	unsigned int feedback_accumulator_pattern[FEEDBACK_ACCUMULATOR_SIZE];
	unsigned int feedback_pattern_out_idx;
	unsigned int feedback_pattern_in_idx;
	bool feedback_synced;
	unsigned int feedback_consecutive_errors;
	unsigned int feedback_urb_skip_count;

	u64 playback_frames_consumed;
	snd_pcm_uframes_t driver_playback_pos;
	u64 last_period_pos;

	const unsigned int (*feedback_patterns)[8];
	unsigned int feedback_base_value;
	unsigned int feedback_max_value;
};

static struct usb_driver tascam_alsa_driver;

/* --- Forward Declarations --- */
static void playback_urb_complete(struct urb *urb);
static void feedback_urb_complete(struct urb *urb);
static int us144mkii_configure_device_for_rate(struct tascam_card *tascam, int rate);
static int tascam_probe(struct usb_interface *intf, const struct usb_device_id *id);
static void tascam_disconnect(struct usb_interface *intf);
static int tascam_suspend(struct usb_interface *intf, pm_message_t message);
static int tascam_resume(struct usb_interface *intf);

/* --- Sysfs Attribute for Driver Version --- */
/**
 * driver_version_show - Sysfs callback to show the driver version.
 * @dev: The device structure.
 * @attr: The device attribute structure.
 * @buf: The buffer to write the version string into.
 *
 * Returns: The number of bytes written.
 */
static ssize_t driver_version_show(struct device *dev,
                                   struct device_attribute *attr, char *buf)
{
    return sysfs_emit(buf, "%s\n", DRIVER_VERSION);
}
static DEVICE_ATTR_RO(driver_version);

/* --- ALSA Control Definitions --- */
static const char * const latency_profile_texts[] = {"Low", "Normal", "High"};

static int tascam_latency_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = 3;
	if (uinfo->value.enumerated.item >= 3)
		uinfo->value.enumerated.item = 2;
	strcpy(uinfo->value.enumerated.name, latency_profile_texts[uinfo->value.enumerated.item]);
	return 0;
}

static int tascam_latency_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct tascam_card *tascam = (struct tascam_card *)snd_kcontrol_chip(kcontrol);
	switch (tascam->latency_profile) {
	case 1: ucontrol->value.enumerated.item[0] = 0; break;
	case 2: ucontrol->value.enumerated.item[0] = 1; break;
	case 5: ucontrol->value.enumerated.item[0] = 2; break;
	default: ucontrol->value.enumerated.item[0] = 1; break;
	}
	return 0;
}

static int tascam_latency_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct tascam_card *tascam = (struct tascam_card *)snd_kcontrol_chip(kcontrol);
	unsigned int new_profile;
	bool changed = false;

	switch (ucontrol->value.enumerated.item[0]) {
	case 0: new_profile = 1; break;
	case 1: new_profile = 2; break;
	case 2: new_profile = 5; break;
	default: return -EINVAL;
	}

	if (tascam->latency_profile != new_profile) {
		tascam->latency_profile = new_profile;
		changed = true;
	}
	return changed;
}

static const struct snd_kcontrol_new tascam_latency_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Latency Profile",
	.info = tascam_latency_info,
	.get = tascam_latency_get,
	.put = tascam_latency_put,
};

static const char * const playback_routing_texts[] = {"Stereo to All", "Swapped", "Digital In to All"};

static int tascam_routing_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = 3;
	if (uinfo->value.enumerated.item >= 3)
		uinfo->value.enumerated.item = 2;
	strcpy(uinfo->value.enumerated.name, playback_routing_texts[uinfo->value.enumerated.item]);
	return 0;
}

static int tascam_routing_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct tascam_card *tascam = (struct tascam_card *)snd_kcontrol_chip(kcontrol);
	ucontrol->value.enumerated.item[0] = tascam->playback_routing;
	return 0;
}

static int tascam_routing_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct tascam_card *tascam = (struct tascam_card *)snd_kcontrol_chip(kcontrol);
	unsigned int new_routing = ucontrol->value.enumerated.item[0];
	bool changed = false;

	if (new_routing >= 3)
		return -EINVAL;

	if (tascam->playback_routing != new_routing) {
		tascam->playback_routing = new_routing;
		changed = true;
	}
	return changed;
}

static const struct snd_kcontrol_new tascam_routing_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Playback Routing",
	.info = tascam_routing_info,
	.get = tascam_routing_get,
	.put = tascam_routing_put,
};

/**
 * tascam_samplerate_info - ALSA control info callback for the sample rate.
 * @kcontrol: The kcontrol instance.
 * @uinfo: The user control element info structure to fill.
 *
 * Provides information about the read-only sample rate control.
 *
 * Returns: 0 on success.
 */
static int tascam_samplerate_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 96000;
	return 0;
}

/**
 * tascam_samplerate_get - ALSA control get callback for the sample rate.
 * @kcontrol: The kcontrol instance.
 * @ucontrol: The user control element value structure to fill.
 *
 * Reports the current sample rate of the device. It first checks the driver's
 * internal state. If no stream is active, it queries the device directly via
 * a USB control message.
 *
 * Returns: 0 on success, or a negative error code on failure.
 */
static int tascam_samplerate_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct tascam_card *tascam = (struct tascam_card *)snd_kcontrol_chip(kcontrol);
	u8 *buf;
	int err;
	u32 rate = 0;

	if (tascam->current_rate > 0) {
		ucontrol->value.integer.value[0] = tascam->current_rate;
		return 0;
	}

	buf = kmalloc(3, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	err = usb_control_msg(tascam->dev, usb_rcvctrlpipe(tascam->dev, 0),
			      UAC_GET_CUR, RT_D2H_CLASS_EP,
			      UAC_SAMPLING_FREQ_CONTROL, EP_AUDIO_IN,
			      buf, 3, USB_CTRL_TIMEOUT_MS);

	if (err >= 3)
		rate = buf[0] | (buf[1] << 8) | (buf[2] << 16);

	ucontrol->value.integer.value[0] = rate;
	kfree(buf);
	return 0;
}

static const struct snd_kcontrol_new tascam_samplerate_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Sample Rate",
	.info = tascam_samplerate_info,
	.get = tascam_samplerate_get,
	.access = SNDRV_CTL_ELEM_ACCESS_READ,
};

/* --- Rate-to-Packet Fixing Data (Verified) --- */
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

static const struct snd_pcm_hardware tascam_pcm_hw = {
	.info = (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
		 SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_MMAP_VALID |
		 SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME),
	.formats = SNDRV_PCM_FMTBIT_S24_3LE,
	.rates = (SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
		  SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000),
	.rate_min = 44100, .rate_max = 96000,
	.channels_min = NUM_CHANNELS,
	.channels_max = NUM_CHANNELS,
	.buffer_bytes_max = 1024 * 1024,
	.period_bytes_min = 48 * BYTES_PER_FRAME,
	.period_bytes_max = 1024 * BYTES_PER_FRAME,
	.periods_min = 2, .periods_max = 1024,
};

/**
 * tascam_free_urbs - Free all allocated URBs and their buffers.
 * @tascam: The card instance.
 */
static void tascam_free_urbs(struct tascam_card *tascam)
{
	int i;

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
}

/**
 * tascam_alloc_urbs - Allocate URBs and their buffers.
 * @tascam: The card instance.
 * Returns: 0 on success, or a negative error code on failure.
 */
static int tascam_alloc_urbs(struct tascam_card *tascam)
{
	int i;
	size_t max_packet_size;

	max_packet_size = ((96000 / 8000) + 2) * BYTES_PER_FRAME;
	tascam->playback_urb_alloc_size = max_packet_size * PLAYBACK_URB_PACKETS;

	for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
		struct urb *urb = usb_alloc_urb(PLAYBACK_URB_PACKETS, GFP_KERNEL);
		if (!urb)
			goto error;
		tascam->playback_urbs[i] = urb;

		urb->transfer_buffer = usb_alloc_coherent(tascam->dev, tascam->playback_urb_alloc_size,
						    GFP_KERNEL, &urb->transfer_dma);
		if (!urb->transfer_buffer)
			goto error;

		urb->dev = tascam->dev;
		urb->pipe = usb_sndisocpipe(tascam->dev, EP_AUDIO_OUT);
		urb->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
		urb->interval = 1;
		urb->context = tascam;
		urb->complete = playback_urb_complete;
	}

	tascam->feedback_urb_alloc_size = FEEDBACK_PACKET_SIZE * MAX_FEEDBACK_PACKETS;

	for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
		struct urb *f_urb = usb_alloc_urb(MAX_FEEDBACK_PACKETS, GFP_KERNEL);
		if (!f_urb)
			goto error;
		tascam->feedback_urbs[i] = f_urb;

		f_urb->transfer_buffer = usb_alloc_coherent(tascam->dev, tascam->feedback_urb_alloc_size,
						      GFP_KERNEL, &f_urb->transfer_dma);
		if (!f_urb->transfer_buffer)
			goto error;

		f_urb->dev = tascam->dev;
		f_urb->pipe = usb_rcvisocpipe(tascam->dev, EP_PLAYBACK_FEEDBACK);
		f_urb->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
		f_urb->interval = 4;
		f_urb->context = tascam;
		f_urb->complete = feedback_urb_complete;
	}

	return 0;

error:
	dev_err(tascam->card->dev, "Failed to allocate URBs\n");
	tascam_free_urbs(tascam);
	return -ENOMEM;
}

static int tascam_pcm_open(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int err;

	runtime->hw = tascam_pcm_hw;
	tascam->playback_substream = substream;
	atomic_set(&tascam->playback_active, 0);

	err = tascam_alloc_urbs(tascam);
	if (err < 0)
		return err;

	return 0;
}

static int tascam_pcm_close(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	tascam_free_urbs(tascam);
	return 0;
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

	switch (rate) {
	case 44100: current_payload_src = payload_44100; rate_vendor_wValue = REG_ADDR_RATE_44100; break;
	case 48000: current_payload_src = payload_48000; rate_vendor_wValue = REG_ADDR_RATE_48000; break;
	case 88200: current_payload_src = payload_88200; rate_vendor_wValue = REG_ADDR_RATE_88200; break;
	case 96000: current_payload_src = payload_96000; rate_vendor_wValue = REG_ADDR_RATE_96000; break;
	default:
		dev_err(&dev->dev, "Unsupported sample rate %d for configuration\n", rate);
		return -EINVAL;
	}

	rate_payload_buf = kmemdup(current_payload_src, 3, GFP_KERNEL);
	if (!rate_payload_buf)
		return -ENOMEM;

	dev_info(&dev->dev, "Configuring device for %d Hz\n", rate);

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_MODE_CONTROL, RT_H2D_VENDOR_DEV, MODE_VAL_CONFIG, 0x0000, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) goto fail;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), UAC_SET_CUR, RT_H2D_CLASS_EP, UAC_SAMPLING_FREQ_CONTROL, EP_AUDIO_IN, rate_payload_buf, 3, USB_CTRL_TIMEOUT_MS);
	if (err < 0) goto fail;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), UAC_SET_CUR, RT_H2D_CLASS_EP, UAC_SAMPLING_FREQ_CONTROL, EP_AUDIO_OUT, rate_payload_buf, 3, USB_CTRL_TIMEOUT_MS);
	if (err < 0) goto fail;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, REG_ADDR_UNKNOWN_0D, REG_VAL_ENABLE, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) goto fail;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, REG_ADDR_UNKNOWN_0E, REG_VAL_ENABLE, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) goto fail;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, REG_ADDR_UNKNOWN_0F, REG_VAL_ENABLE, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) goto fail;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, rate_vendor_wValue, REG_VAL_ENABLE, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) goto fail;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, REG_ADDR_UNKNOWN_11, REG_VAL_ENABLE, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) goto fail;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_MODE_CONTROL, RT_H2D_VENDOR_DEV, MODE_VAL_STREAM_START, 0x0000, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) goto fail;

	kfree(rate_payload_buf);
	return 0;

fail:
	dev_err(&dev->dev, "Device configuration failed at rate %d with error %d\n", rate, err);
	kfree(rate_payload_buf);
	return err;
}

static int tascam_pcm_hw_params(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	int err;
	unsigned int rate = params_rate(params);

	err = snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(params));
	if (err < 0)
		return err;

	/* Set rate-dependent feedback patterns and values */
	switch (rate) {
	case 44100:
		tascam->feedback_patterns = patterns_44khz;
		tascam->feedback_base_value = 42; tascam->feedback_max_value = 46;
		break;
	case 48000:
		tascam->feedback_patterns = patterns_48khz;
		tascam->feedback_base_value = 46; tascam->feedback_max_value = 50;
		break;
	case 88200:
		tascam->feedback_patterns = patterns_88khz;
		tascam->feedback_base_value = 86; tascam->feedback_max_value = 90;
		break;
	case 96000:
		tascam->feedback_patterns = patterns_96khz;
		tascam->feedback_base_value = 94; tascam->feedback_max_value = 98;
		break;
	default:
		return -EINVAL;
	}

	/* Always re-configure hardware to ensure it's in a clean state */
	err = us144mkii_configure_device_for_rate(tascam, rate);
	if (err < 0) {
		tascam->current_rate = 0;
		return err;
	}
	tascam->current_rate = rate;

	return 0;
}

static int tascam_pcm_hw_free(struct snd_pcm_substream *substream)
{
	return snd_pcm_lib_free_pages(substream);
}

static int tascam_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int i, u;
	size_t nominal_frames_per_packet, nominal_bytes_per_packet;
	size_t total_bytes_in_urb;
	unsigned int feedback_packets;

	/* Reset driver state for the new stream */
	tascam->driver_playback_pos = 0;
	tascam->playback_frames_consumed = 0;
	tascam->last_period_pos = 0;
	tascam->feedback_pattern_in_idx = 0;
	tascam->feedback_pattern_out_idx = 0;
	tascam->feedback_synced = false;
	tascam->feedback_consecutive_errors = 0;
	tascam->feedback_urb_skip_count = NUM_FEEDBACK_URBS;

	/* Initialize feedback accumulator with nominal values */
	nominal_frames_per_packet = runtime->rate / 8000;
	for (i = 0; i < FEEDBACK_ACCUMULATOR_SIZE; i++)
		tascam->feedback_accumulator_pattern[i] = nominal_frames_per_packet;

	/* Validate and apply latency profile */
	switch (tascam->latency_profile) {
	case 1:
	case 2:
	case 5:
		feedback_packets = tascam->latency_profile;
		break;
	default:
		dev_warn(tascam->card->dev, "Invalid latency_profile value %d, falling back to default (2).\n", tascam->latency_profile);
		tascam->latency_profile = 2;
		feedback_packets = 2;
	}

	dev_info(tascam->card->dev, "Prepare: Using latency profile %u (%u feedback packets) for %u Hz\n",
		 tascam->latency_profile, feedback_packets, runtime->rate);

	/* Configure Feedback URBs */
	for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
		struct urb *f_urb = tascam->feedback_urbs[i];
		int j;

		f_urb->number_of_packets = feedback_packets;
		f_urb->transfer_buffer_length = feedback_packets * FEEDBACK_PACKET_SIZE;
		for (j = 0; j < feedback_packets; j++) {
			f_urb->iso_frame_desc[j].offset = j * FEEDBACK_PACKET_SIZE;
			f_urb->iso_frame_desc[j].length = FEEDBACK_PACKET_SIZE;
		}
	}

	/* Configure Playback URBs */
	nominal_bytes_per_packet = nominal_frames_per_packet * BYTES_PER_FRAME;
	total_bytes_in_urb = nominal_bytes_per_packet * PLAYBACK_URB_PACKETS;

	for (u = 0; u < NUM_PLAYBACK_URBS; u++) {
		struct urb *urb = tascam->playback_urbs[u];

		memset(urb->transfer_buffer, 0, tascam->playback_urb_alloc_size);
		urb->transfer_buffer_length = total_bytes_in_urb;
		urb->number_of_packets = PLAYBACK_URB_PACKETS;
		for (i = 0; i < PLAYBACK_URB_PACKETS; i++) {
			urb->iso_frame_desc[i].offset = i * nominal_bytes_per_packet;
			urb->iso_frame_desc[i].length = nominal_bytes_per_packet;
		}
	}

	return 0;
}

static int tascam_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	int err = 0, i;
	bool start = false;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START: case SNDRV_PCM_TRIGGER_RESUME:
		if (atomic_xchg(&tascam->playback_active, 1) == 0)
			start = true;
		break;
	case SNDRV_PCM_TRIGGER_STOP: case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		atomic_set(&tascam->playback_active, 0);
		break;
	default: return -EINVAL;
	}

	if (start) {
		for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
			err = usb_submit_urb(tascam->feedback_urbs[i], GFP_ATOMIC);
			if (err < 0) goto rollback;
		}
		for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
			err = usb_submit_urb(tascam->playback_urbs[i], GFP_ATOMIC);
			if (err < 0) goto rollback;
		}
	} else {
		for (i = 0; i < NUM_PLAYBACK_URBS; i++)
			usb_unlink_urb(tascam->playback_urbs[i]);
		for (i = 0; i < NUM_FEEDBACK_URBS; i++)
			usb_unlink_urb(tascam->feedback_urbs[i]);
	}
	return 0;

rollback:
	dev_err(tascam->card->dev, "Failed to submit URBs to start stream: %d\n", err);
	atomic_set(&tascam->playback_active, 0);
	for (i = 0; i < NUM_PLAYBACK_URBS; i++)
		usb_unlink_urb(tascam->playback_urbs[i]);
	for (i = 0; i < NUM_FEEDBACK_URBS; i++)
		usb_unlink_urb(tascam->feedback_urbs[i]);
	return err;
}

static snd_pcm_uframes_t tascam_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	u64 pos;
	unsigned long flags;

	if (!atomic_read(&tascam->playback_active))
		return 0;

	spin_lock_irqsave(&tascam->lock, flags);
	pos = tascam->playback_frames_consumed;
	spin_unlock_irqrestore(&tascam->lock, flags);

	return runtime ? pos % runtime->buffer_size : 0;
}

static struct snd_pcm_ops tascam_playback_ops = {
	.open = tascam_pcm_open, .close = tascam_pcm_close,
	.ioctl = snd_pcm_lib_ioctl, .hw_params = tascam_pcm_hw_params,
	.hw_free = tascam_pcm_hw_free, .prepare = tascam_pcm_prepare,
	.trigger = tascam_pcm_trigger, .pointer = tascam_pcm_pointer,
};

static int tascam_capture_open_stub(struct snd_pcm_substream *s) { return -ENODEV; }
static int tascam_capture_close_stub(struct snd_pcm_substream *s) { return 0; }
static struct snd_pcm_ops tascam_capture_ops = {
	.open = tascam_capture_open_stub, .close = tascam_capture_close_stub,
};

static void playback_urb_complete(struct urb *urb)
{
	struct tascam_card *tascam = urb->context;
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	unsigned long flags;
	char *src_buf, *dst_buf;
	size_t total_bytes_for_urb = 0;
	snd_pcm_uframes_t offset_frames;
	snd_pcm_uframes_t frames_to_copy;
	int ret, i;

	if (urb->status)
		return;
	if (!tascam || !atomic_read(&tascam->playback_active))
		return;
	substream = tascam->playback_substream;
	if (!substream || !substream->runtime)
		return;
	runtime = substream->runtime;

	spin_lock_irqsave(&tascam->lock, flags);

	/* Phase 1: Populate the isochronous frame descriptors from the accumulator. */
	for (i = 0; i < urb->number_of_packets; i++) {
		unsigned int frames_for_packet;
		size_t bytes_for_packet;

		if (tascam->feedback_synced) {
			frames_for_packet = tascam->feedback_accumulator_pattern[tascam->feedback_pattern_out_idx];
			tascam->feedback_pattern_out_idx = (tascam->feedback_pattern_out_idx + 1) % FEEDBACK_ACCUMULATOR_SIZE;
		} else {
			frames_for_packet = runtime->rate / 8000;
		}
		bytes_for_packet = frames_for_packet * BYTES_PER_FRAME;

		urb->iso_frame_desc[i].offset = total_bytes_for_urb;
		urb->iso_frame_desc[i].length = bytes_for_packet;
		total_bytes_for_urb += bytes_for_packet;
	}
	urb->transfer_buffer_length = total_bytes_for_urb;

	/* Phase 2: Atomically update the driver's position. */
	offset_frames = tascam->driver_playback_pos;
	frames_to_copy = bytes_to_frames(runtime, total_bytes_for_urb);
	tascam->driver_playback_pos = (offset_frames + frames_to_copy) % runtime->buffer_size;

	/* --- End of Critical Section --- */
	spin_unlock_irqrestore(&tascam->lock, flags);

	/* Phase 3: Perform the data copy OUTSIDE the lock. */
	if (total_bytes_for_urb > 0) {
		int f;
		src_buf = runtime->dma_area;
		dst_buf = urb->transfer_buffer;

		for (f = 0; f < frames_to_copy; ++f) {
			snd_pcm_uframes_t current_frame_pos = (offset_frames + f) % runtime->buffer_size;
			char *src_frame = src_buf + frames_to_bytes(runtime, current_frame_pos);
			char *dst_frame = dst_buf + (f * BYTES_PER_FRAME);

			switch (tascam->playback_routing) {
			case 0: /* Stereo to All */
				memcpy(dst_frame, src_frame, 6);      /* Copy L/R to Out 1/2 */
				memcpy(dst_frame + 6, src_frame, 6);  /* Copy L/R to Out 3/4 */
				break;
			case 1: /* Swapped */
				memcpy(dst_frame, src_frame + 6, 6);  /* Copy 3/4 to Out 1/2 */
				memcpy(dst_frame + 6, src_frame, 6);  /* Copy 1/2 to Out 3/4 */
				break;
			case 2: /* Digital In to All */
				memcpy(dst_frame, src_frame + 6, 6);  /* Copy 3/4 to Out 1/2 */
				memcpy(dst_frame + 6, src_frame + 6, 6); /* Copy 3/4 to Out 3/4 */
				break;
			}
		}
	}

	urb->dev = tascam->dev;
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0)
		dev_err_ratelimited(tascam->card->dev, "Failed to resubmit playback URB: %d\n", ret);
}

static void feedback_urb_complete(struct urb *urb)
{
	struct tascam_card *tascam = urb->context;
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	unsigned long flags;
	u64 current_period, total_frames_in_urb = 0;
	int ret, p;
	unsigned int old_in_idx, new_in_idx;

	if (urb->status)
		return;
	if (!tascam || !atomic_read(&tascam->playback_active))
		return;
	substream = tascam->playback_substream;
	if (!substream || !substream->runtime)
		return;
	runtime = substream->runtime;

	spin_lock_irqsave(&tascam->lock, flags);

	/* Hybrid Sync: Initial blind period for hardware to settle. */
	if (tascam->feedback_urb_skip_count > 0) {
		tascam->feedback_urb_skip_count--;
		goto unlock_and_resubmit;
	}

	old_in_idx = tascam->feedback_pattern_in_idx;

	for (p = 0; p < urb->number_of_packets; p++) {
		u8 feedback_value = 0;
		const unsigned int *pattern;
		bool packet_ok = (urb->iso_frame_desc[p].status == 0 &&
				  urb->iso_frame_desc[p].actual_length >= 1);

		if (packet_ok)
			feedback_value = *((u8 *)urb->transfer_buffer + urb->iso_frame_desc[p].offset);

		if (packet_ok && feedback_value >= tascam->feedback_base_value &&
		    feedback_value <= tascam->feedback_max_value) {
			pattern = tascam->feedback_patterns[feedback_value - tascam->feedback_base_value];
			tascam->feedback_consecutive_errors = 0;
			int i;
			for (i = 0; i < 8; i++) {
				unsigned int in_idx = (tascam->feedback_pattern_in_idx + i) % FEEDBACK_ACCUMULATOR_SIZE;
				tascam->feedback_accumulator_pattern[in_idx] = pattern[i];
				total_frames_in_urb += pattern[i];
			}
		} else {
			unsigned int nominal_frames = runtime->rate / 8000;
			int i;
			if (tascam->feedback_synced) {
				tascam->feedback_consecutive_errors++;
				if (tascam->feedback_consecutive_errors > 10) {
					dev_warn_ratelimited(tascam->card->dev, "Feedback sync lost! (value: %u, errors: %u)\n",
							 feedback_value, tascam->feedback_consecutive_errors);
					tascam->feedback_synced = false;
				}
			}
			for (i = 0; i < 8; i++) {
				unsigned int in_idx = (tascam->feedback_pattern_in_idx + i) % FEEDBACK_ACCUMULATOR_SIZE;
				tascam->feedback_accumulator_pattern[in_idx] = nominal_frames;
				total_frames_in_urb += nominal_frames;
			}
		}
		tascam->feedback_pattern_in_idx = (tascam->feedback_pattern_in_idx + 8) % FEEDBACK_ACCUMULATOR_SIZE;
	}

	new_in_idx = tascam->feedback_pattern_in_idx;

	/* Hybrid Sync: Pointer Lap check for sync acquisition. */
	if (!tascam->feedback_synced) {
		unsigned int out_idx = tascam->feedback_pattern_out_idx;
		bool is_ahead = (new_in_idx - out_idx) % FEEDBACK_ACCUMULATOR_SIZE < (FEEDBACK_ACCUMULATOR_SIZE / 2);
		bool was_behind = (old_in_idx - out_idx) % FEEDBACK_ACCUMULATOR_SIZE >= (FEEDBACK_ACCUMULATOR_SIZE / 2);

		if (is_ahead && was_behind) {
			dev_dbg(tascam->card->dev, "Sync Acquired! (in: %u, out: %u)\n", new_in_idx, out_idx);
			tascam->feedback_synced = true;
			tascam->feedback_consecutive_errors = 0;
		}
	}

	if (total_frames_in_urb > 0)
		tascam->playback_frames_consumed += total_frames_in_urb;

	current_period = div_u64(tascam->playback_frames_consumed, runtime->period_size);
	if (current_period > tascam->last_period_pos) {
		tascam->last_period_pos = current_period;
		spin_unlock_irqrestore(&tascam->lock, flags);
		snd_pcm_period_elapsed(substream);
		goto resubmit;
	}

unlock_and_resubmit:
	spin_unlock_irqrestore(&tascam->lock, flags);
resubmit:
	urb->dev = tascam->dev;
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0)
		dev_err_ratelimited(tascam->card->dev, "Failed to resubmit feedback URB: %d\n", ret);
}



static int tascam_create_pcm(struct tascam_card *tascam)
{
	struct snd_pcm *pcm;
	int err;

	err = snd_pcm_new(tascam->card, "US144MKII", 0, 1, 1, &pcm);
	if (err < 0)
		return err;

	err = snd_ctl_add(tascam->card, snd_ctl_new1(&tascam_latency_control, tascam));
	if (err < 0)
		return err;

	err = snd_ctl_add(tascam->card, snd_ctl_new1(&tascam_routing_control, tascam));
	if (err < 0)
		return err;

	err = snd_ctl_add(tascam->card, snd_ctl_new1(&tascam_samplerate_control, tascam));
	if (err < 0)
		return err;

	tascam->pcm = pcm;
	pcm->private_data = tascam;
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &tascam_playback_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &tascam_capture_ops);
	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_CONTINUOUS,
					      tascam->dev->dev.parent,
					      64 * 1024,
					      tascam_pcm_hw.buffer_bytes_max);
	return 0;
}

static void tascam_card_private_free(struct snd_card *card)
{
	struct tascam_card *tascam = card->private_data;
	if (tascam && tascam->dev) {
		usb_put_dev(tascam->dev);
		tascam->dev = NULL;
	}
}

/**
 * tascam_suspend - Called when the device is being suspended.
 * @intf: The USB interface.
 * @message: Power management message.
 */
static int tascam_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct tascam_card *tascam = usb_get_intfdata(intf);

	if (!tascam || !tascam->pcm)
		return 0;

	snd_pcm_suspend_all(tascam->pcm);

	return 0;
}

/**
 * tascam_resume - Called when the device is being resumed.
 * @intf: The USB interface.
 */
static int tascam_resume(struct usb_interface *intf)
{
	struct tascam_card *tascam = usb_get_intfdata(intf);
	struct usb_device *dev;
	int err;

	if (!tascam)
		return 0;

	dev = tascam->dev;
	dev_info(&intf->dev, "Resuming and re-initializing device...\n");

	/* Re-establish alternate settings for both interfaces */
	err = usb_set_interface(dev, 0, 1);
	if (err < 0) {
		dev_err(&intf->dev, "Resume: Set Alt Setting on Intf 0 failed: %d\n", err);
		return err;
	}
	err = usb_set_interface(dev, 1, 1);
	if (err < 0) {
		dev_err(&intf->dev, "Resume: Set Alt Setting on Intf 1 failed: %d\n", err);
		return err;
	}

	/* Re-configure the device for the last used sample rate. */
	if (tascam->current_rate > 0) {
		dev_info(&intf->dev, "Restoring sample rate to %d Hz\n", tascam->current_rate);
		err = us144mkii_configure_device_for_rate(tascam, tascam->current_rate);
		if (err < 0) {
			dev_err(&intf->dev, "Resume: Failed to restore sample rate configuration\n");
			tascam->current_rate = 0;
			return err;
		}
	}

	return 0;
}

static int tascam_probe(struct usb_interface *intf, const struct usb_device_id *usb_id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct tascam_card *tascam;
	struct snd_card *card;
	int err;
	u8 *handshake_buf;

	if (intf->cur_altsetting->desc.bInterfaceNumber != 0)
		return -ENODEV;

	if (dev_idx >= SNDRV_CARDS)
		return -ENODEV;
	if (!enable[dev_idx]) {
		dev_idx++;
		return -ENOENT;
	}

	err = snd_card_new(&intf->dev, index[dev_idx], id[dev_idx], THIS_MODULE,
			   sizeof(struct tascam_card), &card);
	if (err < 0)
		return err;

	tascam = card->private_data;
	tascam->card = card;
	tascam->dev = usb_get_dev(dev);
	tascam->iface0 = intf;
	card->private_free = tascam_card_private_free;
	usb_set_intfdata(intf, tascam);
	spin_lock_init(&tascam->lock);
	/* Initialize mixer controls to default values */
	tascam->latency_profile = 2; /* Default to Normal Latency */
	tascam->playback_routing = 0; /* Default to Stereo to All */
	tascam->current_rate = 0; /* Not known until hw_params */

	strscpy(card->driver, DRIVER_NAME, sizeof(card->driver));
	strscpy(card->shortname, "TASCAM US-144MKII", sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname), "TASCAM US-144MKII (VID:%04x, PID:%04x) at %s",
		 le16_to_cpu(dev->descriptor.idVendor),
		 le16_to_cpu(dev->descriptor.idProduct),
		 dev->bus->bus_name);

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
	if (err < 0) {
		dev_err(&intf->dev, "Set Alt Setting on Intf 0 failed: %d\n", err);
		goto release_iface1_and_free_card;
	}
	err = usb_set_interface(dev, 1, 1);
	if (err < 0) {
		dev_err(&intf->dev, "Set Alt Setting on Intf 1 failed: %d\n", err);
		goto release_iface1_and_free_card;
	}

	handshake_buf = kmalloc(1, GFP_KERNEL);
	if (!handshake_buf) {
		err = -ENOMEM;
		goto release_iface1_and_free_card;
	}
	err = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0), VENDOR_REQ_MODE_CONTROL,
			      RT_D2H_VENDOR_DEV, MODE_VAL_HANDSHAKE_READ, 0x0000,
			      handshake_buf, 1, USB_CTRL_TIMEOUT_MS);
	if (err == 1 && handshake_buf[0] == 0x12)
		dev_info(&intf->dev, "Handshake successful.\n");
	else
		dev_warn(&intf->dev, "Handshake failed (err %d, val 0x%02x), continuing anyway.\n", err, err > 0 ? handshake_buf[0] : 0);
	kfree(handshake_buf);

	err = tascam_create_pcm(tascam);
	if (err < 0)
		goto release_iface1_and_free_card;

	if (device_create_file(&intf->dev, &dev_attr_driver_version))
		dev_warn(&intf->dev, "Could not create sysfs attribute for driver version\n");

	err = snd_card_register(card);
	if (err < 0)
		goto release_iface1_and_free_card;

	dev_info(&intf->dev, "TASCAM US-144MKII driver initialized.\n");
	dev_idx++;
	return 0;

release_iface1_and_free_card:
	if (tascam->iface1) {
		usb_set_intfdata(tascam->iface1, NULL);
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

	device_remove_file(&intf->dev, &dev_attr_driver_version);

	if (intf != tascam->iface0)
		return;

	dev_info(&intf->dev, "TASCAM US-144MKII disconnecting...\n");
	snd_card_disconnect(tascam->card);

	if (tascam->iface1) {
		usb_set_intfdata(tascam->iface1, NULL);
		usb_driver_release_interface(&tascam_alsa_driver, tascam->iface1);
		tascam->iface1 = NULL;
	}

	/* Decrement the device index to allow the next probe to use this slot. */
	if (dev_idx > 0)
		dev_idx--;

	snd_card_free_when_closed(tascam->card);
}

static const struct usb_device_id tascam_id_table[] = {
	{ USB_DEVICE(USB_VID_TASCAM, USB_PID_TASCAM_US144MKII) },
	{ }
};
MODULE_DEVICE_TABLE(usb, tascam_id_table);

static struct usb_driver tascam_alsa_driver = {
	.name =		DRIVER_NAME,
	.probe =	tascam_probe,
	.disconnect =	tascam_disconnect,
	.id_table =	tascam_id_table,
	.suspend =	tascam_suspend,
	.resume =	tascam_resume,
};

module_usb_driver(tascam_alsa_driver);
