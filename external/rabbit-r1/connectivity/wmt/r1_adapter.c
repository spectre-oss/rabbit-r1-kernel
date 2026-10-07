// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/sched/debug.h>
#include <linux/sched/signal.h>
#include <linux/mmc/card.h>
void connectivity_export_show_stack(struct task_struct *task, unsigned long *sp)
{
    sched_show_task(task ? task : current);
}
void connectivity_export_dump_thread_state(const char *name)
{
    struct task_struct *task;
    if (!name) return;
    rcu_read_lock();
    for_each_process(task) {
        if (!strncmp(task->comm, name, TASK_COMM_LEN)) sched_show_task(task);
    }
    rcu_read_unlock();
}
int connectivity_export_mmc_io_rw_direct(struct mmc_card *card, int write,
    unsigned int fn, unsigned int addr, u8 in, u8 *out)
{
    /* R1 uses AXI/BTIF. Never report success for unsupported SDIO I/O. */
    return -EOPNOTSUPP;
}

EXPORT_SYMBOL_GPL(connectivity_export_show_stack);
