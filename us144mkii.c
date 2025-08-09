// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2025 Šerif Rami <ramiserifpersia@gmail.com>
/*
 * ALSA Driver for TASCAM US-144MKII Audio Interface
 */

#include "us144mkii.h"

MODULE_AUTHOR("Šerif Rami <ramiserifpersia@gmail.com>");
MODULE_DESCRIPTION("ALSA Driver for TASCAM US-144MKII");
MODULE_LICENSE("GPL");

/**
 * @brief Module parameters for ALSA card instantiation.
 *
 * These parameters allow users to configure how the ALSA sound card
 * for the TASCAM US-144MKII is instantiated.
 *
 * @param index: Array of integers specifying the ALSA card index for each
 * device. Defaults to -1 (automatic).
 * @param id: Array of strings specifying the ALSA card ID for each device.
 *            Defaults to "US144MKII".
 * @param enable: Array of booleans to enable or disable each device.
 *                Defaults to {1, 0, ..., 0} (first device enabled).
 * @param dev_idx: Internal counter for the number of TASCAM devices probed.
 */
static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = {1, [1 ...(SNDRV_CARDS - 1)] = 0};
static int dev_idx;

static int tascam_probe(struct usb_interface *intf,
                        const struct usb_device_id *usb_id);
static void tascam_disconnect(struct usb_interface *intf);
static int tascam_suspend(struct usb_interface *intf, pm_message_t message);
static int tascam_resume(struct usb_interface *intf);

void tascam_free_urbs(struct tascam_card *tascam) {
  int i;

  usb_kill_anchored_urbs(&tascam->playback_anchor);
  for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
    if (tascam->playback_urbs[i]) {
      usb_free_coherent(tascam->dev, tascam->playback_urb_alloc_size,
                        tascam->playback_urbs[i]->transfer_buffer,
                        tascam->playback_urbs[i]->transfer_dma);
      usb_free_urb(tascam->playback_urbs[i]);
      tascam->playback_urbs[i] = NULL;
    }
  }

  usb_kill_anchored_urbs(&tascam->feedback_anchor);
  for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
    if (tascam->feedback_urbs[i]) {
      usb_free_coherent(tascam->dev, tascam->feedback_urb_alloc_size,
                        tascam->feedback_urbs[i]->transfer_buffer,
                        tascam->feedback_urbs[i]->transfer_dma);
      usb_free_urb(tascam->feedback_urbs[i]);
      tascam->feedback_urbs[i] = NULL;
    }
  }
}

int tascam_alloc_urbs(struct tascam_card *tascam) {
  int i;
  size_t max_packet_size;

  max_packet_size = ((96000 / 8000) + 2) * BYTES_PER_FRAME;
  tascam->playback_urb_alloc_size = max_packet_size * PLAYBACK_URB_PACKETS;

  for (i = 0; i < NUM_PLAYBACK_URBS; i++) {
    struct urb *urb = usb_alloc_urb(PLAYBACK_URB_PACKETS, GFP_KERNEL);

    if (!urb)
      goto error;
    tascam->playback_urbs[i] = urb;

    urb->transfer_buffer =
        usb_alloc_coherent(tascam->dev, tascam->playback_urb_alloc_size,
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

  tascam->feedback_urb_alloc_size = FEEDBACK_PACKET_SIZE * FEEDBACK_URB_PACKETS;

  for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
    struct urb *f_urb = usb_alloc_urb(FEEDBACK_URB_PACKETS, GFP_KERNEL);

    if (!f_urb)
      goto error;
    tascam->feedback_urbs[i] = f_urb;

    f_urb->transfer_buffer =
        usb_alloc_coherent(tascam->dev, tascam->feedback_urb_alloc_size,
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

void tascam_stop_work_handler(struct work_struct *work) {
  struct tascam_card *tascam =
      container_of(work, struct tascam_card, stop_work);

  usb_kill_anchored_urbs(&tascam->playback_anchor);
  usb_kill_anchored_urbs(&tascam->feedback_anchor);
  atomic_set(&tascam->active_urbs, 0);
}

/**
 * tascam_card_private_free() - Frees private data associated with the sound
 * card.
 * @card: Pointer to the ALSA sound card instance.
 *
 * This function is called when the sound card is being freed. It releases
 * the reference to the USB device.
 */
static void tascam_card_private_free(struct snd_card *card) {
  struct tascam_card *tascam = card->private_data;

  if (tascam && tascam->dev) {
    usb_put_dev(tascam->dev);
    tascam->dev = NULL;
  }
}

/**
 * tascam_probe() - Probes for the TASCAM US-144MKII device.
 * @intf: The USB interface being probed.
 * @usb_id: The USB device ID.
 *
 * This function is the entry point for the USB driver when a matching device
 * is found. It performs initial device setup, including:
 * - Checking for the second interface (MIDI) and associating it.
 * - Performing a vendor-specific handshake with the device.
 * - Setting alternate settings for USB interfaces.
 * - Creating and registering the ALSA sound card and PCM device.
 * - Allocating and initializing URBs for audio transfers.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
static int tascam_probe(struct usb_interface *intf,
                        const struct usb_device_id *usb_id) {
  struct usb_device *dev = interface_to_usbdev(intf);
  struct snd_card *card;
  struct tascam_card *tascam;
  int err;
  char *handshake_buf __free(kfree);

  if (dev->speed != USB_SPEED_HIGH)
    dev_info(&dev->dev,
             "Device is connected to a USB 1.1 port, this is not supported.\n");

  /* The device has two interfaces; we drive both from this driver. */
  if (intf->cur_altsetting->desc.bInterfaceNumber == 1) {
    tascam = usb_get_intfdata(usb_ifnum_to_if(dev, 0));
    if (tascam) {
      usb_set_intfdata(intf, tascam);
      tascam->iface1 = intf;
    }
    return 0; /* Let the core handle this interface */
  }

  if (dev_idx >= SNDRV_CARDS) {
    dev_err(&dev->dev, "Too many TASCAM devices present");
    return -ENODEV;
  }

  if (!enable[dev_idx]) {
    dev_info(&dev->dev, "TASCAM US-144MKII device disabled");
    return -ENOENT;
  }

  handshake_buf = kmalloc(1, GFP_KERNEL);
  if (!handshake_buf)
    return -ENOMEM;

  /* Perform vendor-specific handshake */
  err = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0), VENDOR_REQ_MODE_CONTROL,
                        RT_D2H_VENDOR_DEV, MODE_VAL_HANDSHAKE_READ, 0x0000,
                        handshake_buf, 1, USB_CTRL_TIMEOUT_MS);
  if (err < 0) {
    dev_err(&dev->dev, "Handshake read failed with %d\n", err);
    return err;
  }

  if (handshake_buf[0] != 0x12 && handshake_buf[0] != 0x16 &&
      handshake_buf[0] != 0x30) {
    dev_err(&dev->dev, "Unexpected handshake value: 0x%x\n", handshake_buf[0]);
    return -ENODEV;
  }

  /* Set alternate settings to enable audio/MIDI endpoints */
  err = usb_set_interface(dev, 0, 1);
  if (err < 0) {
    dev_err(&dev->dev, "Failed to set alt setting 1 on interface 0: %d\n", err);
    return err;
  }

  err = usb_set_interface(dev, 1, 1);
  if (err < 0) {
    dev_err(&dev->dev, "Failed to set alt setting 1 on interface 1: %d\n", err);
    return err;
  }

  err = snd_card_new(&dev->dev, index[dev_idx], id[dev_idx], THIS_MODULE,
                     sizeof(struct tascam_card), &card);
  if (err < 0) {
    dev_err(&dev->dev, "Failed to create sound card instance\n");
    return err;
  }

  tascam = card->private_data;
  card->private_free = tascam_card_private_free;
  tascam->dev = usb_get_dev(dev);
  tascam->card = card;
  tascam->iface0 = intf;

  spin_lock_init(&tascam->lock);
  init_usb_anchor(&tascam->playback_anchor);
  init_usb_anchor(&tascam->feedback_anchor);

  INIT_WORK(&tascam->stop_work, tascam_stop_work_handler);
  INIT_WORK(&tascam->stop_pcm_work, tascam_stop_pcm_work_handler);

  err = snd_pcm_new(card, "US144MKII PCM", 0, 1, 1, &tascam->pcm);
  if (err < 0)
    goto free_card;
  tascam->pcm->private_data = tascam;
  strscpy(tascam->pcm->name, "US144MKII PCM", sizeof(tascam->pcm->name));

  err = tascam_init_pcm(tascam->pcm);
  if (err < 0)
    goto free_card;

  err = tascam_alloc_urbs(tascam);
  if (err < 0)
    goto free_card;

  strscpy(card->driver, DRIVER_NAME, sizeof(card->driver));
  if (dev->descriptor.idProduct == USB_PID_TASCAM_US144) {
    strscpy(card->shortname, "TASCAM US-144", sizeof(card->shortname));
  } else if (dev->descriptor.idProduct == USB_PID_TASCAM_US144MKII) {
    strscpy(card->shortname, "TASCAM US-144MKII", sizeof(card->shortname));
  } else {
    strscpy(card->shortname, "TASCAM Unknown", sizeof(card->shortname));
  }
  snprintf(card->longname, sizeof(card->longname), "%s (%04x:%04x) at %s",
           card->shortname, USB_VID_TASCAM, dev->descriptor.idProduct,
           dev_name(&dev->dev));

  err = snd_card_register(card);
  if (err < 0)
    goto free_card;

  usb_set_intfdata(intf, tascam);

  dev_idx++;
  return 0;

free_card:
  tascam_free_urbs(tascam);
  snd_card_free(card);
  return err;
}

/**
 * tascam_disconnect() - Disconnects the TASCAM US-144MKII device.
 * @intf: The USB interface being disconnected.
 *
 * This function is called when the device is disconnected from the system.
 * It cleans up all allocated resources by freeing the sound card, which in
 * turn triggers freeing of URBs and other resources.
 */
static void tascam_disconnect(struct usb_interface *intf) {
  struct tascam_card *tascam = usb_get_intfdata(intf);

  if (!tascam)
    return;

  if (intf->cur_altsetting->desc.bInterfaceNumber == 0) {
    snd_card_disconnect(tascam->card);
    cancel_work_sync(&tascam->stop_work);
    cancel_work_sync(&tascam->stop_pcm_work);
    tascam_free_urbs(tascam);
    snd_card_free(tascam->card);
    dev_idx--;
  }
}

/**
 * tascam_suspend() - Handles device suspension.
 * @intf: The USB interface being suspended.
 * @message: Power management message.
 *
 * This function is called when the device is suspended. It stops all active
 * streams and kills all URBs.
 *
 * Return: 0 on success.
 */
static int tascam_suspend(struct usb_interface *intf, pm_message_t message) {
  struct tascam_card *tascam = usb_get_intfdata(intf);

  if (!tascam)
    return 0;

  snd_pcm_suspend_all(tascam->pcm);

  cancel_work_sync(&tascam->stop_work);
  cancel_work_sync(&tascam->stop_pcm_work);
  usb_kill_anchored_urbs(&tascam->playback_anchor);
  usb_kill_anchored_urbs(&tascam->feedback_anchor);

  return 0;
}

/**
 * tascam_resume() - Handles device resumption from suspend.
 * @intf: The USB interface being resumed.
 *
 * This function is called when the device resumes from suspend. It
 * re-establishes the active USB interface settings and re-configures the sample
 * rate if it was previously active.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
static int tascam_resume(struct usb_interface *intf) {
  struct tascam_card *tascam = usb_get_intfdata(intf);
  int err;

  if (!tascam)
    return 0;

  dev_info(&intf->dev, "resuming TASCAM US-144MKII\n");

  /* Re-establish the active USB interface settings. */
  err = usb_set_interface(tascam->dev, 0, 1);
  if (err < 0) {
    dev_err(&intf->dev, "resume: failed to set alt setting on intf 0: %d\n",
            err);
    return err;
  }
  err = usb_set_interface(tascam->dev, 1, 1);
  if (err < 0) {
    dev_err(&intf->dev, "resume: failed to set alt setting on intf 1: %d\n",
            err);
    return err;
  }

  /* Re-configure the sample rate if one was previously active */
  if (tascam->current_rate > 0)
    us144mkii_configure_device_for_rate(tascam, tascam->current_rate);

  return 0;
}

static const struct usb_device_id tascam_usb_ids[] = {
    {USB_DEVICE(USB_VID_TASCAM, USB_PID_TASCAM_US144)},
    {USB_DEVICE(USB_VID_TASCAM, USB_PID_TASCAM_US144MKII)},
    {/* Terminating entry */}};
MODULE_DEVICE_TABLE(usb, tascam_usb_ids);

static struct usb_driver tascam_alsa_driver = {
    .name = DRIVER_NAME,
    .probe = tascam_probe,
    .disconnect = tascam_disconnect,
    .suspend = tascam_suspend,
    .resume = tascam_resume,
    .id_table = tascam_usb_ids,
};

module_usb_driver(tascam_alsa_driver);
