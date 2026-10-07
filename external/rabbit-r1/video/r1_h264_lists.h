/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef R1_H264_LISTS_H
#define R1_H264_LISTS_H
#include <linux/v4l2-controls.h>
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif
/* Frame-only short/long-term lists. Inputs must already have valid signed PicNum
 * and POC values. The sentinel leaves unavailable positions explicit. */
static inline int r1_h264_before(const struct v4l2_h264_dpb_entry *a,
 const struct v4l2_h264_dpb_entry *b,int poc,unsigned int type,unsigned int list)
{
 int al=!!(a->flags&V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM);
 int bl=!!(b->flags&V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM);
 if(al!=bl)return al<bl;
 if(al)return a->pic_num<b->pic_num;
 int ap=a->top_field_order_cnt<a->bottom_field_order_cnt?a->top_field_order_cnt:a->bottom_field_order_cnt;
 int bp=b->top_field_order_cnt<b->bottom_field_order_cnt?b->top_field_order_cnt:b->bottom_field_order_cnt;
 if(type==V4L2_H264_SLICE_TYPE_P)return (int)a->pic_num>(int)b->pic_num;
 int ag=list?ap>poc:ap<poc,bg=list?bp>poc:bp<poc;
 if(ag!=bg)return ag>bg;
 return list?(ag?ap<bp:ap>bp):(ag?ap>bp:ap<bp);
}
static inline void r1_h264_default_lists(const struct v4l2_h264_dpb_entry *dpb,
 unsigned int count,unsigned int type,int poc,unsigned int lists[2][17])
{
 unsigned int l,i,k;
 for(l=0;l<2;l++) {
  for(i=0;i<17;i++)lists[l][i]=16;
  for(i=0;i<count;i++) {
   k=i;
   while(k && r1_h264_before(&dpb[i],&dpb[lists[l][k-1]],poc,type,l)) {
    lists[l][k]=lists[l][k-1];k--;
   }
   lists[l][k]=i;
  }
 }
 if(type==V4L2_H264_SLICE_TYPE_B && count>1) {
  for(i=0;i<count && lists[0][i]==lists[1][i];i++);
  if(i==count){i=lists[1][0];lists[1][0]=lists[1][1];lists[1][1]=i;}
 }
}
/* Build hardware default lists from sparse V4L2 DPB slots. Preserve original
 * indices, and derive wrapped short PicNum rather than trusting userspace's
 * unsigned pic_num representation. Returns reference count or -1. */
static inline int r1_h264_dpb_lists(const struct v4l2_ctrl_h264_decode_params *d,
 unsigned int log2_minus4,unsigned int type,unsigned int lists[2][17])
{
 struct v4l2_h264_dpb_entry compact[16];
 unsigned int map[16],count=0,i,l,maximum;
 int poc=d->top_field_order_cnt<d->bottom_field_order_cnt?
         d->top_field_order_cnt:d->bottom_field_order_cnt;
 if(log2_minus4>12 || (type!=V4L2_H264_SLICE_TYPE_P && type!=V4L2_H264_SLICE_TYPE_B))return -1;
 maximum=1U<<(log2_minus4+4);
 if(d->frame_num>=maximum)return -1;
 for(i=0;i<16;i++) {
  const struct v4l2_h264_dpb_entry *r=&d->dpb[i];
  if(!(r->flags&V4L2_H264_DPB_ENTRY_FLAG_ACTIVE))continue;
  if(!(r->flags&V4L2_H264_DPB_ENTRY_FLAG_VALID) ||
     (r->flags&V4L2_H264_DPB_ENTRY_FLAG_FIELD))return -1;
  compact[count]=*r;map[count]=i;
  if(r->flags&V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM) {
   if(r->pic_num>65535)return -1;
  } else {
   if(r->frame_num>=maximum)return -1;
   compact[count].pic_num=r->frame_num>d->frame_num?
      (__u32)((int)r->frame_num-(int)maximum):r->frame_num;
  }
  count++;
 }
 r1_h264_default_lists(compact,count,type,poc,lists);
 for(l=0;l<2;l++)for(i=0;i<count;i++)lists[l][i]=map[lists[l][i]];
 return count;
}
/* PFRAME/BFRAME describe slice classification, not a picture boundary. */
static inline int r1_h264_same_decode(const struct v4l2_ctrl_h264_decode_params *a,
 const struct v4l2_ctrl_h264_decode_params *b)
{
 struct v4l2_ctrl_h264_decode_params copy=*a;
 if((a->flags^b->flags)&~(V4L2_H264_DECODE_PARAM_FLAG_PFRAME|V4L2_H264_DECODE_PARAM_FLAG_BFRAME))return 0;
 copy.flags=b->flags;
 return !memcmp(&copy,b,sizeof(copy));
}
#endif
