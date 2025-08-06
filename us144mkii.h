/* SPDX-License-Identifier: GPL-2.0-only */
// Copyright (c) 2025 Šerif Rami <ramiserifpersia@gmail.com>

#ifndef __US144MKII_H
#define __US144MKII_H

#include <linux/kfifo.h>
#include <linux/usb.h>
#include <linux/workqueue.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/rawmidi.h>

#define DRIVER_NAME "us144mkii"
#define DRIVER_VERSION "1.7.4"

/* --- USB Device Identification --- */
#define USB_VID_TASCAM 0x0644
<<<<<<< HEAD
#define USB_PID_TASCAM_US144 0x800f
=======
>>>>>>> f44b75094c078b0354fac280d769bc9a1bb6133b
#define USB_PID_TASCAM_US144MKII 0x8020

/* --- USB Endpoints (Alternate Setting 1) --- */
#define EP_PLAYBACK_FEEDBACK 0x81
#define EP_AUDIO_OUT 0x02
#define EP_MIDI_IN 0x83
#define EP_MIDI_OUT 0x04
#define EP_AUDIO_IN 0x86

/* --- USB Control Message Protocol --- */
#define RT_H2D_CLASS_EP (USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_ENDPOINT)
#define RT_D2H_CLASS_EP (USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_ENDPOINT)
#define RT_H2D_VENDOR_DEV (USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE)
#define RT_D2H_VENDOR_DEV (USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE)

enum uac_request {
<<<<<<< HEAD
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
=======
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
>>>>>>> f44b75094c078b0354fac280d769bc9a1bb6133b
};

#define HANDSHAKE_SUCCESS_VAL 0x12

enum tascam_register {
<<<<<<< HEAD
  REG_ADDR_UNKNOWN_0D = 0x0d04,
  REG_ADDR_UNKNOWN_0E = 0x0e00,
  REG_ADDR_UNKNOWN_0F = 0x0f00,
  REG_ADDR_RATE_44100 = 0x1000,
  REG_ADDR_RATE_48000 = 0x1002,
  REG_ADDR_RATE_88200 = 0x1008,
  REG_ADDR_RATE_96000 = 0x100a,
  REG_ADDR_UNKNOWN_11 = 0x110b,
=======
	REG_ADDR_UNKNOWN_0D = 0x0d04,
	REG_ADDR_UNKNOWN_0E = 0x0e00,
	REG_ADDR_UNKNOWN_0F = 0x0f00,
	REG_ADDR_RATE_44100 = 0x1000,
	REG_ADDR_RATE_48000 = 0x1002,
	REG_ADDR_RATE_88200 = 0x1008,
	REG_ADDR_RATE_96000 = 0x100a,
	REG_ADDR_UNKNOWN_11 = 0x110b,
>>>>>>> f44b75094c078b0354fac280d769bc9a1bb6133b
};

#define REG_VAL_ENABLE 0x0101

/* --- URB Configuration --- */
#define NUM_PLAYBACK_URBS 8
#define PLAYBACK_URB_PACKETS 4
#define NUM_FEEDBACK_URBS 4
#define MAX_FEEDBACK_PACKETS 5
#define FEEDBACK_PACKET_SIZE 3
#define NUM_CAPTURE_URBS 8
#define CAPTURE_URB_SIZE 512
#define CAPTURE_RING_BUFFER_SIZE (CAPTURE_URB_SIZE * NUM_CAPTURE_URBS * 4)
#define NUM_MIDI_IN_URBS 4
#define MIDI_IN_BUF_SIZE 64
#define MIDI_IN_FIFO_SIZE (MIDI_IN_BUF_SIZE * NUM_MIDI_IN_URBS)
#define MIDI_OUT_BUF_SIZE 64
#define NUM_MIDI_OUT_URBS 4
#define USB_CTRL_TIMEOUT_MS 1000
#define FEEDBACK_SYNC_LOSS_THRESHOLD 41

/* --- Audio Format Configuration --- */
#define BYTES_PER_SAMPLE 3
#define NUM_CHANNELS 4
#define BYTES_PER_FRAME (NUM_CHANNELS * BYTES_PER_SAMPLE)
#define FEEDBACK_ACCUMULATOR_SIZE 128

/* --- Capture Decoding Defines --- */
#define DECODED_CHANNELS_PER_FRAME 4
#define DECODED_SAMPLE_SIZE 4
#define FRAMES_PER_DECODE_BLOCK 8
#define RAW_BYTES_PER_DECODE_BLOCK 512

/**
 * struct tascam_card - Main driver data structure for the TASCAM US-144MKII.
 * @dev: Pointer to the USB device.
 * @iface0: Pointer to USB interface 0 (audio).
 * @iface1: Pointer to USB interface 1 (MIDI).
 * @card: Pointer to the ALSA sound card instance.
 * @pcm: Pointer to the ALSA PCM device.
 * @rmidi: Pointer to the ALSA rawmidi device.
 *
 * @playback_substream: Pointer to the active playback PCM substream.
 * @playback_urbs: Array of URBs for playback.
 * @playback_urb_alloc_size: Size of allocated buffer for each playback URB.
 * @feedback_urbs: Array of URBs for feedback.
 * @feedback_urb_alloc_size: Size of allocated buffer for each feedback URB.
 * @playback_active: Atomic flag indicating if playback is active.
 * @playback_frames_consumed: Total frames consumed by playback.
 * @driver_playback_pos: Current position in the ALSA playback buffer (frames).
 * @last_period_pos: Last reported period position for playback.
 * @playback_routing_buffer: Intermediate buffer for playback routing.
 *
 * @capture_substream: Pointer to the active capture PCM substream.
 * @capture_urbs: Array of URBs for capture.
 * @capture_urb_alloc_size: Size of allocated buffer for each capture URB.
 * @capture_active: Atomic flag indicating if capture is active.
 * @driver_capture_pos: Current position in the ALSA capture buffer (frames).
 * @capture_frames_processed: Total frames processed for capture.
 * @last_capture_period_pos: Last reported period position for capture.
 * @capture_ring_buffer: Ring buffer for raw capture data from USB.
 * @capture_ring_buffer_read_ptr: Read pointer for the capture ring buffer.
 * @capture_ring_buffer_write_ptr: Write pointer for the capture ring buffer.
 * @capture_decode_raw_block: Buffer for a raw 512-byte capture block.
 * @capture_decode_dst_block: Buffer for decoded 32-bit capture samples.
 * @capture_routing_buffer: Intermediate buffer for capture routing.
 * @capture_work: Work struct for deferred capture processing.
 * @stop_work: Work struct for deferred stream stopping.
 *
 * @midi_in_substream: Pointer to the active MIDI input substream.
 * @midi_out_substream: Pointer to the active MIDI output substream.
 * @midi_in_urbs: Array of URBs for MIDI input.
 * @midi_in_active: Atomic flag indicating if MIDI input is active.
 * @midi_in_fifo: FIFO for raw MIDI input data.
 * @midi_in_work: Work struct for deferred MIDI input processing.
 * @midi_in_lock: Spinlock for MIDI input FIFO.
 * @midi_out_urbs: Array of URBs for MIDI output.
 * @midi_out_active: Atomic flag indicating if MIDI output is active.
 * @midi_out_work: Work struct for deferred MIDI output processing.
 * @midi_out_urbs_in_flight: Bitmap of MIDI output URBs currently in flight.
 * @midi_out_lock: Spinlock for MIDI output.
 * @midi_running_status: Stores the last MIDI status byte for running status.
 *
 * @lock: Main spinlock for protecting shared driver state.
 * @active_urbs: Atomic counter for active URBs.
 * @current_rate: Currently configured sample rate of the device.
 * @line_out_source: Source for Line Outputs (0: Playback 1-2, 1: Playback 3-4).
 * @digital_out_source: Source for Digital Outputs (0: Playback 1-2, 1: Playback
 * 3-4).
 * @capture_12_source: Source for Capture channels 1-2 (0: Analog In, 1: Digital
 * In).
 * @capture_34_source: Source for Capture channels 3-4 (0: Analog In, 1: Digital
 * In).
 *
 * @feedback_accumulator_pattern: Stores the calculated frames per packet for
 * feedback.
 * @feedback_pattern_out_idx: Read index for feedback_accumulator_pattern.
 * @feedback_pattern_in_idx: Write index for feedback_accumulator_pattern.
 * @feedback_synced: Flag indicating if feedback is synced.
 * @feedback_consecutive_errors: Counter for consecutive feedback errors.
 * @feedback_urb_skip_count: Number of feedback URBs to skip initially for
 * stabilization.
 * @feedback_patterns: Pointer to the current feedback patterns based on sample
 * rate.
 * @feedback_base_value: Base value for feedback pattern lookup.
 * @feedback_max_value: Max value for feedback pattern lookup.
 *
 * @playback_anchor: USB anchor for playback URBs.
 * @capture_anchor: USB anchor for capture URBs.
 * @feedback_anchor: USB anchor for feedback URBs.
 * @midi_in_anchor: USB anchor for MIDI input URBs.
 * @midi_out_anchor: USB anchor for MIDI output URBs.
 */
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

<<<<<<< HEAD
  /* Shared state & Routing Matrix */
  spinlock_t lock;
  atomic_t active_urbs;
  int current_rate;
  unsigned int line_out_source;    /* 0: Playback 1-2, 1: Playback 3-4 */
  unsigned int digital_out_source; /* 0: Playback 1-2, 1: Playback 3-4 */
  unsigned int capture_12_source;  /* 0: Analog In, 1: Digital In */
  unsigned int capture_34_source;  /* 0: Analog In, 1: Digital In */
=======
	/* Shared state & Routing Matrix */
	spinlock_t lock;
	atomic_t active_urbs;
	int current_rate;
	unsigned int line_out_source; /* 0: Playback 1-2, 1: Playback 3-4 */
	unsigned int digital_out_source; /* 0: Playback 1-2, 1: Playback 3-4 */
	unsigned int capture_12_source; /* 0: Analog In, 1: Digital In */
	unsigned int capture_34_source; /* 0: Analog In, 1: Digital In */
>>>>>>> f44b75094c078b0354fac280d769bc9a1bb6133b

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

/* main */
/**
 * tascam_free_urbs() - Free all allocated URBs and associated buffers.
 * @tascam: the tascam_card instance
 *
 * This function kills, unlinks, and frees all playback, feedback, capture,
 * and MIDI URBs, along with their transfer buffers and the capture
 * ring/decode buffers.
 */
void tascam_free_urbs(struct tascam_card *tascam);

/**
 * tascam_alloc_urbs() - Allocate all URBs and associated buffers.
 * @tascam: the tascam_card instance
 *
 * This function allocates and initializes all URBs for playback, feedback,
 * capture, and MIDI, as well as the necessary buffers for data processing.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int tascam_alloc_urbs(struct tascam_card *tascam);

/**
 * tascam_stop_work_handler() - Work handler to stop all active streams.
 * @work: Pointer to the work_struct.
 *
 * This function is scheduled to stop all active URBs (playback, feedback,
 * capture) and reset the active_urbs counter. It is used to gracefully stop
 * streams from a workqueue context.
 */
void tascam_stop_work_handler(struct work_struct *work);

/* us144mkii_pcm.h */
#include "us144mkii_pcm.h"

/* us144mkii_midi.c */
/**
 * tascam_midi_in_urb_complete() - Completion handler for MIDI IN URBs
 * @urb: The completed URB.
 *
 * This function runs in interrupt context. It places the raw data from the
 * USB endpoint into a kfifo and schedules a work item to process it later,
 * ensuring the interrupt handler remains fast.
 */
void tascam_midi_in_urb_complete(struct urb *urb);

/**
 * tascam_midi_out_urb_complete() - Completion handler for MIDI OUT bulk URB.
 * @urb: The completed URB.
 *
 * This function runs in interrupt context. It marks the output URB as no
 * longer in-flight. It then re-schedules the work handler to check for and
 * send any more data waiting in the ALSA buffer. This is a safe, non-blocking
 * way to continue the data transmission chain.
 */
void tascam_midi_out_urb_complete(struct urb *urb);

/**
 * tascam_create_midi() - Create and initialize the ALSA rawmidi device.
 * @tascam: The driver instance.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int tascam_create_midi(struct tascam_card *tascam);

/* us144mkii_controls.c */
/**
 * tascam_create_controls() - Creates and adds ALSA mixer controls for the
 * device.
 * @tascam: The driver instance.
 *
 * This function registers custom ALSA controls for managing audio routing
 * (line out source, digital out source, capture 1-2 source, capture 3-4 source)
 * and displaying the current sample rate.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int tascam_create_controls(struct tascam_card *tascam);

#endif /* __US144MKII_H */
