#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/minmax.h>
#include "tascam_common.h"

#define DRIVER_NAME "tascam_raw"
#define TASCAM_VID 0x0644
#define TASCAM_PID_144 0x800f
#define TASCAM_PID_144MKII 0x8020

#define EP_AUDIO_OUT 0x02
#define EP_FEEDBACK_IN 0x81
#define EP_AUDIO_IN 0x86
#define EP_MIDI_IN 0x83
#define EP_MIDI_OUT 0x04

#define NUM_CHANNELS 4
#define BYTES_PER_SAMPLE 3
#define TASCAM_FRAME_SIZE (NUM_CHANNELS * BYTES_PER_SAMPLE)
#define PACKETS_PER_URB 8
#define NUM_PLAYBACK_URBS 2
#define NUM_FEEDBACK_URBS 2
#define NUM_CAPTURE_URBS 4
#define CAPTURE_URB_SIZE 512
#define RING_BUFFER_SIZE (128 * 1024)

#define NUM_MIDI_IN_URBS 4
#define NUM_MIDI_OUT_URBS 4
#define MIDI_PACKET_SIZE 9
#define MIDI_BUF_SIZE 4096

#define FEEDBACK_ACCUMULATOR_SIZE 128
#define FEEDBACK_SYNC_LOSS_THRESHOLD 41

struct us144mkii_frame_pattern_observer {
    unsigned int sample_rate_khz;
    unsigned int base_feedback_value;
    int feedback_offset;
    unsigned int full_frame_patterns[5][8];
    unsigned int current_index;
    unsigned int previous_index;
    bool sync_locked;
};

struct tascam_raw {
    struct usb_device *udev;

    struct urb *playback_urbs[NUM_PLAYBACK_URBS];
    struct urb *feedback_urbs[NUM_FEEDBACK_URBS];
    struct urb *capture_urbs[NUM_CAPTURE_URBS];

    struct urb *midi_in_urbs[NUM_MIDI_IN_URBS];
    struct urb *midi_out_urbs[NUM_MIDI_OUT_URBS];

    int major;
    struct cdev cdev;
    struct class *cls;

    unsigned char *pb_ring_buf;
    unsigned int pb_head;
    unsigned int pb_tail;
    unsigned char *cap_ring_buf;
    unsigned int cap_head;
    unsigned int cap_tail;

    unsigned char *midi_in_buf;
    unsigned int midi_in_head;
    unsigned int midi_in_tail;

    unsigned char *midi_out_buf;
    unsigned int midi_out_head;
    unsigned int midi_out_tail;
    bool midi_out_busy[NUM_MIDI_OUT_URBS];

    spinlock_t lock;
    wait_queue_head_t write_wait;
    wait_queue_head_t read_wait;

    bool running;
    int current_rate;

    unsigned int feedback_accumulator_pattern[FEEDBACK_ACCUMULATOR_SIZE];
    unsigned int feedback_pattern_out_idx;
    unsigned int feedback_pattern_in_idx;
    bool feedback_synced;
    unsigned int feedback_consecutive_errors;
    unsigned int feedback_urb_skip_count;
    struct us144mkii_frame_pattern_observer fpo;
};

static struct tascam_raw *g_dev = NULL;

static void fpo_init_pattern(unsigned int size, unsigned int *pattern_array,
                             unsigned int initial_value, int target_sum)
{
    int diff, i;
    if (!size) return;
    for (i = 0; i < size; ++i) pattern_array[i] = initial_value;
    diff = target_sum - (size * initial_value);
    for (i = 0; i < abs(diff); ++i) {
        if (diff > 0) pattern_array[i]++; else pattern_array[i]--;
    }
}

static void try_send_midi(struct tascam_raw *dev) {
    int i;
    unsigned long flags;
    spin_lock_irqsave(&dev->lock, flags);

    for (i = 0; i < NUM_MIDI_OUT_URBS; i++) {
        if (dev->midi_out_busy[i]) continue;

        int available = (dev->midi_out_head - dev->midi_out_tail) & (MIDI_BUF_SIZE - 1);
        if (available >= MIDI_PACKET_SIZE) {
            int tail = dev->midi_out_tail;
            int j;
            for(j=0; j<MIDI_PACKET_SIZE; j++) {
                ((u8*)dev->midi_out_urbs[i]->transfer_buffer)[j] = dev->midi_out_buf[(tail + j) & (MIDI_BUF_SIZE - 1)];
            }
            dev->midi_out_tail = (tail + MIDI_PACKET_SIZE) & (MIDI_BUF_SIZE - 1);
            dev->midi_out_busy[i] = true;

            usb_submit_urb(dev->midi_out_urbs[i], GFP_ATOMIC);
        }
    }
    spin_unlock_irqrestore(&dev->lock, flags);
}

static void midi_out_complete(struct urb *urb) {
    struct tascam_raw *dev = urb->context;
    int i;
    for(i=0; i<NUM_MIDI_OUT_URBS; i++) {
        if (dev->midi_out_urbs[i] == urb) {
            dev->midi_out_busy[i] = false;
            break;
        }
    }
    if (dev->running) try_send_midi(dev);
}

static void midi_in_complete(struct urb *urb) {
    struct tascam_raw *dev = urb->context;
    unsigned long flags;
    if (!dev->running) return;

    if (urb->status == 0 && urb->actual_length > 0) {
        spin_lock_irqsave(&dev->lock, flags);
        int available = (MIDI_BUF_SIZE - 1) - ((dev->midi_in_head - dev->midi_in_tail) & (MIDI_BUF_SIZE - 1));
        if (available >= urb->actual_length) {
            int i;
            u8 *src = urb->transfer_buffer;
            for(i=0; i<urb->actual_length; i++) {
                dev->midi_in_buf[dev->midi_in_head] = src[i];
                dev->midi_in_head = (dev->midi_in_head + 1) & (MIDI_BUF_SIZE - 1);
            }
        }
        spin_unlock_irqrestore(&dev->lock, flags);
        wake_up_interruptible(&dev->read_wait);
    }
    usb_submit_urb(urb, GFP_ATOMIC);
}

static void capture_complete(struct urb *urb) {
    struct tascam_raw *dev = urb->context;
    unsigned long flags;
    if (!dev->running) return;

    if (urb->status == 0 && urb->actual_length > 0) {
        spin_lock_irqsave(&dev->lock, flags);
        int available = (RING_BUFFER_SIZE - 1) - ((dev->cap_head - dev->cap_tail) & (RING_BUFFER_SIZE - 1));
        if (available >= urb->actual_length) {
            int chunk1 = min_t(int, urb->actual_length, RING_BUFFER_SIZE - dev->cap_head);
            memcpy(dev->cap_ring_buf + dev->cap_head, urb->transfer_buffer, chunk1);
            if (chunk1 < urb->actual_length) {
                memcpy(dev->cap_ring_buf, urb->transfer_buffer + chunk1, urb->actual_length - chunk1);
            }
            dev->cap_head = (dev->cap_head + urb->actual_length) & (RING_BUFFER_SIZE - 1);
        }
        spin_unlock_irqrestore(&dev->lock, flags);
        wake_up_interruptible(&dev->read_wait);
    }
    usb_submit_urb(urb, GFP_ATOMIC);
}

static void feedback_complete(struct urb *urb) {
    struct tascam_raw *dev = urb->context;
    int p;
    if (!dev->running) return;
    spin_lock(&dev->lock);
    if (dev->feedback_urb_skip_count > 0) {
        dev->feedback_urb_skip_count--;
        spin_unlock(&dev->lock);
        goto resubmit;
    }
    for (p = 0; p < urb->number_of_packets; p++) {
        u8 feedback_value = 0;
        const unsigned int *pattern;
        bool packet_ok = (urb->iso_frame_desc[p].status == 0 && urb->iso_frame_desc[p].actual_length >= 1);
        if (packet_ok) feedback_value = *((u8 *)urb->transfer_buffer + urb->iso_frame_desc[p].offset);
        if (packet_ok) {
            int delta = feedback_value - dev->fpo.base_feedback_value + dev->fpo.feedback_offset;
            int pattern_idx = (delta < 0) ? 0 : (delta >= 5 ? 4 : delta);
            pattern = dev->fpo.full_frame_patterns[pattern_idx];
            dev->feedback_consecutive_errors = 0;
            int i;
            for (i = 0; i < 8; i++) {
                unsigned int in_idx = (dev->feedback_pattern_in_idx + i) % FEEDBACK_ACCUMULATOR_SIZE;
                dev->feedback_accumulator_pattern[in_idx] = pattern[i];
            }
        } else {
            unsigned int nominal_frames = dev->current_rate / 8000;
            int i;
            if (dev->feedback_synced) {
                dev->feedback_consecutive_errors++;
                if (dev->feedback_consecutive_errors > FEEDBACK_SYNC_LOSS_THRESHOLD) dev->feedback_synced = false;
            }
            for (i = 0; i < 8; i++) {
                unsigned int in_idx = (dev->feedback_pattern_in_idx + i) % FEEDBACK_ACCUMULATOR_SIZE;
                dev->feedback_accumulator_pattern[in_idx] = nominal_frames;
            }
        }
        dev->feedback_pattern_in_idx = (dev->feedback_pattern_in_idx + 8) % FEEDBACK_ACCUMULATOR_SIZE;
    }
    if (!dev->feedback_synced) {
        unsigned int out_idx = dev->feedback_pattern_out_idx;
        unsigned int new_in_idx = dev->feedback_pattern_in_idx;
        bool is_ahead = (new_in_idx - out_idx + FEEDBACK_ACCUMULATOR_SIZE) % FEEDBACK_ACCUMULATOR_SIZE < (FEEDBACK_ACCUMULATOR_SIZE / 2);
        if (is_ahead) dev->feedback_synced = true;
    }
    spin_unlock(&dev->lock);
    resubmit:
    usb_submit_urb(urb, GFP_ATOMIC);
}

static void playback_complete(struct urb *urb) {
    struct tascam_raw *dev = urb->context;
    unsigned long flags;
    int i, total_bytes_for_urb = 0;
    if (!dev->running) return;
    spin_lock_irqsave(&dev->lock, flags);
    for (i = 0; i < urb->number_of_packets; i++) {
        unsigned int frames_for_packet;
        if (dev->feedback_synced) {
            frames_for_packet = dev->feedback_accumulator_pattern[dev->feedback_pattern_out_idx];
            dev->feedback_pattern_out_idx = (dev->feedback_pattern_out_idx + 1) % FEEDBACK_ACCUMULATOR_SIZE;
        } else {
            frames_for_packet = dev->current_rate / 8000;
        }
        size_t bytes_for_packet = frames_for_packet * TASCAM_FRAME_SIZE;
        urb->iso_frame_desc[i].offset = total_bytes_for_urb;
        urb->iso_frame_desc[i].length = bytes_for_packet;
        total_bytes_for_urb += bytes_for_packet;
    }
    urb->transfer_buffer_length = total_bytes_for_urb;
    uint8_t *dst_buf = urb->transfer_buffer;
    int available = (dev->pb_head - dev->pb_tail) & (RING_BUFFER_SIZE - 1);
    if (available >= total_bytes_for_urb) {
        int chunk1 = min_t(int, total_bytes_for_urb, RING_BUFFER_SIZE - dev->pb_tail);
        memcpy(dst_buf, dev->pb_ring_buf + dev->pb_tail, chunk1);
        if (chunk1 < total_bytes_for_urb) memcpy(dst_buf + chunk1, dev->pb_ring_buf, total_bytes_for_urb - chunk1);
        dev->pb_tail = (dev->pb_tail + total_bytes_for_urb) & (RING_BUFFER_SIZE - 1);
    } else {
        memset(dst_buf, 0, total_bytes_for_urb);
    }
    spin_unlock_irqrestore(&dev->lock, flags);
    wake_up_interruptible(&dev->write_wait);
    usb_submit_urb(urb, GFP_ATOMIC);
}

struct rate_config { int rate; u8 payload[3]; u16 reg_value; };
static const struct rate_config rates[] = {
    { 44100, {0x44, 0xac, 0x00}, 0x1000 },
    { 48000, {0x80, 0xbb, 0x00}, 0x1002 },
    { 88200, {0x88, 0x58, 0x01}, 0x1008 },
    { 96000, {0x00, 0x77, 0x01}, 0x100a },
};

static int tascam_start_stream(struct tascam_raw *dev, int rate) {
    int i;
    const struct rate_config *cfg = NULL;
    for (i = 0; i < 4; i++) { if (rates[i].rate == rate) { cfg = &rates[i]; break; } }
    if (!cfg) return -EINVAL;

    dev->running = false;
    for (i=0; i<NUM_PLAYBACK_URBS; i++) usb_kill_urb(dev->playback_urbs[i]);
    for (i=0; i<NUM_FEEDBACK_URBS; i++) usb_kill_urb(dev->feedback_urbs[i]);
    for (i=0; i<NUM_CAPTURE_URBS; i++) usb_kill_urb(dev->capture_urbs[i]);
    for (i=0; i<NUM_MIDI_IN_URBS; i++) usb_kill_urb(dev->midi_in_urbs[i]);
    for (i=0; i<NUM_MIDI_OUT_URBS; i++) usb_kill_urb(dev->midi_out_urbs[i]);

    dev->current_rate = rate;
    dev->pb_head = dev->pb_tail = 0;
    dev->cap_head = dev->cap_tail = 0;
    dev->midi_in_head = dev->midi_in_tail = 0;
    dev->midi_out_head = dev->midi_out_tail = 0;
    for(i=0; i<NUM_MIDI_OUT_URBS; i++) dev->midi_out_busy[i] = false;

    dev->fpo.sample_rate_khz = rate / 1000;
    dev->fpo.base_feedback_value = dev->fpo.sample_rate_khz;
    dev->fpo.feedback_offset = 2;
    dev->fpo.current_index = 0;
    dev->fpo.previous_index = 0;
    dev->fpo.sync_locked = false;
    unsigned int initial_value = dev->fpo.sample_rate_khz / 8;
    for (i = 0; i < 5; i++) {
        int target_sum = dev->fpo.sample_rate_khz - dev->fpo.feedback_offset + i;
        fpo_init_pattern(8, dev->fpo.full_frame_patterns[i], initial_value, target_sum);
    }
    dev->feedback_pattern_in_idx = 0;
    dev->feedback_pattern_out_idx = 0;
    dev->feedback_synced = false;
    dev->feedback_consecutive_errors = 0;
    dev->feedback_urb_skip_count = NUM_FEEDBACK_URBS;
    unsigned int nominal = rate / 8000;
    for (i = 0; i < FEEDBACK_ACCUMULATOR_SIZE; i++) dev->feedback_accumulator_pattern[i] = nominal;

    usb_set_interface(dev->udev, 0, 1);
    usb_set_interface(dev->udev, 1, 1);

    u8 *buf = kmalloc(64, GFP_KERNEL);
    usb_control_msg(dev->udev, usb_rcvctrlpipe(dev->udev, 0), 0x49, 0xc0, 0x0000, 0x0000, buf, 1, 1000);
    usb_control_msg(dev->udev, usb_sndctrlpipe(dev->udev, 0), 0x49, 0x40, 0x0010, 0x0000, NULL, 0, 1000);

    u8 *rate_payload = kmemdup(cfg->payload, 3, GFP_KERNEL);
    usb_control_msg(dev->udev, usb_sndctrlpipe(dev->udev, 0), 0x01, 0x22, 0x0100, EP_AUDIO_OUT, rate_payload, 3, 1000);
    usb_control_msg(dev->udev, usb_sndctrlpipe(dev->udev, 0), 0x01, 0x22, 0x0100, EP_FEEDBACK_IN, rate_payload, 3, 1000);
    usb_control_msg(dev->udev, usb_sndctrlpipe(dev->udev, 0), 0x01, 0x22, 0x0100, EP_AUDIO_IN, rate_payload, 3, 1000);
    kfree(rate_payload);

    usb_control_msg(dev->udev, usb_sndctrlpipe(dev->udev, 0), 0x41, 0x40, 0x0d04, 0x0101, NULL, 0, 1000);
    usb_control_msg(dev->udev, usb_sndctrlpipe(dev->udev, 0), 0x41, 0x40, 0x0e00, 0x0101, NULL, 0, 1000);
    usb_control_msg(dev->udev, usb_sndctrlpipe(dev->udev, 0), 0x41, 0x40, 0x0f00, 0x0101, NULL, 0, 1000);
    usb_control_msg(dev->udev, usb_sndctrlpipe(dev->udev, 0), 0x41, 0x40, cfg->reg_value, 0x0101, NULL, 0, 1000);
    usb_control_msg(dev->udev, usb_sndctrlpipe(dev->udev, 0), 0x41, 0x40, 0x110b, 0x0101, NULL, 0, 1000);
    usb_control_msg(dev->udev, usb_sndctrlpipe(dev->udev, 0), 0x49, 0x40, 0x0030, 0x0000, NULL, 0, 1000);
    kfree(buf);

    dev->running = true;

    for (i=0; i<NUM_FEEDBACK_URBS; i++) {
        dev->feedback_urbs[i]->dev = dev->udev;
        dev->feedback_urbs[i]->number_of_packets = 1;
        dev->feedback_urbs[i]->transfer_buffer_length = 3;
        dev->feedback_urbs[i]->iso_frame_desc[0].offset = 0;
        dev->feedback_urbs[i]->iso_frame_desc[0].length = 3;
        usb_submit_urb(dev->feedback_urbs[i], GFP_KERNEL);
    }
    for (i=0; i<NUM_CAPTURE_URBS; i++) {
        dev->capture_urbs[i]->dev = dev->udev;
        usb_submit_urb(dev->capture_urbs[i], GFP_KERNEL);
    }
    for (i=0; i<NUM_MIDI_IN_URBS; i++) {
        dev->midi_in_urbs[i]->dev = dev->udev;
        usb_submit_urb(dev->midi_in_urbs[i], GFP_KERNEL);
    }
    for (i=0; i<NUM_PLAYBACK_URBS; i++) {
        dev->playback_urbs[i]->dev = dev->udev;
        int j, total = 0;
        for(j=0; j<PACKETS_PER_URB; j++) {
            int len = nominal * TASCAM_FRAME_SIZE;
            dev->playback_urbs[i]->iso_frame_desc[j].offset = total;
            dev->playback_urbs[i]->iso_frame_desc[j].length = len;
            total += len;
        }
        dev->playback_urbs[i]->transfer_buffer_length = total;
        memset(dev->playback_urbs[i]->transfer_buffer, 0, total);
        usb_submit_urb(dev->playback_urbs[i], GFP_KERNEL);
    }

    printk(KERN_INFO "Tascam Raw: Started stream at %d Hz\n", rate);
    return 0;
}

static int dev_open(struct inode *inode, struct file *file) {
    if (!g_dev) return -ENODEV;
    file->private_data = g_dev;
    return 0;
}

static long dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    struct tascam_raw *dev = file->private_data;
    int new_rate;
    if (iminor(file_inode(file)) != 0) return -ENOTTY;
    switch (cmd) {
        case TASCAM_IOC_SET_RATE:
            if (copy_from_user(&new_rate, (int __user *)arg, sizeof(int))) return -EFAULT;
            return tascam_start_stream(dev, new_rate);
        default: return -ENOTTY;
    }
}

static ssize_t dev_write(struct file *file, const char __user *user_buf, size_t count, loff_t *ppos) {
    struct tascam_raw *dev = file->private_data;
    int written = 0;
    unsigned long flags;
    if (!dev->running) return -EIO;

    int minor = iminor(file_inode(file));

    if (minor == 1) {
        spin_lock_irqsave(&dev->lock, flags);
        int available = (MIDI_BUF_SIZE - 1) - ((dev->midi_out_head - dev->midi_out_tail) & (MIDI_BUF_SIZE - 1));
        int to_copy = min_t(int, count, available);
        int chunk1 = min_t(int, to_copy, MIDI_BUF_SIZE - dev->midi_out_head);

        if (copy_from_user(dev->midi_out_buf + dev->midi_out_head, user_buf, chunk1)) {
            spin_unlock_irqrestore(&dev->lock, flags); return -EFAULT;
        }
        if (chunk1 < to_copy) {
            if (copy_from_user(dev->midi_out_buf, user_buf + chunk1, to_copy - chunk1)) {
                spin_unlock_irqrestore(&dev->lock, flags); return -EFAULT;
            }
        }
        dev->midi_out_head = (dev->midi_out_head + to_copy) & (MIDI_BUF_SIZE - 1);
        spin_unlock_irqrestore(&dev->lock, flags);

        try_send_midi(dev);
        return to_copy;
    }

    while (written < count) {
        spin_lock_irqsave(&dev->lock, flags);
        int available = (RING_BUFFER_SIZE - 1) - ((dev->pb_head - dev->pb_tail) & (RING_BUFFER_SIZE - 1));
        spin_unlock_irqrestore(&dev->lock, flags);

        if (available == 0) {
            if (file->f_flags & O_NONBLOCK) return written > 0 ? written : -EAGAIN;
            if (wait_event_interruptible(dev->write_wait,
                ((RING_BUFFER_SIZE - 1) - ((dev->pb_head - dev->pb_tail) & (RING_BUFFER_SIZE - 1))) > 0))
                return -ERESTARTSYS;
            continue;
        }

        int to_copy = min_t(int, count - written, available);
        int chunk1 = min_t(int, to_copy, RING_BUFFER_SIZE - dev->pb_head);

        if (copy_from_user(dev->pb_ring_buf + dev->pb_head, user_buf + written, chunk1)) return -EFAULT;
        if (chunk1 < to_copy) {
            if (copy_from_user(dev->pb_ring_buf, user_buf + written + chunk1, to_copy - chunk1)) return -EFAULT;
        }

        spin_lock_irqsave(&dev->lock, flags);
        dev->pb_head = (dev->pb_head + to_copy) & (RING_BUFFER_SIZE - 1);
        spin_unlock_irqrestore(&dev->lock, flags);
        written += to_copy;
    }
    return written;
}

static ssize_t dev_read(struct file *file, char __user *user_buf, size_t count, loff_t *ppos) {
    struct tascam_raw *dev = file->private_data;
    int read = 0;
    unsigned long flags;
    if (!dev->running) return -EIO;

    int minor = iminor(file_inode(file));

    if (minor == 1) {
        spin_lock_irqsave(&dev->lock, flags);
        int available = (dev->midi_in_head - dev->midi_in_tail) & (MIDI_BUF_SIZE - 1);
        if (available == 0) {
            spin_unlock_irqrestore(&dev->lock, flags);
            if (file->f_flags & O_NONBLOCK) return -EAGAIN;
            return 0;
        }
        int to_copy = min_t(int, count, available);
        int chunk1 = min_t(int, to_copy, MIDI_BUF_SIZE - dev->midi_in_tail);

        if (copy_to_user(user_buf, dev->midi_in_buf + dev->midi_in_tail, chunk1)) {
            spin_unlock_irqrestore(&dev->lock, flags); return -EFAULT;
        }
        if (chunk1 < to_copy) {
            if (copy_to_user(user_buf + chunk1, dev->midi_in_buf, to_copy - chunk1)) {
                spin_unlock_irqrestore(&dev->lock, flags); return -EFAULT;
            }
        }
        dev->midi_in_tail = (dev->midi_in_tail + to_copy) & (MIDI_BUF_SIZE - 1);
        spin_unlock_irqrestore(&dev->lock, flags);
        return to_copy;
    }

    while (read < count) {
        spin_lock_irqsave(&dev->lock, flags);
        int available = (dev->cap_head - dev->cap_tail) & (RING_BUFFER_SIZE - 1);
        spin_unlock_irqrestore(&dev->lock, flags);

        if (available == 0) {
            if (file->f_flags & O_NONBLOCK) return read > 0 ? read : -EAGAIN;
            if (wait_event_interruptible(dev->read_wait,
                ((dev->cap_head - dev->cap_tail) & (RING_BUFFER_SIZE - 1)) > 0))
                return -ERESTARTSYS;
            continue;
        }

        int to_copy = min_t(int, count - read, available);
        int chunk1 = min_t(int, to_copy, RING_BUFFER_SIZE - dev->cap_tail);

        if (copy_to_user(user_buf + read, dev->cap_ring_buf + dev->cap_tail, chunk1)) return -EFAULT;
        if (chunk1 < to_copy) {
            if (copy_to_user(user_buf + read + chunk1, dev->cap_ring_buf, to_copy - chunk1)) return -EFAULT;
        }

        spin_lock_irqsave(&dev->lock, flags);
        dev->cap_tail = (dev->cap_tail + to_copy) & (RING_BUFFER_SIZE - 1);
        spin_unlock_irqrestore(&dev->lock, flags);
        read += to_copy;
    }
    return read;
}

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = dev_open,
    .write = dev_write,
    .read = dev_read,
    .unlocked_ioctl = dev_ioctl,
};

static int tascam_probe(struct usb_interface *intf, const struct usb_device_id *id) {
    struct tascam_raw *dev;
    int ret, i;

    if (intf->cur_altsetting->desc.bInterfaceNumber != 0) return -ENODEV;

    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev) return -ENOMEM;

    dev->udev = interface_to_usbdev(intf);
    dev->pb_ring_buf = kzalloc(RING_BUFFER_SIZE, GFP_KERNEL);
    dev->cap_ring_buf = kzalloc(RING_BUFFER_SIZE, GFP_KERNEL);
    dev->midi_in_buf = kzalloc(MIDI_BUF_SIZE, GFP_KERNEL);
    dev->midi_out_buf = kzalloc(MIDI_BUF_SIZE, GFP_KERNEL);
    spin_lock_init(&dev->lock);
    init_waitqueue_head(&dev->write_wait);
    init_waitqueue_head(&dev->read_wait);

    g_dev = dev;

    dev->major = register_chrdev(0, DRIVER_NAME, &fops);
    if (dev->major < 0) goto err_free;

    dev->cls = class_create(DRIVER_NAME);
    device_create(dev->cls, NULL, MKDEV(dev->major, 0), NULL, DRIVER_NAME);
    device_create(dev->cls, NULL, MKDEV(dev->major, 1), NULL, "tascam_midi");

    for (i=0; i<NUM_FEEDBACK_URBS; i++) {
        dev->feedback_urbs[i] = usb_alloc_urb(1, GFP_KERNEL);
        usb_fill_int_urb(dev->feedback_urbs[i], dev->udev, usb_rcvintpipe(dev->udev, EP_FEEDBACK_IN),
                         kzalloc(4, GFP_KERNEL), 4, feedback_complete, dev, 1);
    }
    for (i=0; i<NUM_PLAYBACK_URBS; i++) {
        dev->playback_urbs[i] = usb_alloc_urb(PACKETS_PER_URB, GFP_KERNEL);
        dev->playback_urbs[i]->dev = dev->udev;
        dev->playback_urbs[i]->context = dev;
        dev->playback_urbs[i]->pipe = usb_sndisocpipe(dev->udev, EP_AUDIO_OUT);
        dev->playback_urbs[i]->transfer_flags = URB_ISO_ASAP;
        dev->playback_urbs[i]->interval = 1;
        dev->playback_urbs[i]->complete = playback_complete;
        dev->playback_urbs[i]->number_of_packets = PACKETS_PER_URB;
        dev->playback_urbs[i]->transfer_buffer = kzalloc(PACKETS_PER_URB * 144, GFP_KERNEL);
        dev->playback_urbs[i]->transfer_buffer_length = PACKETS_PER_URB * 144;
    }
    for (i=0; i<NUM_CAPTURE_URBS; i++) {
        dev->capture_urbs[i] = usb_alloc_urb(0, GFP_KERNEL);
        dev->capture_urbs[i]->transfer_buffer = kzalloc(CAPTURE_URB_SIZE, GFP_KERNEL);
        usb_fill_bulk_urb(dev->capture_urbs[i], dev->udev, usb_rcvbulkpipe(dev->udev, EP_AUDIO_IN),
                          dev->capture_urbs[i]->transfer_buffer, CAPTURE_URB_SIZE, capture_complete, dev);
    }
    for (i=0; i<NUM_MIDI_IN_URBS; i++) {
        dev->midi_in_urbs[i] = usb_alloc_urb(0, GFP_KERNEL);
        dev->midi_in_urbs[i]->transfer_buffer = kzalloc(MIDI_PACKET_SIZE, GFP_KERNEL);
        usb_fill_bulk_urb(dev->midi_in_urbs[i], dev->udev, usb_rcvbulkpipe(dev->udev, EP_MIDI_IN),
                          dev->midi_in_urbs[i]->transfer_buffer, MIDI_PACKET_SIZE, midi_in_complete, dev);
    }
    for (i=0; i<NUM_MIDI_OUT_URBS; i++) {
        dev->midi_out_urbs[i] = usb_alloc_urb(0, GFP_KERNEL);
        dev->midi_out_urbs[i]->transfer_buffer = kzalloc(MIDI_PACKET_SIZE, GFP_KERNEL);
        usb_fill_bulk_urb(dev->midi_out_urbs[i], dev->udev, usb_sndbulkpipe(dev->udev, EP_MIDI_OUT),
                          dev->midi_out_urbs[i]->transfer_buffer, MIDI_PACKET_SIZE, midi_out_complete, dev);
    }

    printk(KERN_INFO "Tascam Raw: Probed. Waiting for IOCTL.\n");
    return 0;

    err_free:
    kfree(dev->pb_ring_buf);
    kfree(dev->cap_ring_buf);
    kfree(dev->midi_in_buf);
    kfree(dev->midi_out_buf);
    kfree(dev);
    return ret;
}

static void tascam_disconnect(struct usb_interface *intf) {
    struct tascam_raw *dev = g_dev;
    int i;
    if (!dev) return;

    dev->running = false;
    for (i=0; i<NUM_PLAYBACK_URBS; i++) { usb_kill_urb(dev->playback_urbs[i]); usb_free_urb(dev->playback_urbs[i]); }
    for (i=0; i<NUM_FEEDBACK_URBS; i++) { usb_kill_urb(dev->feedback_urbs[i]); usb_free_urb(dev->feedback_urbs[i]); }
    for (i=0; i<NUM_CAPTURE_URBS; i++) { usb_kill_urb(dev->capture_urbs[i]); usb_free_urb(dev->capture_urbs[i]); }
    for (i=0; i<NUM_MIDI_IN_URBS; i++) { usb_kill_urb(dev->midi_in_urbs[i]); usb_free_urb(dev->midi_in_urbs[i]); }
    for (i=0; i<NUM_MIDI_OUT_URBS; i++) { usb_kill_urb(dev->midi_out_urbs[i]); usb_free_urb(dev->midi_out_urbs[i]); }

    device_destroy(dev->cls, MKDEV(dev->major, 0));
    device_destroy(dev->cls, MKDEV(dev->major, 1));
    class_destroy(dev->cls);
    unregister_chrdev(dev->major, DRIVER_NAME);

    kfree(dev->pb_ring_buf);
    kfree(dev->cap_ring_buf);
    kfree(dev->midi_in_buf);
    kfree(dev->midi_out_buf);
    kfree(dev);
    g_dev = NULL;
}

static struct usb_device_id tascam_table[] = {
    { USB_DEVICE(TASCAM_VID, TASCAM_PID_144) },
    { USB_DEVICE(TASCAM_VID, TASCAM_PID_144MKII) },
    { }
};
MODULE_DEVICE_TABLE(usb, tascam_table);
static struct usb_driver tascam_driver = { .name = DRIVER_NAME, .probe = tascam_probe, .disconnect = tascam_disconnect, .id_table = tascam_table };
module_usb_driver(tascam_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TASCAM US-144/US-144MKII Raw Driver (Audio + MIDI)");
