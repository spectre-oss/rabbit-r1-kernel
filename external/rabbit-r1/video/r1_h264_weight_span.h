/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef R1_H264_WEIGHT_SPAN_H
#define R1_H264_WEIGHT_SPAN_H
#include <linux/v4l2-controls.h>
/* Locate/verify explicit P/B-slice weight syntax for the hardware's parser. Input is
 * NAL payload after its one-byte header; offsets exclude emulation prevention.
 * No allocation, MMIO or input mutation; every read is bounded. */
struct r1_bits {
 const unsigned char *data;
 unsigned int size, byte, used, value, zeros, bits;
 int error;
};
static inline unsigned int r1_bit(struct r1_bits *b)
{
 if(b->error)return 0;
 if(!b->used) {
  if(b->byte>=b->size){b->error=1;return 0;}
  unsigned int v=b->data[b->byte++];
  if(b->zeros==2 && v==3) {
   if(b->byte>=b->size || b->data[b->byte]>3){b->error=1;return 0;}
   v=b->data[b->byte++];b->zeros=0;
  }
  b->zeros=v==0?b->zeros+1:0;
  if(b->zeros>2)b->zeros=2;
  b->value=v;b->used=8;
 }
 b->used--;b->bits++;
 return (b->value>>b->used)&1;
}
static inline unsigned int r1_u(struct r1_bits *b,unsigned int n)
{
 unsigned int v=0;
 if(n>32){b->error=1;return 0;}
 while(n--)v=(v<<1)|r1_bit(b);
 return v;
}
static inline unsigned int r1_ue(struct r1_bits *b)
{
 unsigned int zeros=0;
 while(!b->error && !r1_bit(b))if(++zeros>30){b->error=1;return 0;}
 return ((1U<<zeros)-1)+r1_u(b,zeros);
}
static inline int r1_se(struct r1_bits *b)
{ unsigned int u=r1_ue(b);return u&1?(int)((u+1)/2):-(int)(u/2); }
static inline int r1_h264_reorder_span(const unsigned char *data,unsigned int size,
 const struct v4l2_ctrl_h264_sps *s,const struct v4l2_ctrl_h264_pps *p,
 const struct v4l2_ctrl_h264_slice_params *sl,const struct v4l2_ctrl_h264_decode_params *d,
 unsigned int *start,unsigned int *end,struct r1_bits *after)
{
 struct r1_bits b={.data=data,.size=size};
 unsigned int k,list,n=sl->num_ref_idx_l0_active_minus1+1;
 unsigned int is_b=sl->slice_type==V4L2_H264_SLICE_TYPE_B;
 if((sl->slice_type!=V4L2_H264_SLICE_TYPE_P && !is_b) ||
    (d->flags&(V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC|V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD)) ||
    s->chroma_format_idc!=1 || s->log2_max_frame_num_minus4>12 ||
    s->log2_max_pic_order_cnt_lsb_minus4>12 || s->pic_order_cnt_type>2 ||
    n>V4L2_H264_NUM_DPB_ENTRIES ||
    (is_b && sl->num_ref_idx_l1_active_minus1>=V4L2_H264_NUM_DPB_ENTRIES) ||
    (d->flags&V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC))return -1;
 if(r1_ue(&b)!=sl->first_mb_in_slice)return -1;
 unsigned int type=r1_ue(&b);
 if(type>9 || type%5!=sl->slice_type || r1_ue(&b)!=p->pic_parameter_set_id || r1_u(&b,s->log2_max_frame_num_minus4+4)!=d->frame_num)return -1;
 /* Interlaced SPS frame pictures carry field_pic_flag before POC syntax.
  * Field pictures still require separate reference-list/backend support. */
 if(!(s->flags&V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY) && r1_bit(&b))return -1;
 if(s->pic_order_cnt_type==0) {
  if(r1_u(&b,s->log2_max_pic_order_cnt_lsb_minus4+4)!=d->pic_order_cnt_lsb)return -1;
  if((p->flags&V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT) && r1_se(&b)!=d->delta_pic_order_cnt_bottom)return -1;
 } else if(s->pic_order_cnt_type==1 && !(s->flags&V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO)) {
  if(r1_se(&b)!=d->delta_pic_order_cnt0)return -1;
  if((p->flags&V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT) && r1_se(&b)!=d->delta_pic_order_cnt1)return -1;
 }
 if(p->flags&V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT)(void)r1_ue(&b);
 if(is_b && r1_bit(&b)!=!!(sl->flags&V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED))return -1;
 if(r1_bit(&b)) {
  if(r1_ue(&b)!=sl->num_ref_idx_l0_active_minus1)return -1;
  if(is_b && r1_ue(&b)!=sl->num_ref_idx_l1_active_minus1)return -1;
 } else if(p->num_ref_idx_l0_default_active_minus1!=sl->num_ref_idx_l0_active_minus1 ||
           (is_b && p->num_ref_idx_l1_default_active_minus1!=sl->num_ref_idx_l1_active_minus1))return -1;
 *start=b.bits;
 for(list=0;list<=is_b;list++)if(r1_bit(&b)) {
  for(k=0;k<=V4L2_H264_REF_LIST_LEN;k++) {
   unsigned int id=r1_ue(&b);
   if(id==3)break;
   if(id>2 || b.error || k==V4L2_H264_REF_LIST_LEN)return -1;
   (void)r1_ue(&b);
  }
 }
 *end=b.bits;
 if(b.error || sl->header_bit_size<8 || b.bits>sl->header_bit_size-8)return -1;
 if(after)*after=b;
 return 0;
}
static inline int r1_h264_weight_span(const unsigned char *data,unsigned int size,
 const struct v4l2_ctrl_h264_sps *s,const struct v4l2_ctrl_h264_pps *p,
 const struct v4l2_ctrl_h264_slice_params *sl,const struct v4l2_ctrl_h264_decode_params *d,
 const struct v4l2_ctrl_h264_pred_weights *w,unsigned int *start,unsigned int *end)
{
 struct r1_bits b;
 unsigned int k,j,list,n,reorder_start,reorder_end;
 unsigned int is_b=sl->slice_type==V4L2_H264_SLICE_TYPE_B;
 if(is_b ? p->weighted_bipred_idc!=1 : !(p->flags&V4L2_H264_PPS_FLAG_WEIGHTED_PRED))return -1;
 if(r1_h264_reorder_span(data,size,s,p,sl,d,&reorder_start,&reorder_end,&b))return -1;
 *start=b.bits;
 if(r1_ue(&b)!=w->luma_log2_weight_denom || r1_ue(&b)!=w->chroma_log2_weight_denom ||
    w->luma_log2_weight_denom>7 || w->chroma_log2_weight_denom>7)return -1;
 for(list=0;list<=is_b;list++) {
 n=(list?sl->num_ref_idx_l1_active_minus1:sl->num_ref_idx_l0_active_minus1)+1;
 for(k=0;k<n;k++) {
  int weight=1<<w->luma_log2_weight_denom,offset=0;
  if(r1_bit(&b)){weight=r1_se(&b);offset=r1_se(&b);if(weight < -128 || weight>127 || offset < -128 || offset>127)return -1;}
  if(weight!=w->weight_factors[list].luma_weight[k] || offset!=w->weight_factors[list].luma_offset[k])return -1;
  unsigned int present=r1_bit(&b);
  for(j=0;j<2;j++) {
   weight=1<<w->chroma_log2_weight_denom;offset=0;
   if(present){weight=r1_se(&b);offset=r1_se(&b);if(weight < -128 || weight>127 || offset < -128 || offset>127)return -1;}
   if(weight!=w->weight_factors[list].chroma_weight[k][j] || offset!=w->weight_factors[list].chroma_offset[k][j])return -1;
  }
 }
 }
 *end=b.bits;
 return b.error || sl->header_bit_size<8 || b.bits>sl->header_bit_size-8?-1:0;
}
#endif
