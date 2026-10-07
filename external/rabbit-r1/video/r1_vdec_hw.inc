/* SPDX-License-Identifier: GPL-2.0-only */
#include "r1_h264_limits.h"
#include "r1_h264_lists.h"
/* Frame-only hardware setup; caller validates geometry and buffer sizes. */
struct r1_h264_job {
 struct v4l2_ctrl_h264_sps sps;
 struct v4l2_ctrl_h264_pps pps;
 struct v4l2_ctrl_h264_slice_params slice;
 struct v4l2_ctrl_h264_decode_params dec;
 struct v4l2_ctrl_h264_scaling_matrix scaling;
 struct v4l2_ctrl_h264_pred_weights weights;
 bool picture_has_p,picture_has_b;
 unsigned int p_slot[16];
 unsigned int default_lists[2][17],reference_count;
 __u8 reference_identity[2][V4L2_H264_NUM_DPB_ENTRIES];
 dma_addr_t reference_pixels[2][V4L2_H264_NUM_DPB_ENTRIES];
 dma_addr_t reference_mv[V4L2_H264_NUM_DPB_ENTRIES];
};
/* Current backend limits, to be expanded with hardware evidence. */
static int validate_job(const struct r1_h264_job *j)
{
 const struct v4l2_ctrl_h264_sps *s = &j->sps;
 const struct v4l2_ctrl_h264_pps *p = &j->pps;
 const struct v4l2_ctrl_h264_slice_params *sl = &j->slice;
 if (!r1_h264_geometry_fits((s->pic_width_in_mbs_minus1+1U)*16,(s->pic_height_in_map_units_minus1+1U)*16) ||
     s->log2_max_frame_num_minus4 > 12 || s->log2_max_pic_order_cnt_lsb_minus4 > 12 ||
     s->pic_order_cnt_type > 2 || j->dec.nal_ref_idc > 3 ||
     j->dec.frame_num >= (1U << (s->log2_max_frame_num_minus4 + 4)) ||
     (sl->slice_type != V4L2_H264_SLICE_TYPE_I && sl->ref_pic_list0[0].index >= V4L2_H264_NUM_DPB_ENTRIES) ||
     sl->disable_deblocking_filter_idc > 2 || sl->slice_qp_delta < -51 || sl->slice_qp_delta > 51 ||
     sl->slice_alpha_c0_offset_div2 < -6 || sl->slice_alpha_c0_offset_div2 > 6 ||
     sl->slice_beta_offset_div2 < -6 || sl->slice_beta_offset_div2 > 6 ||
     p->pic_init_qp_minus26 < -26 || p->pic_init_qp_minus26 > 25 ||
     26 + p->pic_init_qp_minus26 + sl->slice_qp_delta < 0 ||
     26 + p->pic_init_qp_minus26 + sl->slice_qp_delta > 51 ||
     p->chroma_qp_index_offset < -12 || p->chroma_qp_index_offset > 12 ||
     p->second_chroma_qp_index_offset < -12 || p->second_chroma_qp_index_offset > 12)
  return -EINVAL;
 if ((s->profile_idc != 100 && (p->flags & (V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE|V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT))) ||
     (s->profile_idc == 66 && (p->flags & (V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE|V4L2_H264_PPS_FLAG_WEIGHTED_PRED))) ||
     (s->profile_idc != 66 && s->profile_idc != 77 && s->profile_idc != 100) || s->chroma_format_idc != 1 || s->bit_depth_luma_minus8 ||
     s->bit_depth_chroma_minus8 || !(s->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY) ||
     (s->flags & (V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD | V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE | V4L2_H264_SPS_FLAG_QPPRIME_Y_ZERO_TRANSFORM_BYPASS)) ||
     (j->dec.flags & (V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC | V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD)) ||
     p->num_slice_groups_minus1 || p->weighted_bipred_idc>2 ||
     sl->first_mb_in_slice >= (s->pic_width_in_mbs_minus1+1U)*(s->pic_height_in_map_units_minus1+1U) || sl->cabac_init_idc > 2 || sl->num_ref_idx_l0_active_minus1 >= V4L2_H264_NUM_DPB_ENTRIES ||
     sl->num_ref_idx_l1_active_minus1 >= V4L2_H264_NUM_DPB_ENTRIES || s->max_num_ref_frames > V4L2_H264_NUM_DPB_ENTRIES ||
     (sl->slice_type != V4L2_H264_SLICE_TYPE_I && sl->slice_type != V4L2_H264_SLICE_TYPE_P && sl->slice_type != V4L2_H264_SLICE_TYPE_B))
  return -EOPNOTSUPP;
 if(sl->slice_type==V4L2_H264_SLICE_TYPE_B &&
    (s->profile_idc==66 || j->dec.top_field_order_cnt < -0x20000 || j->dec.bottom_field_order_cnt < -0x20000 ||
     j->dec.top_field_order_cnt>0x3ffff || j->dec.bottom_field_order_cnt>0x3ffff))return -EOPNOTSUPP;
 if(V4L2_H264_CTRL_PRED_WEIGHTS_REQUIRED(p,sl) && (j->weights.luma_log2_weight_denom>7 || j->weights.chroma_log2_weight_denom>7))return -EINVAL;
 if(p->flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT) {
  unsigned int i,k;
  for(i=0;i<6;i++)for(k=0;k<16;k++)if(!j->scaling.scaling_list_4x4[i][k])return -EINVAL;
  for(i=0;i<2;i++)for(k=0;k<64;k++)if(!j->scaling.scaling_list_8x8[i][k])return -EINVAL;
 }
 return 0;
}
static void h264_program(struct decoder *d, const struct r1_h264_job *job,
                         u32 output, u32 mv, u32 wrap0, u32 wrap1)
{
 u32 width = (job->sps.pic_width_in_mbs_minus1 + 1) * 16;
 u32 height = (job->sps.pic_height_in_map_units_minus1 + 1) * 16;
 u32 y_size = (width / 16) * DIV_ROUND_UP(height, 32) * 512;
 u32 range_size = ALIGN(y_size * 3 / 4, 512);
 u32 mv_size = width * 4 * (height / 16) + 512;
 struct r1_h264_slice_regs sl = r1_h264_pack_slice(&job->slice, &job->dec,
                                                  rd(d, 0x208c), rd(d, 0x2098));
 struct r1_h264_sps_regs sr = r1_h264_pack_sps(&job->sps);
 struct r1_h264_pps_regs pr = r1_h264_pack_pps(&job->pps);
 wr(d, 0x2088, sr.avcvld_88); wr(d, 0x868, sr.vldtop_68);
 wr(d, 0x900, sr.vldtop_100);
 if(job->pps.flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT) {
  unsigned int i,k;
  for(i=0;i<8;i++) {
   const u8 *list=i<6?job->scaling.scaling_list_4x4[i]:job->scaling.scaling_list_8x8[i-6];
   unsigned int base=i<6?i*16:i==6?0x60:0xa0;
   for(k=0;k<(i<6?16:64);k+=4) {
    wr(d,0x260,0x200+base+k);wr(d,0x264,r1_h264_scaling_word(list+k));wr(d,0x260,0x100+base+k);
   }
  }
 }
 wr(d, 0x2090, pr.avcvld_90); wr(d, 0x2094, pr.avcvld_94);
 wr(d, 0x208c, sl.avcvld_8c); wr(d, 0x2098, sl.avcvld_98);
 wr(d, 0x5004, 0x3);
 wr(d, 0x1090, 0x4);
 wr(d, 0x858, 0xffffffff);
 wr(d, 0x1208, width);
 wr(d, 0x120c, height);
 wr(d, 0x5004, 0x10);
 wr(d, 0x870, width | (height << 16));
 wr(d, 0x2280, 0xbfdfb0f7);
 wr(d, 0x226c, 0xbfdfb0f7);
 wr(d, 0x1220, 0x1);
 wr(d, 0x1224, output >> 9);
 wr(d, 0x1228, (output + y_size) >> 8);
 wr(d, 0x1250, 0x1);
 wr(d, 0x1238, 0x3);
 wr(d, 0x1260, width / 16 - 1);
 wr(d, 0x1264, height / 16 - 1);
 wr(d, 0x122c, width / 16);
 wr(d, 0x1850, (output + y_size * 3 / 2) >> 4);
 wr(d, 0x1854, output >> 4);
 wr(d, 0x1858, 0x1);
 wr(d, 0x15e4, 0x20000);
 wr(d, 0x1560, output);
 wr(d, 0x155c, y_size);
 wr(d, 0x1980, width / 16);
 wr(d, 0x22c8, 0xf900a03b);
 wr(d, 0x320c, mv >> 4);
 wr(d, 0x209c, sl.avcvld_9c);
 wr(d, 0x15e4, 0x21000);
 wr(d, 0x1920, rd(d, 0x1920) | BIT(24));
 wr(d, 0x1768, (rd(d, 0x1768) & ~7U) | 4);
 wr(d, 0x50f0, rd(d, 0x50f0) | 1);
 wr(d, 0x86c, (width / 16) * (height / 16));
 wr(d, 0x850, 0xffffffff);
 wr(d, 0x1a20, wrap0);
 wr(d, 0x1a24, wrap1);
 wr(d, 0x22d0, 0x1201);
 wr(d, 0x22d4, 0x204008);
 wr(d, 0x89c, (rd(d, 0x89c) & ~0x1fffU) | 0x400);
 wr(d, 0x5028, rd(d, 0x5028) | 1);
 wr(d, 0x503c, output);
 wr(d, 0x5040, output + range_size);
 wr(d, 0x5054, mv);
 wr(d, 0x5058, mv + mv_size);
 wr(d, 0x2084, 0xae00e007);
 /* Actual kick, not performed here: AVCVLD+0x238 = 1. */
}
