#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise extracted DMA-heap/ION and cgroup.kill production C.

These bounded host models check error unwinding and controlled interleavings;
they do not replace DMA coherency, real fork races, or an on-device boot test.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]


def function(source, name):
    match = re.search(r"(?m)^[\w *\n]+\b" + name + r"\([^;]*?\)\s*\{", source)
    if not match:
        raise ValueError(name)
    end = source.index("{", match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


COMMON = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#define __user
#define unlikely(x) (x)
#define GFP_KERNEL 0
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((long)(intptr_t)(p))
#define ERR_CAST(p) ((void *)(p))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR_OR_ZERO(p) (IS_ERR(p) ? PTR_ERR(p) : 0)
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define max(a,b) ((a) > (b) ? (a) : (b))
#define WARN_ON_ONCE(x) (assert(!(x)), false)
#define pr_debug(...) ((void)0)
#define __ffs(v) __builtin_ctz(v)
'''

DMA = r'''
#include "@ROOT@/include/uapi/linux/dma-heap.h"
#define PAGE_SIZE 4096UL
#define PAGE_ALIGN(n) (((n)+PAGE_SIZE-1)&~(PAGE_SIZE-1))
#define IOC_IN (_IOC_WRITE << _IOC_DIRSHIFT)
#define IOC_OUT (_IOC_READ << _IOC_DIRSHIFT)
#define array_index_nospec(i,n) (i)
struct file { void *private_data; unsigned int flags; };
struct ion_heap { int type; unsigned id; };
struct ion_buffer { size_t size; unsigned flags; };
struct ion_device { int dummy; };
static struct ion_device device, *internal_dev=&device;
struct dma_buf { struct file *file; struct ion_buffer *priv; };
struct dma_buf_export_info { const void *ops; size_t size; unsigned flags; void *priv; };
#define DEFINE_DMA_BUF_EXPORT_INFO(v) struct dma_buf_export_info v={0}
static int dma_buf_ops;
#define ION_FLAG_CACHED 1U
#define ION_SYSTEM_HEAP_ID (1U<<25)
#define ION_HEAP_TYPE_MSM_SYSTEM 17
struct dma_heap;
struct dma_heap_ops { struct dma_buf *(*allocate)(struct dma_heap *,unsigned long,unsigned long,unsigned long); };
struct dma_heap { const char *name; const struct dma_heap_ops *ops; void *priv; };
struct dma_heap_export_info { const char *name; const struct dma_heap_ops *ops; void *priv; };
static struct dma_heap heaps[4];
static int heap_count, fail_registration, fail_buffer, fail_export, fail_fd;
static int fail_in, fail_out, fail_kmalloc, live_buffers, live_exports, reserved, published, releases;
static int ion_calls;
static size_t last_len;
static unsigned last_mask, last_ion_flags, last_fd_flags;
static struct dma_buf *installed;
static struct dma_heap *dma_heap_add(const struct dma_heap_export_info *info)
{
    if (fail_registration == heap_count+1) return ERR_PTR(-ENOMEM);
    heaps[heap_count]=(struct dma_heap){info->name,info->ops,info->priv};
    return &heaps[heap_count++];
}
static void *dma_heap_get_drvdata(struct dma_heap *h) { return h->priv; }
static struct ion_buffer *ion_buffer_alloc(struct ion_device *d,size_t n,unsigned mask,unsigned flags)
{
    ++ion_calls; last_len=n; last_mask=mask; last_ion_flags=flags;
    if (fail_buffer) return ERR_PTR(-ENOMEM);
    struct ion_buffer *b=calloc(1,sizeof(*b)); assert(b); b->size=n; b->flags=flags;
    ++live_buffers; return b;
}
static void ion_buffer_destroy(struct ion_device *d,struct ion_buffer *b)
{ assert(live_buffers>0); --live_buffers; free(b); }
static struct dma_buf *dma_buf_export(struct dma_buf_export_info *i)
{
    last_fd_flags=i->flags;
    if (fail_export) return ERR_PTR(-ENOMEM);
    struct dma_buf *b=calloc(1,sizeof(*b)); assert(b);
    b->file=calloc(1,sizeof(*b->file)); assert(b->file);
    b->file->flags=i->flags; b->priv=i->priv; b->file->private_data=b;
    ++live_exports; return b;
}
static void dma_buf_put(struct dma_buf *b)
{
    assert(live_exports>0); --live_exports; ++releases;
    ion_buffer_destroy(&device,b->priv); free(b->file); free(b);
}
static int get_unused_fd_flags(unsigned flags)
{ if(fail_fd)return -EMFILE; assert(!reserved); reserved=1; return 9; }
static void put_unused_fd(int fd) { assert(fd==9 && reserved); reserved=0; }
static void fd_install(int fd,struct file *f)
{ assert(fd==9 && reserved && !installed); reserved=0; ++published; installed=f->private_data; }
static int copy_from_user(void *to,const void *from,size_t n)
{ if(fail_in)return n ?: 1; memcpy(to,from,n);return 0; }
static int copy_to_user(void *to,const void *from,size_t n)
{ if(fail_out)return n ?: 1;memcpy(to,from,n);return 0; }
static void *kmalloc(size_t n,int flags) { return fail_kmalloc ? NULL : malloc(n); }
static void kfree(void *p) { free(p); }
@ION@
@BACKEND@
static const struct dma_heap_ops msm_system_heap_ops={.allocate=msm_system_heap_allocate};
@REGISTER@
static unsigned int dma_heap_ioctl_cmds[]={DMA_HEAP_IOCTL_ALLOC};
@CORE@
static void clean(void) { assert(!live_buffers && !live_exports && !reserved && !installed); }
static int invoke(struct dma_heap_allocation_data *a)
{ struct file f={.private_data=&heaps[0]};return dma_heap_ioctl(&f,DMA_HEAP_IOCTL_ALLOC,(unsigned long)a); }
int main(void)
{
    struct ion_heap secure={.type=99,.id=25}, wrong_id={.type=17,.id=9}, valid={.type=17,.id=25};
    assert(!msm_ion_register_dma_heaps(&secure) && !heap_count);
    assert(!msm_ion_register_dma_heaps(&wrong_id) && !heap_count);
    fail_registration=1;assert(msm_ion_register_dma_heaps(&valid)==-ENOMEM && !heap_count);
    fail_registration=0;assert(!msm_ion_register_dma_heaps(&valid) && heap_count==2);
    assert(!strcmp(heaps[0].name,"system") && !strcmp(heaps[1].name,"system-uncached"));
    unsigned modes[]={O_RDONLY,O_WRONLY,O_RDWR,O_RDWR|O_CLOEXEC};
    for(unsigned h=0;h<2;h++) for(unsigned i=0;i<ARRAY_SIZE(modes);i++) {
        struct dma_heap_allocation_data a={.len=4097,.fd_flags=modes[i]};
        struct file f={.private_data=&heaps[h]};
        assert(!dma_heap_ioctl(&f,DMA_HEAP_IOCTL_ALLOC,(unsigned long)&a));
        assert(a.fd==9 && installed && installed->file->flags==modes[i]);
        assert(last_len==8192 && last_mask==ION_SYSTEM_HEAP_ID);
        assert(last_ion_flags==(h ? 0 : ION_FLAG_CACHED));
        dma_buf_put(installed);installed=NULL;clean();
    }
    /* Legacy callers still export O_RDWR. */
    struct dma_buf *legacy=ion_dmabuf_alloc(&device,4096,1,1);
    assert(!IS_ERR(legacy) && legacy->file->flags==O_RDWR);dma_buf_put(legacy);clean();
    assert(IS_ERR(ion_alloc_fd_flags(4096,1,0,O_CREAT)));clean();
    struct dma_heap_allocation_data good={.len=4096,.fd_flags=O_RDWR}, a;
    a=good;a.fd=7;assert(invoke(&a)==-EINVAL);clean();
    a=good;a.fd_flags|=O_CREAT;assert(invoke(&a)==-EINVAL);clean();
    a=good;a.heap_flags=UINT64_C(1)<<63;assert(invoke(&a)==-EINVAL);clean();
    a=good;a.len=UINT64_MAX;assert(invoke(&a)==-EINVAL);clean();
    a=good;a.len=0;assert(invoke(&a)==-EINVAL);clean();
    fail_buffer=1;a=good;assert(invoke(&a)==-ENOMEM);fail_buffer=0;clean();
    fail_export=1;a=good;assert(invoke(&a)==-ENOMEM);fail_export=0;clean();
    fail_fd=1;a=good;assert(invoke(&a)==-EMFILE);fail_fd=0;clean();
    fail_in=1;a=good;assert(invoke(&a)==-EFAULT);fail_in=0;clean();
    int old_published=published;
    fail_out=1;a=good;assert(invoke(&a)==-EFAULT);fail_out=0;clean();assert(published==old_published);
    struct file f={.private_data=&heaps[0]};
    a=good;assert(dma_heap_ioctl(&f,_IOWR('X',0,struct dma_heap_allocation_data),(unsigned long)&a)==-ENOTTY);
    assert(dma_heap_ioctl(&f,_IOWR('H',1,struct dma_heap_allocation_data),(unsigned long)&a)==-EINVAL);
    assert(dma_heap_ioctl(&f,_IOC(_IOC_READ|_IOC_WRITE,'H',0,8),(unsigned long)&a)==-EINVAL);
    unsigned char extended[256]={0};memcpy(extended,&good,sizeof(good));
    unsigned cmd=_IOC(_IOC_READ|_IOC_WRITE,'H',0,sizeof(extended));
    fail_kmalloc=1;assert(dma_heap_ioctl(&f,cmd,(unsigned long)extended)==-ENOMEM);fail_kmalloc=0;clean();
    assert(!dma_heap_ioctl(&f,cmd,(unsigned long)extended));dma_buf_put(installed);installed=NULL;clean();
    puts("PASS: DMA-heap ABI, cached/uncached backend, access modes, legacy ION, allocation/export/fd/copyout failures and no leaked references");
}
'''

CGROUP = r'''
#include <limits.h>
#define PF_KTHREAD 1
#define PF_EXITING 2
#define CSS_TASK_ITER_PROCS 1
#define CSS_TASK_ITER_THREADED 2
#define PIDTYPE_TGID 1
#define SIGKILL 9
#define SEND_SIG_NOINFO NULL
struct cgroup;
struct cgroup_subsys_state { struct cgroup *cgroup; };
struct css_set { struct cgroup *dfl_cgrp; };
struct task_struct { int flags, group, killed, leader; struct css_set *cset; };
struct cgroup { unsigned id; bool dfl, threaded, live; struct cgroup_subsys_state self; struct cgroup *next; struct task_struct *tasks[8]; unsigned count; };
struct kernfs_node { struct cgroup *cgrp; };
struct kernfs_open_file { struct kernfs_node *kn; };
struct css_task_iter { struct cgroup *c; unsigned i, flags; };
struct cgroup_kill_state { unsigned seq; };
static int cgroup_mutex, css_set_lock, fail_alloc, fail_insert, live_states, in_kill;
static bool use_task_css_set_links=true;
static struct task_struct *current;
static struct task_struct *groups[12][8];
static unsigned group_count[12];
static void *cgroup_kill_states[16];
#define lockdep_assert_held(p) assert(*(p)>0)
static void spin_lock_irq(int *l) { assert(!*l); ++*l; }
static void spin_unlock_irq(int *l) { assert(*l==1); --*l; }
static struct css_set *task_css_set(struct task_struct *t) { return t->cset; }
static bool cgroup_on_dfl(struct cgroup *c) { return c->dfl; }
static void *kzalloc(size_t n,int flags) { assert(!in_kill); if(fail_alloc)return NULL;void *p=calloc(1,n);assert(p);++live_states;return p; }
static void kfree(void *p) { if(p){assert(live_states>0);--live_states;free(p);} }
static int xa_insert(void *table,unsigned id,void *v,int flags)
{ assert(!in_kill);if(fail_insert)return -ENOMEM;void **a=table;if(a[id])return -EBUSY;a[id]=v;return 0; }
static void *xa_erase(void *table,unsigned id) { void **a=table;void *v=a[id];a[id]=NULL;return v; }
static void *xa_load(void *table,unsigned id) { return ((void **)table)[id]; }
static bool __fatal_signal_pending(struct task_struct *t) { return t->killed; }
static void mark_group(struct task_struct *t)
{ for(unsigned i=0;i<group_count[t->group];i++)groups[t->group][i]->killed=1; }
static int send_sig(int sig,struct task_struct *t,int priv)
{
    /* 5.4 wants_signal()/complete_signal(): PIDTYPE_PID does not retarget an exiting thread. */
    if(!(t->flags&PF_EXITING)) mark_group(t);
    return 0;
}
static int do_send_sig_info(int sig,void *info,struct task_struct *t,int type)
{ assert(type==PIDTYPE_TGID);mark_group(t);return 0; }
static void css_task_iter_start(struct cgroup_subsys_state *css,unsigned flags,struct css_task_iter *it)
{ *it=(struct css_task_iter){css->cgroup,0,flags}; }
static struct task_struct *css_task_iter_next(struct css_task_iter *it)
{ while(it->i<it->c->count){struct task_struct *t=it->c->tasks[it->i++];if((it->flags&CSS_TASK_ITER_PROCS)&&!t->leader)continue;return t;}return NULL; }
static void css_task_iter_end(struct css_task_iter *it) {}
#define cgroup_for_each_live_descendant_pre(c,css,root) for(c=(root);c;c=c->next) if((css=&c->self),css->cgroup->live)
static char *strstrip(char *s) { return s; }
static int kstrtoint(const char *s,unsigned base,int *v)
{ char *e;errno=0;long n=strtol(s,&e,base);if(e==s||(*e&&*e!='\n')||errno||n>INT_MAX||n<INT_MIN)return -EINVAL;*v=n;return 0; }
static struct cgroup *cgroup_kn_lock_live(struct kernfs_node *kn,bool unused)
{ if(!kn->cgrp->live)return NULL;assert(!cgroup_mutex);cgroup_mutex=1;return kn->cgrp; }
static void cgroup_kn_unlock(struct kernfs_node *kn) { assert(cgroup_mutex==1);cgroup_mutex=0; }
static bool cgroup_is_threaded(struct cgroup *c) { return c->threaded; }
@FUNCTIONS@
static void init_group(struct cgroup *c,unsigned id)
{ *c=(struct cgroup){.id=id,.dfl=true,.live=true};c->self.cgroup=c;cgroup_mutex=1;assert(!cgroup_kill_state_alloc(c));cgroup_mutex=0; }
static void add(struct cgroup *c,struct task_struct *t,int group,bool leader,int flags,struct css_set *cs)
{ *t=(struct task_struct){.flags=flags,.group=group,.leader=leader,.cset=cs};c->tasks[c->count++]=t;groups[group][group_count[group]++]=t; }
static void sweep(struct cgroup *c)
{ cgroup_mutex=1;in_kill=1;cgroup_kill(c);in_kill=0;cgroup_mutex=0; }
int main(void)
{
    struct cgroup root={.id=1,.dfl=true,.live=true}, a,b,unused;
    struct css_set root_cs={&root},a_cs={&a},b_cs={&b};
    struct task_struct parent={.cset=&root_cs};current=&parent;
    assert(!cgroup_fork_kill_seq());
    init_group(&a,2);init_group(&b,3);a.next=&b;
    unused=(struct cgroup){.id=4,.dfl=true};
    cgroup_mutex=1;fail_alloc=1;assert(cgroup_kill_state_alloc(&unused)==-ENOMEM);fail_alloc=0;
    fail_insert=1;assert(cgroup_kill_state_alloc(&unused)==-ENOMEM);fail_insert=0;cgroup_mutex=0;assert(live_states==2);
    struct task_struct leader,thread,migrated,exiting,kthread;
    add(&a,&leader,0,true,PF_EXITING,&a_cs);add(&b,&thread,0,false,0,&b_cs);
    add(&b,&exiting,1,false,PF_EXITING,&b_cs);add(&root,&migrated,1,true,0,&root_cs);
    add(&b,&kthread,2,true,PF_KTHREAD,&b_cs);
    sweep(&b);assert(thread.killed);assert(!migrated.killed && !kthread.killed);
    struct task_struct child={.cset=&a_cs,.group=3};groups[3][group_count[3]++]=&child;
    parent.cset=&a_cs;
    unsigned seq=cgroup_fork_kill_seq();
    sweep(&a); /* Complete the sweep before the not-yet-linked child is posted. */
    cgroup_post_fork_kill(&child,seq);assert(child.killed);
    child.killed=0;seq=cgroup_fork_kill_seq();cgroup_post_fork_kill(&child,seq);assert(!child.killed);
    child.flags=PF_KTHREAD;sweep(&a);cgroup_post_fork_kill(&child,seq);assert(!child.killed);child.flags=0;
    struct kernfs_node kn={&a};struct kernfs_open_file of={&kn};char zero[]="0",bad[]="bad",one[]="1";
    assert(cgroup_kill_write(&of,zero,1,0)==-ERANGE);
    assert(cgroup_kill_write(&of,bad,3,0)==-EINVAL);
    a.threaded=true;assert(cgroup_kill_write(&of,one,1,0)==-EOPNOTSUPP);a.threaded=false;
    a.live=false;assert(cgroup_kill_write(&of,one,1,0)==-ENODEV);a.live=true;
    seq=cgroup_fork_kill_seq();assert(cgroup_kill_write(&of,one,1,0)==1);assert(cgroup_fork_kill_seq()==seq+1);
    cgroup_mutex=1;cgroup_kill_state_free(&a);cgroup_kill_state_free(&b);cgroup_mutex=0;assert(!live_states);
    init_group(&a,2);assert(!cgroup_fork_kill_seq());cgroup_mutex=1;cgroup_kill_state_free(&a);cgroup_mutex=0;assert(!live_states);
    puts("PASS: recursive cgroup.kill, fork window, exited leaders, migrated groups, kthreads, errors, ID reuse and allocation-free kill path");
}
'''


def main():
    read = lambda p: (ROOT / p).read_text()
    ion = function(read("drivers/staging/android/ion/ion_dma_buf.c"), "ion_dmabuf_alloc_flags")
    ion += function(read("drivers/staging/android/ion/ion_dma_buf.c"), "ion_dmabuf_alloc")
    ion += function(read("drivers/staging/android/ion/ion.c"), "ion_alloc_fd_flags")
    backend = read("drivers/staging/android/ion/heaps/ion_dma_heap.c")
    core = read("drivers/dma-buf/dma-heap.c")
    dma = DMA.replace("@ROOT@", str(ROOT)).replace("@ION@", ion)
    dma = dma.replace("@BACKEND@", function(backend, "msm_system_heap_allocate"))
    dma = dma.replace("@REGISTER@", function(backend, "msm_ion_register_dma_heaps"))
    dma = dma.replace("@CORE@", "\n".join(function(core, n) for n in (
        "dma_heap_buffer_alloc", "dma_heap_ioctl_allocate", "dma_heap_ioctl")))
    source = read("kernel/cgroup/cgroup.c")
    cg = CGROUP.replace("@FUNCTIONS@", "\n".join(function(source, n) for n in (
        "cgroup_kill_state_alloc", "cgroup_kill_state_free", "cgroup_read_kill_seq",
        "__cgroup_kill", "cgroup_kill", "cgroup_kill_write", "cgroup_fork_kill_seq",
        "cgroup_post_fork_kill")))
    # Mutation controls cover fork escape, exited leaders and migrated groups.
    variants = [("dma", dma, True), ("cgroup", cg, True),
                ("leaders_only", cg.replace("CSS_TASK_ITER_THREADED, &it)",
                    "CSS_TASK_ITER_THREADED | CSS_TASK_ITER_PROCS, &it)"), False),
                ("fork_race", cg.replace("kill = kill_seq !=", "kill = false && kill_seq !="), False),
                ("migrated_group", cg.replace("send_sig(SIGKILL, task, 0);",
                    "do_send_sig_info(SIGKILL, SEND_SIG_NOINFO, task, PIDTYPE_TGID);"), False)]
    with tempfile.TemporaryDirectory(prefix="venus-a17-priority-") as tmp:
        for name, body, should_pass in variants:
            src = Path(tmp) / (name + ".c")
            exe = src.with_suffix("")
            src.write_text(COMMON + body)
            subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g",
                            "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                            "-Wno-unused-function", "-Wno-unused-variable", "-Wno-sign-compare",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                            str(src), "-o", str(exe)], check=True)
            result = subprocess.run([str(exe)], text=True, capture_output=True, timeout=10)
            if (result.returncode == 0) != should_pass:
                raise RuntimeError(name + "\n" + result.stdout + result.stderr)
            print(result.stdout.strip() if should_pass else "PASS mutation detected: " + name)


if __name__ == "__main__":
    main()
