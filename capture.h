/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __US144MKII_CAPTURE_H
#define __US144MKII_CAPTURE_H

#include "us144mkii.h"

void process_capture_routing_us144mkii(struct tascam_card *tascam, const s32 *decoded_block, s32 *routed_block);
void decode_tascam_capture_block(const u8 *src_block, s32 *dst_block);
void tascam_capture_work_handler(struct work_struct *work);
void capture_urb_complete(struct urb *urb);

#endif /* __US144MKII_CAPTURE_H */
