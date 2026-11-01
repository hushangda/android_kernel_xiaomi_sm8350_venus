#!/usr/bin/env python3
"""Actual ioctl dispatch/credentials with task lookup and uaccess host stubs."""
import pathlib,re,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[4]
src=(ROOT/'kernel/sched/xr_qos_device.c').read_text()
def function(name):
 m=re.search(r'(?m)^static [\w *]+\b'+name+r'\([^;]*?\)\s*\{',src);assert m,name
 e=src.index('{',m.start())+1;d=1
 while d:d+=(src[e]=='{')-(src[e]=='}');e+=1
 return src[m.start():e]
pre=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>
typedef uint32_t __u32;typedef int32_t __s32;
#define __user
#define GLOBAL_ROOT_UID 0
#define KUIDT_INIT(x) (x)
#define CAP_SYS_NICE 1
#define PF_KTHREAD 1
#define PF_EXITING 2
#define uid_eq(x,y) ((x)==(y))
#define max(a,b) ((a)>(b)?(a):(b))
struct file {int unused;};struct cred {unsigned euid;};
struct task_struct {unsigned uid;int flags,refs,tgid;unsigned qos;bool armed;};
static struct task_struct tasks[4];static struct cred cred;
static bool cap,faultin,faultout,lsmdeny;static int seterror;
static const struct cred*current_cred(void){return &cred;}
static unsigned current_euid(void){return cred.euid;}
static unsigned task_uid(struct task_struct*p){return p->uid;}
static int capable(int c){(void)c;return cap;}
static int copy_from_user(void*d,const void*s,size_t n){if(faultin)return 1;memcpy(d,s,n);return 0;}
static int copy_to_user(void*d,const void*s,size_t n){if(faultout)return 1;memcpy(d,s,n);return 0;}
static void rcu_read_lock(void){}static void rcu_read_unlock(void){}
static struct task_struct*find_task_by_vpid(int pid){return pid>0&&pid<=4?&tasks[pid-1]:NULL;}
static void get_task_struct(struct task_struct*p){p->refs++;}static void put_task_struct(struct task_struct*p){assert(p->refs>1);p->refs--;}
static int security_task_setscheduler(struct task_struct*p){(void)p;return lsmdeny?-EACCES:0;}
static int xr_qos_set(struct task_struct*p,unsigned qos){if(seterror)return seterror;if(p->flags&PF_EXITING)return -ESRCH;p->qos=qos;return 0;}
static int xr_qos_async_arm(struct task_struct*p,bool enable){p->armed=enable;return 0;}
static unsigned xr_qos_get(struct task_struct*p){return p->qos&15;}
#define for_each_thread(p,t) for(t=tasks;t<tasks+4;t++)if(t->tgid==p->tgid)
'''
abi=src[src.index('struct xr_qos_request {'):src.index('static bool xr_qos_admin')]
main=r'''
static struct xr_qos_request req;
static long call(unsigned cmd,int id,unsigned qos){req.id=id;req.qos=qos;long ret=xr_qos_ioctl(NULL,cmd,(unsigned long)&req);for(int i=0;i<4;i++)assert(tasks[i].refs==1);return ret;}
int main(void){assert(sizeof(req)==8);assert(XR_QOS_SET==0x40087807&&XR_QOS_GET_PID==0xc0087808&&XR_QOS_GET_TID==0xc0087809&&XR_QOS_ASYNC==0x4008780b);
 for(int i=0;i<4;i++){tasks[i].uid=10000+(i/2);tasks[i].refs=1;tasks[i].tgid=i/2;}
 cred.euid=10000;assert(call(0x40087801,1,0)==-ENOTTY);assert(call(XR_QOS_SET,0,0)==-EINVAL);assert(call(XR_QOS_SET,8,0)==-ESRCH);
 assert(call(XR_QOS_SET,3,0x29)==-EPERM);assert(!call(XR_QOS_SET,1,0x29));assert(tasks[0].qos==0x29);
 assert(call(XR_QOS_SET,1,0x19)==-EPERM);cap=true;assert(!call(XR_QOS_SET,1,0x19));cap=false;
 cred.euid=1000;assert(!call(XR_QOS_SET,3,0x19));cred.euid=0;assert(!call(XR_QOS_SET,3,0x29));cred.euid=10000;
 tasks[1].qos=0x2a;assert(!call(XR_QOS_GET_PID,1,123)&&req.qos==10&&req.id==1);assert(!call(XR_QOS_GET_TID,1,123)&&req.qos==9);
 assert(call(XR_QOS_GET_TID,3,0)==-EPERM);assert(call(XR_QOS_ASYNC,1,2)==-EINVAL);assert(!call(XR_QOS_ASYNC,1,1)&&tasks[0].armed);assert(!call(XR_QOS_ASYNC,1,0)&&!tasks[0].armed);
 faultin=true;assert(call(XR_QOS_SET,1,0)==-EFAULT);faultin=false;faultout=true;assert(call(XR_QOS_GET_PID,1,0)==-EFAULT);faultout=false;
 lsmdeny=true;assert(call(XR_QOS_SET,1,0)==-EACCES);assert(call(XR_QOS_ASYNC,1,1)==-EACCES);lsmdeny=false;seterror=-ENOSPC;assert(call(XR_QOS_SET,1,0x29)==-ENOSPC);seterror=0;
 tasks[0].flags=PF_EXITING;assert(call(XR_QOS_SET,1,0)==-ESRCH);tasks[0].flags=PF_KTHREAD;cred.euid=0;assert(call(XR_QOS_SET,1,0)==-EPERM);
 puts("PASS: exact v2 ABI, same UID/cross UID, direct RT authorization, LSM, process aggregation, async validation, uaccess and backend errors, task references; ASan+UBSan");}
'''
with tempfile.TemporaryDirectory(prefix='xr-ioctl-') as d:
 c=pathlib.Path(d)/'test.c';e=pathlib.Path(d)/'test';c.write_text(pre+abi+'\n'.join(function(n) for n in ['xr_qos_admin','xr_qos_access','xr_qos_ioctl'])+main)
 subprocess.run(['clang','-std=gnu11','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',str(c),'-o',str(e)],check=True)
 subprocess.run([str(e)],check=True)
