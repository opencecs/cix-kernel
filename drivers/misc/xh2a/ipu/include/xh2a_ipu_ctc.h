/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _XH2A_IPU_CTC_H_
#define _XH2A_IPU_CTC_H_

#include <linux/types.h>

struct xh2a_ipu_device;

#define XH2A_CTC_UNINIT_VAL 0xAAAA

enum xh2a_ipu_ctc_crg_reset_mode {
	XH2A_IPU_CTC_CRG_RESET_PHY_ONLY,
	XH2A_IPU_CTC_CRG_RESET_PHY_AND_CTRL,
};

int xh2a_ipu_ctc_phy_prepare(struct xh2a_ipu_device *ipu_dev,
			      enum xh2a_ipu_ctc_crg_reset_mode reset_mode);
int xh2a_ipu_ctc_phy_wait_linkup(struct xh2a_ipu_device *ipu_dev);
int xh2a_ipu_ctc_phy_init_linkup(struct xh2a_ipu_device *ipu_dev);
void xh2a_ipu_ctc_reinit_inhibit_get(struct xh2a_ipu_device *ipu_dev);
void xh2a_ipu_ctc_reinit_inhibit_put(struct xh2a_ipu_device *ipu_dev);
bool xh2a_ipu_ctc_reinit_inhibited(struct xh2a_ipu_device *ipu_dev);
void xh2a_ipu_ctc_shutdown_single(struct xh2a_ipu_device *ipu_dev);
void xh2a_ipu_ctc_pm_shutdown(struct xh2a_ipu_device *ipu_dev);
int xh2a_ipu_ctc_pm_startup(struct xh2a_ipu_device *ipu_dev);

#endif /* _XH2A_IPU_CTC_H_ */
