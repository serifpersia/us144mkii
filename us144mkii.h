/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __US144MKII_H
#define __US144MKII_H

#include <linux/usb.h>
#include <linux/workqueue.h>
#include <linux/kfifo.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/rawmidi.h>
#include <sound/initval.h>
#include <sound/control.h>

#define DRIVER_NAME "snd-usb-us144mkii"
#define DRIVER_VERSION "1.7.4"

/* --- USB Device Identification --- */
#define USB_VID_TASCAM				0x0644
#define USB_PID_TASCAM_US144MKII	0x8020

/* --- USB Endpoints (Alternate Setting 1) --- */
#define EP_PLAYBACK_FEEDBACK		0x81
#define EP_AUDIO_OUT			0x02
#define EP_MIDI_IN			0x83
#define EP_MIDI_OUT			0x04
#define EP_AUDIO_IN			0x86

/* --- USB Control Message Protocol --- */
#define RT_H2D_CLASS_EP		(USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_ENDPOINT)
#define RT_D2H_CLASS_EP		(USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_ENDPOINT)
#define RT_H2D_VENDOR_DEV	(USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE)
#define RT_D2H_VENDOR_DEV	(USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE)

enum uac_request {
	UAC_SET_CUR			= 0x01,
	UAC_GET_CUR			= 0x81,
};

enum uac_control_selector {
	UAC_SAMPLING_FREQ_CONTROL	= 0x0100,
};

enum tascam_vendor_request {
	VENDOR_REQ_REGISTER_WRITE	= 0x41,
	VENDOR_REQ_MODE_CONTROL		= 0x49,
};

enum tascam_mode_value {
	MODE_VAL_HANDSHAKE_READ		= 0x0000,
	MODE_VAL_CONFIG			= 0x0010,
	MODE_VAL_STREAM_START		= 0x0030,
};

#define HANDSHAKE_SUCCESS_VAL		0x12

enum tascam_register {
	REG_ADDR_UNKNOWN_0D		= 0x0d04,
	REG_ADDR_UNKNOWN_0E		= 0x0e00,
	REG_ADDR_UNKNOWN_0F		= 0x0f00,
	REG_ADDR_RATE_44100		= 0x1000,
	REG_ADDR_RATE_48000		= 0x1002,
	REG_ADDR_RATE_88200		= 0x1008,
	REG_ADDR_RATE_96000		= 0x100a,
	REG_ADDR_UNKNOWN_11		= 0x110b,
};

#define REG_VAL_ENABLE			0x0101



/* --- URB Configuration --- */
#define NUM_PLAYBACK_URBS		8
#define PLAYBACK_URB_PACKETS		4
#define NUM_FEEDBACK_URBS		4
#define MAX_FEEDBACK_PACKETS		5
#define FEEDBACK_PACKET_SIZE		3
#define NUM_CAPTURE_URBS		8
#define CAPTURE_URB_SIZE		512
#define CAPTURE_RING_BUFFER_SIZE	(CAPTURE_URB_SIZE * NUM_CAPTURE_URBS * 4)
#define NUM_MIDI_IN_URBS		4
#define MIDI_IN_BUF_SIZE		64
#define MIDI_IN_FIFO_SIZE		(MIDI_IN_BUF_SIZE * NUM_MIDI_IN_URBS)
#define MIDI_OUT_BUF_SIZE		64
#define NUM_MIDI_OUT_URBS		4
#define USB_CTRL_TIMEOUT_MS		1000
#define FEEDBACK_SYNC_LOSS_THRESHOLD	41

/* --- Audio Format Configuration --- */
#define BYTES_PER_SAMPLE		3
#define NUM_CHANNELS			4
#define BYTES_PER_FRAME			(NUM_CHANNELS * BYTES_PER_SAMPLE)
#define FEEDBACK_ACCUMULATOR_SIZE	128

/* --- Capture Decoding Defines --- */
#define DECODED_CHANNELS_PER_FRAME	4
#define DECODED_SAMPLE_SIZE		4
#define FRAMES_PER_DECODE_BLOCK		8
#define RAW_BYTES_PER_DECODE_BLOCK	512

/* --- Main Driver Data Structure --- */
struct tascam_card {
	struct usb_device *dev;
	struct usb_interface *iface0;
	struct usb_interface *iface1;
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct snd_rawmidi *rmidi;

	/* Playback stream */
	struct snd_pcm_substream *playback_substream;
	struct urb *playback_urbs[NUM_PLAYBACK_URBS];
	size_t playback_urb_alloc_size;
	struct urb *feedback_urbs[NUM_FEEDBACK_URBS];
	size_t feedback_urb_alloc_size;
	atomic_t playback_active;
	u64 playback_frames_consumed;
	snd_pcm_uframes_t driver_playback_pos;
	u64 last_period_pos;
	u8 *playback_routing_buffer;

	/* Capture stream */
	struct snd_pcm_substream *capture_substream;
	struct urb *capture_urbs[NUM_CAPTURE_URBS];
	size_t capture_urb_alloc_size;
	atomic_t capture_active;
	snd_pcm_uframes_t driver_capture_pos;
	u64 capture_frames_processed;
	u64 last_capture_period_pos;
	u8 *capture_ring_buffer;
	size_t capture_ring_buffer_read_ptr;
	size_t capture_ring_buffer_write_ptr;
	u8 *capture_decode_raw_block;
	s32 *capture_decode_dst_block;
	s32 *capture_routing_buffer;
	struct work_struct capture_work;
	struct work_struct stop_work;

	/* MIDI streams */
	struct snd_rawmidi_substream *midi_in_substream;
	struct snd_rawmidi_substream *midi_out_substream;
	struct urb *midi_in_urbs[NUM_MIDI_IN_URBS];
	atomic_t midi_in_active;
	struct kfifo midi_in_fifo;
	struct work_struct midi_in_work;
	spinlock_t midi_in_lock;
	struct urb *midi_out_urbs[NUM_MIDI_OUT_URBS];
	atomic_t midi_out_active;
	struct work_struct midi_out_work;
	unsigned long midi_out_urbs_in_flight;
	spinlock_t midi_out_lock;
	u8 midi_running_status;

	/* Shared state & Routing Matrix */
	spinlock_t lock;
	atomic_t active_urbs;
	int current_rate;
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

	const unsigned int (*feedback_patterns)[8];
	unsigned int feedback_base_value;
	unsigned int feedback_max_value;

	struct usb_anchor playback_anchor;
	struct usb_anchor capture_anchor;
	struct usb_anchor feedback_anchor;
	struct usb_anchor midi_in_anchor;
	struct usb_anchor midi_out_anchor;
};

/* from pcm.c */
extern const struct snd_pcm_ops tascam_playback_ops;
extern const struct snd_pcm_ops tascam_capture_ops;
int tascam_create_pcm(struct tascam_card *tascam);
void tascam_free_urbs(struct tascam_card *tascam);
int tascam_alloc_urbs(struct tascam_card *tascam);
int us144mkii_configure_device_for_rate(struct tascam_card *tascam, int rate);
void tascam_stop_work_handler(struct work_struct *work);

/* from playback.c */
void process_playback_routing_us144mkii(struct tascam_card *tascam, const u8 *src_buffer, u8 *dst_buffer, size_t frames);
void playback_urb_complete(struct urb *urb);
void feedback_urb_complete(struct urb *urb);

/* from capture.c */
void process_capture_routing_us144mkii(struct tascam_card *tascam, const s32 *decoded_block, s32 *routed_block);
void decode_tascam_capture_block(const u8 *src_block, s32 *dst_block);
void tascam_capture_work_handler(struct work_struct *work);
void capture_urb_complete(struct urb *urb);

/* from controls.c */
int tascam_create_controls(struct tascam_card *tascam);

/* from midi.c */
int tascam_create_midi(struct tascam_card *tascam);
void tascam_midi_in_work_handler(struct work_struct *work);
void tascam_midi_out_work_handler(struct work_struct *work);
void tascam_midi_in_urb_complete(struct urb *urb);
void tascam_midi_out_urb_complete(struct urb *urb);

#endif /* __US144MKII_H */
