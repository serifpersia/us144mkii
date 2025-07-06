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

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>

MODULE_AUTHOR("serifpersia <ramiserifpersia@gmail.com>");
MODULE_DESCRIPTION("ALSA Driver for TASCAM US-144MKII");
MODULE_LICENSE("GPL");

#define DRIVER_NAME "snd-usb-us144mkii"

/*
 * TODO:
 * - Implement audio input capture.
 * - Implement MIDI IN/OUT.
 * - Expose hardware features via the ALSA Control API (mixers):
 *   - Digital output format selection.
 *   - Input/output routing.
 */


/* --- Module Parameters --- */
static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = {1, [1 ... (SNDRV_CARDS - 1)] = 0};
static int dev_idx;

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for the US-144MKII soundcard.");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for the US-144MKII soundcard.");
module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "Enable this US-144MKII soundcard.");

/* --- USB Device Identification --- */
#define USB_VID_TASCAM				0x0644
#define USB_PID_TASCAM_US144MKII	0x8020

/* --- USB Endpoints (Alternate Setting 1) --- */
#define EP_PLAYBACK_FEEDBACK	0x81
#define EP_AUDIO_OUT			0x02
#define EP_MIDI_IN				0x83
#define EP_MIDI_OUT				0x04
#define EP_AUDIO_IN				0x86

/* --- USB Control Message Protocol --- */
#define RT_H2D_CLASS_EP		(USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_ENDPOINT)
#define RT_H2D_VENDOR_DEV	(USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE)
#define RT_D2H_VENDOR_DEV	(USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE)
#define UAC_SET_CUR					0x01
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
#define NUM_PLAYBACK_URBS				8
#define NUM_FEEDBACK_URBS				4
#define MAX_FEEDBACK_PACKETS			5
#define MAX_PLAYBACK_URB_ISO_PACKETS 	8
#define FEEDBACK_PACKET_SIZE			3
#define USB_CTRL_TIMEOUT_MS				1000

/* --- Audio Format Configuration --- */
#define BYTES_PER_SAMPLE	3
#define NUM_CHANNELS		4 /* Changed: Match hardware for efficient copy */
#define BYTES_PER_FRAME		(NUM_CHANNELS * BYTES_PER_SAMPLE)
#define FEEDBACK_ACCUMULATOR_SIZE 128

struct tascam_card;
static struct usb_driver tascam_alsa_driver;

/* --- Forward Declarations --- */
static void playback_urb_complete(struct urb *urb);
static void feedback_urb_complete(struct urb *urb);
static int us144mkii_configure_device_for_rate(struct tascam_card *tascam, int rate);
static int tascam_probe(struct usb_interface *intf, const struct usb_device_id *id);
static void tascam_disconnect(struct usb_interface *intf);
static int tascam_suspend(struct usb_interface *intf, pm_message_t message);
static int tascam_resume(struct usb_interface *intf);

/* --- Rate-to-Packet Fixing Data --- */
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

	/* Stores the hardware profile index decided in hw_params for use in prepare */
	int profile_idx;

	unsigned int feedback_accumulator_pattern[FEEDBACK_ACCUMULATOR_SIZE];
	unsigned int feedback_pattern_out_idx;
	unsigned int feedback_pattern_in_idx;
	bool feedback_synced;
	unsigned int feedback_urb_skip_count;

	u64 playback_frames_consumed;
	snd_pcm_uframes_t driver_playback_pos;
	u64 last_period_pos;

	const unsigned int (*feedback_patterns)[8];
	unsigned int feedback_base_value;
	unsigned int feedback_max_value;
};

static const struct snd_pcm_hardware tascam_pcm_hw = {
	.info = (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
		 SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_MMAP_VALID |
		 SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME),
	.formats = SNDRV_PCM_FMTBIT_S24_3LE,
	.rates = (SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
		  SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000),
	.rate_min = 44100, .rate_max = 96000,
	.channels_min = NUM_CHANNELS, /* Changed: Expose 4 channels */
	.channels_max = NUM_CHANNELS, /* Changed: Expose 4 channels */
	.buffer_bytes_max = 1024 * 1024,
	.period_bytes_min = 48 * BYTES_PER_FRAME,
	.period_bytes_max = 1024 * BYTES_PER_FRAME,
	.periods_min = 2, .periods_max = 1024,
};

/**
 * tascam_free_urbs - Free all allocated URBs and their buffers.
 * @tascam: The card instance.
 *
 * This function is the counterpart to tascam_alloc_urbs. It is called
 * when the PCM device is closed to release all USB resources.
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
 *
 * Allocates all necessary URBs for playback and feedback. This is called
 * when the PCM device is opened.
 * Returns: 0 on success, or a negative error code on failure.
 */
static int tascam_alloc_urbs(struct tascam_card *tascam)
{
	int i;
	size_t max_packet_size;

	max_packet_size = ((96000 / 8000) + 2) * BYTES_PER_FRAME;
	tascam->playback_urb_alloc_size = max_packet_size * MAX_PLAYBACK_URB_ISO_PACKETS;

	for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
		struct urb *urb = usb_alloc_urb(MAX_PLAYBACK_URB_ISO_PACKETS, GFP_KERNEL);
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
	unsigned int period_frames = params_period_size(params);

	/* Latency profile thresholds (in frames) based on hardware specification */
	unsigned int profile_thresholds[5];

	err = snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(params));
	if (err < 0)
		return err;

	/* Set profile thresholds based on the selected sample rate */
	switch (rate) {
	case 44100:
		profile_thresholds[0] = 49;   /* Lowest */
		profile_thresholds[1] = 64;   /* Low */
		profile_thresholds[2] = 128;  /* Normal */
		profile_thresholds[3] = 256;  /* High */
		profile_thresholds[4] = 512;  /* Highest */
		break;
	case 48000:
		profile_thresholds[0] = 48;
		profile_thresholds[1] = 64;
		profile_thresholds[2] = 128;
		profile_thresholds[3] = 256;
		profile_thresholds[4] = 512;
		break;
	case 88200:
		profile_thresholds[0] = 98;
		profile_thresholds[1] = 128;
		profile_thresholds[2] = 256;
		profile_thresholds[3] = 512;
		profile_thresholds[4] = 1024;
		break;
	case 96000:
		profile_thresholds[0] = 96;
		profile_thresholds[1] = 128;
		profile_thresholds[2] = 256;
		profile_thresholds[3] = 512;
		profile_thresholds[4] = 1024;
		break;
	default:
		return -EINVAL;
	}

	/* Map the application's requested period size to a hardware profile */
	if (period_frames <= profile_thresholds[0])
		tascam->profile_idx = 0;
	else if (period_frames <= profile_thresholds[1])
		tascam->profile_idx = 1;
	else if (period_frames <= profile_thresholds[2])
		tascam->profile_idx = 2;
	else if (period_frames <= profile_thresholds[3])
		tascam->profile_idx = 3;
	else /* Anything larger falls into the highest latency profile */
		tascam->profile_idx = 4;

	dev_info(tascam->card->dev,
		 "User requested period of %u frames @ %u Hz, mapping to hardware profile %d\n",
		 period_frames, rate, tascam->profile_idx);

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
	}

	/* Re-configure hardware only if the sample rate has changed */
	if (tascam->current_rate != rate) {
		err = us144mkii_configure_device_for_rate(tascam, rate);
		if (err < 0) {
			tascam->current_rate = 0;
			return err;
		}
		tascam->current_rate = rate;
	}

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
	unsigned int playback_urb_iso_packets;

	/* Feedback packet counts for each of the 5 hardware profiles */
	static const unsigned int feedback_packets_for_profile[] = { 1, 1, 2, 5, 5 };

	/* Reset driver state for the new stream */
	tascam->driver_playback_pos = 0;
	tascam->playback_frames_consumed = 0;
	tascam->last_period_pos = 0;
	tascam->feedback_pattern_in_idx = 0;
	tascam->feedback_pattern_out_idx = 0;
	tascam->feedback_synced = false;
	tascam->feedback_urb_skip_count = NUM_FEEDBACK_URBS * 2;

	dev_dbg(tascam->card->dev, "Prepare: Sync state reset, starting in unsynced mode.\n");

	/* Initialize feedback accumulator with nominal values */
	nominal_frames_per_packet = runtime->rate / 8000;
	for (i = 0; i < FEEDBACK_ACCUMULATOR_SIZE; i++)
		tascam->feedback_accumulator_pattern[i] = nominal_frames_per_packet;

	/*
	 * Program URBs safely based on the configuration chosen in hw_params.
	 * This is the correct location, as the stream is guaranteed to be stopped.
	 */

	/* Configure Feedback URBs with the correct number of packets for the profile */
	for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
		struct urb *f_urb = tascam->feedback_urbs[i];
		unsigned int packets = feedback_packets_for_profile[tascam->profile_idx];
		int j;

		f_urb->number_of_packets = packets;
		f_urb->transfer_buffer_length = packets * FEEDBACK_PACKET_SIZE;
		for (j = 0; j < packets; j++) {
			f_urb->iso_frame_desc[j].offset = j * FEEDBACK_PACKET_SIZE;
			f_urb->iso_frame_desc[j].length = FEEDBACK_PACKET_SIZE;
		}
	}

	/*
	 * Configure Playback URBs. The number of packets is always 40,
	 * as per the hardware specification.
	 */
	playback_urb_iso_packets = MAX_PLAYBACK_URB_ISO_PACKETS;
	nominal_bytes_per_packet = nominal_frames_per_packet * BYTES_PER_FRAME;
	total_bytes_in_urb = nominal_bytes_per_packet * playback_urb_iso_packets;

	for (u = 0; u < NUM_PLAYBACK_URBS; u++) {
		struct urb *urb = tascam->playback_urbs[u];

		memset(urb->transfer_buffer, 0, tascam->playback_urb_alloc_size);
		urb->transfer_buffer_length = total_bytes_in_urb;
		urb->number_of_packets = playback_urb_iso_packets;
		for (i = 0; i < playback_urb_iso_packets; i++) {
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
	unsigned int total_frames_for_urb = 0;
	size_t total_bytes_for_urb = 0;
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

	/*
	 * Phase 1: Calculate the total number of frames needed for this URB.
	 * The number of frames per packet varies based on the feedback from the device.
	 */
	for (i = 0; i < urb->number_of_packets; i++) {
		unsigned int frames_for_packet;

		if (tascam->feedback_synced) {
			frames_for_packet = tascam->feedback_accumulator_pattern[
				(tascam->feedback_pattern_out_idx + i) % FEEDBACK_ACCUMULATOR_SIZE];
		} else {
			frames_for_packet = runtime->rate / 8000;
		}
		total_frames_for_urb += frames_for_packet;
	}
	total_bytes_for_urb = total_frames_for_urb * BYTES_PER_FRAME;

	/*
	 * Phase 2: Perform an efficient bulk memory copy.
	 * This replaces the inefficient per-frame copy loop. It handles the
	 * wrap-around case for the ALSA circular buffer.
	 */
	src_buf = runtime->dma_area;
	dst_buf = urb->transfer_buffer;
	if (total_bytes_for_urb > 0) {
		snd_pcm_uframes_t offset_frames = tascam->driver_playback_pos;
		snd_pcm_uframes_t frames_to_end = runtime->buffer_size - offset_frames;
		size_t bytes_to_end = frames_to_bytes(runtime, frames_to_end);

		if (total_bytes_for_urb > bytes_to_end) {
			/* Data wraps around the end of the circular buffer */
			memcpy(dst_buf, src_buf + frames_to_bytes(runtime, offset_frames), bytes_to_end);
			memcpy(dst_buf + bytes_to_end, src_buf, total_bytes_for_urb - bytes_to_end);
		} else {
			/* Data is in a single contiguous block */
			memcpy(dst_buf, src_buf + frames_to_bytes(runtime, offset_frames), total_bytes_for_urb);
		}
	}
	tascam->driver_playback_pos = (tascam->driver_playback_pos + total_frames_for_urb) % runtime->buffer_size;

	/*
	 * Phase 3: Populate the isochronous frame descriptors.
	 * The USB controller requires the offset and length for each packet within the URB.
	 */
	urb->transfer_buffer_length = total_bytes_for_urb;
	total_bytes_for_urb = 0; /* Reuse as running offset */
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

	spin_unlock_irqrestore(&tascam->lock, flags);

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
	bool was_synced, sync_lost_this_urb = false;
	int ret, p;

	if (urb->status) return;
	if (!tascam || !atomic_read(&tascam->playback_active)) return;
	substream = tascam->playback_substream;
	if (!substream || !substream->runtime) return;
	runtime = substream->runtime;

	spin_lock_irqsave(&tascam->lock, flags);
	if (urb->status != 0) {
		dev_warn_ratelimited(tascam->card->dev, "Feedback URB failed with status %d\n", urb->status);
		sync_lost_this_urb = true;
		goto update_sync_state;
	}
	if (tascam->feedback_urb_skip_count > 0) {
		tascam->feedback_urb_skip_count--;
		goto unlock_and_resubmit;
	}
	for (p = 0; p < urb->number_of_packets; p++) {
		u8 feedback_value;
		const unsigned int *pattern;
		if (urb->iso_frame_desc[p].status != 0 || urb->iso_frame_desc[p].actual_length < 1) {
			sync_lost_this_urb = true;
			continue;
		}
		feedback_value = *((u8 *)urb->transfer_buffer + urb->iso_frame_desc[p].offset);
		if (feedback_value >= tascam->feedback_base_value &&
		    feedback_value <= tascam->feedback_max_value) {
			pattern = tascam->feedback_patterns[feedback_value - tascam->feedback_base_value];
			int i;
			for (i = 0; i < 8; i++) {
				unsigned int in_idx = (tascam->feedback_pattern_in_idx + i) % FEEDBACK_ACCUMULATOR_SIZE;
				tascam->feedback_accumulator_pattern[in_idx] = pattern[i];
				total_frames_in_urb += pattern[i];
			}
			tascam->feedback_pattern_in_idx = (tascam->feedback_pattern_in_idx + 8) % FEEDBACK_ACCUMULATOR_SIZE;
		} else {
			sync_lost_this_urb = true;
			total_frames_in_urb += runtime->rate / 1000;
		}
	}
update_sync_state:
	was_synced = tascam->feedback_synced;
	if (sync_lost_this_urb) {
		if (was_synced) dev_dbg(tascam->card->dev, "Sync Lost!\n");
		tascam->feedback_synced = false;
	} else {
		if (!was_synced) dev_dbg(tascam->card->dev, "Sync Acquired!\n");
		tascam->feedback_synced = true;
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

	err = snd_pcm_new(tascam->card, "US144MKII PCM", 0, 1, 1, &pcm);
	if (err < 0) return err;
	tascam->pcm = pcm;
	pcm->private_data = tascam;
	strscpy(pcm->name, "US-144MKII Audio", sizeof(pcm->name));
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
 *
 * Stops all active audio streams to prepare for system sleep.
 *
 * Returns: 0 on success.
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
 *
 * Re-initializes the device hardware after system resume, restoring its
 * alternate settings and sample rate configuration. This is necessary because
 * the device may lose its state during suspend.
 *
 * Returns: 0 on success, or a negative error code on failure.
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

	/*
	 * Re-configure the device for the last used sample rate.
	 * If no stream was ever started, current_rate will be 0, and we skip this.
	 * The ALSA core will handle resuming the PCM streams, which will
	 * trigger our .prepare and .trigger ops as needed.
	 */
	if (tascam->current_rate > 0) {
		dev_info(&intf->dev, "Restoring sample rate to %d Hz\n", tascam->current_rate);
		err = us144mkii_configure_device_for_rate(tascam, tascam->current_rate);
		if (err < 0) {
			dev_err(&intf->dev, "Resume: Failed to restore sample rate configuration\n");
			/* Invalidate the rate so the next hw_params will re-configure fully. */
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

	if (intf != tascam->iface0)
		return;

	dev_info(&intf->dev, "TASCAM US-144MKII disconnecting...\n");
	snd_card_disconnect(tascam->card);

	if (tascam->iface1) {
		usb_set_intfdata(tascam->iface1, NULL);
		usb_driver_release_interface(&tascam_alsa_driver, tascam->iface1);
		tascam->iface1 = NULL;
	}

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
