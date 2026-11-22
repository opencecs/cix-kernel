#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Host regression tests for the actual bonding miimon state machine.

Run in the source tree with Python 3 and a host C compiler; no root, network
namespaces, interfaces or running kernel modules are changed. The C functions
and link-state helpers are extracted without rewriting them. Only external
kernel services are stubbed, so this checks state transitions and notification /
failover requests, not actual LACP, packet forwarding or real concurrent RTNL.

Covers upstream fixes 3348be7978f4 and a76ab5e733b4. --source permits a negative
control using an older bond_main.c; the matching helpers come from this tree.
"""
import argparse
import os
import pathlib
import shlex
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[4]


def block(text, begin, end):
    start = text.index(begin)
    return text[start:text.index(end, start)]


STUBS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define BOND_LINK_NOCHANGE -1
#define BOND_SLAVE_NOTIFY_NOW true
#define BOND_SLAVE_NOTIFY_LATER false
#define SPEED_UNKNOWN UINT_MAX
#define NETDEV_NOTIFY_PEERS 1
#define fallthrough __attribute__((fallthrough))
#define container_of(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
#define BOND_MODE(b) ((b)->params.mode)
#define rcu_dereference(p) (p)
#define rcu_access_pointer(p) (p)
#define rtnl_dereference(p) (p)
#define msecs_to_jiffies(n) (n)

struct bonding;
struct slave;
struct net_device { struct slave *owner; };
struct list_head { int unused; };
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; };
struct bond_up_slave { int count; };
struct slave {
    struct bonding *bond;
    struct slave *next;
    struct net_device *dev;
    int link, link_new_state, delay, should_notify_link;
    int prio, duplex, inactive;
    unsigned int speed, link_failure_count;
    unsigned long last_link_up;
    bool physical_up, speed_error;
    int speed_seen_link, speed_calls, queue_events, lower_events;
    int up_events, down_events, inactive_calls, up_calls, down_calls;
    int mode_callbacks, speed_rechecks;
};
struct bonding {
    struct net_device *dev;
    struct { int mode, miimon, updelay, downdelay; } params;
    struct slave *slaves, *curr_active_slave, *primary_slave;
    struct bond_up_slave *usable_slaves;
    struct delayed_work mii_work;
    void *wq;
    int send_peer_notif;
};
#define bond_for_each_slave(b,s,i) \
    for ((i)=NULL,(s)=(b)->slaves; (s); (s)=(s)->next)
#define bond_for_each_slave_rcu bond_for_each_slave

static unsigned long jiffies;
static bool lock_available, rtnl_locked;
static int rcu_depth, errors, warnings, debug_messages, failovers;
static int carrier_calls, queued_work, array_updates;
static unsigned long queued_delay;
static int failures, checks;

static void quiet_log(void *a, void *b, const char *fmt, ...) {
    (void)a; (void)b; (void)fmt;
}
#define slave_info quiet_log
#define slave_err(a,b,...) (errors++, quiet_log(a,b,__VA_ARGS__))
#define slave_warn(a,b,...) (warnings++, quiet_log(a,b,__VA_ARGS__))
#define slave_dbg(a,b,...) (debug_messages++, quiet_log(a,b,__VA_ARGS__))
#define ASSERT_RTNL() assert(rtnl_locked)
static void rcu_read_lock(void) { rcu_depth++; }
static void rcu_read_unlock(void) { assert(rcu_depth > 0); rcu_depth--; }
static bool rtnl_trylock(void) {
    if (!lock_available) return false;
    assert(!rtnl_locked);
    rtnl_locked = true;
    return true;
}
static void rtnl_unlock(void) { assert(rtnl_locked); rtnl_locked = false; }
static bool net_ratelimit(void) { return true; }
static bool bond_has_slaves(struct bonding *b) { return b->slaves != NULL; }
static bool bond_should_notify_peers(struct bonding *b) { (void)b; return false; }
static int bond_check_dev_link(struct bonding *b, struct net_device *d, int x) {
    (void)b; (void)x; return d->owner->physical_up;
}
static bool bond_is_active_slave(struct slave *s) {
    return s->bond->curr_active_slave == s;
}
static bool bond_is_lb(const struct bonding *b) {
    return BOND_MODE(b) == BOND_MODE_TLB || BOND_MODE(b) == BOND_MODE_ALB;
}
static void bond_queue_slave_event(struct slave *s) { s->queue_events++; }
static void bond_lower_state_changed(struct slave *s) {
    s->lower_events++;
    if (s->link == BOND_LINK_UP) s->up_events++;
    if (s->link == BOND_LINK_DOWN) s->down_events++;
}
static int bond_update_speed_duplex(struct slave *s) {
    s->speed_seen_link = s->link;
    s->speed_calls++;
    return s->speed_error ? -1 : 0;
}
static void record_link_change(struct slave *s, char link) {
    s->mode_callbacks++;
    if (link == BOND_LINK_UP) s->up_calls++;
    if (link == BOND_LINK_DOWN) s->down_calls++;
}
static void bond_3ad_handle_link_change(struct slave *s, char link) {
    record_link_change(s, link);
}
static void bond_alb_handle_link_change(struct bonding *b, struct slave *s,
                                       char link) {
    (void)b; record_link_change(s, link);
}
static void bond_update_slave_arr(struct bonding *b, void *unused) {
    (void)b; (void)unused; array_updates++;
}
static void bond_3ad_adapter_speed_duplex_changed(struct slave *s) {
    s->speed_rechecks++;
}
static void bond_set_slave_inactive_flags(struct slave *s, bool notify) {
    (void)notify; s->inactive = 1; s->inactive_calls++;
}
static void bond_set_backup_slave(struct slave *s) { s->inactive = 1; }
static void bond_set_active_slave(struct slave *s) { s->inactive = 0; }
static void block_netpoll_tx(void) {}
static void unblock_netpoll_tx(void) {}
static void bond_select_active_slave(struct bonding *b) {
    struct slave *s;
    failovers++;
    if (b->curr_active_slave && b->curr_active_slave->link == BOND_LINK_UP)
        return;
    b->curr_active_slave = NULL;
    for (s=b->slaves; s; s=s->next)
        if (s->link == BOND_LINK_UP) { b->curr_active_slave=s; break; }
}
static void bond_set_carrier(struct bonding *b) { (void)b; carrier_calls++; }
static void queue_delayed_work(void *wq, struct delayed_work *w,
                               unsigned long delay) {
    (void)wq; (void)w; queued_work++; queued_delay=delay;
}
static void call_netdevice_notifiers(int event, struct net_device *dev) {
    (void)event; (void)dev;
}
"""

TESTS = r"""
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", test_name, __LINE__, #expr); \
    failures++; } } while (0)
static const char *test_name;
static struct bonding b;
static struct slave a, spare;
static struct net_device bd, ad, sd;
static struct bond_up_slave usable;

static void setup(const char *name, int mode) {
    test_name = name;
    memset(&b,0,sizeof(b)); memset(&a,0,sizeof(a));
    memset(&spare,0,sizeof(spare));
    b.dev=&bd; b.slaves=&a; b.curr_active_slave=&a;
    b.params.mode=mode; b.params.miimon=100;
    b.params.updelay=2; b.params.downdelay=2;
    usable.count=2; b.usable_slaves=&usable;
    a.bond=spare.bond=&b; a.next=&spare;
    a.dev=&ad; spare.dev=&sd; ad.owner=&a; sd.owner=&spare;
    a.link=spare.link=BOND_LINK_UP;
    a.link_new_state=spare.link_new_state=BOND_LINK_NOCHANGE;
    a.physical_up=spare.physical_up=true;
    a.speed=spare.speed=10000; a.duplex=spare.duplex=1;
    jiffies=0; lock_available=true; rtnl_locked=false; rcu_depth=0;
    errors=warnings=debug_messages=failovers=carrier_calls=0;
    queued_work=array_updates=0; queued_delay=0;
}
static void tick(void) {
    jiffies+=100;
    bond_mii_monitor(&b.mii_work.work);
    CHECK(!rtnl_locked && rcu_depth == 0);
}
static void commit(void) {
    rtnl_locked=true;
    bond_miimon_commit(&b);
    rtnl_unlock();
}
static void delay_cycle(int mode) {
    setup("complete delay cycle", mode);
    a.physical_up=false;
    tick();
    CHECK(a.link==BOND_LINK_FAIL && a.delay==1);
    CHECK(errors==0 && a.link_failure_count==0 && failovers==0);
    CHECK(a.should_notify_link && a.down_events==0);
    tick();
    CHECK(a.link==BOND_LINK_FAIL && a.delay==0);
    tick();
    CHECK(a.link==BOND_LINK_DOWN && a.link_failure_count==1);
    CHECK(a.down_events==1 && !a.should_notify_link && failovers==1);
    CHECK(b.curr_active_slave==&spare);
    if (mode==BOND_MODE_ACTIVEBACKUP || mode==BOND_MODE_8023AD)
        CHECK(a.inactive_calls==1);
    if (mode==BOND_MODE_8023AD || mode==BOND_MODE_TLB || mode==BOND_MODE_ALB)
        CHECK(a.down_calls==1);
    if (mode==BOND_MODE_XOR) CHECK(array_updates==1);
    a.physical_up=true;
    tick();
    CHECK(a.link==BOND_LINK_BACK && a.delay==1 && errors==0);
    CHECK(a.up_events==0 && a.should_notify_link);
    tick();
    CHECK(a.link==BOND_LINK_BACK && a.delay==0);
    tick();
    CHECK(a.link==BOND_LINK_UP && a.up_events==1);
    CHECK(!a.should_notify_link && a.link_failure_count==1 && errors==0);
    CHECK(a.speed_seen_link==BOND_LINK_BACK);
}
static void early_recovery(void) {
    setup("recovery before downdelay", BOND_MODE_8023AD);
    a.physical_up=false; tick();
    a.physical_up=true; tick();
    CHECK(a.link==BOND_LINK_UP && a.link_failure_count==0);
    CHECK(a.speed_seen_link==BOND_LINK_FAIL && a.up_events==1);
    CHECK(a.down_calls==0 && failovers==0 && errors==0);
}
static void failed_recovery(int mode, bool active) {
    setup(active ? "speed failure on active FAIL->UP" :
                   "speed failure on backup FAIL->UP", mode);
    if (!active) b.curr_active_slave=&spare;
    a.physical_up=false; tick();
    CHECK(a.link==BOND_LINK_FAIL);
    a.physical_up=true; a.speed_error=true; tick();
    CHECK(a.speed_seen_link==BOND_LINK_FAIL);
    CHECK(a.link==BOND_LINK_DOWN && a.link_failure_count==1);
    CHECK(a.down_calls==1 && a.down_events==1 && a.up_events==0);
    CHECK(warnings==1 && errors==0 && failovers==(active ? 1 : 0));
    CHECK(b.curr_active_slave==&spare);
    if (mode==BOND_MODE_8023AD) CHECK(a.inactive_calls==1);
    /* A failed initial retry must not increment the failure count again. */
    tick(); tick(); tick();
    CHECK(a.link==BOND_LINK_DOWN && a.link_failure_count==1);
    CHECK(a.down_calls==1 && a.down_events==1);
}
static void never_up_speed_failure(int mode, int prior) {
    setup("speed failure before becoming usable", mode);
    b.curr_active_slave=&spare; a.link=prior;
    a.speed_error=true; a.delay=0; b.params.updelay=0;
    tick();
    CHECK(a.speed_seen_link==prior && a.link==BOND_LINK_DOWN);
    CHECK(a.link_failure_count==0 && a.down_calls==0 && a.down_events==0);
    CHECK(a.up_events==0 && failovers==0 && warnings==1);
}
static void other_states(void) {
    setup("BACK interrupted", BOND_MODE_8023AD);
    a.link=BOND_LINK_DOWN; b.curr_active_slave=&spare;
    tick(); CHECK(a.link==BOND_LINK_BACK);
    a.physical_up=false; tick();
    CHECK(a.link==BOND_LINK_DOWN && a.link_failure_count==1);
    CHECK(a.down_events==1 && errors==0);

    setup("zero delay", BOND_MODE_ACTIVEBACKUP);
    b.params.updelay=b.params.downdelay=0;
    a.physical_up=false; tick();
    CHECK(a.link==BOND_LINK_DOWN && debug_messages==0 && errors==0);
    a.physical_up=true; tick();
    CHECK(a.link==BOND_LINK_UP && debug_messages==0 && errors==0);

    setup("first usable slave bypasses updelay", BOND_MODE_ACTIVEBACKUP);
    b.curr_active_slave=NULL; a.link=spare.link=BOND_LINK_DOWN;
    tick();
    CHECK(a.link==BOND_LINK_UP && spare.link==BOND_LINK_BACK);
    CHECK(b.curr_active_slave==&a && errors==0);

    setup("non AB first usable bypass", BOND_MODE_TLB);
    b.curr_active_slave=NULL; usable.count=0;
    a.link=spare.link=BOND_LINK_DOWN; tick();
    CHECK(a.link==BOND_LINK_UP && spare.link==BOND_LINK_BACK);

    setup("failure count saturation", BOND_MODE_8023AD);
    a.link_failure_count=UINT_MAX; a.link_new_state=BOND_LINK_DOWN;
    commit(); CHECK(a.link_failure_count==UINT_MAX && a.down_events==1);

    setup("invalid state stays an error", BOND_MODE_8023AD);
    a.link_new_state=99; commit();
    CHECK(errors==1 && a.link_new_state==BOND_LINK_NOCHANGE);
    CHECK(a.link==BOND_LINK_UP && a.lower_events==0);

    setup("nochange speed recheck", BOND_MODE_8023AD);
    commit(); CHECK(a.speed_rechecks==1 && a.lower_events==0);
    CHECK(a.link==BOND_LINK_UP && errors==0);

    setup("UP speed failure full down path", BOND_MODE_TLB);
    a.link_new_state=BOND_LINK_UP; a.speed_error=true; commit();
    CHECK(a.link==BOND_LINK_DOWN && a.link_failure_count==1);
    CHECK(a.down_calls==1 && failovers==1 && b.curr_active_slave==&spare);

    setup("AB tolerates unknown speed", BOND_MODE_ACTIVEBACKUP);
    a.link=BOND_LINK_FAIL; a.speed_error=true; tick();
    CHECK(a.link==BOND_LINK_UP && a.link_failure_count==0);
    CHECK(warnings==0 && a.up_events==1);

    setup("RTNL contention reinspection", BOND_MODE_8023AD);
    lock_available=false; a.physical_up=false; tick();
    CHECK(a.link==BOND_LINK_UP && a.lower_events==0 && queued_delay==1);
    a.physical_up=true; lock_available=true; tick();
    CHECK(a.link==BOND_LINK_UP && a.link_new_state==BOND_LINK_NOCHANGE);
    CHECK(errors==0 && a.lower_events==0 && queued_delay==100);

    setup("empty bond reschedules", BOND_MODE_8023AD);
    b.slaves=NULL; tick();
    CHECK(queued_work==1 && queued_delay==100 && carrier_calls==0);
}
int main(void) {
    int modes[]={BOND_MODE_ACTIVEBACKUP,BOND_MODE_8023AD,
                 BOND_MODE_TLB,BOND_MODE_ALB,BOND_MODE_XOR};
    for (unsigned int i=0;i<sizeof(modes)/sizeof(modes[0]);i++)
        delay_cycle(modes[i]);
    early_recovery();
    for (int mode=BOND_MODE_8023AD; mode<=BOND_MODE_ALB; mode++) {
        if (mode!=BOND_MODE_8023AD && mode!=BOND_MODE_TLB && mode!=BOND_MODE_ALB)
            continue;
        failed_recovery(mode,true); failed_recovery(mode,false);
        never_up_speed_failure(mode,BOND_LINK_DOWN);
        never_up_speed_failure(mode,BOND_LINK_BACK);
    }
    other_states();
    printf("miimon actual-source checks: %d, failures: %d\n", checks, failures);
    return failures ? 1 : 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=pathlib.Path,
                        default=ROOT / "drivers/net/bonding/bond_main.c")
    args = parser.parse_args()
    source = args.source.read_text()
    uapi = (ROOT / "include/uapi/linux/if_bonding.h").read_text()
    constants = "\n".join(line for line in uapi.splitlines()
                          if line.startswith(("#define BOND_LINK_",
                                              "#define BOND_MODE_"))) + "\n"
    header = (ROOT / "include/net/bonding.h").read_text()
    helpers = block(header, "static inline void bond_propose_link_state(",
                    "static inline void bond_slave_link_notify(")
    needs_speed = block(header, "static inline bool bond_needs_speed_duplex(",
                        "static inline bool bond_is_nondyn_tlb(")
    functions = block(source, "static int bond_miimon_inspect(",
                      "static int bond_upper_dev_walk(")
    with tempfile.TemporaryDirectory(prefix="bond-miimon-state-") as tmp:
        cfile = pathlib.Path(tmp) / "test.c"
        binary = pathlib.Path(tmp) / "test"
        cfile.write_text(constants + STUBS + helpers + needs_speed + functions + TESTS)
        compiler = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run(compiler + ["-std=gnu11", "-Wall", "-Wextra", "-Werror",
                                   "-Wno-unused-but-set-variable",
                                   str(cfile), "-o", str(binary)], check=True)
        return subprocess.run([str(binary)]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
