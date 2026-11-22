/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef OPENCECS_LIFECYCLE_REQUEST_H
#define OPENCECS_LIFECYCLE_REQUEST_H
#include <linux/opencecs_lifecycle.h>
/* Pure shape validation only. Never a caller/authentication/device grant. */
static inline int opencecs_emergency_request_valid(
 const struct opencecs_emergency_restart *request)
{
 return request && request->version == OPENCECS_EMERGENCY_VERSION &&
  (request->action == OPENCECS_EMERGENCY_RESTART ||
   request->action == OPENCECS_EMERGENCY_FASTBOOT) &&
  request->flags == OPENCECS_EMERGENCY_ACK_UNCLEAN && !request->reserved;
}
#endif
