/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef R1_CPU_VOLTAGE_H
#define R1_CPU_VOLTAGE_H
#define R1_VPROC_MIN 775000
#define R1_VPROC_MAX 800000
#define R1_SRAM_DELTA 100000
/* One ordered regulator operation. A failed operation may stop here safely.
 * Return 0 when done, 1 for VPROC, 2 for VSRAM, -1 for an invalid state.
 */
static inline int r1_voltage_next(int vp, int vs, int target, int *value)
{
 int sram = target + R1_SRAM_DELTA;
 if (vp < R1_VPROC_MIN || vp > R1_VPROC_MAX ||
     vs < R1_VPROC_MIN + R1_SRAM_DELTA ||
     vs > R1_VPROC_MAX + R1_SRAM_DELTA ||
     vs - vp < R1_SRAM_DELTA || vs - vp > 125000 ||
     target < R1_VPROC_MIN || target > R1_VPROC_MAX ||
     vp % 6250 || vs % 6250 || target % 6250)
  return -1;
 if (vs < sram) { *value = sram; return 2; }
 if (vp != target) { *value = target; return 1; }
 if (vs != sram) { *value = sram; return 2; }
 return 0;
}
#endif
