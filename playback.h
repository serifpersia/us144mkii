/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __US144MKII_PLAYBACK_H
#define __US144MKII_PLAYBACK_H

#include "us144mkii.h"

void process_playback_routing_us144mkii(struct tascam_card *tascam, const u8 *src_buffer, u8 *dst_buffer, size_t frames);
void playback_urb_complete(struct urb *urb);
void feedback_urb_complete(struct urb *urb);

#endif /* __US144MKII_PLAYBACK_H */
