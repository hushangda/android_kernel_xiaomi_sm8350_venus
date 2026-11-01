#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Test real MILLET send/dispatch return and statistics paths with netlink mocks."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


source = (ROOT / "drivers/mihw/millet_core.c").read_text()
send = function(source, "int millet_sendto_user(")
dispatch = function(source, "int millet_sendmsg(")
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
#define RET_OK 0
#define RET_ERR (-1)
#define GFP_ATOMIC 0
#define MILLET_KERNEL_ID 0x12341234
#define MILLET_USER_ID 0xabcddcba
#define TYPE_VALID(x) ((x) > 0 && (x) < 3)
#define MSG_VALID(x) ((x) > 0 && (x) < 4)
#define pr_err(...) ((void)0)
#define pr_info(...) ((void)0)
#define spin_lock_irqsave(lock, flags) ((void)(lock), (flags) = 0)
#define spin_unlock_irqrestore(lock, flags) ((void)(lock), (void)(flags))
enum MILLET_TYPE { O_TYPE, BINDER_TYPE, SIG_TYPE };
struct task_struct { int unused; };
struct timespec64 { long tv_sec, tv_nsec; };
struct millet_data {
    int msg_type, owner, src_port, dst_port, monitor;
    struct { long sec, nsec; } tm;
};
struct sk_buff { struct millet_data data; };
struct nlmsghdr { int unused; };
struct millet_sock {
    int has_init;
    void *sock;
    struct {
        int monitor, port, lock;
        int (*send_to)(struct task_struct *, struct millet_data *, struct millet_sock *);
        struct { u64 send_suc, send_fail, runtime; } stat;
    } mod[3];
};
static struct millet_sock millet_sk;
static struct sk_buff skb;
static int active = 1, millet_debug, allocation_fail, put_fail, filtered;
static int send_result, send_calls, live_skb;
static bool millet_is_active(void) { return active; }
static int atomic_read(int *v) { return *v; }
static struct sk_buff *nlmsg_new(int len, int flags)
{
    (void)len; (void)flags;
    if (allocation_fail) return NULL;
    assert(!live_skb); ++live_skb; return &skb;
}
static void kfree_skb(struct sk_buff *b)
{ assert(b == &skb && live_skb); --live_skb; }
static struct nlmsghdr *nlmsg_put(struct sk_buff *b, int a, int c, int d, int e, int f)
{ (void)b; (void)a; (void)c; (void)d; (void)e; (void)f; return put_fail ? NULL : (void *)b; }
static void *nlmsg_data(struct nlmsghdr *h) { return &((struct sk_buff *)h)->data; }
static void ktime_get_ts64(struct timespec64 *ts) { ts->tv_sec = 1; ts->tv_nsec = 2; }
static u64 ktime_get(void) { static u64 now; return ++now; }
static u64 ktime_to_us(u64 now) { return now; }
static void dump_send_msg(struct millet_data *data) { (void)data; }
static int nlmsg_unicast(void *sock, struct sk_buff *b, int port)
{
    (void)sock;
    assert(port == 1234 && live_skb == 1 && b->data.owner == BINDER_TYPE);
    assert(b->data.src_port == MILLET_KERNEL_ID && b->data.dst_port == (int)MILLET_USER_ID);
    kfree_skb(b); ++send_calls;
    return send_result; /* Kernel nlmsg_unicast returns zero on successful send. */
}
#include "delivery.h"
static int send_hook(struct task_struct *t, struct millet_data *d, struct millet_sock *sk)
{ return filtered ? 0 : millet_sendto_user(t, d, sk); }
int main(void)
{
    struct task_struct task = {0};
    struct millet_data data = {.msg_type = 2, .owner = BINDER_TYPE};
    millet_sk.has_init = 1;
    millet_sk.mod[BINDER_TYPE].send_to = send_hook;
    millet_sk.mod[BINDER_TYPE].monitor = BINDER_TYPE;
    millet_sk.mod[BINDER_TYPE].port = 1234;
    active = 0;
    assert(millet_sendmsg(BINDER_TYPE, &task, &data) == 0 && !send_calls);
    active = 1; filtered = 1;
    assert(millet_sendmsg(BINDER_TYPE, &task, &data) == 0 && !send_calls);
    assert(!millet_sk.mod[BINDER_TYPE].stat.send_suc && !millet_sk.mod[BINDER_TYPE].stat.send_fail);
    filtered = 0;
    if (millet_sendmsg(BINDER_TYPE, &task, &data) != 1 || millet_sk.mod[BINDER_TYPE].stat.send_suc != 1) {
        fprintf(stderr, "successful netlink send was not counted\n"); return 1;
    }
    assert(send_calls == 1 && !live_skb);
    send_result = -111;
    assert(millet_sendmsg(BINDER_TYPE, &task, &data) < 0);
    assert(millet_sk.mod[BINDER_TYPE].stat.send_suc == 1 && millet_sk.mod[BINDER_TYPE].stat.send_fail == 1);
    allocation_fail = 1;
    assert(millet_sendmsg(BINDER_TYPE, &task, &data) < 0 && send_calls == 2);
    allocation_fail = 0; put_fail = 1;
    assert(millet_sendmsg(BINDER_TYPE, &task, &data) < 0 && !live_skb);
    put_fail = 0; millet_sk.has_init = 0;
    assert(millet_sendmsg(BINDER_TYPE, &task, &data) < 0 && send_calls == 2);
    assert(millet_sk.mod[BINDER_TYPE].stat.send_suc == 1 && millet_sk.mod[BINDER_TYPE].stat.send_fail == 4);
    puts("PASS delivered, filtered, inactive, unregistered, allocation/header failure, netlink error");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="venus-millet-delivery-") as tmp:
    tmp = Path(tmp)
    (tmp / "test.c").write_text(harness)
    (tmp / "delivery.h").write_text(send + "\n" + dispatch)
    command = [os.environ.get("HOSTCC", "cc"), "-std=c11", "-Wall", "-Wextra",
               "-Wno-unused-parameter", str(tmp / "test.c"), "-o", str(tmp / "test")]
    subprocess.run(command, check=True)
    subprocess.run([str(tmp / "test")], check=True)
    old = send.replace("return 1;\n}", "return RET_OK;\n}")
    assert old != send
    (tmp / "delivery.h").write_text(old + "\n" + dispatch)
    subprocess.run(command, check=True)
    result = subprocess.run([str(tmp / "test")], capture_output=True, text=True)
    assert result.returncode == 1 and "successful netlink send was not counted" in result.stderr
    print("PASS negative control: original successful-send return loses the counter")
