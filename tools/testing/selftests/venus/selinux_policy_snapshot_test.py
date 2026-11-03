#!/usr/bin/env python3
"""Exercise the actual policy writer and RCU-policy snapshot function.

The policy tree serializer is a deterministic environment stub using the real
put_entry(). The caller holds policy_mutex across both serialization passes;
policy changes are simulated between separate reads, never inside a snapshot.
This checks sizing, bytes and ownership, not SELinux permission decisions.
"""
import pathlib
import shutil
import subprocess
import tempfile

from scoped_reclaim_test import function

ROOT = pathlib.Path(__file__).resolve().parents[4]

PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

#define check_add_overflow __builtin_add_overflow
#define check_mul_overflow __builtin_mul_overflow
#define lockdep_is_held(lock) (*(lock) == 1)
#define lockdep_assert_held(lock) assert(lockdep_is_held(lock))
#define rcu_dereference_protected(pointer, condition) \
    ({ assert(condition); (pointer); })

struct policy_file { char *data; size_t len; };
struct policydb { size_t len, serialized_len; unsigned char seed; };
struct selinux_policy { struct policydb policydb; };
struct selinux_state { int policy_mutex; struct selinux_policy *policy; };

static struct selinux_policy policy;
static struct selinux_state state;
static int count_walks, write_walks, alloc_calls, free_calls;
static int count_error, write_error;
static bool fail_alloc, inside_snapshot, count_overflow;
static size_t fail_write_after, allocated_size;
static void *allocation;
static unsigned long checks;

static void mutex_lock(int *lock) { assert(!*lock); *lock = 1; }
static void mutex_unlock(int *lock) { assert(*lock == 1); *lock = 0; }

static void *vmalloc_user(size_t size)
{
    assert(inside_snapshot && state.policy_mutex == 1);
    assert(!allocation);
    alloc_calls++;
    allocated_size = size;
    if (fail_alloc)
        return NULL;
    assert(size && size < 1024 * 1024);
    allocation = calloc(1, size);
    assert(allocation);
    return allocation;
}

static void vfree(void *data)
{
    if (inside_snapshot)
        assert(state.policy_mutex == 1);
    if (!data)
        return;
    assert(data == allocation);
    free_calls++;
    free(data);
    allocation = NULL;
}
'''

SERIALIZER = r'''
/* Stand in for the policy tree walk; all byte accounting uses real put_entry. */
static int policydb_write(struct policydb *database, void *output)
{
    struct policy_file *fp = output;
    unsigned char chunk[67];
    bool counting = !fp->data;
    size_t done = 0;

    assert(inside_snapshot && state.policy_mutex == 1);
    assert(database == &state.policy->policydb);
    if (counting) {
        count_walks++;
        if (count_error)
            return count_error;
        if (count_overflow) {
            int rc = put_entry(chunk, SIZE_MAX, 1, fp);

            assert(!rc && fp->len == SIZE_MAX);
            return put_entry(chunk, 1, 1, fp);
        }
    } else {
        write_walks++;
    }

    while (done < database->serialized_len) {
        size_t size = database->serialized_len - done;
        int rc;

        if (!counting && write_error && done >= fail_write_after)
            return write_error;
        if (size > sizeof(chunk))
            size = sizeof(chunk);
        for (size_t i = 0; i < size; i++)
            chunk[i] = (unsigned char)(database->seed + done + i);
        rc = put_entry(chunk, 1, (int)size, fp);
        if (rc)
            return rc;
        done += size;
    }
    return 0;
}
'''

TESTS = r'''
static void reset(void)
{
    assert(!allocation && !inside_snapshot && !state.policy_mutex);
    policy.policydb = (struct policydb){ .len = 8, .serialized_len = 5000, .seed = 19 };
    state.policy = &policy;
    count_walks = write_walks = alloc_calls = free_calls = 0;
    count_error = write_error = 0;
    fail_alloc = count_overflow = false;
    fail_write_after = allocated_size = 0;
}

static int snapshot(void **data, size_t *len)
{
    int rc;

    assert(state.policy_mutex == 1);
    *data = (void *)(uintptr_t)1;
    *len = SIZE_MAX;
    inside_snapshot = true;
    rc = security_read_policy(&state, data, len);
    inside_snapshot = false;
    assert(state.policy_mutex == 1); /* The callee must not drop our lock. */
    if (rc)
        assert(!*data && !*len && !allocation);
    checks++;
    return rc;
}

static void check_bytes(const void *data, size_t len, unsigned char seed)
{
    const unsigned char *bytes = data;

    for (size_t i = 0; i < len; i++)
        assert(bytes[i] == (unsigned char)(seed + i));
}

static void check_success(void)
{
    void *data;
    size_t len;
    size_t stale_len = policy.policydb.len;

    mutex_lock(&state.policy_mutex);
    assert(!snapshot(&data, &len));
    assert(data == allocation && len == policy.policydb.serialized_len);
    assert(allocated_size == len); /* No allocation from stale policydb.len. */
    assert(count_walks == 1 && write_walks == 1 && alloc_calls == 1 && !free_calls);
    assert(policy.policydb.len == stale_len);
    check_bytes(data, len, policy.policydb.seed);
    mutex_unlock(&state.policy_mutex);
    vfree(data);
    assert(!allocation && free_calls == 1);
}

static void test_put_entry(void)
{
    unsigned char source[16], dest[16], before[16];
    struct policy_file fp = { 0 };

    for (size_t i = 0; i < sizeof(source); i++)
        source[i] = (unsigned char)i;
    assert(!put_entry(source, 1, 16, &fp) && fp.len == 16 && !fp.data);
    assert(!put_entry(source, 1, 3, &fp) && fp.len == 19);
    assert(!put_entry(source, SIZE_MAX, 0, &fp) && fp.len == 19);
    fp.len = SIZE_MAX - 1;
    assert(put_entry(source, 1, 2, &fp) == -EOVERFLOW && fp.len == SIZE_MAX - 1);
    assert(!put_entry(source, 1, 1, &fp) && fp.len == SIZE_MAX);
    fp.len = 9;
    assert(put_entry(source, SIZE_MAX, 2, &fp) == -EOVERFLOW && fp.len == 9);
    assert(put_entry(source, 1, -1, &fp) == -EOVERFLOW && fp.len == 9);

    memset(dest, 0xa5, sizeof(dest));
    memcpy(before, dest, sizeof(before));
    fp = (struct policy_file){ (char *)dest, 15 };
    assert(put_entry(source, 1, 16, &fp) == -EINVAL);
    assert(fp.data == (char *)dest && fp.len == 15 && !memcmp(dest, before, sizeof(dest)));
    assert(put_entry(source, SIZE_MAX, 2, &fp) == -EOVERFLOW);
    assert(put_entry(source, 1, -1, &fp) == -EOVERFLOW);
    assert(fp.data == (char *)dest && fp.len == 15 && !memcmp(dest, before, sizeof(dest)));
    fp.len = sizeof(dest);
    assert(!put_entry(source, 1, 7, &fp));
    assert(!put_entry(source + 7, 1, 9, &fp));
    assert(fp.data == (char *)dest + sizeof(dest) && !fp.len);
    assert(!memcmp(source, dest, sizeof(dest)));
    checks++;
}

static void test_errors(void)
{
    void *data;
    size_t len;

    for (int variant = 0; variant < 7; variant++) {
        int expected;

        reset();
        switch (variant) {
        case 0: state.policy = NULL; expected = -EINVAL; break; /* Not initialized. */
        case 1: count_error = -EIO; expected = -EIO; break;
        case 2: policy.policydb.serialized_len = 0; expected = -EINVAL; break;
        case 3: fail_alloc = true; expected = -ENOMEM; break;
        case 4: write_error = -EINVAL; expected = -EINVAL; break;
        case 5: write_error = -EIO; fail_write_after = 134; expected = -EIO; break;
        default: count_overflow = true; expected = -EOVERFLOW; break;
        }
        mutex_lock(&state.policy_mutex);
        assert(snapshot(&data, &len) == expected);
        mutex_unlock(&state.policy_mutex);
        assert(count_walks == (variant != 0));
        assert(write_walks == (variant == 4 || variant == 5));
        assert(alloc_calls == (variant >= 3 && variant <= 5));
        assert(free_calls == (variant == 4 || variant == 5));
    }
}

static void test_policy_updates(void)
{
    static const size_t sizes[] = { 23, 5000, 11000, 3 };

    reset();
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        mutex_lock(&state.policy_mutex);
        /* Publish a content change between reads while loaded len stays stale. */
        policy.policydb.serialized_len = sizes[i];
        policy.policydb.seed = (unsigned char)(i * 31);
        mutex_unlock(&state.policy_mutex);
        count_walks = write_walks = alloc_calls = free_calls = 0;
        check_success();
    }
}

int main(void)
{
    static const size_t stale_lengths[] = { 0, 1, 8, 5000, 20000, SIZE_MAX };

    test_put_entry();
    for (size_t i = 0; i < sizeof(stale_lengths) / sizeof(stale_lengths[0]); i++) {
        reset();
        policy.policydb.len = stale_lengths[i];
        check_success();
    }
    test_errors();
    test_policy_updates();
    for (size_t size = 1; size < 10000; size += 13) {
        reset();
        policy.policydb.serialized_len = size;
        policy.policydb.seed = (unsigned char)size;
        check_success();
    }
    assert(!allocation && !state.policy_mutex && !inside_snapshot);
    printf("PASS %lu actual SELinux snapshot checks: exact sizing, stale len, "
           "growth/shrink between reads, mutex ownership, overflow, allocation/write "
           "failure cleanup, uninitialized policy and byte preservation\n", checks);
    return 0;
}
'''


def main():
    compiler = shutil.which("clang") or shutil.which("clang-14")
    if not compiler:
        raise RuntimeError("Clang is required for the ASan/UBSan snapshot selftest")
    header = (ROOT / "security/selinux/ss/policydb.h").read_text()
    services = (ROOT / "security/selinux/ss/services.c").read_text()
    source = "\n".join((PRELUDE, function(header, "put_entry"), SERIALIZER,
                        function(services, "security_read_policy"), TESTS))
    with tempfile.TemporaryDirectory(prefix="venus-policy-snapshot-") as temporary:
        path = pathlib.Path(temporary)
        (path / "test.c").write_text(source)
        subprocess.run([compiler, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fno-omit-frame-pointer", "-fsanitize=address,undefined", "-no-pie",
                        str(path / "test.c"), "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    main()
