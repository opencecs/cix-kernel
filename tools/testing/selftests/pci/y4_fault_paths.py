#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Compile the actual Y4 C callbacks against fault-injecting host stubs.

This does not inject errors into hardware.  It checks error propagation,
callback ordering, AER multi-source/mask handling and IRQ ownership/unwind.
Run from any directory with Python 3 and a host C compiler.
"""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[4]


def block(path, begin, end):
    text = (ROOT / path).read_text()
    return text[text.index(begin):text.index(end, text.index(begin))]


COMMON = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint64_t u64;
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
struct device { int dummy; };
static void log_stub(const void *d, const char *s, ...) { (void)d; (void)s; }
#define dev_err log_stub
#define dev_info log_stub
#define dev_crit log_stub
#define pci_err log_stub
#define pci_info log_stub
#define pci_warn log_stub
"""

PM_STUB = r"""
struct pci_dev { struct device dev; bool master; void *data; };
struct xh2a_pcie_client {
    int work_num;
    struct { void *msi_work; } work[1];
    int (*runtime_suspend_cb)(void *);
    int (*runtime_resume_cb)(void *);
};
struct hdma_stub { int suspend_in_progress; int hdma_work; };
struct clients { struct xh2a_pcie_client *v; int n; };
struct xh2a_pcie_dev {
    bool runtime_pm_failed, pm_failed;
    int system_pm_phase, system_pm_error;
    int iatu_lock[1];
    struct pci_dev *pdev;
    struct clients client_list;
    struct hdma_stub hdma;
};
#define to_pci_dev(d) ((struct pci_dev *)(d))
#define pci_get_drvdata(p) ((p)->data)
#define list_for_each_entry(p,h,member) \
    for (int idx=0; idx<(h)->n && ((p)=&(h)->v[idx],1); idx++)
#define XH2A_LPCTRL_ALT_MODE_REG 1
#define XH2A_LPCTRL_ALT_MODE_NORMAL 2
#define XH2A_LPCTRL_START_SLEEP_ID 3
#define XH2A_LPCTRL_START_SLEEP_STR "sleep"
#define XH2A_LPCTRL_START_SLEEP_LEN 5
#define XH2A_LPSTS_E2_ENTER_LP 0x22
#define XH2A_LPSTS_U7_EXIT_LP 0x77
#define XH2A_LPCTRL_EXIT_IDLE_OR_L1_CODE 4
#define XH2A_PCIE_MSG_TO_E2 5
static unsigned long jiffies;
#define msecs_to_jiffies(m) (m)
#define time_before(a,b) ((long)((a)-(b))<0)
static void usleep_range(int a,int b) { (void)a; (void)b; ++jiffies; }
static u32 states[8];
static int reads, read_fail_at, read_error, state_count;
static int suspend_calls,resume_calls,client_fail_at,client_error;
static int alt_error,send_error,wake_error,sends,wakes,posts,stops;
static int check_pwrsts(struct xh2a_pcie_dev *x,u32 *s) {
    (void)x; ++reads;
    if(reads==read_fail_at) return read_error;
    *s=states[reads<=state_count?reads-1:state_count-1]; return 0;
}
#define xh2a_pcie_check_pwrsts check_pwrsts
static int xh2a_pcie_pio_writel(void *x,int a,int b) {
    (void)x;(void)a;(void)b; return alt_error;
}
static int xh2a_rpmsg_lite_ext_send(void *x,int a,const char*b,int c) {
    (void)x;(void)a;(void)b;(void)c; ++sends; return send_error;
}
static int xh2a_pcie_write_bar_msgbit(void*x,int a,int b) {
    (void)x;(void)a;(void)b; ++wakes; return wake_error;
}
static int post_error;
static int xh2a_pcie_probe_post(void*x) {(void)x; ++posts; return post_error;}
static int xh2a_pcie_hdma_stop(struct hdma_stub*x) {(void)x; ++stops; return 0;}
static void xh2a_pcie_hdma_wake_waiters_locked(struct hdma_stub*x) {(void)x;}
static void xh2a_pcie_pm_mask_irqs(struct xh2a_pcie_dev*x) {(void)x;}
#define XH2A_PCIE_REGBAR 0
#define XH2A_SYSTEM_PM_FAILED 1
#define READ_ONCE(x) (x)
#define atomic_set(p,v) (*(p)=(v))
#define spin_lock_irqsave(p,f) ((void)(p),(f)=0)
#define spin_unlock_irqrestore(p,f) ((void)(p),(void)(f))
#define cancel_work_sync(p) ((void)(p))
static void pci_clear_master(struct pci_dev*p) {p->master=false;}
static int suspend_client(void*x) {
    (void)x; return ++suspend_calls==client_fail_at?client_error:0;
}
static int resume_client(void*x) {
    (void)x; return ++resume_calls==client_fail_at?client_error:0;
}
"""
PM_TEST = r"""
static struct pci_dev p;
static struct xh2a_pcie_dev x;
static struct xh2a_pcie_client c[3];
static void reset(u32 state) {
    memset(&p,0,sizeof(p));memset(&x,0,sizeof(x));memset(c,0,sizeof(c));
    x.pdev=&p;p.data=&x;p.master=true;
    for(int i=0;i<2;i++) { c[i].runtime_suspend_cb=suspend_client;
        c[i].runtime_resume_cb=resume_client; }
    x.client_list=(struct clients){c,3};
    states[0]=state;state_count=1;jiffies=0;
    reads=read_fail_at=suspend_calls=resume_calls=client_fail_at=0;
    read_error=-EREMOTEIO;client_error=-EINVAL;
    alt_error=send_error=wake_error=post_error=sends=wakes=posts=stops=0;
}
static void failed(int ret,int expected) {
    assert(ret==expected);assert(x.runtime_pm_failed);assert(!p.master);
    assert(stops==1);
    int old_reads=reads;
    assert(xh2a_pcie_pm_runtime_resume(&p.dev)==-EIO);
    assert(reads==old_reads);
}
int main(void) {
    reset(XH2A_LPSTS_E2_ENTER_LP);
    assert(xh2a_pcie_pm_runtime_suspend(&p.dev)==0);
    assert(suspend_calls==2 && sends==1 && !x.runtime_pm_failed && p.master);
    reset(0);
    failed(xh2a_pcie_pm_runtime_suspend(&p.dev),-ETIMEDOUT);
    assert(jiffies==1000);
    reset(0);read_fail_at=1;
    failed(xh2a_pcie_pm_runtime_suspend(&p.dev),-EREMOTEIO);
    reset(0);alt_error=-EIO;
    failed(xh2a_pcie_pm_runtime_suspend(&p.dev),-EIO);assert(sends==0);
    reset(0);send_error=-ENOSPC;
    failed(xh2a_pcie_pm_runtime_suspend(&p.dev),-ENOSPC);assert(reads==0);
    for(int n=1;n<=2;n++) {
        reset(0);client_fail_at=n;
        failed(xh2a_pcie_pm_runtime_suspend(&p.dev),-EINVAL);
        assert(suspend_calls==n && sends==0);
    }
    reset(0);client_fail_at=1;client_error=-EBUSY;
    failed(xh2a_pcie_pm_runtime_suspend(&p.dev),-EIO);
    reset(XH2A_LPSTS_U7_EXIT_LP);
    assert(xh2a_pcie_pm_runtime_resume(&p.dev)==0);
    assert(posts==1 && resume_calls==2 && wakes==0);
    reset(XH2A_LPSTS_E2_ENTER_LP);
    states[1]=0;states[2]=XH2A_LPSTS_U7_EXIT_LP;state_count=3;
    assert(xh2a_pcie_pm_runtime_resume(&p.dev)==0);
    assert(posts==1 && resume_calls==2 && wakes==1 && reads==3);
    reset(0);
    failed(xh2a_pcie_pm_runtime_resume(&p.dev),-ETIMEDOUT);
    assert(posts==0 && resume_calls==0 && jiffies==2000);
    for(int n=1;n<=2;n++) {
        reset(0);read_fail_at=n;
        failed(xh2a_pcie_pm_runtime_resume(&p.dev),-EREMOTEIO);
        assert(posts==0 && resume_calls==0);
    }
    reset(XH2A_LPSTS_E2_ENTER_LP);wake_error=-EIO;
    failed(xh2a_pcie_pm_runtime_resume(&p.dev),-EIO);
    assert(reads==1 && posts==0);
    for(int n=1;n<=2;n++) {
        reset(XH2A_LPSTS_U7_EXIT_LP);client_fail_at=n;
        failed(xh2a_pcie_pm_runtime_resume(&p.dev),-EINVAL);
        assert(resume_calls==n);
    }
    reset(XH2A_LPSTS_U7_EXIT_LP);post_error=-EIO;
    failed(xh2a_pcie_pm_runtime_resume(&p.dev),-EIO);
    assert(posts==1 && resume_calls==0 && x.pm_failed);
    reset(0);p.data=NULL;
    assert(xh2a_pcie_pm_runtime_suspend(&p.dev)==-ENODEV);
    assert(xh2a_pcie_pm_runtime_resume(&p.dev)==-ENODEV);
    puts("PM fault/success cases passed");
    return 0;
}
"""

AER_STUB = r"""
#include "include/uapi/linux/pci_regs.h"
#include <setjmp.h>
#define __iomem
#define CDNS_PCIE_RP_BASE 0
#define HZ 100
#define time_before(a,b) ((long)((a)-(b))<0)
static unsigned long jiffies;
#define IRQ_NONE 0
#define IRQ_HANDLED 1
#define IRQF_SHARED 1
#define IRQF_ONESHOT 2
typedef int irqreturn_t;
struct pci_dev {
    struct device dev; u16 aer_cap; int type; u32 cfg[1024];
    bool master,disconnected; int read_error;
};
struct pci_bus { struct pci_dev *devices; int n; };
struct pci_host_bridge { struct pci_bus *bus; };
struct sky1_aer_stats {
    u64 rp_total_cor_errs,rp_total_fatal_errs,rp_total_nonfatal_errs;
    u64 ep_total_cor_errs,ep_total_fatal_errs,ep_total_nonfatal_errs;
};
struct sky1_pcie {
    bool aer_log_ce_ratelimited;
    unsigned long aer_ce_last_log;
    unsigned int aer_ce_suppressed;
    struct device *dev; void *reg_base;
    struct pci_host_bridge *cdns_pcie_rc;
    struct sky1_aer_stats aer_stats;
    bool aer_uncor_dump,is_aer_uncor_panic,irqs_requested,aer_failed;
    int aer_lock,pme_work,local_irq,aer_c_irq,aer_f_irq,aer_nf_irq;
};
static int ack_devctl,link_disabled,requested[4],released[4],req_idx,fail_at,cancelled;
static jmp_buf panic_jump;
static int disabled[4];
static void disable_irq_nosync(int irq) {++disabled[irq];}
static const char *dev_name(struct device*d) {(void)d; return "test";}
static void panic(const char*s,...) {(void)s; longjmp(panic_jump,1);}
static void mutex_lock(int*p) {(void)p;}
static void mutex_unlock(int*p) {(void)p;}
static u16 cdns_pcie_find_ext_capability(void*b,int c) {(void)b;(void)c;return 0x100;}
static u16 cdns_pcie_find_capability(void*b,int c) {(void)b;(void)c;return 0x40;}
static u32 readl(void*p) {return *(u32*)p;}
static void *root_base;
static void writel(u32 v,void*p) {
    size_t off=(char*)p-(char*)root_base;
    if(off==0x100+PCI_ERR_ROOT_STATUS || off==0x100+PCI_ERR_COR_STATUS ||
       off==0x100+PCI_ERR_UNCOR_STATUS) *(u32*)p &= ~v;
    else *(u32*)p=v;
}
static void writew(u16 v,void*p) {*(u16*)p &= ~v;}
static struct pci_host_bridge *pci_host_bridge_from_priv(struct pci_host_bridge*p) {return p;}
static int pci_pcie_type(struct pci_dev*p) {return p->type;}
static int pci_read_config_dword(struct pci_dev*p,int off,u32*v) {
    *v=p->cfg[off/4];return p->read_error;
}
static int pci_write_config_dword(struct pci_dev*p,int off,u32 v) {
    assert(off!=0x40+PCI_EXP_DEVCTL);p->cfg[off/4] &= ~v;return 0;
}
static int pcie_capability_write_word(struct pci_dev*p,int off,u16 v) {
    assert(off==PCI_EXP_DEVSTA);
    u16 *q=(u16*)((char*)p->cfg+0x40+off);*q &= ~v;return 0;
}
static void pci_walk_bus(struct pci_bus*b,int(*fn)(struct pci_dev*,void*),void*d) {
    for(int i=0;i<b->n;i++) assert(fn(&b->devices[i],d)==0);
}
static void pci_clear_master(struct pci_dev*p) {p->master=false;}
static void pci_dev_set_disconnected(struct pci_dev*p, void*unused) {assert(!p->master);p->disconnected=true;}
static void sky1_pcie_set_link_disable(struct sky1_pcie*p) {(void)p;++link_disabled;}
static int request(int irq) {
    ++req_idx;if(req_idx==fail_at)return -EBUSY;
    assert(irq>=0 && irq<4);assert(!requested[irq]);requested[irq]=1;return 0;
}
#define devm_request_irq(d,i,h,f,n,p) request(i)
#define devm_request_threaded_irq(d,i,h,t,f,n,p) request(i)
static void devm_free_irq(struct device*d,int i,void*p) {
    (void)d;(void)p;assert(requested[i]);requested[i]=0;++released[i];
}
static void cancel_work_sync(int*w) {(void)w;++cancelled;}
"""
AER_TEST=r"""
static struct pci_dev ep[3];
static struct pci_bus bus;
static struct pci_host_bridge bridge;
static struct sky1_pcie sky;
static struct device dev;
static u32 root[1024];
#define REG(d,r) ((d).cfg[(0x100+(r))/4])
#define RREG(r) (root[(0x100+(r))/4])
static void reset(void) {
    memset(ep,0,sizeof(ep));memset(&sky,0,sizeof(sky));memset(root,0,sizeof(root));
    for(int i=0;i<3;i++) {ep[i].aer_cap=0x100;ep[i].master=true;}
    bus=(struct pci_bus){ep,3};bridge.bus=&bus;
    sky.dev=&dev;sky.cdns_pcie_rc=&bridge;sky.reg_base=root;root_base=root;
    sky.local_irq=0;sky.aer_c_irq=1;sky.aer_f_irq=2;sky.aer_nf_irq=3;
    memset(requested,0,sizeof(requested));memset(released,0,sizeof(released));memset(disabled,0,sizeof(disabled));
    req_idx=fail_at=cancelled=ack_devctl=link_disabled=0;
}
int main(void) {
    reset();
    sky.aer_log_ce_ratelimited = true;
    jiffies = 100;
    assert(sky1_aer_log_event(&sky, PCI_ERR_ROOT_COR_RCV));
    assert(!sky1_aer_log_event(&sky, PCI_ERR_ROOT_COR_RCV));
    assert(sky.aer_ce_suppressed == 1);
    assert(sky1_aer_log_event(&sky, PCI_ERR_ROOT_UNCOR_RCV | PCI_ERR_ROOT_COR_RCV));
    assert(sky1_aer_log_event(&sky, 0));
    jiffies += 5 * HZ;
    assert(sky1_aer_log_event(&sky, PCI_ERR_ROOT_COR_RCV));
    assert(!sky.aer_ce_suppressed);
    reset();
    REG(ep[0],PCI_ERR_COR_STATUS)=PCI_ERR_COR_RCVR;
    REG(ep[1],PCI_ERR_COR_STATUS)=PCI_ERR_COR_BAD_TLP;
    REG(ep[2],PCI_ERR_COR_STATUS)=PCI_ERR_COR_ADV_NFAT;
    REG(ep[2],PCI_ERR_COR_MASK)=PCI_ERR_COR_ADV_NFAT;
    root[(0x40+PCI_EXP_DEVCTL)/4]=0x1120;
    RREG(PCI_ERR_ROOT_STATUS)=PCI_ERR_ROOT_COR_RCV|PCI_ERR_ROOT_MULTI_COR_RCV;
    RREG(PCI_ERR_ROOT_ERR_SRC)=0xffff; /* nonexistent RID still scans all */
    assert(sky1_pcie_aer_irq_handler(0,&sky)==IRQ_HANDLED);
    assert(sky.aer_stats.ep_total_cor_errs==2);
    assert(REG(ep[0],PCI_ERR_COR_STATUS)==0 && REG(ep[1],PCI_ERR_COR_STATUS)==0);
    assert(REG(ep[2],PCI_ERR_COR_STATUS)==PCI_ERR_COR_ADV_NFAT);
    assert(root[(0x40+PCI_EXP_DEVCTL)/4]==0x1120 && !sky.aer_failed);
    reset();
    REG(ep[0],PCI_ERR_UNCOR_STATUS)=PCI_ERR_UNC_COMP_TIME;
    REG(ep[1],PCI_ERR_UNCOR_STATUS)=PCI_ERR_UNC_MALF_TLP;
    REG(ep[1],PCI_ERR_UNCOR_SEVER)=PCI_ERR_UNC_MALF_TLP;
    REG(ep[2],PCI_ERR_UNCOR_STATUS)=PCI_ERR_UNC_UNSUP;
    REG(ep[2],PCI_ERR_UNCOR_MASK)=PCI_ERR_UNC_UNSUP;
    RREG(PCI_ERR_ROOT_STATUS)=PCI_ERR_ROOT_UNCOR_RCV|PCI_ERR_ROOT_MULTI_UNCOR_RCV;
    RREG(PCI_ERR_ROOT_ERR_SRC)=0xffff0000;
    sky.is_aer_uncor_panic=true;
    if(!setjmp(panic_jump)) {sky1_pcie_aer_irq_handler(0,&sky);assert(0);}
    assert(sky.aer_stats.ep_total_nonfatal_errs==1 && sky.aer_stats.ep_total_fatal_errs==1);
    assert(REG(ep[0],PCI_ERR_UNCOR_STATUS)==PCI_ERR_UNC_COMP_TIME);
    assert(REG(ep[1],PCI_ERR_UNCOR_STATUS)==PCI_ERR_UNC_MALF_TLP);
    reset();
    RREG(PCI_ERR_ROOT_STATUS)=PCI_ERR_ROOT_UNCOR_RCV;
    ep[0].read_error=-EIO;ep[1].cfg[(0x100+PCI_ERR_UNCOR_STATUS)/4]=~0U;
    assert(sky1_pcie_aer_irq_handler(0,&sky)==IRQ_HANDLED);
    assert(sky.aer_failed && link_disabled==1);
    assert(disabled[0]==0 && disabled[1]==1 && disabled[2]==1 && disabled[3]==1);
    assert(sky1_pcie_aer_irq_handler(0,&sky)==IRQ_HANDLED);
    assert(link_disabled==1 && disabled[1]==1);
    assert((RREG(PCI_ERR_ROOT_COMMAND)&7)==0);
    for(int i=0;i<3;i++) assert(ep[i].disconnected && !ep[i].master);
    reset();
    RREG(PCI_ERR_UNCOR_STATUS)=PCI_ERR_UNC_MALF_TLP;
    RREG(PCI_ERR_UNCOR_SEVER)=PCI_ERR_UNC_MALF_TLP;
    sky1_pcie_aer_irq_handler(0,&sky);
    assert(sky.aer_stats.rp_total_fatal_errs==1 && sky.aer_failed);
    reset();
    RREG(PCI_ERR_UNCOR_STATUS)=PCI_ERR_UNC_UNSUP;
    RREG(PCI_ERR_UNCOR_MASK)=PCI_ERR_UNC_UNSUP;
    sky1_pcie_aer_irq_handler(0,&sky);
    assert(!sky.aer_failed);
    reset();
    sky1_pcie_aer_root_command(&sky,true);
    assert((RREG(PCI_ERR_ROOT_COMMAND)&7)==7);
    sky1_pcie_aer_root_command(&sky,false);
    assert((RREG(PCI_ERR_ROOT_COMMAND)&7)==0);
    for(int n=1;n<=4;n++) {
        reset();fail_at=n;
        assert(sky1_pcie_request_irq(&sky)==-EBUSY);
        assert(req_idx==n && !sky.irqs_requested);
        for(int i=0;i<4;i++) {
            assert(!requested[i]);assert(released[i]==(i<n-1));
        }
        sky1_pcie_free_irq(&sky); /* retry cleanup must not free twice */
        for(int i=0;i<4;i++) assert(released[i]==(i<n-1));
        req_idx=0;fail_at=0;
        assert(sky1_pcie_request_irq(&sky)==0 && sky.irqs_requested);
        sky1_pcie_free_irq(&sky);assert(!sky.irqs_requested);
        for(int i=0;i<4;i++) assert(!requested[i]);
    }
    puts("AER/IRQ: multi-source, invalid RID, masks, unreadable source, panic, isolation and 4 IRQ failure positions passed");
    return 0;
}
"""


class FaultPaths(unittest.TestCase):
    def compile_run(self, name, code):
        with tempfile.TemporaryDirectory(prefix="y4-fault-") as tmp:
            source = pathlib.Path(tmp) / (name + ".c")
            executable = pathlib.Path(tmp) / name
            source.write_text(code)
            subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-parameter", "-Wno-sign-compare",
                            "-Wno-unused-function", "-I", str(ROOT),
                            str(source), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=10)

    def test_m50_runtime_pm(self):
        path = "drivers/misc/xh2a/pcie/xh2a_pcie.c"
        code = block(path, "static int xh2a_pcie_pm_error(",
                     "static int xh2a_pcie_wait_power_states(")
        code += block(path, "static int xh2a_pcie_runtime_pm_error(",
                     "static irqreturn_t xh2a_pcie_msi_handler_hdma(")
        self.compile_run("pm", COMMON + PM_STUB + code + PM_TEST)

    def test_sky1_aer_and_irq_unwind(self):
        code = block("drivers/pci/controller/cadence/pci-sky1.c",
                     "struct sky1_aer_event {",
                     "static void sky1_pcie_disable_irq(")
        self.compile_run("aer", COMMON + AER_STUB + code + AER_TEST)


if __name__ == "__main__":
    unittest.main(verbosity=2)
