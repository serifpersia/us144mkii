#ifndef TASCAM_COMMON_H
#define TASCAM_COMMON_H

#include <linux/ioctl.h>

#define TASCAM_IOC_MAGIC 'T'
#define TASCAM_IOC_SET_RATE _IOW(TASCAM_IOC_MAGIC, 1, int)

#endif
