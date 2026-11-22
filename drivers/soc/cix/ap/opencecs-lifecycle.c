// SPDX-License-Identifier: GPL-2.0-only
/* OpenCecs explicit emergency reset: PID1 authentication is supplied by the
 * independent lifecycle service. This kernel entry grants nothing to arbitrary
 * root processes/signals and deliberately performs no clean-shutdown claim.
 */
#include <linux/build_bug.h>
#include <linux/capability.h>
#include <linux/compat.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/pid_namespace.h>
#include <linux/reboot.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include "opencecs-lifecycle-request.h"

static long opencecs_lifecycle_ioctl(struct file *file, unsigned int command,
 unsigned long argument)
{
 struct opencecs_emergency_restart request;
 (void)file;
 if (command != OPENCECS_IOC_EMERGENCY_RESTART)
  return -ENOTTY;
 if (task_active_pid_ns(current) != &init_pid_ns ||
     task_pid_nr(current) != 1 || !capable(CAP_SYS_BOOT))
  return -EPERM;
 if (copy_from_user(&request, (const void __user *)argument, sizeof(request)))
  return -EFAULT;
 if (!opencecs_emergency_request_valid(&request))
  return -EINVAL;
 /* PID1 records the explicit unclean operation before entering this API.
  * Avoid taking a console lock in this last-resort reset path. */
 WRITE_ONCE(system_state, SYSTEM_RESTART);
 /* Unlike reboot(2), this intentionally does not acquire the global reboot
  * mutex or invoke blocking reboot notifiers/device_shutdown/syscore_shutdown.
  * ARM64 machine_restart still stops other CPUs and runs the existing atomic
  * PSCI reset handler, including the unchanged CIX bootloader reason mapping.
  * A pre-existing CPU/SMC/firmware hard lock may still require physical reset.
  */
 machine_restart(request.action == OPENCECS_EMERGENCY_FASTBOOT ? "bootloader" : NULL);
 return -EIO;
}
static const struct file_operations opencecs_lifecycle_fops = {
 .owner = THIS_MODULE,
 .open = nonseekable_open,
 .unlocked_ioctl = opencecs_lifecycle_ioctl,
#ifdef CONFIG_COMPAT
 .compat_ioctl = compat_ptr_ioctl,
#endif
 .llseek = no_llseek,
};
static struct miscdevice opencecs_lifecycle_device = {
 .minor = MISC_DYNAMIC_MINOR,
 .name = "opencecs-lifecycle",
 .fops = &opencecs_lifecycle_fops,
 .mode = 0600,
};
static int __init opencecs_lifecycle_init(void)
{
 BUILD_BUG_ON(sizeof(struct opencecs_emergency_restart) != 16);
 return misc_register(&opencecs_lifecycle_device);
}
device_initcall(opencecs_lifecycle_init);
MODULE_DESCRIPTION("OpenCecs PID1-only explicit emergency restart");
MODULE_LICENSE("GPL");
