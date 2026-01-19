// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2025 Šerif Rami <ramiserifpersia@gmail.com>

#include "us144mkii.h"

static void tascam_midi_out_complete(struct urb *urb)
{
	struct tascam_card *tascam = urb->context;
	struct snd_rawmidi_substream *substream;
	unsigned long flags;
	int count;
	bool submit = false;
	bool active;

	spin_lock_irqsave(&tascam->midi_lock, flags);

	substream = tascam->midi_output;
	active = tascam->midi_out_active;

	if (urb->status || !substream) {
		tascam->midi_out_active = false;
		spin_unlock_irqrestore(&tascam->midi_lock, flags);
		return;
	}

	if (!active) {
		spin_unlock_irqrestore(&tascam->midi_lock, flags);
		return;
	}

	count = snd_rawmidi_transmit(substream, tascam->midi_out_buf, MIDI_PAYLOAD_SIZE);
	if (count > 0) {
		if (count < MIDI_PAYLOAD_SIZE)
			memset(tascam->midi_out_buf + count, 0xFD, MIDI_PAYLOAD_SIZE - count);

		tascam->midi_out_buf[8] = 0xE0;
		urb->transfer_buffer_length = MIDI_PACKET_SIZE;
		submit = true;
	} else {
		tascam->midi_out_active = false;
	}

	if (submit) {
		usb_anchor_urb(urb, &tascam->midi_anchor);
		if (usb_submit_urb(urb, GFP_ATOMIC) < 0) {
			usb_unanchor_urb(urb);
			tascam->midi_out_active = false;
		}
	}

	spin_unlock_irqrestore(&tascam->midi_lock, flags);
}

static void tascam_midi_output_trigger(struct snd_rawmidi_substream *substream, int up)
{
	struct tascam_card *tascam = substream->rmidi->private_data;
	unsigned long flags;
	int count;

	spin_lock_irqsave(&tascam->midi_lock, flags);

	if (up) {
		tascam->midi_output = substream;
		if (!tascam->midi_out_active) {
			tascam->midi_out_active = true;

			count = snd_rawmidi_transmit(substream, tascam->midi_out_buf, MIDI_PAYLOAD_SIZE);
			if (count > 0) {
				if (count < MIDI_PAYLOAD_SIZE)
					memset(tascam->midi_out_buf + count, 0xFD, MIDI_PAYLOAD_SIZE - count);

				tascam->midi_out_buf[8] = 0xE0;
				tascam->midi_out_urb->transfer_buffer_length = MIDI_PACKET_SIZE;

				usb_anchor_urb(tascam->midi_out_urb, &tascam->midi_anchor);
				if (usb_submit_urb(tascam->midi_out_urb, GFP_ATOMIC) < 0) {
					usb_unanchor_urb(tascam->midi_out_urb);
					tascam->midi_out_active = false;
				}
			} else {
				tascam->midi_out_active = false;
			}
		}
	} else {
		tascam->midi_output = NULL;
	}

	spin_unlock_irqrestore(&tascam->midi_lock, flags);
}

static void tascam_midi_in_complete(struct urb *urb)
{
	struct tascam_card *tascam = urb->context;
	struct snd_rawmidi_substream *substream;
	unsigned long flags;

	if (urb->status)
		return;

	spin_lock_irqsave(&tascam->midi_lock, flags);
	substream = tascam->midi_input;
	spin_unlock_irqrestore(&tascam->midi_lock, flags);

	if (urb->actual_length == MIDI_PACKET_SIZE && substream) {
		u8 *data = urb->transfer_buffer;
		int len = 0;

		/* Find the actual length of the MIDI message (stop at 0xFD padding) */
		while (len < MIDI_PAYLOAD_SIZE && data[len] != 0xFD)
			len++;

		if (len > 0)
			snd_rawmidi_receive(substream, data, len);
	}

	usb_anchor_urb(urb, &tascam->midi_anchor);
	if (usb_submit_urb(urb, GFP_ATOMIC) < 0)
		usb_unanchor_urb(urb);
}

static void tascam_midi_input_trigger(struct snd_rawmidi_substream *substream, int up)
{
	struct tascam_card *tascam = substream->rmidi->private_data;
	unsigned long flags;

	spin_lock_irqsave(&tascam->midi_lock, flags);
	if (up) {
		tascam->midi_input = substream;
	} else {
		tascam->midi_input = NULL;
	}
	spin_unlock_irqrestore(&tascam->midi_lock, flags);
}

static int tascam_midi_open(struct snd_rawmidi_substream *substream)
{
	struct tascam_card *tascam = substream->rmidi->private_data;

	if (substream->stream == SNDRV_RAWMIDI_STREAM_OUTPUT) {
		unsigned long flags;

		spin_lock_irqsave(&tascam->midi_lock, flags);
		tascam->midi_out_active = false;
		spin_unlock_irqrestore(&tascam->midi_lock, flags);
	} else if (substream->stream == SNDRV_RAWMIDI_STREAM_INPUT) {
		usb_anchor_urb(tascam->midi_in_urb, &tascam->midi_anchor);
		if (usb_submit_urb(tascam->midi_in_urb, GFP_KERNEL) < 0) {
			usb_unanchor_urb(tascam->midi_in_urb);
			return -EIO;
		}
	}
	return 0;
}

static int tascam_midi_close(struct snd_rawmidi_substream *substream)
{
	struct tascam_card *tascam = substream->rmidi->private_data;

	if (substream->stream == SNDRV_RAWMIDI_STREAM_INPUT)
		usb_kill_urb(tascam->midi_in_urb);
	return 0;
}

static const struct snd_rawmidi_ops midi_output_ops = {
	.open = tascam_midi_open,
	.close = tascam_midi_close,
	.trigger = tascam_midi_output_trigger,
};

static const struct snd_rawmidi_ops midi_input_ops = {
	.open = tascam_midi_open,
	.close = tascam_midi_close,
	.trigger = tascam_midi_input_trigger,
};

/**
 * tascam_create_midi - create and initialize the MIDI device
 * @tascam: the tascam_card instance
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int tascam_create_midi(struct tascam_card *tascam)
{
	int err;
	struct snd_rawmidi *rmidi;

	err = snd_rawmidi_new(tascam->card, "TASCAM MIDI", 0, 1, 1, &rmidi);
	if (err < 0)
		return err;

	rmidi->private_data = tascam;
	strscpy(rmidi->name, "TASCAM US-144MKII MIDI", sizeof(rmidi->name));

	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT, &midi_output_ops);
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_INPUT, &midi_input_ops);

	rmidi->info_flags = SNDRV_RAWMIDI_INFO_OUTPUT |
	SNDRV_RAWMIDI_INFO_INPUT |
	SNDRV_RAWMIDI_INFO_DUPLEX;
	tascam->rmidi = rmidi;

	tascam->midi_out_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!tascam->midi_out_urb) {
		err = -ENOMEM;
		goto err_out_urb;
	}

	tascam->midi_in_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!tascam->midi_in_urb) {
		err = -ENOMEM;
		goto err_in_urb;
	}

	tascam->midi_out_buf = usb_alloc_coherent(tascam->dev, MIDI_PACKET_SIZE,
											  GFP_KERNEL, &tascam->midi_out_urb->transfer_dma);
	if (!tascam->midi_out_buf) {
		err = -ENOMEM;
		goto err_out_buf;
	}

	tascam->midi_in_buf = usb_alloc_coherent(tascam->dev, MIDI_PACKET_SIZE,
											 GFP_KERNEL, &tascam->midi_in_urb->transfer_dma);
	if (!tascam->midi_in_buf) {
		err = -ENOMEM;
		goto err_in_buf;
	}

	usb_fill_bulk_urb(tascam->midi_out_urb, tascam->dev,
					  usb_sndbulkpipe(tascam->dev, EP_MIDI_OUT),
					  tascam->midi_out_buf, MIDI_PACKET_SIZE,
				   tascam_midi_out_complete, tascam);
	tascam->midi_out_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	usb_fill_bulk_urb(tascam->midi_in_urb, tascam->dev,
					  usb_rcvbulkpipe(tascam->dev, EP_MIDI_IN),
					  tascam->midi_in_buf, MIDI_PACKET_SIZE,
				   tascam_midi_in_complete, tascam);
	tascam->midi_in_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	spin_lock_init(&tascam->midi_lock);
	init_usb_anchor(&tascam->midi_anchor);

	return 0;

	err_in_buf:
	usb_free_coherent(tascam->dev, MIDI_PACKET_SIZE,
					  tascam->midi_out_buf, tascam->midi_out_urb->transfer_dma);
	err_out_buf:
	usb_free_urb(tascam->midi_in_urb);
	tascam->midi_in_urb = NULL;
	err_in_urb:
	usb_free_urb(tascam->midi_out_urb);
	tascam->midi_out_urb = NULL;
	err_out_urb:
	return err;
}
