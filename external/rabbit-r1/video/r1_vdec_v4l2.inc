/* SPDX-License-Identifier: GPL-2.0-only */
/* Stateless request frontend. Internal tiled snapshots preserve references
 * independently of the linear NV12 capture presentation. */
struct video_context {
 struct v4l2_fh fh;
 struct v4l2_ctrl_handler controls;
 struct v4l2_ctrl *sps, *pps, *slice, *dec, *scaling, *weights;
 struct decoder *d;
 struct work_struct work;
 unsigned int width, height, sequence;
 struct v4l2_ctrl_h264_sps sequence_sps;
 bool sequence_valid;
 struct r1_h264_job *picture_job;
 u8 *picture_bytes;
 size_t picture_size;
 u64 picture_timestamp;
 unsigned int picture_capture, picture_last_mb;

};
struct video_buffer {
 struct v4l2_m2m_buffer m2m;
 void *tiled;
 dma_addr_t tiled_dma;
 struct sg_table *tiled_sgt;
 bool tiled_device_owned;
 /* Full coherent MV allocation retained until this capture is reused. */
 void *motion_vectors;
 dma_addr_t motion_dma;
 bool valid;
};
static struct video_context *video_ctx(struct file *f)
{ return container_of(file_to_v4l2_fh(f), struct video_context, fh); }
static struct video_buffer *video_buf(struct vb2_buffer *b)
{ return container_of(to_vb2_v4l2_buffer(b), struct video_buffer, m2m.vb); }
static void linear_nv12(void *dst,const void *src,unsigned w,unsigned h)
{
 u8 *out=dst;const u8 *in=src;const unsigned rows=16;
 unsigned cols=w/16,ysize=cols*((h+31)/32)*512;
 for(unsigned plane=0;plane<2;plane++) {
  unsigned shift=plane?4:5,height=plane?h/2:h;
  for(unsigned y=0;y<height;y+=rows)
   for(unsigned x=0;x<w;x+=16)
    for(unsigned k=0;k<rows && y+k<height;k++)
     memcpy(out+(y+k)*w+x,in+((y>>shift)*cols+x/16)*(16U<<shift)+((y+k)&((1U<<shift)-1))*16,16);
  if(!plane){out+=w*h;in+=ysize;}
 }
}
static int video_reference(struct video_context *c, struct r1_h264_job *j)
{
 struct vb2_queue *q=v4l2_m2m_get_dst_vq(c->fh.m2m_ctx);
 unsigned int list,slot,i;
 int refs=r1_h264_dpb_lists(&j->dec,j->sps.log2_max_frame_num_minus4,(j->picture_has_b?V4L2_H264_SLICE_TYPE_B:V4L2_H264_SLICE_TYPE_P),j->default_lists);
 if(refs<=0)return -EINVAL;
 j->reference_count=refs;
 {
  unsigned int p_lists[2][17],k;
  if(r1_h264_dpb_lists(&j->dec,j->sps.log2_max_frame_num_minus4,V4L2_H264_SLICE_TYPE_P,p_lists)!=refs)return -EINVAL;
  for(slot=0;slot<refs;slot++) {
   for(k=0;k<refs && j->default_lists[0][k]!=p_lists[0][slot];k++);
   if(k==refs)return -EINVAL;
   j->p_slot[slot]=k;
  }
 }
 unsigned int lists=(j->picture_has_b?V4L2_H264_SLICE_TYPE_B:V4L2_H264_SLICE_TYPE_P)==V4L2_H264_SLICE_TYPE_B?2:1;
 for(list=0;list<lists;list++) {
  unsigned int count=j->reference_count;
  if(count>V4L2_H264_NUM_DPB_ENTRIES)return -EINVAL;
  for(slot=0;slot<count;slot++) {
   unsigned int index=j->default_lists[list][slot];
   const struct v4l2_h264_dpb_entry *ref;
   struct vb2_buffer *found=NULL;
   if(index>=V4L2_H264_NUM_DPB_ENTRIES)return -EINVAL;
   ref=&j->dec.dpb[index];
   if(!(ref->flags & V4L2_H264_DPB_ENTRY_FLAG_ACTIVE) ||
      (ref->flags & V4L2_H264_DPB_ENTRY_FLAG_FIELD) ||
      ((ref->flags & V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM) ?
       ref->pic_num>65535 : ref->frame_num >= (1U << (j->sps.log2_max_frame_num_minus4+4))) ||
      ref->top_field_order_cnt < -0x20000 || ref->bottom_field_order_cnt < -0x20000 ||
      ref->top_field_order_cnt>(lists==2?0x3ffff:0x7ffff) ||
      ref->bottom_field_order_cnt>(lists==2?0x3ffff:0x7ffff) ||
      ref->fields!=V4L2_H264_FRAME_REF)return -EOPNOTSUPP;
   for(i=0;i<vb2_get_num_buffers(q);i++) {
    struct vb2_buffer *b=vb2_get_buffer(q,i);
    if(b && b->timestamp==ref->reference_ts && video_buf(b)->valid){found=b;break;}
   }
   if(!found)return -ENOENT;
   if(found->index>31)return -EOPNOTSUPP;
   j->reference_identity[list][slot]=found->index;
   if(!video_buf(found)->tiled_device_owned)return -EINVAL;
   j->reference_pixels[list][slot]=video_buf(found)->tiled_dma;
   if(list) {
    j->reference_mv[slot]=video_buf(found)->motion_dma;
   }
  }
 }
 return 0;
}

static bool video_has_control(struct video_context *c,struct media_request *req,u32 id)
{
 struct v4l2_ctrl_handler *h=v4l2_ctrl_request_hdl_find(req,&c->controls);
 bool found=h && v4l2_ctrl_request_hdl_ctrl_find(h,id);
 if(h)v4l2_ctrl_request_hdl_put(h);
 return found;
}
static void video_picture_reset(struct video_context *c)
{
 kvfree(c->picture_bytes);kfree(c->picture_job);
 c->picture_bytes=NULL;c->picture_job=NULL;c->picture_size=0;
}
/* Reference lists are checked against each encoded header separately.
 * These picture-wide parameters must still agree across held slices. */
static bool video_same_picture(const struct r1_h264_job *a,const struct r1_h264_job *b)
{
 if(memcmp(&a->sps,&b->sps,sizeof(a->sps)) || memcmp(&a->pps,&b->pps,sizeof(a->pps)) ||
    !r1_h264_same_decode(&a->dec,&b->dec) || memcmp(&a->scaling,&b->scaling,sizeof(a->scaling)) ||
    ((a->slice.flags^b->slice.flags)&~V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED))return false;
 return true;
}
static void video_run_work(struct work_struct *work)
{
 struct video_context *c=container_of(work,struct video_context,work);
 struct decoder *d=c->d;
 struct vb2_v4l2_buffer *src=v4l2_m2m_next_src_buf(c->fh.m2m_ctx);
 struct vb2_v4l2_buffer *dst=v4l2_m2m_next_dst_buf(c->fh.m2m_ctx);
 struct media_request *req=src ? src->vb2_buf.req_obj.req : NULL;
 struct r1_h264_job *j=NULL;
 const u8 *payload; size_t size;
 bool held=false;
 int ret=-EINVAL;
 if(!src || !dst || !req) goto done;
 video_buf(&dst->vb2_buf)->valid=false;
 if(!video_has_control(c,req,V4L2_CID_STATELESS_H264_SLICE_PARAMS))goto done;
 ret=v4l2_ctrl_request_setup(req,&c->controls);
 if(ret) goto done;
 j=kzalloc(sizeof(*j),GFP_KERNEL); if(!j) {ret=-ENOMEM;goto done;}
 j->slice=*c->slice->p_cur.p_h264_slice_params;
 ret=-EINVAL;
 if(video_has_control(c,req,V4L2_CID_STATELESS_H264_SPS))j->sps=*c->sps->p_cur.p_h264_sps;
 else if(c->sequence_valid)j->sps=c->sequence_sps;
 else goto done;
 if(j->slice.first_mb_in_slice) {
  if(!c->picture_job || c->picture_timestamp!=src->vb2_buf.timestamp || c->picture_capture!=dst->vb2_buf.index)goto done;
  j->pps=c->picture_job->pps;j->dec=c->picture_job->dec;j->scaling=c->picture_job->scaling;
 } else if(!video_has_control(c,req,V4L2_CID_STATELESS_H264_PPS) ||
           !video_has_control(c,req,V4L2_CID_STATELESS_H264_DECODE_PARAMS))goto done;
 if(video_has_control(c,req,V4L2_CID_STATELESS_H264_PPS))j->pps=*c->pps->p_cur.p_h264_pps;
 if(video_has_control(c,req,V4L2_CID_STATELESS_H264_DECODE_PARAMS))j->dec=*c->dec->p_cur.p_h264_decode_params;
 if(j->pps.flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT) {
  if(video_has_control(c,req,V4L2_CID_STATELESS_H264_SCALING_MATRIX))j->scaling=*c->scaling->p_cur.p_h264_scaling_matrix;
  else if(!j->slice.first_mb_in_slice)goto done;
 }
 if(V4L2_H264_CTRL_PRED_WEIGHTS_REQUIRED(&j->pps,&j->slice)) {
  struct v4l2_ctrl_handler *h=v4l2_ctrl_request_hdl_find(req,&c->controls);
  bool found=h && v4l2_ctrl_request_hdl_ctrl_find(h,V4L2_CID_STATELESS_H264_PRED_WEIGHTS);
  if(h)v4l2_ctrl_request_hdl_put(h);
  if(!found){ret=-EINVAL;goto done;}
  j->weights=*c->weights->p_cur.p_h264_pred_weights;
 }
 ret=validate_job(j); if(ret) goto done;
 if((j->sps.pic_width_in_mbs_minus1+1)*16!=c->width ||
    (j->sps.pic_height_in_map_units_minus1+1)*16!=c->height || j->slice.header_bit_size<8) {ret=-EINVAL;goto done;}
 size=vb2_get_plane_payload(&src->vb2_buf,0);
 payload=vb2_plane_vaddr(&src->vb2_buf,0);
 if(size<2 || size>INPUT_SIZE-259 || !payload || (payload[0]&0x80) ||
    ((payload[0]&31)!=1 && (payload[0]&31)!=5) || j->slice.header_bit_size>(size*8)) {ret=-EINVAL;goto done;}
 {
  struct r1_bits bits={.data=payload+1,.size=size-1};
  unsigned int start,end;
  if(r1_ue(&bits)!=j->slice.first_mb_in_slice || bits.error){ret=-EINVAL;goto done;}
  if(V4L2_H264_CTRL_PRED_WEIGHTS_REQUIRED(&j->pps,&j->slice) &&
     r1_h264_weight_span(payload+1,size-1,&j->sps,&j->pps,&j->slice,&j->dec,&j->weights,&start,&end)) {ret=-EINVAL;goto done;}
 }
 if(!j->slice.first_mb_in_slice) {
  if(c->picture_job && c->picture_timestamp==src->vb2_buf.timestamp) {ret=-EINVAL;goto done;}
  video_picture_reset(c);
  c->picture_bytes=kvmalloc(INPUT_SIZE,GFP_KERNEL);
  c->picture_job=kmemdup(j,sizeof(*j),GFP_KERNEL);
  if(!c->picture_bytes || !c->picture_job){ret=-ENOMEM;goto done;}
  c->picture_timestamp=src->vb2_buf.timestamp;c->picture_capture=dst->vb2_buf.index;c->picture_last_mb=0;
 } else if(!c->picture_job || c->picture_timestamp!=src->vb2_buf.timestamp ||
           c->picture_capture!=dst->vb2_buf.index || j->slice.first_mb_in_slice<=c->picture_last_mb ||
           !video_same_picture(c->picture_job,j)) {ret=-EINVAL;goto done;}
 if(j->slice.slice_type!=V4L2_H264_SLICE_TYPE_I &&
    r1_h264_check_reordering(payload+1,size-1,&j->sps,&j->pps,&j->slice,&j->dec)) {ret=-EINVAL;goto done;}
 if(j->slice.slice_type==V4L2_H264_SLICE_TYPE_P)c->picture_job->picture_has_p=true;
 if(j->slice.slice_type==V4L2_H264_SLICE_TYPE_B)c->picture_job->picture_has_b=true;
 if(c->picture_size>INPUT_SIZE-256-3-size){ret=-E2BIG;goto done;}
 memcpy(c->picture_bytes+c->picture_size,"\0\0\1",3);
 memcpy(c->picture_bytes+c->picture_size+3,payload,size);c->picture_size+=size+3;
 c->picture_last_mb=j->slice.first_mb_in_slice;
 c->sequence_sps=j->sps;c->sequence_valid=true;
 v4l2_m2m_buf_copy_metadata(src,dst);
 if(src->flags&V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF) {held=true;ret=0;goto done;}
 kfree(j);j=c->picture_job;c->picture_job=NULL;
 if(j->picture_has_p || j->picture_has_b) {ret=video_reference(c,j);if(ret)goto done;}
 ret=submit_job(d,j,c->picture_bytes,c->picture_size,j->slice.header_bit_size-8,true);
 if(ret)goto done;
 /* Keep completed pixels in their original DMA allocation. The old capture
  * allocation becomes the next output; reclaim CPU ownership before clearing. */
 swap(video_buf(&dst->vb2_buf)->tiled,d->work[0]);
 swap(video_buf(&dst->vb2_buf)->tiled_dma,d->work_dma[0]);
 swap(video_buf(&dst->vb2_buf)->tiled_sgt,d->pixels_sgt);
 if(video_buf(&dst->vb2_buf)->tiled_device_owned)
  pixels_cpu(d->dma_dev,d->work[0],d->pixels_sgt);
 video_buf(&dst->vb2_buf)->tiled_device_owned=false;
 /* Hardware is stopped and ownership released before exchanging allocations. */
 swap(video_buf(&dst->vb2_buf)->motion_vectors,d->work[1]);
 swap(video_buf(&dst->vb2_buf)->motion_dma,d->work_dma[1]);
 /* Detile the completed CPU snapshot, avoiding byte-wise coherent DMA reads. */
 linear_nv12(vb2_plane_vaddr(&dst->vb2_buf,0),video_buf(&dst->vb2_buf)->tiled,c->width,c->height);
 /* Future reference jobs read these pixels without CPU copies. */
 pixels_device(d->dma_dev,video_buf(&dst->vb2_buf)->tiled,video_buf(&dst->vb2_buf)->tiled_sgt);
 video_buf(&dst->vb2_buf)->tiled_device_owned=true;
 vb2_set_plane_payload(&dst->vb2_buf,0,c->width*c->height*3/2);
 v4l2_m2m_buf_copy_metadata(src,dst);
 video_buf(&dst->vb2_buf)->valid=true;
 src->sequence=dst->sequence=c->sequence++;
done:
 if(ret)dev_warn(d->dev,"request rejected ret=%d controls=%u first_mb=%u\n",ret,req?video_has_control(c,req,V4L2_CID_STATELESS_H264_SLICE_PARAMS):0,j?j->slice.first_mb_in_slice:~0U);
 kfree(j);
 if(!held || ret)video_picture_reset(c);
 if(req)v4l2_ctrl_request_complete(req,&c->controls);
 if(ret && src)src->flags&=~V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF;
 if(dst && (ret || held))vb2_set_plane_payload(&dst->vb2_buf,0,0);
 v4l2_m2m_buf_done_and_job_finish(d->m2m,c->fh.m2m_ctx,ret?VB2_BUF_STATE_ERROR:VB2_BUF_STATE_DONE);
}
static void video_run(void *priv)
{ struct video_context *c=priv;schedule_work(&c->work); }
static const struct v4l2_m2m_ops video_m2m_ops={.device_run=video_run};
static unsigned int video_size(struct video_context *c,enum v4l2_buf_type t)
{ return V4L2_TYPE_IS_OUTPUT(t)?INPUT_SIZE:c->width*c->height*3/2; }
static int video_queue_setup(struct vb2_queue *q,unsigned int *n,unsigned int *planes,
                            unsigned int sizes[],struct device *devs[])
{
 struct video_context *c=vb2_get_drv_priv(q);
 unsigned int size=video_size(c,q->type);
 if(READ_ONCE(c->d->pinned))return -EIO;
 if(*planes)return *planes==1 && sizes[0]>=size?0:-EINVAL;
 *planes=1;sizes[0]=size;return 0;
}
static int video_init_buf(struct vb2_buffer *b)
{
 struct video_context *c=vb2_get_drv_priv(b->vb2_queue);
 struct video_buffer *v=video_buf(b);
 /* A fatal stop retains existing DMA memory; never grow that pool on retries. */
 if(READ_ONCE(c->d->pinned))return -EIO;
 if(V4L2_TYPE_IS_OUTPUT(b->type))return 0;
 v->tiled=pixels_alloc(c->d->dma_dev,&v->tiled_dma,&v->tiled_sgt);
 if(!v->tiled)return -ENOMEM;
 if((v->tiled_dma&4095) || v->tiled_dma>U32_MAX-work_sizes[0]) {
  pixels_free(c->d->dma_dev,v->tiled,v->tiled_sgt);
  v->tiled=NULL;return -ERANGE;
 }
 v->motion_vectors=dma_alloc_coherent(c->d->dma_dev,work_sizes[1],&v->motion_dma,GFP_KERNEL);
 if(!v->motion_vectors){pixels_free(c->d->dma_dev,v->tiled,v->tiled_sgt);v->tiled=NULL;return -ENOMEM;}
 if((v->motion_dma&4095) || v->motion_dma>U32_MAX-work_sizes[1]) {
  dma_free_coherent(c->d->dma_dev,work_sizes[1],v->motion_vectors,v->motion_dma);
  pixels_free(c->d->dma_dev,v->tiled,v->tiled_sgt);v->tiled=NULL;v->motion_vectors=NULL;return -ERANGE;
 }
 return 0;
}
static void video_cleanup_buf(struct vb2_buffer *b)
{
 struct video_context *c=vb2_get_drv_priv(b->vb2_queue);
 struct video_buffer *v=video_buf(b);
 if(v->tiled && !READ_ONCE(c->d->pinned))
  pixels_free(c->d->dma_dev,v->tiled,v->tiled_sgt);
 v->tiled=NULL;
 /* An unverified DMA stop pins all potentially referenced allocations. */
 if(v->motion_vectors && !READ_ONCE(c->d->pinned))
  dma_free_coherent(c->d->dma_dev,work_sizes[1],v->motion_vectors,v->motion_dma);
 v->motion_vectors=NULL;
}
static int video_prepare(struct vb2_buffer *b)
{
 if(vb2_plane_size(b,0)<video_size(vb2_get_drv_priv(b->vb2_queue),b->type) || b->planes[0].data_offset)return -EINVAL;
 if(!V4L2_TYPE_IS_OUTPUT(b->type))vb2_set_plane_payload(b,0,0);
 return 0;
}
static void video_queue(struct vb2_buffer *b)
{
 struct video_context *c=vb2_get_drv_priv(b->vb2_queue);
 if(!V4L2_TYPE_IS_OUTPUT(b->type))video_buf(b)->valid=false;
 v4l2_m2m_buf_queue(c->fh.m2m_ctx,to_vb2_v4l2_buffer(b));
}
static int video_out_validate(struct vb2_buffer *b)
{ to_vb2_v4l2_buffer(b)->field=V4L2_FIELD_NONE;return 0; }
static void video_request_complete(struct vb2_buffer *b)
{ struct video_context *c=vb2_get_drv_priv(b->vb2_queue);v4l2_ctrl_request_complete(b->req_obj.req,&c->controls); }
static void video_stop(struct vb2_queue *q)
{
 struct video_context *c=vb2_get_drv_priv(q);struct vb2_v4l2_buffer *b;
 struct vb2_queue *capture=v4l2_m2m_get_dst_vq(c->fh.m2m_ctx);
 unsigned int i;
 video_picture_reset(c);
 /* STREAMOFF ends this decoding session; stale snapshots must not resolve. */
 for(i=0;i<vb2_get_num_buffers(capture);i++) {
  struct vb2_buffer *vb=vb2_get_buffer(capture,i);
  if(vb)video_buf(vb)->valid=false;
 }
 while((b=V4L2_TYPE_IS_OUTPUT(q->type)?v4l2_m2m_src_buf_remove(c->fh.m2m_ctx):v4l2_m2m_dst_buf_remove(c->fh.m2m_ctx)))
  v4l2_m2m_buf_done(b,VB2_BUF_STATE_ERROR);
}
static const struct vb2_ops video_queue_ops={.queue_setup=video_queue_setup,.buf_init=video_init_buf,
 .buf_cleanup=video_cleanup_buf,.buf_prepare=video_prepare,.buf_queue=video_queue,
 .buf_out_validate=video_out_validate,.buf_request_complete=video_request_complete,.stop_streaming=video_stop};
static int video_init_queues(void *priv,struct vb2_queue *src,struct vb2_queue *dst)
{
 struct video_context *c=priv;struct vb2_queue *qs[]={src,dst};int i,r;
 for(i=0;i<2;i++) {
  struct vb2_queue *q=qs[i];q->type=i?V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  q->io_modes=VB2_MMAP;q->drv_priv=c;q->buf_struct_size=sizeof(struct video_buffer);
  q->ops=&video_queue_ops;q->mem_ops=&vb2_dma_contig_memops;q->timestamp_flags=V4L2_BUF_FLAG_TIMESTAMP_COPY;
  q->lock=&c->d->lock;q->dev=c->d->dma_dev;
  if(!i){q->supports_requests=true;q->requires_requests=true;q->subsystem_flags=VB2_V4L2_FL_SUPPORTS_M2M_HOLD_CAPTURE_BUF;}
  r=vb2_queue_init(q);if(r)return r;
 }return 0;
}
static struct v4l2_ctrl *video_control(struct video_context *c,u32 id)
{ struct v4l2_ctrl_config config={.id=id};return v4l2_ctrl_new_custom(&c->controls,&config,NULL); }
static int video_open(struct file *f)
{
 struct decoder *d=video_drvdata(f);struct video_context *c;int ret;
 if(READ_ONCE(d->pinned))return -EIO;
 c=kzalloc(sizeof(*c),GFP_KERNEL);
 if(!c)return -ENOMEM;
 c->d=d;c->width=320;c->height=240;INIT_WORK(&c->work,video_run_work);
 v4l2_fh_init(&c->fh,&d->video_dev);f->private_data=&c->fh;
 v4l2_ctrl_handler_init(&c->controls,8);
 c->weights=video_control(c,V4L2_CID_STATELESS_H264_PRED_WEIGHTS);
 c->scaling=video_control(c,V4L2_CID_STATELESS_H264_SCALING_MATRIX);
 c->sps=video_control(c,V4L2_CID_STATELESS_H264_SPS);c->pps=video_control(c,V4L2_CID_STATELESS_H264_PPS);
 c->slice=video_control(c,V4L2_CID_STATELESS_H264_SLICE_PARAMS);c->dec=video_control(c,V4L2_CID_STATELESS_H264_DECODE_PARAMS);
 v4l2_ctrl_new_std_menu(&c->controls,NULL,V4L2_CID_STATELESS_H264_DECODE_MODE,0,0,V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED);
 v4l2_ctrl_new_std_menu(&c->controls,NULL,V4L2_CID_STATELESS_H264_START_CODE,0,0,V4L2_STATELESS_H264_START_CODE_NONE);
 if(c->controls.error){ret=c->controls.error;goto fail;}
 c->fh.ctrl_handler=&c->controls;
 mutex_lock(&d->lock);c->fh.m2m_ctx=v4l2_m2m_ctx_init(d->m2m,c,video_init_queues);mutex_unlock(&d->lock);
 if(IS_ERR(c->fh.m2m_ctx)){ret=PTR_ERR(c->fh.m2m_ctx);goto fail;}
 v4l2_fh_add(&c->fh,f);return 0;
fail:v4l2_ctrl_handler_free(&c->controls);v4l2_fh_exit(&c->fh);kfree(c);return ret;
}
static int video_close(struct file *f)
{
 struct video_context *c=video_ctx(f);mutex_lock(&c->d->lock);
 v4l2_m2m_ctx_release(c->fh.m2m_ctx);cancel_work_sync(&c->work);video_picture_reset(c);
 v4l2_ctrl_handler_free(&c->controls);v4l2_fh_del(&c->fh,f);v4l2_fh_exit(&c->fh);
 mutex_unlock(&c->d->lock);kfree(c);return 0;
}
static int video_query(struct file *f,void *p,struct v4l2_capability *cap)
{ strscpy(cap->driver,"r1-vdec",sizeof(cap->driver));strscpy(cap->card,"Rabbit R1 H264 decoder",sizeof(cap->card));strscpy(cap->bus_info,"platform:17040000.vdec",sizeof(cap->bus_info));return 0; }
static int video_enum(struct file *f,void *p,struct v4l2_fmtdesc *fmt)
{ if(fmt->index)return -EINVAL;fmt->pixelformat=V4L2_TYPE_IS_OUTPUT(fmt->type)?V4L2_PIX_FMT_H264_SLICE:V4L2_PIX_FMT_NV12;return 0; }
static int video_enum_sizes(struct file *f,void *p,struct v4l2_frmsizeenum *size)
{
 if(size->index || size->pixel_format!=V4L2_PIX_FMT_H264_SLICE)return -EINVAL;
 size->type=V4L2_FRMSIZE_TYPE_STEPWISE;
 size->stepwise=(struct v4l2_frmsize_stepwise){
  .min_width=16,.max_width=R1_H264_MAX_WIDTH,.step_width=16,
  .min_height=16,.max_height=R1_H264_MAX_HEIGHT,.step_height=16};
 return 0;
}
static int video_fill(struct video_context *c,struct v4l2_format *fmt,unsigned int w,unsigned int h)
{
 struct v4l2_pix_format_mplane *p=&fmt->fmt.pix_mp;bool out=V4L2_TYPE_IS_OUTPUT(fmt->type);
 memset(p,0,sizeof(*p));p->width=w;p->height=h;p->field=V4L2_FIELD_NONE;p->num_planes=1;
 p->pixelformat=out?V4L2_PIX_FMT_H264_SLICE:V4L2_PIX_FMT_NV12;p->colorspace=V4L2_COLORSPACE_REC709;
 p->plane_fmt[0].sizeimage=out?INPUT_SIZE:w*h*3/2;p->plane_fmt[0].bytesperline=out?0:w;return 0;
}
static int video_get_fmt(struct file *f,void *p,struct v4l2_format *fmt)
{ struct video_context *c=video_ctx(f);return video_fill(c,fmt,c->width,c->height); }
static int video_try_fmt(struct file *f,void *p,struct v4l2_format *fmt)
{ return video_fill(video_ctx(f),fmt,ALIGN(clamp_t(u32,fmt->fmt.pix_mp.width,16,R1_H264_MAX_WIDTH),16),ALIGN(clamp_t(u32,fmt->fmt.pix_mp.height,16,R1_H264_MAX_HEIGHT),16)); }
static int video_set_fmt(struct file *f,void *p,struct v4l2_format *fmt)
{
 struct video_context *c=video_ctx(f);
 if(vb2_is_busy(v4l2_m2m_get_src_vq(c->fh.m2m_ctx))||vb2_is_busy(v4l2_m2m_get_dst_vq(c->fh.m2m_ctx)))return -EBUSY;
 video_try_fmt(f,p,fmt);c->width=fmt->fmt.pix_mp.width;c->height=fmt->fmt.pix_mp.height;return 0;
}
static const struct v4l2_ioctl_ops video_ioctls={.vidioc_querycap=video_query,
 .vidioc_enum_framesizes=video_enum_sizes,
 .vidioc_enum_fmt_vid_cap=video_enum,.vidioc_enum_fmt_vid_out=video_enum,
 .vidioc_g_fmt_vid_cap_mplane=video_get_fmt,.vidioc_g_fmt_vid_out_mplane=video_get_fmt,
 .vidioc_try_fmt_vid_cap_mplane=video_try_fmt,.vidioc_try_fmt_vid_out_mplane=video_try_fmt,
 .vidioc_s_fmt_vid_cap_mplane=video_set_fmt,.vidioc_s_fmt_vid_out_mplane=video_set_fmt,
 .vidioc_reqbufs=v4l2_m2m_ioctl_reqbufs,.vidioc_querybuf=v4l2_m2m_ioctl_querybuf,
 .vidioc_expbuf=v4l2_m2m_ioctl_expbuf,
 .vidioc_qbuf=v4l2_m2m_ioctl_qbuf,.vidioc_dqbuf=v4l2_m2m_ioctl_dqbuf,
 .vidioc_streamon=v4l2_m2m_ioctl_streamon,.vidioc_streamoff=v4l2_m2m_ioctl_streamoff,
 .vidioc_create_bufs=v4l2_m2m_ioctl_create_bufs,.vidioc_prepare_buf=v4l2_m2m_ioctl_prepare_buf,
 .vidioc_subscribe_event=v4l2_ctrl_subscribe_event,.vidioc_unsubscribe_event=v4l2_event_unsubscribe};
static const struct v4l2_file_operations video_fops={.owner=THIS_MODULE,.open=video_open,.release=video_close,
 .unlocked_ioctl=video_ioctl2,.poll=v4l2_m2m_fop_poll,.mmap=v4l2_m2m_fop_mmap};
static int video_req_validate(struct media_request *r)
{ unsigned int n=vb2_request_buffer_cnt(r);return n==1?vb2_request_validate(r):n?-EINVAL:-ENOENT; }
static const struct media_device_ops video_media_ops={.req_validate=video_req_validate,.req_queue=v4l2_m2m_request_queue};
static int register_video(struct decoder *d)
{
 int ret;media_device_init(&d->media);d->media.dev=d->dev;d->media.ops=&video_media_ops;
 strscpy(d->media.model,"Rabbit R1 decoder",sizeof(d->media.model));d->v4l2.mdev=&d->media;
 ret=v4l2_device_register(d->dev,&d->v4l2);if(ret)goto media;
 d->m2m=v4l2_m2m_init(&video_m2m_ops);if(IS_ERR(d->m2m)){ret=PTR_ERR(d->m2m);goto v4l2;}
 strscpy(d->video_dev.name,"r1-vdec",sizeof(d->video_dev.name));d->video_dev.v4l2_dev=&d->v4l2;
 d->video_dev.fops=&video_fops;d->video_dev.ioctl_ops=&video_ioctls;d->video_dev.release=video_device_release_empty;
 d->video_dev.lock=&d->lock;d->video_dev.vfl_dir=VFL_DIR_M2M;d->video_dev.device_caps=V4L2_CAP_VIDEO_M2M_MPLANE|V4L2_CAP_STREAMING;
 video_set_drvdata(&d->video_dev,d);ret=video_register_device(&d->video_dev,VFL_TYPE_VIDEO,-1);if(ret)goto m2m;
 ret=v4l2_m2m_register_media_controller(d->m2m,&d->video_dev,MEDIA_ENT_F_PROC_VIDEO_DECODER);if(ret)goto node;
 ret=media_device_register(&d->media);if(ret)goto controller;
 return 0;
controller:v4l2_m2m_unregister_media_controller(d->m2m);
node:video_unregister_device(&d->video_dev);
m2m:v4l2_m2m_release(d->m2m);
v4l2:v4l2_device_unregister(&d->v4l2);
media:media_device_cleanup(&d->media);return ret;
}
static void unregister_video(struct decoder *d)
{
 media_device_unregister(&d->media);v4l2_m2m_unregister_media_controller(d->m2m);
 video_unregister_device(&d->video_dev);v4l2_m2m_release(d->m2m);
 v4l2_device_unregister(&d->v4l2);media_device_cleanup(&d->media);
}
