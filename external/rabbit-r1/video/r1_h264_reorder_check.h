/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef R1_H264_REORDER_CHECK_H
#define R1_H264_REORDER_CHECK_H
#include "r1_h264_lists.h"
#include "r1_h264_weight_span.h"
/* Independently reconstruct each slice list from its encoded modifications.
 * No device access; reject unavailable references and disagreeing controls. */
static inline int r1_h264_check_reordering(const unsigned char *data,unsigned int size,
 const struct v4l2_ctrl_h264_sps *s,const struct v4l2_ctrl_h264_pps *p,
 const struct v4l2_ctrl_h264_slice_params *sl,const struct v4l2_ctrl_h264_decode_params *d)
{
 unsigned int lists[2][17],start,end,l,k,maximum;
 struct r1_bits b={.data=data,.size=size};
 if(r1_h264_reorder_span(data,size,s,p,sl,d,&start,&end,0) ||
    r1_h264_dpb_lists(d,s->log2_max_frame_num_minus4,sl->slice_type,lists)<=0)return -1;
 maximum=1U<<(s->log2_max_frame_num_minus4+4);
 while(b.bits<start && !b.error)r1_bit(&b);
 for(l=0;l<(sl->slice_type==V4L2_H264_SLICE_TYPE_B?2U:1U);l++) {
  unsigned int position=0,pred=d->frame_num;
  unsigned int active=1+(l?sl->num_ref_idx_l1_active_minus1:sl->num_ref_idx_l0_active_minus1);
  const struct v4l2_h264_reference *requested=l?sl->ref_pic_list1:sl->ref_pic_list0;
  if(r1_bit(&b))for(k=0;k<=V4L2_H264_REF_LIST_LEN;k++) {
   unsigned int id=r1_ue(&b),value,selected=16,j,dest;
   if(id==3)break;
   if(id>2 || b.error || position>=active)return -1;
   value=r1_ue(&b);
   if(id<2) {
    if(value>=maximum)return -1;
    pred=(pred+(id==0?maximum-value-1:value+1))%maximum;
   }
   for(j=0;j<16;j++) {
    const struct v4l2_h264_dpb_entry *r=&d->dpb[j];
    if(!(r->flags&V4L2_H264_DPB_ENTRY_FLAG_ACTIVE))continue;
    if(id==2 ? ((r->flags&V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM) && r->pic_num==value) :
       (!(r->flags&V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM) && r->frame_num==pred)) {
     if(selected!=16)return -1;
     selected=j;
    }
   }
   if(selected==16 || b.error)return -1;
   for(j=16;j>position;j--)lists[l][j]=lists[l][j-1];
   lists[l][position++]=selected;dest=position;
   for(j=position;j<=16;j++)if(lists[l][j]!=selected)lists[l][dest++]=lists[l][j];
   while(dest<=16)lists[l][dest++]=16;
  }
  for(k=0;k<active;k++)if(lists[l][k]>=16 || requested[k].index!=lists[l][k] ||
      requested[k].fields!=V4L2_H264_FRAME_REF)return -1;
 }
 return b.error || b.bits!=end?-1:0;
}
#endif
