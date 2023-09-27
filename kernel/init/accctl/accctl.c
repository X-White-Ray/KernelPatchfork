#include "accctl.h"

#include <taskext.h>
#include <linux/spinlock.h>
#include <linux/capability.h>
#include <linux/security.h>
#include <asm/current.h>
#include <linux/pid.h>
#include <linux/sched/task.h>
#include <linux/sched.h>
#include <pgtable.h>
#include <ksyms.h>
#include <error.h>

int make_cred_su(struct cred *cred, const char *sctx)
{
    *(kernel_cap_t *)((uintptr_t)cred + cred_offset.cap_inheritable_offset) = full_cap;
    *(kernel_cap_t *)((uintptr_t)cred + cred_offset.cap_permitted_offset) = full_cap;
    *(kernel_cap_t *)((uintptr_t)cred + cred_offset.cap_effective_offset) = full_cap;
    *(kernel_cap_t *)((uintptr_t)cred + cred_offset.cap_bset_offset) = full_cap;
    *(kernel_cap_t *)((uintptr_t)cred + cred_offset.cap_ambient_offset) = full_cap;

    *(uid_t *)((uintptr_t)cred + cred_offset.uid_offset) = 0;
    *(uid_t *)((uintptr_t)cred + cred_offset.euid_offset) = 0;
    *(uid_t *)((uintptr_t)cred + cred_offset.fsuid_offset) = 0;
    *(uid_t *)((uintptr_t)cred + cred_offset.suid_offset) = 0;

    *(uid_t *)((uintptr_t)cred + cred_offset.gid_offset) = 0;
    *(uid_t *)((uintptr_t)cred + cred_offset.egid_offset) = 0;
    *(uid_t *)((uintptr_t)cred + cred_offset.fsgid_offset) = 0;
    *(uid_t *)((uintptr_t)cred + cred_offset.sgid_offset) = 0;

    if (sctx) {
        int rc = set_security_override_from_ctx(cred, sctx);
        if (rc)
            logkw("set sctx: %s\n", sctx);
        return rc;
    }
    return 0;
}

int commit_kernel_cred()
{
    int rc = 0;
    struct task_struct *task = current;
    struct task_ext *ext = get_task_ext(task);
    if (!task_ext_valid(ext))
        goto out;

    const struct cred *old = get_task_cred(task);
    struct cred *new = prepare_kernel_cred(0);
    u32 secid;
    if (kfunc_def(security_cred_getsecid)) {
        kfunc_def(security_cred_getsecid)(old, &secid);
        set_security_override(new, secid);
    }
    commit_creds(new);

out:
    return rc;
}

int commit_su(int super, const char *sctx)
{
    int rc = 0;
    struct task_struct *task = current;
    struct task_ext *ext = get_task_ext(task);
    if (!task_ext_valid(ext)) {
        logkfe("dirty task_ext, pid(maybe dirty): %d\n", ext->pid);
        rc = ERR_DIRTY_EXT;
        goto out;
    }

    ext->super = super;
    ext->selinux_allow = 1;

    struct cred *new = prepare_creds();

    rc = make_cred_su(new, sctx);
    if (rc)
        goto out;

    commit_creds(new);

    ext->selinux_allow = !sctx;
    // todo: ranchu-4.4.302, ranchu-4.14.175, and ..., BUG: recent printk recursion!
    // logkfi("pid: %d, tgid: %d\n", ext->pid, ext->tgid);
out:
    return rc;
}

int effect_su_unsafe(const char *sctx)
{
    int rc = 0;
    struct task_struct *task = current;
    struct task_ext *ext = get_task_ext(task);
    if (!task_ext_valid(ext)) {
        logkfe("dirty task_ext, pid(maybe dirty): %d\n", ext->pid);
        rc = ERR_DIRTY_EXT;
        goto out;
    }
    ext->selinux_allow = 1;
    struct cred *cred = *(struct cred **)((uintptr_t)task + task_struct_offset.cred_offset);
    rc = make_cred_su(cred, sctx);
    if (rc)
        goto out;
    ext->selinux_allow = !sctx;
    // logkfi("pid: %d, tgid: %d\n", ext->pid, ext->tgid);
out:
    return rc;
}

// todo: cow, rcu
int thread_su(pid_t vpid, const char *sctx)
{
    int rc = 0;
    struct task_struct *task = find_get_task_by_vpid(vpid);
    if (!task) {
        rc = ERR_NO_SUCH_ID;
        goto out;
    }
    struct task_ext *ext = get_task_ext(task);

    if (!task_ext_valid(ext)) {
        logkfe("dirty task_ext, pid(maybe dirty): %d\n", ext->pid);
        rc = ERR_DIRTY_EXT;
        goto out;
    }

    struct cred *cred = *(struct cred **)((uintptr_t)task + task_struct_offset.cred_offset);
    rc = make_cred_su(cred, sctx);
    if (rc)
        goto out;
    struct cred *real_cred = *(struct cred **)((uintptr_t)task + task_struct_offset.real_cred_offset);
    if (cred != real_cred) {
        rc = make_cred_su(real_cred, sctx);
        if (rc)
            goto out;
    }

    ext->selinux_allow = !sctx;

    logkfi("pid: %d, tgid: %d\n", ext->pid, ext->tgid);
out:
    __put_task_struct(task);
    return rc;
}
