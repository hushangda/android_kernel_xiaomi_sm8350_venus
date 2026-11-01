#!/usr/bin/env python3
"""Exercise real vmscan functions with deterministic reclaim/iterator stubs.

This checks page-class selection and API lifetime/accounting; it does not
simulate hardware reclaim or substitute for a booted-kernel stress test.
"""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[4]
SOURCE = (ROOT / "mm/vmscan.c").read_text()


def function(source, name):
    import re
    match = re.search(r"(?m)^(?:static (?:inline )?)?[\w *]+\b" + name + r"\([^;]*?\)\s*\{", source)
    if not match:
        raise RuntimeError(name)
    pos = source.index("{", match.start())
    depth = 1
    end = pos + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_XIAOMI_MEMCG_RECLAIM 1
#define BIT(n) (1U << (n))
#define IS_ENABLED(x) 0
#define ANON_AND_FILE 2
#define SWAP_CLUSTER_MAX 32UL
#define MIN_LRU_BATCH 128UL
#define MIN_NR_GENS 2
#define DEF_PRIORITY 12
#define MAX_NR_ZONES 1
#define GFP_KERNEL 3
#define GFP_RECLAIM_MASK 3
#define GFP_HIGHUSER_MOVABLE 7
#define N_MEMORY 0
#define EXPORT_SYMBOL_GPL(x)
#define max(a,b) ((a) > (b) ? (a) : (b))
#define min(a,b) ((a) < (b) ? (a) : (b))
#define div64_u64(a,b) ((a)/(b))
#define DIV64_U64_ROUND_UP(a,b) (((a)+(b)-1)/(b))
#define BUG() assert(0)
typedef uint64_t u64;
enum lru_list { LRU_INACTIVE_ANON, LRU_ACTIVE_ANON, LRU_INACTIVE_FILE, LRU_ACTIVE_FILE };
enum scan_balance { SCAN_EQUAL, SCAN_FRACT, SCAN_FILE, SCAN_ANON };
enum xiaomi_reclaim_type { XIAOMI_RECLAIM_INACTIVE_ANON, XIAOMI_RECLAIM_ANON,
 XIAOMI_RECLAIM_INACTIVE_FILE, XIAOMI_RECLAIM_FILE };
#define LRU_GEN_ANON 0
#define LRU_GEN_FILE 1
#define for_each_evictable_lru(lru) for ((lru)=0; (lru)<4; (lru)++)
#define for_each_node_state(nid, state) for ((nid)=0; (nid)<2; (nid)++)
#define is_file_lru(lru) ((lru)>=LRU_INACTIVE_FILE)
struct mem_cgroup { int id, online, swappiness; long swap; unsigned long low, min, size; };
struct lruvec { struct mem_cgroup *memcg; unsigned long sizes[4], min_seq[2], max_seq; };
struct scan_control {
 unsigned int xiaomi_lru_mask;
 int priority, may_swap, may_writepage, may_unmap, reclaim_idx, force_deactivate;
 int file_is_tiny, cache_trim_mode, memcg_low_reclaim, memcg_low_skipped;
 unsigned long nr_to_reclaim, nr_reclaimed, last_reclaimed, anon_cost, file_cost, gfp_mask;
 struct mem_cgroup *target_mem_cgroup;
 int reclaim_state;
};
struct task { void *reclaim_state; } task;
#define current (&task)
struct zone { int unused; };
typedef struct { int id; struct zone node_zones[1]; } pg_data_t;
struct list_head { int unused; };
static struct mem_cgroup groups[3];
static struct lruvec vectors[3][2];
static pg_data_t nodes[2] = {{0},{1}};
static int fatal, iterator_refs, restores, drains, calls, stop_after, offline_after;
static int fatal_check, fatal_on_check;
static unsigned int noreclaim, observed_mask;
static unsigned long actual_pages, expected_target, reclaim_per_call;
static int scan_calls[2], scan_result[2], age_calls, need_age;
#define NODE_DATA(n) (&nodes[n])
#define DEFINE_MIN_SEQ(v) unsigned long *min_seq = (v)->min_seq
#define DEFINE_MAX_SEQ(v) unsigned long max_seq = (v)->max_seq
static int fatal_signal_pending(struct task *p) { (void)p; fatal_check++; return fatal || (fatal_on_check && fatal_check>=fatal_on_check); }
static int mem_cgroup_online(struct mem_cgroup *m) { return m->online; }
static struct mem_cgroup *lruvec_memcg(struct lruvec *v) { return v->memcg; }
static int mem_cgroup_swappiness(struct mem_cgroup *m) { return m->swappiness; }
static long mem_cgroup_get_nr_swap_pages(struct mem_cgroup *m) { return m->swap; }
static int cgroup_reclaim(struct scan_control *s) { return s->target_mem_cgroup != NULL; }
static unsigned long lruvec_lru_size(struct lruvec *v, int l, int idx) { (void)idx; return v->sizes[l]; }
static void mem_cgroup_protection(struct mem_cgroup *root, struct mem_cgroup *m,
 unsigned long *min, unsigned long *low) { (void)root; *min=m->min; *low=m->low; }
static unsigned long mem_cgroup_size(struct mem_cgroup *m) { return m->size; }
static int current_gfp_context(int gfp) { return gfp; }
static void set_task_reclaim_state(struct task *p, void *s) { p->reclaim_state=s; }
static unsigned int memalloc_noreclaim_save(void) { unsigned int old=noreclaim; noreclaim=1; return old; }
static void memalloc_noreclaim_restore(unsigned int old) { noreclaim=old; restores++; }
static void lru_add_drain(void) { drains++; }
static void cond_resched(void) {}
static struct mem_cgroup *mem_cgroup_iter(struct mem_cgroup *root, struct mem_cgroup *prev, void *cookie) {
 (void)root; (void)cookie; int n=prev ? prev->id+1 : 0;
 if (prev) iterator_refs--;
 if (n==3) return NULL;
 iterator_refs++; return &groups[n];
}
static void mem_cgroup_iter_break(struct mem_cgroup *root, struct mem_cgroup *iter) {
 (void)root; assert(iter); iterator_refs--;
}
static void mem_cgroup_calculate_protection(struct mem_cgroup *root, struct mem_cgroup *m) { (void)root; (void)m; }
static int mem_cgroup_below_min(struct mem_cgroup *root, struct mem_cgroup *m) { return root!=m && m->size<=m->min; }
static int mem_cgroup_below_low(struct mem_cgroup *root, struct mem_cgroup *m) { return root!=m && m->size<=m->low; }
static struct lruvec *mem_cgroup_lruvec(struct mem_cgroup *m, pg_data_t *n) { return &vectors[m->id][n->id]; }
static void prepare_scan_count(pg_data_t *n, struct scan_control *s) { (void)n; assert(s->target_mem_cgroup==&groups[0]); }
static void shrink_lruvec(struct lruvec *v, struct scan_control *s) {
 assert(current->reclaim_state == &s->reclaim_state && noreclaim);
 assert(s->xiaomi_lru_mask==observed_mask && s->nr_to_reclaim==expected_target);
 assert(v->memcg->size > v->memcg->min && v->memcg->size > v->memcg->low);
 s->nr_reclaimed+=reclaim_per_call; actual_pages+=reclaim_per_call; calls++;
 if (stop_after && calls==stop_after) fatal=1;
 if (offline_after && calls==offline_after) groups[0].online=0;
}
static int get_tier_idx(struct lruvec *v, int t) { (void)v; (void)t; return 0; }
static int get_type_to_scan(struct lruvec *v, int s, int *tier) { (void)v; (void)s; *tier=0; return 0; }
static int scan_pages(struct lruvec *v, struct scan_control *s, int t, int tier, struct list_head *list) {
 (void)v;(void)s;(void)tier;(void)list;scan_calls[t]++;return scan_result[t];
}
static bool should_run_aging(struct lruvec *v, unsigned long max_seq, unsigned long *min_seq,
 struct scan_control *s, bool swap, unsigned long *nr) {
 (void)v;(void)max_seq;(void)min_seq;(void)s;(void)swap;*nr=99;return need_age;
}
static bool current_is_kswapd(void) { return false; }
static pg_data_t *lruvec_pgdat(struct lruvec *v) { (void)v;return &nodes[0]; }
static int managed_zone(struct zone *z) { (void)z;return 1; }
static unsigned long high_wmark_pages(struct zone *z) { (void)z;return 1; }
static unsigned long low_wmark_pages(struct zone *z) { (void)z;return 1; }
#define NR_FREE_PAGES 0
static unsigned long zone_page_state(struct zone *z,int item) { (void)z;(void)item;return 2; }
static bool try_to_inc_max_seq(struct lruvec *v, unsigned long seq, struct scan_control *s, bool swap, bool force) {
 (void)v;(void)seq;(void)s;(void)swap;(void)force;age_calls++;return true;
}
'''

TESTS = r'''
static void reset(void) {
 memset(groups,0,sizeof(groups));memset(vectors,0,sizeof(vectors));
 fatal=iterator_refs=restores=drains=calls=stop_after=offline_after=0;
 fatal_check=fatal_on_check=0;
 noreclaim=actual_pages=0;current->reclaim_state=NULL;reclaim_per_call=3;
 memset(scan_calls,0,sizeof(scan_calls));memset(scan_result,0,sizeof(scan_result));
 age_calls=0;need_age=1;
 for(int i=0;i<3;i++) { groups[i]=(struct mem_cgroup){i,1,60,100000,0,0,10000};
  for(int n=0;n<2;n++) {vectors[i][n].memcg=&groups[i];
   for(int l=0;l<4;l++) vectors[i][n].sizes[l]=1024;}}
}
int main(void) {
 const unsigned int masks[]={1,3,4,12}; unsigned long checks=0;
 /* API success, no progress, mid-iteration cancellation/offline, protected child. */
 for(int type=0;type<4;type++) for(int mode=0;mode<6;mode++) {
  reset();observed_mask=masks[type];expected_target=20;
  if(mode==1) stop_after=2;
  if(mode==2) offline_after=2;
  if(mode==3) reclaim_per_call=0;
  if(mode==4) groups[1].low=groups[1].size;
  if(mode==5) groups[2].min=groups[2].size;
  unsigned long result=xiaomi_reclaim_memcg(&groups[0],20,type);
  assert(result==actual_pages && iterator_refs==0 && !noreclaim && !current->reclaim_state);
  assert(restores==1 && drains==1);
  if(mode==1 || mode==2) assert(calls==2 && result==6);
  if(mode==3) assert(result==0 && calls==6*(DEF_PRIORITY+1));
  if(mode==0 || mode>=4) assert(result>=20 && result<23);
  checks++;
 }
 for(int mode=0;mode<6;mode++) {
  reset(); if(mode==0) fatal=1;if(mode==1) groups[0].online=0;
  if(mode==2) current->reclaim_state=&task;
  assert(!xiaomi_reclaim_memcg(mode==3?NULL:&groups[0],mode==4?0:1,mode==5?4:0));
  assert(!calls && !drains && !restores && !iterator_refs);checks++;
 }
 /* Signal arriving between MGLRU's two checks must not invent progress. */
 for(int type=0;type<4;type++) {
  reset();struct scan_control s={.xiaomi_lru_mask=masks[type],.nr_to_reclaim=100,
   .nr_reclaimed=7,.priority=1,.target_mem_cgroup=&groups[0]};
  fatal_on_check=2;
  assert(should_abort_scan(&vectors[0][0],0,&s,true));assert(s.nr_reclaimed==7);checks++;
 }
 /* Classic selection must not cross the requested anon/file/active boundary. */
 for(int type=0;type<4;type++) for(int priority=0;priority<=12;priority++)
 for(int swap=0;swap<2;swap++) for(int tiny=0;tiny<2;tiny++) for(int trim=0;trim<2;trim++) {
  reset();struct scan_control s={.xiaomi_lru_mask=masks[type],.may_swap=type<2,
   .priority=priority,.target_mem_cgroup=&groups[0],.file_is_tiny=tiny,.cache_trim_mode=trim};
  groups[0].swap=swap?10000:0;unsigned long nr[4]={99,99,99,99};get_scan_count(&vectors[0][0],&s,nr);
  for(int l=0;l<4;l++) {bool allowed=(masks[type]&BIT(l)) && (l>=2 || swap);
   assert(nr[l]==(allowed ? 1024UL>>priority : 0));checks++;}
 }
 /* With no request, compare actual current function to the unmodified baseline. */
 for(int p=0;p<13;p++) for(int swap=0;swap<2;swap++) for(int sw=0;sw<=200;sw+=10)
 for(int tiny=0;tiny<2;tiny++) for(int trim=0;trim<2;trim++) {
  reset();struct scan_control s={.may_swap=swap,.priority=p,.anon_cost=53,.file_cost=11,
   .target_mem_cgroup=&groups[0],.file_is_tiny=tiny,.cache_trim_mode=trim};
  groups[0].swappiness=sw;unsigned long a[4],b[4];
  get_scan_count(&vectors[0][0],&s,a);baseline_get_scan_count(&vectors[0][0],&s,b);
  assert(!memcmp(a,b,sizeof(a)));checks++;
 }
 /* MGLRU: no fallback, respect swap limits, inactive requests never invoke aging. */
 for(int type=0;type<4;type++) for(int swap=0;swap<2;swap++) for(int found=0;found<2;found++) {
  reset();struct scan_control s={.xiaomi_lru_mask=masks[type],.priority=1,.target_mem_cgroup=&groups[0]};
  struct lruvec *v=&vectors[0][0];int t=-1,wanted=type>=2;struct list_head list;
  scan_result[0]=scan_result[1]=found?7:0;groups[0].swap=swap?10000:0;
  int sw=get_swappiness(v,&s);int result=isolate_pages(v,&s,sw,&t,&list);
  assert(t==wanted && !scan_calls[!wanted]);
  assert(scan_calls[wanted]==(wanted || swap));
  assert(result==((wanted || swap) && found?7:0));checks++;
  v->min_seq[0]=v->min_seq[1]=1;v->max_seq=3;bool aging=false;
  assert(get_nr_to_scan(v,&s,!wanted,&aging)==99);
  assert(age_calls==(type==1 || type==3));assert(aging==(type==1 || type==3));checks++;
  if(type==0 || type==2) {v->max_seq=2;assert(!get_nr_to_scan(v,&s,!wanted,&aging));assert(!age_calls && !aging);checks++;}
 }
 printf("PASS %lu actual-function scoped reclaim checks (host stubs; no runtime reclaim proof)\n",checks);
}
'''


def main():
    baseline = subprocess.check_output(["git", "show", "HEAD:mm/vmscan.c"], cwd=ROOT, text=True)
    pieces = [PRELUDE]
    for name in ("xiaomi_reclaim_mask", "xiaomi_reclaim_lru_allowed", "xiaomi_reclaim_file", "xiaomi_reclaim_active",
                 "get_scan_count", "get_swappiness", "isolate_pages", "get_nr_to_scan", "should_abort_scan", "xiaomi_reclaim_memcg"):
        pieces.append(function(SOURCE, name))
    pieces.append(function(baseline, "get_scan_count").replace("get_scan_count(", "baseline_get_scan_count(", 1))
    pieces.append(TESTS)
    with tempfile.TemporaryDirectory(prefix="venus-reclaim-") as temporary:
        path = pathlib.Path(temporary)
        (path / "test.c").write_text("\n".join(pieces))
        subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-fno-omit-frame-pointer", "-fsanitize=address,undefined",
                        "-no-pie", str(path / "test.c"), "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    main()
