/* SPDX-License-Identifier: GPL-2.0-only */
/* Host port audit: legacy external-combo GPIOs fail closed. */
#include <linux/of.h>
#include <linux/errno.h>
static inline int r1_unsupported_named_gpio(struct device_node *node, const char *name, int index)
{
    if (!of_find_property(node, name, NULL)) return -ENOENT;
    pr_err("r1-wmt: legacy GPIO property %s needs descriptor conversion\n", name);
    return -EOPNOTSUPP;
}
#define of_get_named_gpio r1_unsupported_named_gpio
