// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2025 serifpersia <ramiserifpersia@gmail.com>
/*
 * ALSA Driver for TASCAM US-144MKII Audio Interface
 */

#include <linux/usb.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/control.h>

MODULE_AUTHOR("serifpersia <ramiserifpersia@gmail.com>");
MODULE_DESCRIPTION("ALSA Driver for TASCAM US-144MKII");
MODULE_LICENSE("GPL v2");

#define DRIVER_NAME "us144mkii"
#define DRIVER_VERSION "1.5"

/*
 * TODO:
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
#define HANDSHAKE_SUCCESS_VAL		0x12
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
#define NUM_CAPTURE_URBS			8
#define CAPTURE_URB_SIZE			512
#define CAPTURE_RING_BUFFER_SIZE	(CAPTURE_URB_SIZE * NUM_CAPTURE_URBS * 4)
#define USB_CTRL_TIMEOUT_MS			1000

/* --- Audio Format Configuration --- */
#define BYTES_PER_SAMPLE			3
#define NUM_CHANNELS				4
#define BYTES_PER_FRAME		(NUM_CHANNELS * BYTES_PER_SAMPLE)
#define FEEDBACK_ACCUMULATOR_SIZE 	128

/* --- Capture Decoding Defines --- */
#define DECODED_CHANNELS_PER_FRAME	4
#define DECODED_SAMPLE_SIZE			4  /* 32-bit */
#define FRAMES_PER_DECODE_BLOCK		8
#define RAW_BYTES_PER_DECODE_BLOCK	512

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

	struct snd_pcm_substream *capture_substream;
	atomic_t capture_active;
	snd_pcm_uframes_t driver_capture_pos;
	u64 capture_frames_processed;
	u64 last_capture_period_pos;
	u8 *capture_ring_buffer;
	size_t capture_ring_buffer_read_ptr;
	volatile size_t capture_ring_buffer_write_ptr;
	u8 *capture_decode_raw_block;
	s32 *capture_decode_dst_block;
	struct work_struct capture_work;

	struct urb *feedback_urbs[NUM_FEEDBACK_URBS];
	size_t feedback_urb_alloc_size;

	spinlock_t lock;
	atomic_t active_urbs;
	int current_rate;
	unsigned int latency_profile;
	unsigned int line_out_source;     /* 0: Playback 1-2, 1: Playback 3-4 */
	unsigned int digital_out_source;  /* 0: Playback 1-2, 1: Playback 3-4 */
	unsigned int capture_12_source;   /* 0: Analog In, 1: Digital In */
	unsigned int capture_34_source;   /* 0: Analog In, 1: Digital In */

	unsigned int feedback_accumulator_pattern[FEEDBACK_ACCUMULATOR_SIZE];
	unsigned int feedback_pattern_out_idx;
	unsigned int feedback_pattern_in_idx;
	bool feedback_synced;
	unsigned int feedback_consecutive_errors;
	unsigned int feedback_urb_skip_count;

	u64 playback_frames_consumed;
	snd_pcm_uframes_t driver_playback_pos;
	u64 last_period_pos;

	u64 capture_frames_produced;
	u64 last_capture_period_pos;

	const unsigned int (*feedback_patterns)[8];
	unsigned int feedback_base_value;
	unsigned int feedback_max_value;
};

static struct usb_driver tascam_alsa_driver;

/* --- Forward Declarations --- */
static void playback_urb_complete(struct urb *urb);
static void feedback_urb_complete(struct urb *urb);
static void capture_urb_complete(struct urb *urb);
static void tascam_capture_work_handler(struct work_struct *work);
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
/* --- ALSA Control Definitions --- */
static const char * const latency_profile_texts[] = {"Low", "Normal", "High"};
static const char * const playback_source_texts[] = {"Playback 1-2", "Playback 3-4"};
static const char * const capture_source_texts[] = {"Analog In", "Digital In"};

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
	struct tascam_card *tascam = snd_kcontrol_chip(kcontrol);
	ucontrol->value.enumerated.item[0] = tascam->latency_profile;
	return 0;
}

static int tascam_latency_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct tascam_card *tascam = snd_kcontrol_chip(kcontrol);
	unsigned int new_profile = ucontrol->value.enumerated.item[0];

	if (new_profile >= 3)
		return -EINVAL;

	if (tascam->latency_profile != new_profile) {
		tascam->latency_profile = new_profile;
		return 1;
	}
	return 0;
}

static const struct snd_kcontrol_new tascam_latency_control = {
.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "Latency Profile",
.info = tascam_latency_info, .get = tascam_latency_get, .put = tascam_latency_put,
};

static int tascam_playback_source_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
uinfo->count = 1;
uinfo->value.enumerated.items = 2;
if (uinfo->value.enumerated.item >= 2)
uinfo->value.enumerated.item = 1;
strcpy(uinfo->value.enumerated.name, playback_source_texts[uinfo->value.enumerated.item]);
return 0;
}

static int tascam_line_out_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct tascam_card *tascam = snd_kcontrol_chip(kcontrol);
	ucontrol->value.enumerated.item[0] = tascam->line_out_source;
	return 0;
}

static int tascam_line_out_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct tascam_card *tascam = snd_kcontrol_chip(kcontrol);
	if (ucontrol->value.enumerated.item[0] > 1)
		return -EINVAL;
	if (tascam->line_out_source == ucontrol->value.enumerated.item[0])
		return 0;
	tascam->line_out_source = ucontrol->value.enumerated.item[0];
	return 1;
}

static const struct snd_kcontrol_new tascam_line_out_control = {
.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "Line Out Source",
.info = tascam_playback_source_info, .get = tascam_line_out_get, .put = tascam_line_out_put,
};

static int tascam_digital_out_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct tascam_card *tascam = snd_kcontrol_chip(kcontrol);
	ucontrol->value.enumerated.item[0] = tascam->digital_out_source;
	return 0;
}

static int tascam_digital_out_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct tascam_card *tascam = snd_kcontrol_chip(kcontrol);
	if (ucontrol->value.enumerated.item[0] > 1)
		return -EINVAL;
	if (tascam->digital_out_source == ucontrol->value.enumerated.item[0])
		return 0;
	tascam->digital_out_source = ucontrol->value.enumerated.item[0];
	return 1;
}

static const struct snd_kcontrol_new tascam_digital_out_control = {
.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "Digital Out Source",
.info = tascam_playback_source_info, .get = tascam_digital_out_get, .put = tascam_digital_out_put,
};

static int tascam_capture_source_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
uinfo->count = 1;
uinfo->value.enumerated.items = 2;
if (uinfo->value.enumerated.item >= 2)
uinfo->value.enumerated.item = 1;
strcpy(uinfo->value.enumerated.name, capture_source_texts[uinfo->value.enumerated.item]);
return 0;
}

static int tascam_capture_12_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct tascam_card *tascam = snd_kcontrol_chip(kcontrol);
	ucontrol->value.enumerated.item[0] = tascam->capture_12_source;
	return 0;
}

static int tascam_capture_12_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct tascam_card *tascam = snd_kcontrol_chip(kcontrol);
	if (ucontrol->value.enumerated.item[0] > 1)
		return -EINVAL;
	if (tascam->capture_12_source == ucontrol->value.enumerated.item[0])
		return 0;
	tascam->capture_12_source = ucontrol->value.enumerated.item[0];
	return 1;
}

static const struct snd_kcontrol_new tascam_capture_12_control = {
.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "Capture 1-2 Source",
.info = tascam_capture_source_info, .get = tascam_capture_12_get, .put = tascam_capture_12_put,
};

static int tascam_capture_34_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct tascam_card *tascam = snd_kcontrol_chip(kcontrol);
	ucontrol->value.enumerated.item[0] = tascam->capture_34_source;
	return 0;
}

static int tascam_capture_34_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct tascam_card *tascam = snd_kcontrol_chip(kcontrol);
	if (ucontrol->value.enumerated.item[0] > 1)
		return -EINVAL;
	if (tascam->capture_34_source == ucontrol->value.enumerated.item[0])
		return 0;
	tascam->capture_34_source = ucontrol->value.enumerated.item[0];
	return 1;
}

static const struct snd_kcontrol_new tascam_capture_34_control = {
.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "Capture 3-4 Source",
.info = tascam_capture_source_info, .get = tascam_capture_34_get, .put = tascam_capture_34_put,
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
 * tascam_free_urbs - Free all allocated URBs and associated buffers.
 * @tascam: the tascam_card instance.
 *
 * This function kills, unlinks, and frees all playback, feedback, and capture
 * URBs, along with their transfer buffers and the capture ring/decode buffers.
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

	for (i = 0; i < NUM_CAPTURE_URBS; i++) {
		if (tascam->capture_urbs[i]) {
			usb_kill_urb(tascam->capture_urbs[i]);
			usb_free_coherent(tascam->dev, tascam->capture_urb_alloc_size,
					  tascam->capture_urbs[i]->transfer_buffer,
					  tascam->capture_urbs[i]->transfer_dma);
			usb_free_urb(tascam->capture_urbs[i]);
			tascam->capture_urbs[i] = NULL;
		}
	}

	kfree(tascam->capture_decode_dst_block);
	tascam->capture_decode_dst_block = NULL;
	kfree(tascam->capture_decode_raw_block);
	tascam->capture_decode_raw_block = NULL;
	kfree(tascam->capture_ring_buffer);
	tascam->capture_ring_buffer = NULL;
}

/**
 * tascam_alloc_urbs - Allocate all URBs and associated buffers.
 * @tascam: the tascam_card instance.
 *
 * This function allocates and initializes all URBs for playback, feedback,
 * and capture, as well as the necessary buffers for capture data processing.
 *
 * Return: 0 on success, or a negative error code on failure.
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

	tascam->capture_urb_alloc_size = CAPTURE_URB_SIZE;
	for (i = 0; i < NUM_CAPTURE_URBS; i++) {
		struct urb *c_urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!c_urb)
			goto error;
		tascam->capture_urbs[i] = c_urb;

		c_urb->transfer_buffer = usb_alloc_coherent(tascam->dev, tascam->capture_urb_alloc_size,
						      GFP_KERNEL, &c_urb->transfer_dma);
		if (!c_urb->transfer_buffer)
			goto error;

		usb_fill_bulk_urb(c_urb, tascam->dev,
				  usb_rcvbulkpipe(tascam->dev, EP_AUDIO_IN),
				  c_urb->transfer_buffer,
				  tascam->capture_urb_alloc_size,
				  capture_urb_complete,
				  tascam);
		c_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	}

	tascam->capture_ring_buffer = kmalloc(CAPTURE_RING_BUFFER_SIZE, GFP_KERNEL);
	if (!tascam->capture_ring_buffer)
		goto error;

	tascam->capture_decode_raw_block = kmalloc(RAW_BYTES_PER_DECODE_BLOCK, GFP_KERNEL);
	if (!tascam->capture_decode_raw_block)
		goto error;

	tascam->capture_decode_dst_block = kmalloc(FRAMES_PER_DECODE_BLOCK * DECODED_CHANNELS_PER_FRAME * DECODED_SAMPLE_SIZE, GFP_KERNEL);
	if (!tascam->capture_decode_dst_block)
		goto error;
  
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

/**
 * us144mkii_configure_device_for_rate - Send USB control messages to set sample rate.
 * @tascam: the tascam_card instance.
 * @rate: the target sample rate (e.g., 44100, 96000).
 *
 * This function sends a sequence of vendor-specific and UAC control messages
 * to configure the device hardware for the specified sample rate.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
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
	case 0: feedback_packets = 1; break; /* Low */
	case 1: feedback_packets = 2; break; /* Normal */
	case 2: feedback_packets = 5; break; /* High */
	default: feedback_packets = 2;
	}

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

static int tascam_capture_prepare(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);

	tascam->driver_capture_pos = 0;
	tascam->capture_frames_processed = 0;
	tascam->last_capture_period_pos = 0;
	tascam->capture_ring_buffer_read_ptr = 0;
	tascam->capture_ring_buffer_write_ptr = 0;

	return 0;
}

static int tascam_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	unsigned long flags;
	int err = 0;
	int i;
	bool do_start = false;
	bool do_stop = false;

	spin_lock_irqsave(&tascam->lock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		if (!atomic_read(&tascam->playback_active)) {
			atomic_set(&tascam->playback_active, 1);
			atomic_set(&tascam->capture_active, 1);
			do_start = true;
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		if (atomic_read(&tascam->playback_active)) {
			atomic_set(&tascam->playback_active, 0);
			atomic_set(&tascam->capture_active, 0);
			do_stop = true;
		}
		break;
	default:
		err = -EINVAL;
		break;
	}
	spin_unlock_irqrestore(&tascam->lock, flags);

	if (do_start) {
		if (atomic_read(&tascam->active_urbs) > 0) {
			dev_warn(tascam->card->dev, "Cannot start, URBs still active.\n");
			return -EAGAIN;
		}

		for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
			err = usb_submit_urb(tascam->feedback_urbs[i], GFP_ATOMIC);
			if (err < 0)
				goto start_rollback;
			atomic_inc(&tascam->active_urbs);
		}
		for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
			err = usb_submit_urb(tascam->playback_urbs[i], GFP_ATOMIC);
			if (err < 0)
				goto start_rollback;
			atomic_inc(&tascam->active_urbs);
		}
		for (i = 0; i < NUM_CAPTURE_URBS; i++) {
			err = usb_submit_urb(tascam->capture_urbs[i], GFP_ATOMIC);
			if (err < 0)
				goto start_rollback;
			atomic_inc(&tascam->active_urbs);
		}
		return 0;
start_rollback:
		dev_err(tascam->card->dev, "Failed to submit URBs to start stream: %d\n", err);
		do_stop = true;
	}

	if (do_stop) {
		for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
			usb_unlink_urb(tascam->playback_urbs[i]);
			atomic_dec(&tascam->active_urbs);
		}
		for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
			usb_unlink_urb(tascam->feedback_urbs[i]);
			atomic_dec(&tascam->active_urbs);
		}
		for (i = 0; i < NUM_CAPTURE_URBS; i++) {
			usb_unlink_urb(tascam->capture_urbs[i]);
			atomic_dec(&tascam->active_urbs);
		}
		cancel_work_sync(&tascam->capture_work);
	}

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

static snd_pcm_uframes_t tascam_capture_pointer(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	u64 pos;
	unsigned long flags;

	if (!atomic_read(&tascam->capture_active))
		return 0;

	spin_lock_irqsave(&tascam->lock, flags);
	pos = tascam->capture_frames_processed;
	spin_unlock_irqrestore(&tascam->lock, flags);

	return runtime ? pos % runtime->buffer_size : 0;
}

static struct snd_pcm_ops tascam_playback_ops = {
	.open = tascam_playback_open,
	.close = tascam_playback_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = tascam_pcm_hw_params,
	.hw_free = tascam_pcm_hw_free,
	.prepare = tascam_playback_prepare,
	.trigger = tascam_pcm_trigger,
	.pointer = tascam_playback_pointer,
};

static struct snd_pcm_ops tascam_capture_ops = {
	.open = tascam_capture_open,
	.close = tascam_capture_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = tascam_pcm_hw_params,
	.hw_free = tascam_pcm_hw_free,
	.prepare = tascam_capture_prepare,
	.trigger = tascam_pcm_trigger,
	.pointer = tascam_capture_pointer,
};

/**
 * playback_urb_complete - Completion handler for playback isochronous URBs.
 * @urb: the completed URB.
 *
 * This function runs in interrupt context. It calculates the number of bytes
 * to send in the next set of packets based on the feedback-driven clock,
 * copies the audio data from the ALSA ring buffer (applying routing), and
 * resubmits the URB.
 */
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

	if (urb->status) {
		if (urb->status != -ENOENT && urb->status != -ECONNRESET && urb->status != -ESHUTDOWN)
			dev_err_ratelimited(tascam->card->dev, "Playback URB failed: %d\n", urb->status);
		return;
	}
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

			char *src_12 = src_frame;
			char *src_34 = src_frame + 6;
			char *dst_line_out = dst_frame;
			char *dst_digital_out = dst_frame + 6;

			if (tascam->line_out_source == 0)
				memcpy(dst_line_out, src_12, 6);
			else
				memcpy(dst_line_out, src_34, 6);

			if (tascam->digital_out_source == 0)
				memcpy(dst_digital_out, src_12, 6);
			else
				memcpy(dst_digital_out, src_34, 6);
		}
	}

	urb->dev = tascam->dev;
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0)
		dev_err_ratelimited(tascam->card->dev, "Failed to resubmit playback URB: %d\n", ret);
}

/**
 * feedback_urb_complete - Completion handler for feedback isochronous URBs.
 * @urb: the completed URB.
 *
 * This is the master clock for the driver. It runs in interrupt context.
 * It reads the feedback value from the device, which indicates how many
 * samples the device has consumed. This information is used to adjust the
 * playback rate and to advance the capture stream pointer, keeping both
 * streams in sync. It then calls snd_pcm_period_elapsed if necessary and
 * resubmits itself.
 */
static void feedback_urb_complete(struct urb *urb)
{
	struct tascam_card *tascam = urb->context;
	struct snd_pcm_substream *playback_ss, *capture_ss;
	struct snd_pcm_runtime *playback_rt, *capture_rt;
	unsigned long flags;
	u64 total_frames_in_urb = 0;
	int ret, p;
	unsigned int old_in_idx, new_in_idx;
	bool playback_period_elapsed = false;
	bool capture_period_elapsed = false;

	if (urb->status) {
		if (urb->status != -ENOENT && urb->status != -ECONNRESET && urb->status != -ESHUTDOWN)
			dev_err_ratelimited(tascam->card->dev, "Feedback URB failed: %d\n", urb->status);
		return;
	}
	if (!tascam || !atomic_read(&tascam->playback_active))
		return;

	playback_ss = tascam->playback_substream;
	if (!playback_ss || !playback_ss->runtime)
		return;
	playback_rt = playback_ss->runtime;

	capture_ss = tascam->capture_substream;
	capture_rt = capture_ss ? capture_ss->runtime : NULL;

	spin_lock_irqsave(&tascam->lock, flags);

	/* Hybrid Sync: Initial blind period for hardware to settle. */
	if (tascam->feedback_urb_skip_count > 0) {
		tascam->feedback_urb_skip_count--;
		goto unlock_and_continue;
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
			unsigned int nominal_frames = playback_rt->rate / 8000;
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

	if (total_frames_in_urb > 0) {
		tascam->playback_frames_consumed += total_frames_in_urb;
		if (atomic_read(&tascam->capture_active))
			tascam->capture_frames_processed += total_frames_in_urb;
	}

	if (playback_rt->period_size > 0) {
		u64 current_period = div_u64(tascam->playback_frames_consumed, playback_rt->period_size);
		if (current_period > tascam->last_period_pos) {
			tascam->last_period_pos = current_period;
			playback_period_elapsed = true;
		}
	}

	if (atomic_read(&tascam->capture_active) && capture_rt && capture_rt->period_size > 0) {
		u64 current_capture_period = div_u64(tascam->capture_frames_processed, capture_rt->period_size);
		if (current_capture_period > tascam->last_capture_period_pos) {
			tascam->last_capture_period_pos = current_capture_period;
			capture_period_elapsed = true;
		}
	}

unlock_and_continue:
	spin_unlock_irqrestore(&tascam->lock, flags);

	if (playback_period_elapsed)
		snd_pcm_period_elapsed(playback_ss);
	if (capture_period_elapsed)
		snd_pcm_period_elapsed(capture_ss);

	urb->dev = tascam->dev;
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0)
		dev_err_ratelimited(tascam->card->dev, "Failed to resubmit feedback URB: %d\n", ret);
}

/**
 * decode_tascam_capture_block - Decodes a raw 512-byte block from the device.
 * @src_block: Pointer to the 512-byte raw source block.
 * @dst_block: Pointer to the destination buffer for decoded audio frames.
 *
 * The device sends audio data in a complex, multiplexed format. This function
 * demultiplexes the bits from the raw block into 8 frames of 4-channel,
 * 24-bit audio (stored in 32-bit containers).
 */
static void decode_tascam_capture_block(const u8 *src_block, s32 *dst_block)
{
	int frame, bit;

	memset(dst_block, 0, FRAMES_PER_DECODE_BLOCK * DECODED_CHANNELS_PER_FRAME * DECODED_SAMPLE_SIZE);

	for (frame = 0; frame < FRAMES_PER_DECODE_BLOCK; ++frame) {
		const u8 *p_src_frame_base = src_block + frame * 64;
		s32 *p_dst_frame = dst_block + frame * 4;

		s32 ch[4] = {0};

		for (bit = 0; bit < 24; ++bit) {
			u8 byte1 = p_src_frame_base[bit];
			u8 byte2 = p_src_frame_base[bit + 32];

			ch[0] = (ch[0] << 1) | (byte1 & 1);
			ch[2] = (ch[2] << 1) | ((byte1 >> 1) & 1);

			ch[1] = (ch[1] << 1) | (byte2 & 1);
			ch[3] = (ch[3] << 1) | ((byte2 >> 1) & 1);
		}

		/* The result is a 24-bit sample. Shift left by 8 to align it to
		 * the most significant bits of a 32-bit integer (S32_LE format).
		 */
		p_dst_frame[0] = ch[0] << 8;
		p_dst_frame[1] = ch[1] << 8;
		p_dst_frame[2] = ch[2] << 8;
		p_dst_frame[3] = ch[3] << 8;
	}
}

/**
 * tascam_capture_work_handler - Deferred work handler for processing capture data.
 * @work: the work_struct instance.
 *
 * This function runs in a kernel thread context, not an IRQ context. It reads
 * raw data from the capture ring buffer, decodes it, applies routing, and
 * copies the final audio data into the ALSA capture ring buffer. This offloads
 * the CPU-intensive decoding from the time-sensitive URB completion handlers.
 */
static void tascam_capture_work_handler(struct work_struct *work)
{
	struct tascam_card *tascam = container_of(work, struct tascam_card, capture_work);
	struct snd_pcm_substream *substream = tascam->capture_substream;
	struct snd_pcm_runtime *runtime;
	unsigned long flags;

	if (!substream || !substream->runtime)
		return;
	runtime = substream->runtime;

	u8 *raw_block = tascam->capture_decode_raw_block;
	s32 *decoded_block = tascam->capture_decode_dst_block;

	if (!raw_block || !decoded_block) {
		dev_err(tascam->card->dev, "Capture decode buffers not allocated!\n");
		return;
	}

	if (runtime->period_size > 0) {
		u64 current_period = div_u64(tascam->playback_frames_consumed, runtime->period_size);
		if (current_period > tascam->last_period_pos) {
			tascam->last_period_pos = current_period;
			playback_period_elapsed = true;
		}
		spin_unlock_irqrestore(&tascam->lock, flags);

		if (!can_process)
			break;

		decode_tascam_capture_block(raw_block, decoded_block);

		spin_lock_irqsave(&tascam->lock, flags);
		if (atomic_read(&tascam->capture_active)) {
			int f;
			for (f = 0; f < FRAMES_PER_DECODE_BLOCK; ++f) {
				s32 *decoded_frame = decoded_block + (f * DECODED_CHANNELS_PER_FRAME);
				char *dst_frame = runtime->dma_area + frames_to_bytes(runtime, tascam->driver_capture_pos);

				s32 *src_analog = decoded_frame;
				s32 *src_digital = decoded_frame + 2;

				/* The decoded samples are in S32_LE format. The ALSA format is
				 * S24_3LE. We copy the 3 least significant bytes by starting
				 * the memcpy from the second byte of the 32-bit integer.
				 */
				if (tascam->capture_12_source == 0) {
					memcpy(dst_frame, ((char *)src_analog) + 1, 3);     // Ch1 from Analog 1
					memcpy(dst_frame + 3, ((char *)(src_analog + 1)) + 1, 3); // Ch2 from Analog 2
				} else {
					memcpy(dst_frame, ((char *)src_digital) + 1, 3);     // Ch1 from Digital 1
					memcpy(dst_frame + 3, ((char *)(src_digital + 1)) + 1, 3); // Ch2 from Digital 2
				}

				/* Since the device has only two analog inputs, channels 3-4 can be
				 * sourced from a copy of the analog inputs or the digital input.
				 */
				if (tascam->capture_34_source == 0) {
					memcpy(dst_frame + 6, ((char *)src_analog) + 1, 3);     // Ch3 from Analog 1
					memcpy(dst_frame + 9, ((char *)(src_analog + 1)) + 1, 3); // Ch4 from Analog 2
				} else {
					memcpy(dst_frame + 6, ((char *)src_digital) + 1, 3);     // Ch3 from Digital 1
					memcpy(dst_frame + 9, ((char *)(src_digital + 1)) + 1, 3); // Ch4 from Digital 2
				}

				tascam->driver_capture_pos++;
				if (tascam->driver_capture_pos >= runtime->buffer_size)
					tascam->driver_capture_pos = 0;
			}
		}
		spin_unlock_irqrestore(&tascam->lock, flags);
	}
}

/**
 * capture_urb_complete - Completion handler for capture bulk URBs.
 * @urb: the completed URB.
 *
 * This function runs in interrupt context. It copies the received raw data
 * into an intermediate ring buffer and then schedules the workqueue to process
 * it. It then resubmits the URB to receive more data.
 */
static void capture_urb_complete(struct urb *urb)
{
	struct tascam_card *tascam = urb->context;
	int ret;
	unsigned long flags;

	if (urb->status) {
		if (urb->status != -ENOENT && urb->status != -ECONNRESET && urb->status != -ESHUTDOWN)
			dev_err_ratelimited(tascam->card->dev, "Capture URB failed: %d\n", urb->status);
		return;
	}
	if (!tascam || !atomic_read(&tascam->capture_active))
		return;

	if (urb->actual_length > 0) {
		size_t i;
		size_t write_ptr;

		spin_lock_irqsave(&tascam->lock, flags);
		write_ptr = tascam->capture_ring_buffer_write_ptr;
		for (i = 0; i < urb->actual_length; i++) {
			tascam->capture_ring_buffer[write_ptr] = ((u8 *)urb->transfer_buffer)[i];
			write_ptr = (write_ptr + 1) % CAPTURE_RING_BUFFER_SIZE;
		}
		tascam->capture_ring_buffer_write_ptr = write_ptr;
		spin_unlock_irqrestore(&tascam->lock, flags);

		schedule_work(&tascam->capture_work);
	}

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
	if (err < 0) return err;
	err = snd_ctl_add(tascam->card, snd_ctl_new1(&tascam_line_out_control, tascam));
	if (err < 0) return err;
	err = snd_ctl_add(tascam->card, snd_ctl_new1(&tascam_digital_out_control, tascam));
	if (err < 0) return err;
	err = snd_ctl_add(tascam->card, snd_ctl_new1(&tascam_capture_12_control, tascam));
	if (err < 0) return err;
	err = snd_ctl_add(tascam->card, snd_ctl_new1(&tascam_capture_34_control, tascam));
	if (err < 0) return err;

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

static int tascam_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct tascam_card *tascam = usb_get_intfdata(intf);

	if (!tascam || !tascam->pcm)
		return 0;

	snd_pcm_suspend_all(tascam->pcm);
	cancel_work_sync(&tascam->capture_work);

	return 0;
}

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

/**
 * tascam_probe - Entry point for when the USB device is detected.
 * @intf: the USB interface that was matched.
 * @usb_id: the matching USB device ID.
 *
 * This function is called by the USB core when a device matching the driver's
 * ID table is connected. It allocates the sound card, initializes the driver
 * data structure, claims interfaces, sets up the device, creates the PCM
 * and control interfaces, and registers the sound card with ALSA.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
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
	atomic_set(&tascam->active_urbs, 0);
	INIT_WORK(&tascam->capture_work, tascam_capture_work_handler);
	tascam->latency_profile = 1;
	tascam->line_out_source = 0;
	tascam->digital_out_source = 1;
	tascam->capture_12_source = 0;
	tascam->capture_34_source = 1;
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
	if (err == 1 && handshake_buf[0] == HANDSHAKE_SUCCESS_VAL)
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

/**
 * tascam_disconnect - Entry point for when the USB device is disconnected.
 * @intf: the USB interface being disconnected.
 *
 * This function is called by the USB core when the device is removed. It
 * cancels any pending work, disconnects the sound card from ALSA, releases
 * claimed interfaces, and schedules the card structure to be freed.
 */
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

	cancel_work_sync(&tascam->capture_work);

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
