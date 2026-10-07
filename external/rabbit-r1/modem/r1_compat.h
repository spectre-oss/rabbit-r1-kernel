/* SPDX-License-Identifier: GPL-2.0 */
#ifndef R1_CCCI_COMPAT_H
#define R1_CCCI_COMPAT_H
#include <linux/types.h>
#include <linux/timekeeping.h>
#include <linux/sched/clock.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <asm/cacheflush.h>
/* Preserve legacy CCCI userspace wire sizes (arm64 timeval=16, compat=8). */
struct r1_timeval { long tv_sec; long tv_usec; };
struct r1_compat_timeval { s32 tv_sec; s32 tv_usec; };
static_assert(sizeof(struct r1_timeval) == 16);
static_assert(sizeof(struct r1_compat_timeval) == 8);
static inline void r1_gettimeofday(struct r1_timeval *tv)
{
 struct timespec64 ts;
 ktime_get_real_ts64(&ts);
 tv->tv_sec = ts.tv_sec;
 tv->tv_usec = ts.tv_nsec / 1000;
}
static inline void r1_inval_dcache_area(const void *p, size_t size)
{
 dcache_inval_poc((unsigned long)p, (unsigned long)p + size);
}
/* Stock MT6765 SIP TRNG function, same 64-bit convention as vendor. */
#define MTK_SIP_KERNEL_GET_RND 0xc200026a
#endif
