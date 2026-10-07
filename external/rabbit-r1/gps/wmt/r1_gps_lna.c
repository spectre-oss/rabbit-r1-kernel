/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/mutex.h>
static DEFINE_MUTEX(r1_gps_lna_lock);
static struct gpio_desc *r1_gps_lna;
static struct gpiod_lookup_table r1_gps_lna_lookup = {
    .dev_id = NULL,
    .table = { GPIO_LOOKUP("pinctrl_paris", 177, "r1-gps-lna", GPIO_ACTIVE_HIGH), {} },
};
int r1_gps_lna_set(bool high)
{
    int ret = 0;
    mutex_lock(&r1_gps_lna_lock);
    if (high && !r1_gps_lna) {
        gpiod_add_lookup_table(&r1_gps_lna_lookup);
        r1_gps_lna = gpiod_get(NULL, "r1-gps-lna", GPIOD_OUT_LOW);
        gpiod_remove_lookup_table(&r1_gps_lna_lookup);
        if (IS_ERR(r1_gps_lna)) {
            ret = PTR_ERR(r1_gps_lna);
            r1_gps_lna = NULL;
            goto out;
        }
    }
    if (r1_gps_lna) {
        gpiod_set_value_cansleep(r1_gps_lna, high);
        if (!high) {
            gpiod_put(r1_gps_lna);
            r1_gps_lna = NULL;
        }
    }
out:
    mutex_unlock(&r1_gps_lna_lock);
    return ret;
}
