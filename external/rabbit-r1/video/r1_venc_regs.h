/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef R1_VENC_REGS_H
#define R1_VENC_REGS_H

/* MT6765 encoder register offsets, derived from stock libvcodecdrv.so.
 * These declarations are not a tested hardware initialization sequence.
 * The encoder block is 0x17020000; GCON selector is a separate resource.
 */
#define R1_VENC_CONTROL          0x000
#define R1_VENC_SEQUENCE0        0x004
#define R1_VENC_SEQUENCE1        0x008
#define R1_VENC_SEQUENCE2        0x00c
#define R1_VENC_FRAME0           0x010
#define R1_VENC_FRAME1           0x014
#define R1_VENC_FRAME2           0x018
#define R1_VENC_FRAME3           0x01c
#define R1_VENC_GEOMETRY         0x024
#define R1_VENC_STRIDE_FORMAT    0x028
#define R1_VENC_IRQ_ENABLE       0x058
#define R1_VENC_IRQ_STATUS       0x05c
#define R1_VENC_IRQ_ACK          0x060
#define R1_VENC_BS_ADDRESS       0x064 /* address / 16 */
#define R1_VENC_BS_CAPACITY      0x068 /* bytes / 128 */
#define R1_VENC_INPUT_Y          0x06c /* address / 16 */
#define R1_VENC_INPUT_U          0x070 /* address / 16 */
#define R1_VENC_REFERENCE_Y      0x074 /* address / 16 */
#define R1_VENC_REFERENCE_C      0x078 /* address / 16 */
#define R1_VENC_RECONSTRUCT_Y    0x07c /* address / 16 */
#define R1_VENC_RECONSTRUCT_C    0x080 /* address / 16 */
#define R1_VENC_MV_BUFFER0       0x084 /* address / 16 */
#define R1_VENC_MV_BUFFER1       0x088 /* address / 16 */
#define R1_VENC_RC_CODE          0x08c /* address / 16 */
#define R1_VENC_RC_DATA          0x090 /* address / 16 */
#define R1_VENC_INPUT_V          0x094 /* MT6765: address / 16 */
#define R1_VENC_BS_COUNT         0x098 /* bytes */
#define R1_VENC_IRQ_MASK         0x0a4
#define R1_VENC_RESET           0x0a8
#define R1_VENC_PAUSE           0x0ac
#define R1_VENC_START           0x0ec

#define R1_VENC_IRQ_SPS          (1U << 0)
#define R1_VENC_IRQ_PPS          (1U << 1)
#define R1_VENC_IRQ_FRAME        (1U << 2)
#define R1_VENC_IRQ_DRAM         (1U << 3)
#define R1_VENC_IRQ_PAUSE        (1U << 4)
#define R1_VENC_IRQ_SWITCH       (1U << 5)

/* LARB1 ports 5/6 belong to JPEG and must remain independently managed. */
#define R1_VENC_IOMMU_PORTS \
	32, 33, 34, 35, 36, 39, 40, 41, 42

#endif
