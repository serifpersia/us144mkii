// SPDX-License-Identifier: GPL-2.0
// (c) 2024 serifpersia <ramiserifpersia@gmail.com>
/*
 * ALSA Driver for TASCAM US-144MKII Audio Interface
 */

// TODO: Implement MIDI IN/OUT
// TODO: Create ALSA mixer controls and a GUI tool for routing, latency, etc.

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/delay.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>

MODULE_AUTHOR("serifpersia");
MODULE_DESCRIPTION("ALSA Driver for TASCAM US-144MKII");
MODULE_LICENSE("GPL");

#define DRIVER_NAME "us144mkii"

/* Module Parameters */
static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = {1, [1 ... (SNDRV_CARDS - 1)] = 0};

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for the US-144MKII soundcard.");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for the US-144MKII soundcard.");
module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "Enable this US-144MKII soundcard.");

/* Constants and Structs */
#define TASCAM_VID 0x0644
#define TASCAM_PID 0x8020

/* USB Endpoints */
#define EP_AUDIO_OUT         0x02 // Isochronous OUT for playback audio
#define EP_PLAYBACK_FEEDBACK 0x81 // Isochronous IN for clock feedback
#define EP_CAPTURE_DATA      0x86 // Bulk IN for capture audio
#define EP_MIDI_OUT          0x04 // Bulk OUT for MIDI
#define EP_MIDI_IN           0x83 // Bulk IN for MIDI

/* USB Control Message Request Types */
#define RT_H2D_CLASS_EP   0x22 // Host-to-Device, Class, Endpoint
#define RT_H2D_VENDOR_DEV 0x40 // Host-to-Device, Vendor, Device
#define RT_D2H_VENDOR_DEV 0xc0 // Device-to-Host, Vendor, Device

/* USB Control Message Requests */
#define UAC_SET_CUR 0x01
#define UAC_SAMPLING_FREQ_CONTROL 0x0100
#define VENDOR_REQ_REGISTER_WRITE 65 // bRequest 0x41
#define VENDOR_REQ_MODE_CONTROL   73 // bRequest 0x49

/* URB Configuration */
#define NUM_PLAYBACK_URBS 8
#define NUM_FEEDBACK_URBS 4
#define NUM_CAPTURE_URBS 4
#define MAX_FEEDBACK_PACKETS 5
#define MAX_PLAYBACK_URB_ISO_PACKETS 40
#define FEEDBACK_PACKET_SIZE 3
#define USB_CTRL_TIMEOUT_MS 1000

/* Audio Format Configuration */
#define BYTES_PER_SAMPLE_24 3
#define BYTES_PER_SAMPLE_32 4

// Playback: ALSA sees 2ch S24_3LE, device gets 4ch 24-bit (2 padded)
#define PLAYBACK_ALSA_CHANNELS 2
#define PLAYBACK_DEVICE_CHANNELS 4
#define PLAYBACK_ALSA_BYTES_PER_FRAME (PLAYBACK_ALSA_CHANNELS * BYTES_PER_SAMPLE_24)
#define PLAYBACK_DEVICE_BYTES_PER_FRAME (PLAYBACK_DEVICE_CHANNELS * BYTES_PER_SAMPLE_24)

// Capture: Device gives 4ch 24-bit, ALSA sees 4ch S32_LE
#define CAPTURE_CHANNELS 4
#define CAPTURE_DEVICE_BYTES_PER_FRAME (CAPTURE_CHANNELS * BYTES_PER_SAMPLE_24)
#define CAPTURE_ALSA_BYTES_PER_FRAME (CAPTURE_CHANNELS * BYTES_PER_SAMPLE_32)

#define FEEDBACK_ACCUMULATOR_SIZE 128

static struct usb_driver tascam_alsa_driver;

/* Main driver data structure */
struct tascam_card {
	struct usb_device *dev;
	struct usb_interface *iface0;
	struct usb_interface *iface1;
	struct snd_card *card;
	struct snd_pcm *pcm;

	struct snd_pcm_substream *playback_substream;
	struct urb *playback_urbs[NUM_PLAYBACK_URBS];
	size_t playback_urb_alloc_size;
	unsigned int playback_urb_iso_packets;

	struct urb *feedback_urbs[NUM_FEEDBACK_URBS];
	size_t feedback_urb_alloc_size;

	struct snd_pcm_substream *capture_substream;
	struct urb *capture_urbs[NUM_CAPTURE_URBS];
	size_t capture_urb_buffer_size;
	atomic_t capture_active;
	snd_pcm_uframes_t driver_capture_pos;

	spinlock_t lock;
	atomic_t playback_active;
	int current_rate;

	/* Feedback Synchronization State */
	unsigned int feedback_accumulator_pattern[FEEDBACK_ACCUMULATOR_SIZE];
	unsigned int feedback_pattern_out_idx;
	unsigned int feedback_pattern_in_idx;
	bool feedback_synced;
	unsigned int feedback_urb_skip_count;

	/* Playback Position Tracking */
	snd_pcm_uframes_t driver_playback_pos;
	u64 playback_frames_consumed;
	u64 last_period_pos;

	/* Rate-Specific Data */
	const unsigned int (*feedback_patterns)[8];
	unsigned int feedback_base_value;
	unsigned int feedback_max_value;

	// SUSPEND/RESUME: Store stream state across suspend/resume
	bool was_playback_active;
	bool was_capture_active;
};

/*
 * Latency Profile Cheatsheet
 */
static const unsigned int hardware_feedback_packet_counts[] = { 1, 2, 5 };
static const unsigned int playback_iso_packet_counts_tiers[] = { 8, 24, 40 };

/*
 * Pre-calculated patterns for frames-per-microframe based on feedback value.
 */
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

/* Forward Declarations */
static void tascam_free_playback_urbs(struct tascam_card *tascam);
static void tascam_free_capture_urbs(struct tascam_card *tascam);
static void tascam_free_all_urbs(struct tascam_card *tascam);
static int tascam_alloc_urbs(struct tascam_card *tascam, bool is_playback);
static int tascam_playback_open(struct snd_pcm_substream *substream);
static int tascam_playback_close(struct snd_pcm_substream *substream);
static int tascam_playback_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params);
static int tascam_playback_hw_free(struct snd_pcm_substream *substream);
static int tascam_playback_prepare(struct snd_pcm_substream *substream);
static int tascam_playback_trigger(struct snd_pcm_substream *substream, int cmd);
static snd_pcm_uframes_t tascam_playback_pointer(struct snd_pcm_substream *substream);
static void playback_urb_complete(struct urb *urb);
static void feedback_urb_complete(struct urb *urb);
static int tascam_capture_open(struct snd_pcm_substream *substream);
static int tascam_capture_close(struct snd_pcm_substream *substream);
static int tascam_capture_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params);
static int tascam_capture_hw_free(struct snd_pcm_substream *substream);
static int tascam_capture_prepare(struct snd_pcm_substream *substream);
static int tascam_capture_trigger(struct snd_pcm_substream *substream, int cmd);
static snd_pcm_uframes_t tascam_capture_pointer(struct snd_pcm_substream *substream);
static void capture_urb_complete(struct urb *urb);
static int us144mkii_configure_device_for_rate(struct tascam_card *tascam, int rate);
static int tascam_create_pcm(struct tascam_card *tascam);
static int tascam_probe(struct usb_interface *intf, const struct usb_device_id *id);
static void tascam_disconnect(struct usb_interface *intf);
static int tascam_suspend(struct usb_interface *intf, pm_message_t message);
static int tascam_resume(struct usb_interface *intf);

/* PCM Hardware Definitions */
static const struct snd_pcm_hardware tascam_playback_hw = {
	.info = (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
	SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_MMAP_VALID |
	SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME),
	.formats = SNDRV_PCM_FMTBIT_S24_3LE,
	.rates = (SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
	SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000),
	.rate_min = 44100,
	.rate_max = 96000,
	.channels_min = PLAYBACK_ALSA_CHANNELS,
	.channels_max = PLAYBACK_ALSA_CHANNELS,
	.buffer_bytes_max = 1024 * 1024,
	.period_bytes_min = 48 * PLAYBACK_ALSA_BYTES_PER_FRAME,
	.period_bytes_max = 1024 * PLAYBACK_ALSA_BYTES_PER_FRAME,
	.periods_min = 2,
	.periods_max = 1024,
};

static const struct snd_pcm_hardware tascam_capture_hw = {
	.info = (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
	SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_MMAP_VALID |
	SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME),
	.formats = SNDRV_PCM_FMTBIT_S32_LE,
	.rates = (SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
	SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000),
	.rate_min = 44100,
	.rate_max = 96000,
	.channels_min = CAPTURE_CHANNELS,
	.channels_max = CAPTURE_CHANNELS,
	.buffer_bytes_max = 1024 * 1024,
	.period_bytes_min = 48 * CAPTURE_ALSA_BYTES_PER_FRAME,
	.period_bytes_max = 4096 * CAPTURE_ALSA_BYTES_PER_FRAME,
	.periods_min = 2,
	.periods_max = 1024,
};

/* PCM Operations */
static struct snd_pcm_ops tascam_playback_ops = {
	.open =		tascam_playback_open,
	.close =	tascam_playback_close,
	.ioctl =	snd_pcm_lib_ioctl,
	.hw_params =	tascam_playback_hw_params,
	.hw_free =	tascam_playback_hw_free,
	.prepare =	tascam_playback_prepare,
	.trigger =	tascam_playback_trigger,
	.pointer =	tascam_playback_pointer,
};

static struct snd_pcm_ops tascam_capture_ops = {
	.open =		tascam_capture_open,
	.close =	tascam_capture_close,
	.ioctl =	snd_pcm_lib_ioctl,
	.hw_params =	tascam_capture_hw_params,
	.hw_free =	tascam_capture_hw_free,
	.prepare =	tascam_capture_prepare,
	.trigger =	tascam_capture_trigger,
	.pointer =	tascam_capture_pointer,
};

static void deinterleave_capture_data(s32 *dest_buf, const u8 *src_buf, unsigned int frames)
{
	unsigned int i;
	for (i = 0; i < frames; i++) {
		s32 sample;

		// Unpack 24-bit little-endian samples and sign-extend to 32-bit
		sample = src_buf[0] | (src_buf[1] << 8) | (src_buf[2] << 16);
		if (sample & 0x00800000) sample |= 0xff000000;
		dest_buf[0] = sample;

		sample = src_buf[3] | (src_buf[4] << 8) | (src_buf[5] << 16);
		if (sample & 0x00800000) sample |= 0xff000000;
		dest_buf[1] = sample;

		sample = src_buf[6] | (src_buf[7] << 8) | (src_buf[8] << 16);
		if (sample & 0x00800000) sample |= 0xff000000;
		dest_buf[2] = sample;

		sample = src_buf[9] | (src_buf[10] << 8) | (src_buf[11] << 16);
		if (sample & 0x00800000) sample |= 0xff000000;
		dest_buf[3] = sample;

		src_buf += CAPTURE_DEVICE_BYTES_PER_FRAME;
		dest_buf += CAPTURE_CHANNELS;
	}
}

static void tascam_free_playback_urbs(struct tascam_card *tascam)
{
	int i;

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
}

static void tascam_free_capture_urbs(struct tascam_card *tascam)
{
	int i;

	for (i = 0; i < NUM_CAPTURE_URBS; i++) {
		if (tascam->capture_urbs[i]) {
			usb_kill_urb(tascam->capture_urbs[i]);
			if (tascam->capture_urbs[i]->transfer_buffer) {
				usb_free_coherent(tascam->dev, tascam->capture_urb_buffer_size,
								  tascam->capture_urbs[i]->transfer_buffer,
					  tascam->capture_urbs[i]->transfer_dma);
			}
			usb_free_urb(tascam->capture_urbs[i]);
			tascam->capture_urbs[i] = NULL;
		}
	}
}
static void tascam_free_all_urbs(struct tascam_card *tascam)
{
	tascam_free_playback_urbs(tascam);
	tascam_free_capture_urbs(tascam);
}

static int tascam_alloc_urbs(struct tascam_card *tascam, bool is_playback)
{
	int i;

	if (is_playback) {
		size_t max_frames_per_packet, max_packet_size;
		max_frames_per_packet = (96000 / 8000) + 2;
		max_packet_size = max_frames_per_packet * PLAYBACK_DEVICE_BYTES_PER_FRAME;
		tascam->playback_urb_alloc_size = max_packet_size * MAX_PLAYBACK_URB_ISO_PACKETS;

		if (tascam->playback_urb_alloc_size == 0)
			return -EINVAL;

		for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
			struct urb *urb = usb_alloc_urb(MAX_PLAYBACK_URB_ISO_PACKETS, GFP_KERNEL);
			if (!urb) goto error;
			tascam->playback_urbs[i] = urb;
			urb->transfer_buffer = usb_alloc_coherent(tascam->dev, tascam->playback_urb_alloc_size, GFP_KERNEL, &urb->transfer_dma);
			if (!urb->transfer_buffer) goto error;
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
			if (!f_urb) goto error;
			tascam->feedback_urbs[i] = f_urb;
			f_urb->transfer_buffer = usb_alloc_coherent(tascam->dev, tascam->feedback_urb_alloc_size, GFP_KERNEL, &f_urb->transfer_dma);
			if (!f_urb->transfer_buffer) goto error;
			f_urb->dev = tascam->dev;
			f_urb->pipe = usb_rcvisocpipe(tascam->dev, EP_PLAYBACK_FEEDBACK);
			f_urb->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
			f_urb->interval = 4;
			f_urb->context = tascam;
			f_urb->complete = feedback_urb_complete;
		}
	} else {
		for (i = 0; i < NUM_CAPTURE_URBS; i++) {
			struct urb *urb = usb_alloc_urb(0, GFP_KERNEL);
			if (!urb) goto error;
			tascam->capture_urbs[i] = urb;
			urb->transfer_buffer = usb_alloc_coherent(tascam->dev, tascam->capture_urb_buffer_size, GFP_KERNEL, &urb->transfer_dma);
			if (!urb->transfer_buffer) goto error;
			urb->dev = tascam->dev;
			urb->pipe = usb_rcvbulkpipe(tascam->dev, EP_CAPTURE_DATA);
			urb->transfer_flags = URB_NO_TRANSFER_DMA_MAP;
			urb->context = tascam;
			urb->complete = capture_urb_complete;
		}
	}

	return 0;

	error:
	dev_err(tascam->card->dev, "Failed to allocate URBs\n");
	tascam_free_all_urbs(tascam);
	return -ENOMEM;
}

static int tascam_playback_open(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	substream->runtime->hw = tascam_playback_hw;
	tascam->playback_substream = substream;
	atomic_set(&tascam->playback_active, 0);
	return 0;
}

static int tascam_playback_close(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	tascam_free_playback_urbs(tascam);
	return 0;
}

static int tascam_capture_open(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	substream->runtime->hw = tascam_capture_hw;
	tascam->capture_substream = substream;
	atomic_set(&tascam->capture_active, 0);
	return 0;
}

static int tascam_capture_close(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	tascam_free_capture_urbs(tascam);
	return 0;
}

static int tascam_playback_hw_params(struct snd_pcm_substream *substream,
									 struct snd_pcm_hw_params *params)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	int err, i;
	unsigned int rate = params_rate(params);
	unsigned int period_frames = params_period_size(params);
	unsigned int low_asio_frames, normal_asio_frames;
	int tier_idx;
	unsigned int feedback_urb_packets;
	unsigned int playback_iso_packets;

	switch (rate) {
		case 44100: low_asio_frames = 64; normal_asio_frames = 128; break;
		case 48000: low_asio_frames = 64; normal_asio_frames = 128; break;
		case 88200: low_asio_frames = 128; normal_asio_frames = 256; break;
		case 96000: low_asio_frames = 128; normal_asio_frames = 256; break;
		default: return -EINVAL;
	}

	if (period_frames <= low_asio_frames) tier_idx = 0;
	else if (period_frames <= normal_asio_frames) tier_idx = 1;
	else tier_idx = 2;

	dev_info(tascam->card->dev, "Playback: User requested period of %u frames @ %u Hz, mapping to tier %d\n", period_frames, rate, tier_idx);

	feedback_urb_packets = hardware_feedback_packet_counts[tier_idx];
	playback_iso_packets = playback_iso_packet_counts_tiers[tier_idx];
	tascam->playback_urb_iso_packets = playback_iso_packets;

	err = tascam_alloc_urbs(tascam, true);
	if (err < 0)
		return err;

	for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
		struct urb *f_urb = tascam->feedback_urbs[i];
		int j;
		f_urb->number_of_packets = feedback_urb_packets;
		f_urb->transfer_buffer_length = feedback_urb_packets * FEEDBACK_PACKET_SIZE;
		for (j = 0; j < feedback_urb_packets; j++) {
			f_urb->iso_frame_desc[j].offset = j * FEEDBACK_PACKET_SIZE;
			f_urb->iso_frame_desc[j].length = FEEDBACK_PACKET_SIZE;
		}
	}

	for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
		tascam->playback_urbs[i]->number_of_packets = tascam->playback_urb_iso_packets;
	}

	err = snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(params));
	if (err < 0) return err;

	switch (rate) {
		case 44100: tascam->feedback_patterns = patterns_44khz; tascam->feedback_base_value = 42; tascam->feedback_max_value = 46; break;
		case 48000: tascam->feedback_patterns = patterns_48khz; tascam->feedback_base_value = 46; tascam->feedback_max_value = 50; break;
		case 88200: tascam->feedback_patterns = patterns_88khz; tascam->feedback_base_value = 86; tascam->feedback_max_value = 90; break;
		case 96000: tascam->feedback_patterns = patterns_96khz; tascam->feedback_base_value = 94; tascam->feedback_max_value = 98; break;
		default: return -EINVAL;
	}

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

static int tascam_capture_hw_params(struct snd_pcm_substream *substream,
									struct snd_pcm_hw_params *params)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	int err;

	tascam->capture_urb_buffer_size = params_period_bytes(params) * CAPTURE_DEVICE_BYTES_PER_FRAME / CAPTURE_ALSA_BYTES_PER_FRAME;
	if (tascam->capture_urb_buffer_size == 0)
		return -EINVAL;

	err = tascam_alloc_urbs(tascam, false);
	if (err < 0)
		return err;

	return snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(params));
}

static int tascam_playback_hw_free(struct snd_pcm_substream *substream)
{
	tascam_free_playback_urbs(snd_pcm_substream_chip(substream));
	return snd_pcm_lib_free_pages(substream);
}

static int tascam_capture_hw_free(struct snd_pcm_substream *substream)
{
	tascam_free_capture_urbs(snd_pcm_substream_chip(substream));
	return snd_pcm_lib_free_pages(substream);
}

static int tascam_playback_prepare(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int i, u;
	size_t nominal_frames_per_packet, nominal_bytes_per_packet, total_bytes_in_urb;

	tascam->driver_playback_pos = 0;
	tascam->playback_frames_consumed = 0;
	tascam->last_period_pos = 0;
	tascam->feedback_pattern_in_idx = 0;
	tascam->feedback_pattern_out_idx = 0;
	tascam->feedback_synced = false;
	tascam->feedback_urb_skip_count = NUM_FEEDBACK_URBS * 2;

	nominal_frames_per_packet = runtime->rate / 8000;
	for (i = 0; i < FEEDBACK_ACCUMULATOR_SIZE; i++)
		tascam->feedback_accumulator_pattern[i] = nominal_frames_per_packet;

	nominal_bytes_per_packet = nominal_frames_per_packet * PLAYBACK_DEVICE_BYTES_PER_FRAME;
	total_bytes_in_urb = nominal_bytes_per_packet * tascam->playback_urb_iso_packets;

	if (total_bytes_in_urb > tascam->playback_urb_alloc_size)
		return -EINVAL;

	for (u = 0; u < NUM_PLAYBACK_URBS; u++) {
		struct urb *urb = tascam->playback_urbs[u];
		urb->number_of_packets = tascam->playback_urb_iso_packets;
		memset(urb->transfer_buffer, 0, tascam->playback_urb_alloc_size);
		urb->transfer_buffer_length = total_bytes_in_urb;
		for (i = 0; i < tascam->playback_urb_iso_packets; i++) {
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
	return 0;
}

static int tascam_playback_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	int err = 0, i;
	bool start = false;

	switch (cmd) {
		case SNDRV_PCM_TRIGGER_START:
		case SNDRV_PCM_TRIGGER_RESUME:
			if (atomic_xchg(&tascam->playback_active, 1) == 0)
				start = true;
		break;
		case SNDRV_PCM_TRIGGER_STOP:
		case SNDRV_PCM_TRIGGER_SUSPEND:
		case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
			atomic_set(&tascam->playback_active, 0);
			break;
		default: return -EINVAL;
	}

	if (start) {
		for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
			err = usb_submit_urb(tascam->feedback_urbs[i], GFP_ATOMIC);
			if (err < 0) goto trigger_error;
		}
		for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
			err = usb_submit_urb(tascam->playback_urbs[i], GFP_ATOMIC);
			if (err < 0) goto trigger_error;
		}
	} else {
		for (i = 0; i < NUM_PLAYBACK_URBS; i++) usb_unlink_urb(tascam->playback_urbs[i]);
		for (i = 0; i < NUM_FEEDBACK_URBS; i++) usb_unlink_urb(tascam->feedback_urbs[i]);
	}
	return 0;

	trigger_error:
	atomic_set(&tascam->playback_active, 0);
	tascam_free_playback_urbs(tascam);
	return err;
}

static int tascam_capture_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	int err = 0, i;
	bool start = false;

	switch (cmd) {
		case SNDRV_PCM_TRIGGER_START:
		case SNDRV_PCM_TRIGGER_RESUME:
			if (atomic_xchg(&tascam->capture_active, 1) == 0)
				start = true;
		break;
		case SNDRV_PCM_TRIGGER_STOP:
		case SNDRV_PCM_TRIGGER_SUSPEND:
		case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
			atomic_set(&tascam->capture_active, 0);
			break;
		default: return -EINVAL;
	}

	if (start) {
		for (i = 0; i < NUM_CAPTURE_URBS; i++) {
			struct urb *urb = tascam->capture_urbs[i];
			urb->transfer_buffer_length = tascam->capture_urb_buffer_size;
			err = usb_submit_urb(urb, GFP_ATOMIC);
			if (err < 0) {
				dev_err(tascam->card->dev, "Failed to submit capture URB %d: %d\n", i, err);
				atomic_set(&tascam->capture_active, 0);
				for (i = 0; i < NUM_CAPTURE_URBS; i++)
					usb_unlink_urb(tascam->capture_urbs[i]);
				return err;
			}
		}
	} else {
		for (i = 0; i < NUM_CAPTURE_URBS; i++)
			usb_unlink_urb(tascam->capture_urbs[i]);
	}
	return 0;
}

static snd_pcm_uframes_t tascam_playback_pointer(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	u64 pos;

	if (!atomic_read(&tascam->playback_active))
		return 0;

	pos = tascam->playback_frames_consumed;

	return runtime ? pos % runtime->buffer_size : 0;
}

static snd_pcm_uframes_t tascam_capture_pointer(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);
	if (!atomic_read(&tascam->capture_active)) return 0;
	return tascam->driver_capture_pos;
}

static void playback_urb_complete(struct urb *urb)
{
	struct tascam_card *tascam = urb->context;
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	unsigned long flags;
	int ret, i, f;
	char *urb_buf_ptr;
	size_t urb_total_bytes = 0;

	if (urb->status == -ENOENT || urb->status == -ECONNRESET || urb->status == -ESHUTDOWN)
		return;

	if (!tascam || !atomic_read(&tascam->playback_active))
		return;

	substream = tascam->playback_substream;
	if (!substream || !substream->runtime)
		return;
	runtime = substream->runtime;
	urb_buf_ptr = urb->transfer_buffer;

	spin_lock_irqsave(&tascam->lock, flags);

	for (i = 0; i < tascam->playback_urb_iso_packets; i++) {
		unsigned int frames_for_packet;
		size_t bytes_for_packet;

		if (tascam->feedback_synced) {
			frames_for_packet = tascam->feedback_accumulator_pattern[tascam->feedback_pattern_out_idx];
			tascam->feedback_pattern_out_idx = (tascam->feedback_pattern_out_idx + 1) % FEEDBACK_ACCUMULATOR_SIZE;
		} else {
			frames_for_packet = runtime->rate / 8000;
		}

		bytes_for_packet = frames_for_packet * PLAYBACK_DEVICE_BYTES_PER_FRAME;

		if ((urb_total_bytes + bytes_for_packet) > tascam->playback_urb_alloc_size) {
			dev_warn_ratelimited(tascam->card->dev, "Playback URB overflow, truncating packet.\n");
			urb->iso_frame_desc[i].length = 0;
			urb->iso_frame_desc[i].offset = urb_total_bytes;
			continue;
		}

		for (f = 0; f < frames_for_packet; f++) {
			size_t alsa_pos_bytes = frames_to_bytes(runtime, tascam->driver_playback_pos);
			char *alsa_frame_ptr = runtime->dma_area + alsa_pos_bytes;

			memcpy(urb_buf_ptr, alsa_frame_ptr, PLAYBACK_ALSA_BYTES_PER_FRAME);
			memset(urb_buf_ptr + PLAYBACK_ALSA_BYTES_PER_FRAME, 0, PLAYBACK_DEVICE_BYTES_PER_FRAME - PLAYBACK_ALSA_BYTES_PER_FRAME);

			urb_buf_ptr += PLAYBACK_DEVICE_BYTES_PER_FRAME;
			tascam->driver_playback_pos++;
			if (tascam->driver_playback_pos >= runtime->buffer_size)
				tascam->driver_playback_pos = 0;
		}

		urb->iso_frame_desc[i].offset = urb_total_bytes;
		urb->iso_frame_desc[i].length = bytes_for_packet;
		urb_total_bytes += bytes_for_packet;
	}

	spin_unlock_irqrestore(&tascam->lock, flags);

	urb->transfer_buffer_length = urb_total_bytes;

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
	u64 current_period;
	u64 total_frames_in_urb = 0;
	bool was_synced;
	bool sync_lost_this_urb = false;

	if (urb->status == -ENOENT || urb->status == -ECONNRESET || urb->status == -ESHUTDOWN)
		return;

	if (!tascam || !atomic_read(&tascam->playback_active))
		return;

	substream = tascam->playback_substream;
	if (!substream || !substream->runtime)
		return;
	runtime = substream->runtime;

	if (urb->status != 0) {
		dev_warn_ratelimited(tascam->card->dev, "Feedback URB failed with status %d\n", urb->status);
		spin_lock_irqsave(&tascam->lock, flags);
		if (tascam->feedback_synced)
			dev_info(tascam->card->dev, "Sync Lost (URB error)!\n");
		tascam->feedback_synced = false;
		spin_unlock_irqrestore(&tascam->lock, flags);
		goto resubmit;
	}

	spin_lock_irqsave(&tascam->lock, flags);

	was_synced = tascam->feedback_synced;

	if (tascam->feedback_urb_skip_count > 0) {
		tascam->feedback_urb_skip_count--;
		goto unlock_and_resubmit;
	}

	for (p = 0; p < urb->number_of_packets; p++) {
		u8 feedback_value;
		const unsigned int *pattern;
		int pattern_index;

		if (urb->iso_frame_desc[p].status != 0 || urb->iso_frame_desc[p].actual_length < 1) {
			sync_lost_this_urb = true;
			continue;
		}

		feedback_value = *((u8 *)urb->transfer_buffer + urb->iso_frame_desc[p].offset);

		if (feedback_value >= tascam->feedback_base_value &&
			feedback_value <= tascam->feedback_max_value) {
			pattern_index = feedback_value - tascam->feedback_base_value;
		pattern = tascam->feedback_patterns[pattern_index];
			} else {
				sync_lost_this_urb = true;
				pattern = NULL;
			}

			if (pattern) {
				for (i = 0; i < 8; i++) {
					unsigned int in_idx = (tascam->feedback_pattern_in_idx + i) % FEEDBACK_ACCUMULATOR_SIZE;
					tascam->feedback_accumulator_pattern[in_idx] = pattern[i];
					total_frames_in_urb += pattern[i];
				}
				tascam->feedback_pattern_in_idx = (tascam->feedback_pattern_in_idx + 8) % FEEDBACK_ACCUMULATOR_SIZE;
			} else {
				u64 nominal_frames_per_ms = runtime->rate / 1000;
				total_frames_in_urb += nominal_frames_per_ms;
			}
	}

	if (sync_lost_this_urb) {
		if (was_synced)
			dev_info(tascam->card->dev, "Sync Lost (bad packet)!\n");
		tascam->feedback_synced = false;
	} else {
		if (!was_synced)
			dev_info(tascam->card->dev, "Sync Acquired!\n");
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
	if (atomic_read(&tascam->playback_active)) {
		urb->dev = tascam->dev;
		ret = usb_submit_urb(urb, GFP_ATOMIC);
		if (ret < 0)
			dev_err_ratelimited(tascam->card->dev, "Failed to resubmit feedback URB: %d\n", ret);
	}
}

static void capture_urb_complete(struct urb *urb)
{
	struct tascam_card *tascam = urb->context;
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	unsigned long flags;
	int ret;
	unsigned int frames_captured;
	size_t bytes_captured;

	if (urb->status == -ENOENT || urb->status == -ECONNRESET || urb->status == -ESHUTDOWN)
		return;

	if (!tascam || !atomic_read(&tascam->capture_active))
		return;

	substream = tascam->capture_substream;
	if (!substream || !substream->runtime)
		return;
	runtime = substream->runtime;

	if (urb->status != 0) {
		dev_warn_ratelimited(tascam->card->dev, "Capture URB failed with status %d\n", urb->status);
		goto resubmit;
	}

	bytes_captured = urb->actual_length;
	if (bytes_captured == 0)
		goto resubmit;

	if (bytes_captured % CAPTURE_DEVICE_BYTES_PER_FRAME != 0) {
		dev_warn_ratelimited(tascam->card->dev, "Capture URB has partial frame, size %zu\n", bytes_captured);
		goto resubmit;
	}

	frames_captured = bytes_captured / CAPTURE_DEVICE_BYTES_PER_FRAME;

	spin_lock_irqsave(&tascam->lock, flags);

	deinterleave_capture_data(
		(s32 *)(runtime->dma_area + frames_to_bytes(runtime, tascam->driver_capture_pos)),
							  urb->transfer_buffer,
						   frames_captured
	);

	tascam->driver_capture_pos += frames_captured;
	if (tascam->driver_capture_pos >= runtime->buffer_size)
		tascam->driver_capture_pos -= runtime->buffer_size;

	spin_unlock_irqrestore(&tascam->lock, flags);

	snd_pcm_period_elapsed(substream);

	resubmit:
	if (atomic_read(&tascam->capture_active)) {
		urb->dev = tascam->dev;
		ret = usb_submit_urb(urb, GFP_ATOMIC);
		if (ret < 0)
			dev_err_ratelimited(tascam->card->dev, "Failed to resubmit capture URB: %d\n", ret);
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
	if (!rate_payload_buf)
		return -ENOMEM;

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
	dev_info(&dev->dev, "Configuring device for %d Hz\n", rate);

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_MODE_CONTROL, RT_H2D_VENDOR_DEV, 0x0010, 0x0000, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) goto cleanup_buf;

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), UAC_SET_CUR, RT_H2D_CLASS_EP, UAC_SAMPLING_FREQ_CONTROL, EP_CAPTURE_DATA, rate_payload_buf, 3, USB_CTRL_TIMEOUT_MS);
	if (err < 0) goto cleanup_buf;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), UAC_SET_CUR, RT_H2D_CLASS_EP, UAC_SAMPLING_FREQ_CONTROL, EP_AUDIO_OUT, rate_payload_buf, 3, USB_CTRL_TIMEOUT_MS);
	if (err < 0) goto cleanup_buf;

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, 0x0d04, 0x0101, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) goto cleanup_buf;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, 0x0e00, 0x0101, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) goto cleanup_buf;
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, 0x0f00, 0x0101, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) goto cleanup_buf;

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, rate_vendor_wValue, 0x0101, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) goto cleanup_buf;

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, 0x110b, 0x0101, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) goto cleanup_buf;

	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), VENDOR_REQ_MODE_CONTROL, RT_H2D_VENDOR_DEV, 0x0030, 0x0000, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) goto cleanup_buf;

	cleanup_buf:
	if (err < 0)
		dev_err(&dev->dev, "Device configuration failed at rate %d with error %d\n", rate, err);
	kfree(rate_payload_buf);
	return err;
}

static int tascam_create_pcm(struct tascam_card *tascam)
{
	struct snd_pcm *pcm;
	int err;

	err = snd_pcm_new(tascam->card, "US144MKII PCM", 0, 1, 1, &pcm);
	if (err < 0) {
		dev_err(tascam->card->dev, "Failed to create snd_pcm: %d\n", err);
		return err;
	}
	tascam->pcm = pcm;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &tascam_playback_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &tascam_capture_ops);

	pcm->private_data = tascam;
	strscpy(pcm->name, "US-144MKII Audio", sizeof(pcm->name));

	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_CONTINUOUS,
										  tascam->dev->dev.parent,
										  64 * 1024,
										  tascam_playback_hw.buffer_bytes_max);
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

static int tascam_probe(struct usb_interface *intf, const struct usb_device_id *usb_id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct tascam_card *tascam;
	struct snd_card *card;
	int err, dev_idx;
	u8 *handshake_buf;

	dev_idx = intf->cur_altsetting->desc.bInterfaceNumber;
	if (dev_idx != 0)
		return -ENODEV;

	err = snd_card_new(&intf->dev, index[dev_idx], id[dev_idx], THIS_MODULE,
					   sizeof(struct tascam_card), &card);
	if (err < 0) return err;

	tascam = card->private_data;
	tascam->card = card;
	tascam->dev = usb_get_dev(dev);
	tascam->iface0 = intf;
	card->private_free = tascam_card_private_free;
	usb_set_intfdata(intf, tascam);
	spin_lock_init(&tascam->lock);
	atomic_set(&tascam->playback_active, 0);
	atomic_set(&tascam->capture_active, 0);
	tascam->current_rate = 0;
	tascam->playback_urb_iso_packets = 0;
	tascam->was_playback_active = false;
	tascam->was_capture_active = false;

	strscpy(card->driver, DRIVER_NAME, sizeof(card->driver));
	strscpy(card->shortname, "TASCAM US-144MKII", sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname), "%s (VID:%04X PID:%04X) at %s",
			 "TASCAM US-144MKII",
		  le16_to_cpu(dev->descriptor.idVendor),
			 le16_to_cpu(dev->descriptor.idProduct),
			 dev->bus->bus_name);

	tascam->iface1 = usb_ifnum_to_if(dev, 1);
	if (!tascam->iface1) {
		err = -ENODEV;
		goto free_card_obj;
	}
	err = usb_driver_claim_interface(&tascam_alsa_driver, tascam->iface1, tascam);
	if (err < 0) {
		tascam->iface1 = NULL;
		goto free_card_obj;
	}

	err = usb_set_interface(dev, 0, 1);
	if (err < 0) goto release_iface1_and_free_card;
	err = usb_set_interface(dev, 1, 1);
	if (err < 0) goto release_iface1_and_free_card;

	handshake_buf = kmalloc(1, GFP_KERNEL);
	if (!handshake_buf) {
		err = -ENOMEM;
		goto release_iface1_and_free_card;
	}
	err = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0), VENDOR_REQ_MODE_CONTROL,
						  RT_D2H_VENDOR_DEV, 0x0000, 0x0000,
					   handshake_buf, 1, USB_CTRL_TIMEOUT_MS);
	if (err < 0) dev_warn(&intf->dev, "Handshake read failed: %d (continuing)\n", err);
	else if (err == 1 && handshake_buf[0] == 0x12) dev_info(&intf->dev, "Handshake successful (response 0x12).\n");
	else dev_warn(&intf->dev, "Handshake: expected 0x12, got 0x%02x (len %d) (continuing)\n", handshake_buf[0], err);
	kfree(handshake_buf);

	err = tascam_create_pcm(tascam);
	if (err < 0) goto release_iface1_and_free_card;

	err = snd_card_register(card);
	if (err < 0) goto release_iface1_and_free_card;

	dev_info(&intf->dev, "%s: TASCAM US-144MKII ALSA driver initialized.\n", card->longname);
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

	if (!tascam || intf != tascam->iface0)
		return;

	dev_info(&intf->dev, "TASCAM US-144MKII disconnecting...\n");
	snd_card_disconnect(tascam->card);

	atomic_set(&tascam->capture_active, 0);
	atomic_set(&tascam->playback_active, 0);
	tascam_free_all_urbs(tascam);

	if (tascam->iface1) {
		usb_set_intfdata(tascam->iface1, NULL);
		usb_driver_release_interface(&tascam_alsa_driver, tascam->iface1);
		tascam->iface1 = NULL;
	}
	snd_card_free_when_closed(tascam->card);
}

static int tascam_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct tascam_card *tascam = usb_get_intfdata(intf);

	if (!tascam || intf != tascam->iface0)
		return 0;

	snd_power_change_state(tascam->card, SNDRV_CTL_POWER_D3hot);

	tascam->was_playback_active = atomic_read(&tascam->playback_active);
	tascam->was_capture_active = atomic_read(&tascam->capture_active);

	atomic_set(&tascam->playback_active, 0);
	atomic_set(&tascam->capture_active, 0);
	tascam_free_all_urbs(tascam);

	return 0;
}

static int tascam_resume(struct usb_interface *intf)
{
	struct tascam_card *tascam = usb_get_intfdata(intf);
	int err = 0;

	if (!tascam || intf != tascam->iface0)
		return 0;

	err = usb_set_interface(tascam->dev, 0, 1);
	if (err < 0) goto resume_error;
	err = usb_set_interface(tascam->dev, 1, 1);
	if (err < 0) goto resume_error;

	if (tascam->current_rate > 0) {
		err = us144mkii_configure_device_for_rate(tascam, tascam->current_rate);
		if (err < 0)
			goto resume_error;
	}

	if (tascam->was_playback_active && tascam->playback_substream) {
		tascam_playback_trigger(tascam->playback_substream, SNDRV_PCM_TRIGGER_START);
	}
	if (tascam->was_capture_active && tascam->capture_substream) {
		tascam_capture_trigger(tascam->capture_substream, SNDRV_PCM_TRIGGER_START);
	}

	snd_power_change_state(tascam->card, SNDRV_CTL_POWER_D0);
	return 0;

	resume_error:
	dev_err(&intf->dev, "Resume failed with error %d\n", err);
	return err;
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
	.suspend =	tascam_suspend,
	.resume =	tascam_resume,
	.reset_resume = tascam_resume,
	.id_table =	tascam_id_table,
};

module_usb_driver(tascam_alsa_driver);
