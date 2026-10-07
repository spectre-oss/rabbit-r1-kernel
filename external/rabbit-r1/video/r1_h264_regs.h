/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef R1_H264_REGS_H
#define R1_H264_REGS_H
#include <linux/v4l2-controls.h>

/* Register packing from stock H264_HAL_SetSPSAVLD. Validation and supported
 * profile checks belong to the request path before this pure conversion. */
struct r1_h264_sps_regs {
 __u32 avcvld_88, vldtop_68, vldtop_100;
};
static inline struct r1_h264_sps_regs
r1_h264_pack_sps(const struct v4l2_ctrl_h264_sps *s)
{
 struct r1_h264_sps_regs r;
 r.avcvld_88 = (s->chroma_format_idc & 3) |
  ((s->log2_max_frame_num_minus4 & 15) << 2) |
  ((s->pic_order_cnt_type & 3) << 6) |
  ((s->log2_max_pic_order_cnt_lsb_minus4 & 15) << 8) |
  (!!(s->flags & V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO) << 12) |
  ((s->max_num_ref_frames & 31) << 13) |
  (!!(s->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY) << 18) |
  (!!(s->flags & V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD) << 19) |
  (!!(s->flags & V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE) << 20) | (1U << 21);
 r.vldtop_68 = s->pic_width_in_mbs_minus1 |
  ((__u32)s->pic_height_in_map_units_minus1 << 16);
 r.vldtop_100 = (s->bit_depth_luma_minus8 & 15) |
  ((s->bit_depth_chroma_minus8 & 15) << 4);
 return r;
}
struct r1_h264_pps_regs { __u32 avcvld_90, avcvld_94; };
static inline struct r1_h264_pps_regs
r1_h264_pack_pps(const struct v4l2_ctrl_h264_pps *p)
{
 struct r1_h264_pps_regs r;
 r.avcvld_90 = (!!(p->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE)) |
  (!!(p->flags & V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT) << 1) |
  (!!(p->flags & V4L2_H264_PPS_FLAG_WEIGHTED_PRED) << 2) |
  ((p->weighted_bipred_idc & 3) << 3) |
  ((p->pic_init_qp_minus26 & 63) << 5) |
  ((p->chroma_qp_index_offset & 31) << 11) |
  (!!(p->flags & V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT) << 16) |
  (!!(p->flags & V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED) << 17) |
  (!!(p->flags & V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE) << 18) |
  ((p->second_chroma_qp_index_offset & 31) << 19);
 /* Slice-group/scaling-list state is separate, not represented here. */
 r.avcvld_94 = p->num_ref_idx_l0_default_active_minus1 |
  ((__u32)p->num_ref_idx_l1_default_active_minus1 << 5);
 if(p->flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT)r.avcvld_94|=0x7fc00;
 return r;
}
struct r1_h264_slice_regs { __u32 avcvld_8c, avcvld_98, avcvld_9c; };
static inline struct r1_h264_slice_regs r1_h264_pack_slice(
 const struct v4l2_ctrl_h264_slice_params *s,
 const struct v4l2_ctrl_h264_decode_params *d, __u32 old8c, __u32 old98)
{
 struct r1_h264_slice_regs r;
 r.avcvld_8c = (old8c & ~0x1ffffU) | (s->first_mb_in_slice & 0x1fff);
 r.avcvld_98 = (old98 & 0x1fff) | ((s->slice_type & 15) << 13) |
  (!!(d->flags & V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC) << 17) |
  (!!(d->flags & V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD) << 18) |
  (!!(s->flags & V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED) << 19) |
  ((s->num_ref_idx_l0_active_minus1 & 31) << 20) |
  ((s->num_ref_idx_l1_active_minus1 & 31) << 25);
 r.avcvld_9c = (s->slice_qp_delta & 127) |
  ((s->disable_deblocking_filter_idc & 3) << 7) |
  ((s->slice_alpha_c0_offset_div2 & 15) << 9) |
  ((s->slice_beta_offset_div2 & 15) << 13) |
  ((d->nal_ref_idc & 3) << 17) |
  (!!(d->flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC) << 19) |
  ((s->cabac_init_idc & 3) << 20);
 return r;
}
/* Progressive short-term PicNum is signed across frame_num wrap. The stock
 * reference register reserves bit19 for long-term status: negative values
 * must be truncated to19 bits, not written as a full-width signed integer.
 * Callers validate SPS/frame_num bounds before invoking this helper. */
static inline __u32 r1_h264_p_reference_number(__u32 reference, __u32 current_frame,
                                             __u32 log2_minus4)
{
 __u32 maximum=1U<<(log2_minus4+4);
 return (reference>current_frame ? reference-maximum : reference)&0x7ffffU;
}
/* Progressive P reference descriptor, audited against the stock HAL. */
struct r1_h264_ref_regs { __u32 mc_offset, frame_offset, mv_offset, address, frame_num, top, bottom; };
static inline struct r1_h264_ref_regs r1_h264_pack_reference(
 unsigned int slot, unsigned int identity, __u32 address, const struct v4l2_h264_dpb_entry *ref)
{
 __u32 order=ref->top_field_order_cnt<=ref->bottom_field_order_cnt?1U<<21:0;
 __u32 long_term=!!(ref->flags&V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM);
 __u32 number=long_term ? (ref->pic_num&0x7ffffU)|(1U<<19) : ref->frame_num;
 order|=long_term<<20;
 return (struct r1_h264_ref_regs){
  0x13dc+slot*4,0x20a8+slot*4,0x3000+slot*8,address,number,
  (identity<<23)|order|((__u32)ref->top_field_order_cnt&0x7ffffU),(identity<<23)|(1U<<22)|order|((__u32)ref->bottom_field_order_cnt&0x7ffffU)};
}
/* Progressive B-list pair, including collocated list1 vectors. */
struct r1_h264_bref_regs { __u32 offset[10], value[10]; };
static inline struct r1_h264_bref_regs r1_h264_pack_b_reference(
 unsigned int slot, unsigned int identity0, unsigned int identity1,
 __u32 address0, __u32 address1, __u32 mv1,
 const struct v4l2_h264_dpb_entry *a, const struct v4l2_h264_dpb_entry *b)
{
 __u32 order0=a->top_field_order_cnt<=a->bottom_field_order_cnt?1U<<21:0;
 __u32 order1=b->top_field_order_cnt<=b->bottom_field_order_cnt?1U<<21:0;
 __u32 long0=!!(a->flags&V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM);
 __u32 long1=!!(b->flags&V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM);
 order0|=long0<<20;order1|=long1<<20;
 return (struct r1_h264_bref_regs){
  {0x145c+slot*4,0x2128+slot*4,0x3080+slot*8,0x3084+slot*8,
   0x14dc+slot*4,0x21a8+slot*4,0x3100+slot*8,0x3104+slot*8,
   0x3180+slot*8,0x3184+slot*8},
  {address0,long0?((a->pic_num&0x7ffffU)|(1U<<19)):a->frame_num,(identity0<<23)|order0|(a->top_field_order_cnt&0x3ffff),
   (identity0<<23)|(1U<<22)|order0|(a->bottom_field_order_cnt&0x3ffff),
   address1,long1?((b->pic_num&0x7ffffU)|(1U<<19)):b->frame_num,(identity1<<23)|order1|(b->top_field_order_cnt&0x3ffff),
   (identity1<<23)|(1U<<22)|order1|(b->bottom_field_order_cnt&0x3ffff),
   mv1>>4,(mv1>>4)+4}};
}
static inline __u32 r1_h264_scaling_word(const __u8 *bytes)
{ return ((__u32)bytes[0]<<24)|((__u32)bytes[1]<<16)|((__u32)bytes[2]<<8)|bytes[3]; }
#endif
