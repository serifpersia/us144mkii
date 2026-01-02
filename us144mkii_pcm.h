/* SPDX-License-Identifier: GPL-2.0-only */
// Copyright (c) 2025 Šerif Rami <ramiserifpersia@gmail.com>

#ifndef __US144MKII_PCM_H
#define __US144MKII_PCM_H

#include "us144mkii.h"

extern const struct snd_pcm_hardware tascam_playback_hw;
extern const struct snd_pcm_hardware tascam_capture_hw;

extern const struct snd_pcm_ops tascam_playback_ops;
extern const struct snd_pcm_ops tascam_capture_ops;

void playback_urb_complete(struct urb *urb);
void feedback_urb_complete(struct urb *urb);
void capture_urb_complete(struct urb *urb);
void capture_urb_complete_122(struct urb *urb);
int us144mkii_configure_device_for_rate(struct tascam_card *tascam, int rate);
void tascam_stop_pcm_work_handler(struct work_struct *work);
int tascam_pcm_hw_params(struct snd_pcm_substream *substream,
						 struct snd_pcm_hw_params *params);

#endif /* __US144MKII_PCM_H */
