// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2025 Šerif Rami <ramiserifpersia@gmail.com>

#include "us144mkii.h"

/**
 * tascam_capture_open() - Opens the PCM capture substream.
 * @substream: The ALSA PCM substream to open.
 *
 * This function sets the hardware parameters for the capture substream
 * and stores a reference to the substream in the driver's private data.
 *
 * Return: 0 on success.
 */
static int tascam_capture_open(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);

	substream->runtime->hw = tascam_pcm_hw;
	tascam->capture_substream = substream;
	atomic_set(&tascam->capture_active, 0);

	return 0;
}

/**
 * tascam_capture_close() - Closes the PCM capture substream.
 * @substream: The ALSA PCM substream to close.
 *
 * This function clears the reference to the capture substream in the
 * driver's private data.
 *
 * Return: 0 on success.
 */
static int tascam_capture_close(struct snd_pcm_substream *substream)
{
	struct tascam_card *tascam = snd_pcm_substream_chip(substream);

	tascam->capture_substream = NULL;

	return 0;
}

/**
 * tascam_capture_prepare() - Prepares the PCM capture substream for use.
 * @substream: The ALSA PCM substream to prepare.
 *
 * This function initializes capture-related counters and ring buffer pointers.
 *
 * Return: 0 on success.
 */
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

/**
 * tascam_capture_pointer() - Returns the current capture pointer position.
 * @substream: The ALSA PCM substream.
 *
 * This function returns the current position of the capture pointer within
 * the ALSA ring buffer, in frames.
 *
 * Return: The current capture pointer position in frames.
 */
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


	u64 remainder = do_div(pos, runtime->buffer_size);
	return runtime ? remainder : 0;

}

/**
 * tascam_capture_ops - ALSA PCM operations for capture.
 *
 * This structure defines the callback functions for capture stream operations,
 * including open, close, ioctl, hardware parameters, hardware free, prepare,
 * trigger, and pointer.
 */
const struct snd_pcm_ops tascam_capture_ops = {
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
 * decode_tascam_capture_block() - Decodes a raw 512-byte block from the device.
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

		/*
		 * The result is a 24-bit sample. Shift left by 8 to align it to
		 * the most significant bits of a 32-bit integer (S32_LE format).
		 */
		p_dst_frame[0] = ch[0] << 8;
		p_dst_frame[1] = ch[1] << 8;
		p_dst_frame[2] = ch[2] << 8;
		p_dst_frame[3] = ch[3] << 8;
	}
}

/**
 * tascam_capture_work_handler() - Deferred work for processing capture data.
 * @work: the work_struct instance
 *
 * This function runs in a kernel thread context, not an IRQ context. It reads
 * raw data from the capture ring buffer, decodes it, applies routing, and
 * copies the final audio data into the ALSA capture ring buffer. This offloads
 * * the CPU-intensive decoding from the time-sensitive URB completion handlers.
 */
void tascam_capture_work_handler(struct work_struct *work)
{
	struct tascam_card *tascam = container_of(work, struct tascam_card, capture_work);
	struct snd_pcm_substream *substream = tascam->capture_substream;
	struct snd_pcm_runtime *runtime;
	unsigned long flags;
	u8 *raw_block = tascam->capture_decode_raw_block;
	s32 *decoded_block = tascam->capture_decode_dst_block;
	s32 *routed_block = tascam->capture_routing_buffer;

	if (!substream || !substream->runtime)
		return;
	runtime = substream->runtime;

	if (!raw_block || !decoded_block || !routed_block) {
		dev_err(tascam->card->dev, "Capture decode/routing buffers not allocated!\n");
		return;
	}

	while (atomic_read(&tascam->capture_active)) {
		size_t write_ptr, read_ptr, available_data;
		bool can_process;

		spin_lock_irqsave(&tascam->lock, flags);
		write_ptr = tascam->capture_ring_buffer_write_ptr;
		read_ptr = tascam->capture_ring_buffer_read_ptr;
		available_data = (write_ptr >= read_ptr) ? (write_ptr - read_ptr) : (CAPTURE_RING_BUFFER_SIZE - read_ptr + write_ptr);
		can_process = (available_data >= RAW_BYTES_PER_DECODE_BLOCK);

		if (can_process) {
			size_t i;

			for (i = 0; i < RAW_BYTES_PER_DECODE_BLOCK; i++)
				raw_block[i] = tascam->capture_ring_buffer[(read_ptr + i) % CAPTURE_RING_BUFFER_SIZE];
			tascam->capture_ring_buffer_read_ptr = (read_ptr + RAW_BYTES_PER_DECODE_BLOCK) % CAPTURE_RING_BUFFER_SIZE;
		}
		spin_unlock_irqrestore(&tascam->lock, flags);

		if (!can_process)
			break;

		decode_tascam_capture_block(raw_block, decoded_block);
		process_capture_routing_us144mkii(tascam, decoded_block, routed_block);

		spin_lock_irqsave(&tascam->lock, flags);
		if (atomic_read(&tascam->capture_active)) {
			int f;

			for (f = 0; f < FRAMES_PER_DECODE_BLOCK; ++f) {
				u8 *dst_frame_start = runtime->dma_area + frames_to_bytes(runtime, tascam->driver_capture_pos);
				s32 *routed_frame_start = routed_block + (f * NUM_CHANNELS);
				int c;

				for (c = 0; c < NUM_CHANNELS; c++) {
					u8 *dst_channel = dst_frame_start + (c * BYTES_PER_SAMPLE);
					s32 *src_channel_s32 = routed_frame_start + c;

					memcpy(dst_channel, ((char *)src_channel_s32) + 1, 3);
				}

				tascam->driver_capture_pos = (tascam->driver_capture_pos + 1) % runtime->buffer_size;
			}
		}
		spin_unlock_irqrestore(&tascam->lock, flags);
	}
}

/**
 * capture_urb_complete() - Completion handler for capture bulk URBs.
 * @urb: the completed URB
 *
 * This function runs in interrupt context. It copies the received raw data
 * into an intermediate ring buffer and then schedules the workqueue to process
 * it. It then resubmits the URB to receive more data.
 */
void capture_urb_complete(struct urb *urb)
{
	struct tascam_card *tascam = urb->context;
	int ret;
	unsigned long flags;

	if (urb->status) {
		if (urb->status != -ENOENT && urb->status != -ECONNRESET && urb->status != -ESHUTDOWN &&
		    urb->status != -ENODEV && urb->status != -EPROTO)
			dev_err_ratelimited(tascam->card->dev, "Capture URB failed: %d\n", urb->status);
		goto out;
	}
	if (!tascam || !atomic_read(&tascam->capture_active))
		goto out;

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

	usb_get_urb(urb);
	usb_anchor_urb(urb, &tascam->capture_anchor);
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0) {
		dev_err_ratelimited(tascam->card->dev, "Failed to resubmit capture URB: %d\n", ret);
		usb_unanchor_urb(urb);
		usb_put_urb(urb);
	}
out:
	usb_put_urb(urb);
}
