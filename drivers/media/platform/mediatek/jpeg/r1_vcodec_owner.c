// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include "r1_vcodec_owner.h"

/* MT6765 has one shared codec selector, including separate JPEG/H.264 engines.
 * A driver-private mutex cannot serialize another driver's check/write pair.
 * Do not hold a task-owned mutex across runtime-PM callbacks; record ownership
 * here and protect only the state transitions with the mutex.
 */
static DEFINE_MUTEX(owner_lock);
static const void *codec_owner;
static struct regmap *codec_map;
static unsigned int saved_mode;

static int try_acquire(struct regmap *map, const void *owner, unsigned int selector)
{
 unsigned int value, mode;
 int ret;
 if (!map || !owner || (selector != 1 && selector != 2 && selector != 4))
  return -EINVAL;
 mutex_lock(&owner_lock);
 if (codec_owner) { ret = -EBUSY; goto out; }
 ret = regmap_read(map, 0x20, &value);
 if (ret) goto out;
 if (value) { ret = -EBUSY; goto out; }
 ret = regmap_read(map, 0x10, &mode);
 if (ret) goto out;
 /* Set mode first, so failure cannot leave an enabled, unowned engine. */
 /* Stock MT6765 lockhw: selector 2 is decode, selectors 1/4 encode. */
 ret = regmap_update_bits(map, 0x10, BIT(0), selector == 2 ? BIT(0) : 0);
 if (ret) goto out;
 ret = regmap_write(map, 0x20, selector);
 if (ret) {
  regmap_update_bits(map, 0x10, BIT(0), mode & BIT(0));
  goto out;
 }
 codec_owner = owner;
 codec_map = map;
 saved_mode = mode & BIT(0);
out:
 mutex_unlock(&owner_lock);
 return ret;
}
int r1_vcodec_acquire(struct regmap *map, const void *owner, unsigned int selector)
{
 unsigned long deadline = jiffies + msecs_to_jiffies(1000);
 int ret;
 do {
  ret = try_acquire(map, owner, selector);
  if (ret != -EBUSY) return ret;
  /* Never hold owner_lock while waiting for the other engine to finish. */
  usleep_range(1000, 2000);
 } while (time_before(jiffies, deadline));
 return -EBUSY;
}
EXPORT_SYMBOL_GPL(r1_vcodec_acquire);

int r1_vcodec_release(struct regmap *map, const void *owner)
{
 int ret;
 mutex_lock(&owner_lock);
 if (!owner || codec_owner != owner || codec_map != map) {
  ret = -EPERM;
  goto out;
 }
 ret = regmap_write(map, 0x20, 0);
 if (ret) goto out;
 ret = regmap_update_bits(map, 0x10, BIT(0), saved_mode);
 if (ret) goto out;
 codec_owner = NULL;
 codec_map = NULL;
out:
 mutex_unlock(&owner_lock);
 return ret;
}
EXPORT_SYMBOL_GPL(r1_vcodec_release);
MODULE_LICENSE("GPL");
MODULE_VERSION("2");
MODULE_DESCRIPTION("Rabbit R1 shared JPEG/H264 codec ownership");
