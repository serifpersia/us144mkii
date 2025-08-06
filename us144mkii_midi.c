// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2025 Šerif Rami <ramiserifpersia@gmail.com>

#include "us144mkii.h"

/**
 * tascam_midi_in_work_handler() - Deferred work for processing MIDI input.
 * @work: The work_struct instance.
 *
 * This function runs in a thread context. It safely reads raw USB data from
 * the kfifo, processes it by stripping protocol-specific padding bytes, and
 * passes the clean MIDI data to the ALSA rawmidi subsystem.
 */
static void tascam_midi_in_work_handler(struct work_struct *work) {
  struct tascam_card *tascam =
      container_of(work, struct tascam_card, midi_in_work);
  u8 buf[MIDI_IN_BUF_SIZE];
  unsigned int len;
  int i;

  if (!tascam->midi_in_substream)
    return;

  while (!kfifo_is_empty(&tascam->midi_in_fifo)) {
    len = kfifo_out_spinlocked(&tascam->midi_in_fifo, buf, sizeof(buf),
                               &tascam->midi_in_lock);

    if (len == 0)
      continue;

    if (!tascam->midi_in_substream)
      continue;

    for (i = 0; i < len; ++i) {
      u8 byte = buf[i];

      /* Skip padding bytes */
      if (byte == 0xfd)
        continue;

      if (byte == 0xf0) { /* SysEx Start */
        tascam->in_sysex = true;
      } else if (byte == 0xf7) { /* SysEx End */
        tascam->in_sysex = false;
      } else if (tascam->in_sysex) {
        /* Inside a SysEx message */
      } else if (byte & 0x80) { /* Status byte */
        tascam->midi_running_status = byte;
      } else { /* Data byte */
        if (tascam->midi_running_status != 0)
          snd_rawmidi_receive(tascam->midi_in_substream,
                                &tascam->midi_running_status, 1);
      }

      /* Submit valid MIDI bytes one by one */
      snd_rawmidi_receive(tascam->midi_in_substream, &byte, 1);
    }
  }
}

/**
 * tascam_midi_in_urb_complete() - Completion handler for MIDI IN URBs
 * @urb: The completed URB.
 *
 * This function runs in interrupt context. It places the raw data from the
 * USB endpoint into a kfifo and schedules a work item to process it later,
 * ensuring the interrupt handler remains fast.
 */
void tascam_midi_in_urb_complete(struct urb *urb) {
  struct tascam_card *tascam = urb->context;
  int ret;

  if (urb->status) {
    if (urb->status != -ENOENT && urb->status != -ECONNRESET &&
        urb->status != -ESHUTDOWN && urb->status != -EPROTO) {
      dev_err_ratelimited(tascam->card->dev, "MIDI IN URB failed: status %d\n",
                          urb->status);
      mod_timer(&tascam->error_timer, jiffies + msecs_to_jiffies(50));
    }
    goto out;
  }

  if (tascam && atomic_read(&tascam->midi_in_active) &&
      urb->actual_length > 0) {
    kfifo_in_spinlocked(&tascam->midi_in_fifo, urb->transfer_buffer,
                        urb->actual_length, &tascam->midi_in_lock);
    schedule_work(&tascam->midi_in_work);
  }

  usb_get_urb(urb);
  usb_anchor_urb(urb, &tascam->midi_in_anchor);
  ret = usb_submit_urb(urb, GFP_ATOMIC);
  if (ret < 0) {
    dev_err(tascam->card->dev, "Failed to resubmit MIDI IN URB: error %d\n",
            ret);
    usb_unanchor_urb(urb);
    usb_put_urb(urb);
  }
out:
  usb_put_urb(urb);
}

/**
 * tascam_midi_in_open() - Opens the MIDI input substream.
 * @substream: The ALSA rawmidi substream to open.
 *
 * This function stores a reference to the MIDI input substream in the
 * driver's private data.
 *
 * Return: 0 on success.
 */
static int tascam_midi_in_open(struct snd_rawmidi_substream *substream) {
  struct tascam_card *tascam = substream->rmidi->private_data;

  tascam->midi_in_substream = substream;
  return 0;
}

/**
 * tascam_midi_in_close() - Closes the MIDI input substream.
 * @substream: The ALSA rawmidi substream to close.
 *
 * Return: 0 on success.
 */
static int tascam_midi_in_close(struct snd_rawmidi_substream *substream) {
  return 0;
}

/**
 * tascam_midi_in_trigger() - Triggers MIDI input stream activity.
 * @substream: The ALSA rawmidi substream.
 * @up: Boolean indicating whether to start (1) or stop (0) the stream.
 *
 * This function starts or stops the MIDI input URBs based on the 'up'
 * parameter. When starting, it resets the kfifo and submits all MIDI input
 * URBs. When stopping, it kills all anchored MIDI input URBs and cancels the
 * associated workqueue.
 */
static void tascam_midi_in_trigger(struct snd_rawmidi_substream *substream,
                                   int up) {
  struct tascam_card *tascam = substream->rmidi->private_data;
  int i, err;
  unsigned long flags;

  if (up) {
    if (atomic_xchg(&tascam->midi_in_active, 1) == 0) {
      spin_lock_irqsave(&tascam->midi_in_lock, flags);
      kfifo_reset(&tascam->midi_in_fifo);
      spin_unlock_irqrestore(&tascam->midi_in_lock, flags);

      for (i = 0; i < NUM_MIDI_IN_URBS; i++) {
        usb_get_urb(tascam->midi_in_urbs[i]);
        usb_anchor_urb(tascam->midi_in_urbs[i], &tascam->midi_in_anchor);
        err = usb_submit_urb(tascam->midi_in_urbs[i], GFP_KERNEL);
        if (err < 0) {
          dev_err(tascam->card->dev, "Failed to submit MIDI IN URB %d: %d\n", i,
                  err);
          usb_unanchor_urb(tascam->midi_in_urbs[i]);
          usb_put_urb(tascam->midi_in_urbs[i]);
        }
      }
    }
  } else {
    if (atomic_xchg(&tascam->midi_in_active, 0) == 1) {
      usb_kill_anchored_urbs(&tascam->midi_in_anchor);
      cancel_work_sync(&tascam->midi_in_work);
    }
  }
}

/**
 * tascam_midi_in_ops - ALSA rawmidi operations for MIDI input.
 *
 * This structure defines the callback functions for MIDI input stream
 * operations, including open, close, and trigger.
 */
static const struct snd_rawmidi_ops tascam_midi_in_ops = {
    .open = tascam_midi_in_open,
    .close = tascam_midi_in_close,
    .trigger = tascam_midi_in_trigger,
};

/**
 * tascam_midi_out_urb_complete() - Completion handler for MIDI OUT bulk URB.
 * @urb: The completed URB.
 *
 * This function runs in interrupt context. It marks the output URB as no
 * longer in-flight. It then re-schedules the work handler to check for and
 * send any more data waiting in the ALSA buffer. This is a safe, non-blocking
 * way to continue the data transmission chain.
 */
void tascam_midi_out_urb_complete(struct urb *urb) {
  struct tascam_card *tascam = urb->context;
  unsigned long flags;
  int i, urb_index = -1;

  if (urb->status) {
    if (urb->status != -ENOENT && urb->status != -ECONNRESET &&
        urb->status != -ESHUTDOWN) {
      dev_err_ratelimited(tascam->card->dev, "MIDI OUT URB failed: %d\n",
                          urb->status);
      mod_timer(&tascam->error_timer, jiffies + msecs_to_jiffies(50));
    }
  }

  if (!tascam)
    goto out;

  for (i = 0; i < NUM_MIDI_OUT_URBS; i++) {
    if (tascam->midi_out_urbs[i] == urb) {
      urb_index = i;
      break;
    }
  }

  if (urb_index < 0) {
    dev_err_ratelimited(tascam->card->dev, "Unknown MIDI OUT URB completed!\n");
    goto out;
  }

  spin_lock_irqsave(&tascam->midi_out_lock, flags);
  clear_bit(urb_index, &tascam->midi_out_urbs_in_flight);
  spin_unlock_irqrestore(&tascam->midi_out_lock, flags);

  if (atomic_read(&tascam->midi_out_active))
    schedule_work(&tascam->midi_out_work);
out:
  usb_put_urb(urb);
}

/**
 * tascam_midi_out_work_handler() - Deferred work for sending MIDI data
 * @work: The work_struct instance.
 *
 * This function handles the proprietary output protocol: take the raw MIDI
 * message bytes from the application, place them at the start of a 9-byte
 * buffer, pad the rest with 0xFD, and add a terminator byte (0x00).
 * This function pulls as many bytes as will fit into one packet from the
 * ALSA buffer and sends them.
 */
static void tascam_midi_out_work_handler(struct work_struct *work) {
  struct tascam_card *tascam =
      container_of(work, struct tascam_card, midi_out_work);
  struct snd_rawmidi_substream *substream = tascam->midi_out_substream;
  int i;

  if (!substream || !atomic_read(&tascam->midi_out_active))
    return;

  while (snd_rawmidi_transmit_peek(substream, (u8[]){0}, 1) == 1) {
    unsigned long flags;
    int urb_index;
    struct urb *urb;
    u8 *buf;
    int bytes_to_send;

    spin_lock_irqsave(&tascam->midi_out_lock, flags);

    urb_index = -1;
    for (i = 0; i < NUM_MIDI_OUT_URBS; i++) {
      if (!test_bit(i, &tascam->midi_out_urbs_in_flight)) {
        urb_index = i;
        break;
      }
    }

    if (urb_index < 0) {
      spin_unlock_irqrestore(&tascam->midi_out_lock, flags);
      return; /* No free URBs, will be rescheduled by completion handler */
    }

    urb = tascam->midi_out_urbs[urb_index];
    buf = urb->transfer_buffer;
    bytes_to_send = snd_rawmidi_transmit(substream, buf, 8);

    if (bytes_to_send <= 0) {
      spin_unlock_irqrestore(&tascam->midi_out_lock, flags);
      break; /* No more data */
    }

    if (bytes_to_send < 9)
      memset(buf + bytes_to_send, 0xfd, 9 - bytes_to_send);
    buf[8] = 0x00;

    set_bit(urb_index, &tascam->midi_out_urbs_in_flight);
    urb->transfer_buffer_length = 9;
    spin_unlock_irqrestore(&tascam->midi_out_lock, flags);

    usb_get_urb(urb);
    usb_anchor_urb(urb, &tascam->midi_out_anchor);
    if (usb_submit_urb(urb, GFP_KERNEL) < 0) {
      dev_err_ratelimited(tascam->card->dev,
                          "Failed to submit MIDI OUT URB %d\n", urb_index);
      spin_lock_irqsave(&tascam->midi_out_lock, flags);
      clear_bit(urb_index, &tascam->midi_out_urbs_in_flight);
      spin_unlock_irqrestore(&tascam->midi_out_lock, flags);
      usb_unanchor_urb(urb);
      usb_put_urb(urb);
      break; /* Stop on error */
    }
  }
}

/**
 * tascam_midi_out_open() - Opens the MIDI output substream.
 * @substream: The ALSA rawmidi substream to open.
 *
 * This function stores a reference to the MIDI output substream in the
 * driver's private data and initializes the MIDI running status.
 *
 * Return: 0 on success.
 */
static int tascam_midi_out_open(struct snd_rawmidi_substream *substream) {
  struct tascam_card *tascam = substream->rmidi->private_data;

  tascam->midi_out_substream = substream;
  /* Initialize the running status state for the packet packer. */
  tascam->midi_running_status = 0;
  return 0;
}

/**
 * tascam_midi_out_close() - Closes the MIDI output substream.
 * @substream: The ALSA rawmidi substream to close.
 *
 * Return: 0 on success.
 */
static int tascam_midi_out_close(struct snd_rawmidi_substream *substream) {
  return 0;
}

/**
 * tascam_midi_out_drain() - Drains the MIDI output stream.
 * @substream: The ALSA rawmidi substream.
 *
 * This function cancels any pending MIDI output work and kills all
 * anchored MIDI output URBs, ensuring all data is sent or discarded.
 */
static void tascam_midi_out_drain(struct snd_rawmidi_substream *substream) {
  struct tascam_card *tascam = substream->rmidi->private_data;

  cancel_work_sync(&tascam->midi_out_work);
  usb_kill_anchored_urbs(&tascam->midi_out_anchor);
}

/**
 * tascam_midi_out_trigger() - Triggers MIDI output stream activity.
 * @substream: The ALSA rawmidi substream.
 * @up: Boolean indicating whether to start (1) or stop (0) the stream.
 *
 * This function starts or stops the MIDI output workqueue based on the
 * 'up' parameter.
 */
static void tascam_midi_out_trigger(struct snd_rawmidi_substream *substream,
                                    int up) {
  struct tascam_card *tascam = substream->rmidi->private_data;

  if (up) {
    atomic_set(&tascam->midi_out_active, 1);
    schedule_work(&tascam->midi_out_work);
  } else {
    atomic_set(&tascam->midi_out_active, 0);
  }
}

/**
 * tascam_midi_out_ops - ALSA rawmidi operations for MIDI output.
 *
 * This structure defines the callback functions for MIDI output stream
 * operations, including open, close, trigger, and drain.
 */
static const struct snd_rawmidi_ops tascam_midi_out_ops = {
    .open = tascam_midi_out_open,
    .close = tascam_midi_out_close,
    .trigger = tascam_midi_out_trigger,
    .drain = tascam_midi_out_drain,
};

/**
 * tascam_create_midi() - Create and initialize the ALSA rawmidi device.
 * @tascam: The driver instance.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int tascam_create_midi(struct tascam_card *tascam) {
  int err;

  err =
      snd_rawmidi_new(tascam->card, "US144MKII MIDI", 0, 1, 1, &tascam->rmidi);
  if (err < 0)
    return err;

  strscpy(tascam->rmidi->name, "US144MKII MIDI", sizeof(tascam->rmidi->name));
  tascam->rmidi->private_data = tascam;

  snd_rawmidi_set_ops(tascam->rmidi, SNDRV_RAWMIDI_STREAM_INPUT,
                      &tascam_midi_in_ops);
  snd_rawmidi_set_ops(tascam->rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT,
                      &tascam_midi_out_ops);

  tascam->rmidi->info_flags |= SNDRV_RAWMIDI_INFO_INPUT |
                               SNDRV_RAWMIDI_INFO_OUTPUT |
                               SNDRV_RAWMIDI_INFO_DUPLEX;

  INIT_WORK(&tascam->midi_in_work, tascam_midi_in_work_handler);
  INIT_WORK(&tascam->midi_out_work, tascam_midi_out_work_handler);

  return 0;
}
