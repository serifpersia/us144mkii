/* SPDX-License-Identifier: GPL-2.0-only */
// Copyright (c) 2025 Šerif Rami <ramiserifpersia@gmail.com>

#ifndef __US144MKII_H
#define __US144MKII_H

#include <linux/timer.h>
#include <linux/usb.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/rawmidi.h>

#define DRIVER_NAME "us144mkii"

#define USB_VID_TASCAM 0x0644
#define USB_PID_TASCAM_US144 0x800f
#define USB_PID_TASCAM_US144MKII 0x8020

#define EP_PLAYBACK_FEEDBACK 0x81
#define EP_AUDIO_OUT 0x02
#define EP_MIDI_IN 0x83
#define EP_MIDI_OUT 0x04
#define EP_AUDIO_IN 0x86

#define RT_H2D_CLASS_EP (USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_ENDPOINT)
#define RT_D2H_CLASS_EP (USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_ENDPOINT)
#define RT_H2D_VENDOR_DEV (USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE)
#define RT_D2H_VENDOR_DEV (USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE)

enum uac_request {
	UAC_SET_CUR = 0x01,
	UAC_GET_CUR = 0x81,
};

enum uac_control_selector {
	UAC_SAMPLING_FREQ_CONTROL = 0x0100,
};

enum tascam_vendor_request {
	VENDOR_REQ_REGISTER_WRITE = 0x41,
	VENDOR_REQ_MODE_CONTROL = 0x49,
};

enum tascam_mode_value {
	MODE_VAL_HANDSHAKE_READ = 0x0000,
	MODE_VAL_CONFIG = 0x0010,
	MODE_VAL_STREAM_START = 0x0030,
};

enum tascam_register {
	REG_ADDR_INIT_0D = 0x0d04,
	REG_ADDR_INIT_0E = 0x0e00,
	REG_ADDR_INIT_0F = 0x0f00,
	REG_ADDR_RATE_44100 = 0x1000,
	REG_ADDR_RATE_48000 = 0x1002,
	REG_ADDR_RATE_88200 = 0x1008,
	REG_ADDR_RATE_96000 = 0x100a,
	REG_ADDR_INIT_11 = 0x110b,
};

#define REG_VAL_ENABLE 0x0101

#define NUM_PLAYBACK_URBS 2
#define PLAYBACK_URB_PACKETS 8
#define NUM_FEEDBACK_URBS 4
#define FEEDBACK_URB_PACKETS 1
#define FEEDBACK_PACKET_SIZE 3
#define NUM_CAPTURE_URBS 32
#define CAPTURE_PACKET_SIZE 512

#define MIDI_PACKET_SIZE 9
#define MIDI_PAYLOAD_SIZE 8

#define BYTES_PER_SAMPLE 3
#define NUM_CHANNELS 4

#define PLAYBACK_FRAME_SIZE (NUM_CHANNELS * BYTES_PER_SAMPLE)

#define PLL_FILTER_OLD_WEIGHT 3
#define PLL_FILTER_NEW_WEIGHT 1
#define PLL_FILTER_DIVISOR (PLL_FILTER_OLD_WEIGHT + PLL_FILTER_NEW_WEIGHT)

#define USB_CTRL_TIMEOUT_MS 1000

/**
 * struct tascam_card - private data for the TASCAM US-144MKII driver
 * @dev: pointer to the USB device
 * @iface0: pointer to the first interface
 * @card: pointer to the ALSA sound card
 * @pcm: pointer to the ALSA PCM device
 * @rmidi: pointer to the ALSA raw MIDI device
 * @scratch_buf: temporary buffer for control messages
 * @playback_substream: pointer to the PCM playback substream
 * @capture_substream: pointer to the PCM capture substream
 * @playback_urbs: array of URBs for PCM playback
 * @playback_urb_alloc_size: allocated size of each playback URB
 * @feedback_urbs: array of URBs for feedback
 * @feedback_urb_alloc_size: allocated size of each feedback URB
 * @capture_urbs: array of URBs for PCM capture
 * @playback_anchor: anchor for playback URBs
 * @feedback_anchor: anchor for feedback URBs
 * @capture_anchor: anchor for capture URBs
 * @midi_input: pointer to the MIDI input substream
 * @midi_output: pointer to the MIDI output substream
 * @midi_in_urb: URB for MIDI input
 * @midi_out_urb: URB for MIDI output
 * @midi_in_buf: buffer for MIDI input
 * @midi_out_buf: buffer for MIDI output
 * @midi_anchor: anchor for MIDI URBs
 * @midi_out_active: flag indicating if MIDI output is active
 * @midi_lock: spinlock for MIDI operations
 * @lock: spinlock for PCM operations
 * @playback_active: atomic flag indicating if PCM playback is active
 * @capture_active: atomic flag indicating if PCM capture is active
 * @active_urbs: atomic counter for active URBs
 * @current_rate: current sample rate
 * @playback_frames_consumed: number of frames consumed by the playback device
 * @driver_playback_pos: playback position in the ring buffer
 * @last_pb_period_pos: last playback period position
 * @capture_frames_processed: number of frames processed from the capture device
 * @driver_capture_pos: capture position in the ring buffer
 * @last_cap_period_pos: last capture period position
 * @phase_accum: phase accumulator for the playback PLL
 * @freq_q16: current frequency for the playback PLL in Q16.16 format
 * @feedback_synced: flag indicating if feedback is synced
 * @feedback_urb_skip_count: number of feedback URBs to skip at startup
 * @stop_work: work struct for stopping all streams
 * @stop_pcm_work: work struct for stopping PCM streams
 */
struct tascam_card {
	struct usb_device *dev;
	struct usb_interface *iface0;
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct snd_rawmidi *rmidi;

	u16 dev_id;

	u8 *scratch_buf;

	struct snd_pcm_substream *playback_substream;
	struct snd_pcm_substream *capture_substream;

	struct urb *playback_urbs[NUM_PLAYBACK_URBS];
	size_t playback_urb_alloc_size;
	struct urb *feedback_urbs[NUM_FEEDBACK_URBS];
	size_t feedback_urb_alloc_size;
	struct urb *capture_urbs[NUM_CAPTURE_URBS];

	struct usb_anchor playback_anchor;
	struct usb_anchor feedback_anchor;
	struct usb_anchor capture_anchor;

	struct snd_rawmidi_substream *midi_input;
	struct snd_rawmidi_substream *midi_output;
	struct urb *midi_in_urb;
	struct urb *midi_out_urb;
	u8 *midi_in_buf;
	u8 *midi_out_buf;
	struct usb_anchor midi_anchor;
	bool midi_out_active;
	spinlock_t midi_lock;

	spinlock_t lock;
	atomic_t playback_active;
	atomic_t capture_active;
	atomic_t active_urbs;
	int current_rate;

	u64 playback_frames_consumed;
	snd_pcm_uframes_t driver_playback_pos;
	u64 last_pb_period_pos;

	u64 capture_frames_processed;
	snd_pcm_uframes_t driver_capture_pos;
	u64 last_cap_period_pos;

	u32 phase_accum;
	u32 freq_q16;
	bool feedback_synced;
	unsigned int feedback_urb_skip_count;

	struct work_struct stop_work;
	struct work_struct stop_pcm_work;
};

void tascam_free_urbs(struct tascam_card *tascam);
int tascam_alloc_urbs(struct tascam_card *tascam);
void tascam_stop_work_handler(struct work_struct *work);

#include "us144mkii_pcm.h"

int tascam_create_midi(struct tascam_card *tascam);

#endif /* __US144MKII_H */
