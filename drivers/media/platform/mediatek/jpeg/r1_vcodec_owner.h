/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef R1_VCODEC_OWNER_H
#define R1_VCODEC_OWNER_H
struct regmap;
/* Call with codec clocks enabled. Ownership lasts until release succeeds.
 * May sleep up to one second for contention, then returns -EBUSY. Call only
 * while holding this codec's resources, never another codec's PM reference.
 */
int r1_vcodec_acquire(struct regmap *map, const void *owner, unsigned int selector);
int r1_vcodec_release(struct regmap *map, const void *owner);
#endif
