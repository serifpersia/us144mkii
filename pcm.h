/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __US144MKII_PCM_H
#define __US144MKII_PCM_H

#include "us144mkii.h"

extern const struct snd_pcm_ops tascam_playback_ops;
extern const struct snd_pcm_ops tascam_capture_ops;

int tascam_create_pcm(struct tascam_card *tascam);
void tascam_free_urbs(struct tascam_card *tascam);
int tascam_alloc_urbs(struct tascam_card *tascam);
int us144mkii_configure_device_for_rate(struct tascam_card *tascam, int rate);
void tascam_stop_work_handler(struct work_struct *work);

#endif /* __US144MKII_PCM_H */
