/* SPDX-License-Identifier: GPL-2.0-only */

struct context {
 struct v4l2_fh fh;
 struct v4l2_ctrl_handler controls;
 struct v4l2_ctrl *bitrate;
 struct probe *dev;
 struct work_struct job;
 unsigned int input_format;
 const struct mode *mode;
 struct buffer work[9];
 unsigned int sequence, encoded_frame;
 atomic_t force_keyframe;
 spinlock_t drain_lock;
 bool powered;
};
static struct context *file_context(struct file *file)
{ return container_of(file_to_v4l2_fh(file), struct context, fh); }
static unsigned int source_size(struct context *ctx)
{ return ctx->mode->width * ctx->mode->stride_height * 3 / 2; }
static void free_context_buffers(struct context *ctx);
static const struct v4l2_event eos_event = { .type = V4L2_EVENT_EOS };
static void run_job(struct work_struct *work)
{
 struct context *ctx = container_of(work, struct context, job);
 struct probe *p = ctx->dev;
 struct vb2_v4l2_buffer *src, *dst;
 enum vb2_buffer_state state = VB2_BUF_STATE_ERROR;
 void *input, *output;
 unsigned long flags;
 int ret;
 src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
 dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
 if (!src || !dst) goto finish;
 vb2_set_plane_payload(&dst->vb2_buf, 0, 0);
 input = vb2_plane_vaddr(&src->vb2_buf, 0);
 output = vb2_plane_vaddr(&dst->vb2_buf, 0);
 if (!input || !output || p->faulted) goto finish;
 if (ctx->input_format == V4L2_PIX_FMT_YUV420) {
  memcpy(p->input.cpu, input, source_size(ctx));
 } else {
  unsigned int pixels = ctx->mode->width * ctx->mode->stride_height, i;
  unsigned char *yuv = p->input.cpu, *uv = input + pixels;
  unsigned int swap = ctx->input_format == V4L2_PIX_FMT_NV21;
  memcpy(yuv, input, pixels);
  for (i = 0; i < pixels / 4; i++) {
   yuv[pixels + i] = uv[2 * i + swap];
   yuv[pixels * 5 / 4 + i] = uv[2 * i + !swap];
  }
 }
 if (atomic_xchg(&ctx->force_keyframe, 0)) {
  ctx->encoded_frame = 0;
  for (ret = 0; ret < ARRAY_SIZE(ctx->work); ret++)
   if (ret != 1) memset(ctx->work[ret].cpu, 0, ctx->work[ret].size);
 }
 ret = encode_frame(p, ctx->work, ctx->mode, ctx->encoded_frame, v4l2_ctrl_g_ctrl(ctx->bitrate));
 if (ret) goto finish;
 if (p->count + ctx->mode->header_size > vb2_plane_size(&dst->vb2_buf, 0)) goto finish;
 memcpy(output, ctx->mode->headers, ctx->mode->header_size);
 memcpy(output + ctx->mode->header_size, p->output.cpu, p->count);
 vb2_set_plane_payload(&dst->vb2_buf, 0, p->count + ctx->mode->header_size);
 v4l2_m2m_buf_copy_metadata(src, dst);
 dst->flags &= ~(V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME | V4L2_BUF_FLAG_BFRAME);
 dst->flags |= ctx->encoded_frame++ % 30 ? V4L2_BUF_FLAG_PFRAME : V4L2_BUF_FLAG_KEYFRAME;
 src->sequence = dst->sequence = ctx->sequence++;
 state = VB2_BUF_STATE_DONE;
finish:
 spin_lock_irqsave(&ctx->drain_lock, flags);
 src = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
 dst = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
 if (src && dst && v4l2_m2m_is_last_draining_src_buf(ctx->fh.m2m_ctx, src)) {
  dst->flags |= V4L2_BUF_FLAG_LAST;
  v4l2_m2m_mark_stopped(ctx->fh.m2m_ctx);
  v4l2_event_queue_fh(&ctx->fh, &eos_event);
 }
 if (src) v4l2_m2m_buf_done(src, state);
 if (dst) v4l2_m2m_buf_done(dst, state);
 spin_unlock_irqrestore(&ctx->drain_lock, flags);
 v4l2_m2m_job_finish(p->m2m, ctx->fh.m2m_ctx);
}
static void device_run(void *priv)
{ struct context *ctx = priv; schedule_work(&ctx->job); }
static const struct v4l2_m2m_ops m2m_ops = { .device_run = device_run };
static unsigned int size_for(struct context *ctx, enum v4l2_buf_type type)
{ return V4L2_TYPE_IS_OUTPUT(type) ? source_size(ctx) : OUTPUT_SIZE + 64; }
static int queue_setup(struct vb2_queue *q, unsigned int *buffers,
                       unsigned int *planes, unsigned int sizes[], struct device *devices[])
{
 unsigned int size = size_for(vb2_get_drv_priv(q), q->type);
 if (*planes) return *planes == 1 && sizes[0] >= size ? 0 : -EINVAL;
 *planes = 1; sizes[0] = size;
 return 0;
}
static int prepare_buffer(struct vb2_buffer *b)
{
 if (vb2_plane_size(b, 0) < size_for(vb2_get_drv_priv(b->vb2_queue), b->type)) return -EINVAL;
 if (V4L2_TYPE_IS_OUTPUT(b->type)) {
  if (b->planes[0].data_offset || vb2_get_plane_payload(b, 0) < source_size(vb2_get_drv_priv(b->vb2_queue))) return -EINVAL;
 } else vb2_set_plane_payload(b, 0, 0);
 return 0;
}
static void queue_buffer(struct vb2_buffer *b)
{
 struct context *ctx = vb2_get_drv_priv(b->vb2_queue);
 unsigned long flags;
 spin_lock_irqsave(&ctx->drain_lock, flags);
 if (!V4L2_TYPE_IS_OUTPUT(b->type) && vb2_is_streaming(b->vb2_queue) &&
     v4l2_m2m_dst_buf_is_last(ctx->fh.m2m_ctx)) {
  v4l2_m2m_last_buffer_done(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(b));
  v4l2_event_queue_fh(&ctx->fh, &eos_event);
 } else v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(b));
 spin_unlock_irqrestore(&ctx->drain_lock, flags);
}
static int start_streaming(struct vb2_queue *q, unsigned int count)
{
 struct context *ctx = vb2_get_drv_priv(q);
 struct vb2_v4l2_buffer *b;
 int ret, i;
 v4l2_m2m_update_start_streaming_state(ctx->fh.m2m_ctx, q);
 if (!V4L2_TYPE_IS_OUTPUT(q->type)) return 0;
 ctx->sequence = ctx->encoded_frame = 0;
 atomic_set(&ctx->force_keyframe, 0);
 for (i = 0; i < ARRAY_SIZE(ctx->work); i++) {
  if (i == 1) continue;
  ret = allocate(ctx->dev, &ctx->work[i], ctx->mode->sizes[i]);
  if (ret) goto failed;
 }
 ret = pm_runtime_resume_and_get(ctx->dev->dev);
 if (ret < 0) goto failed;
 ret = clk_bulk_prepare_enable(ARRAY_SIZE(ctx->dev->clks), ctx->dev->clks);
 if (ret) { pm_runtime_put_sync(ctx->dev->dev); goto failed; }
 v4l2_ctrl_grab(ctx->bitrate, true);
 ctx->powered = true;
 return 0;
failed:
 free_context_buffers(ctx);
 while ((b = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx)))
  v4l2_m2m_buf_done(b, VB2_BUF_STATE_QUEUED);
 return ret;
}
static void stop_streaming(struct vb2_queue *q)
{
 struct context *ctx = vb2_get_drv_priv(q);
 struct vb2_v4l2_buffer *b;
 v4l2_m2m_update_stop_streaming_state(ctx->fh.m2m_ctx, q);
 while ((b = V4L2_TYPE_IS_OUTPUT(q->type) ? v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
         v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx))) v4l2_m2m_buf_done(b, VB2_BUF_STATE_ERROR);
 if (V4L2_TYPE_IS_OUTPUT(q->type) && ctx->powered) {
  clk_bulk_disable_unprepare(ARRAY_SIZE(ctx->dev->clks), ctx->dev->clks);
  pm_runtime_put_sync(ctx->dev->dev);
  ctx->powered = false;
  v4l2_ctrl_grab(ctx->bitrate, false);
  free_context_buffers(ctx);
 }
}
static const struct vb2_ops queue_ops = { .queue_setup = queue_setup, .buf_prepare = prepare_buffer,
 .buf_queue = queue_buffer, .start_streaming = start_streaming, .stop_streaming = stop_streaming };
static int init_queues(void *priv, struct vb2_queue *src, struct vb2_queue *dst)
{
 struct context *ctx = priv;
 struct vb2_queue *queues[] = {src, dst};
 int i, ret;
 for (i = 0; i < 2; i++) {
  struct vb2_queue *q = queues[i];
  q->type = i ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  q->io_modes = VB2_MMAP;
  q->drv_priv = ctx; q->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
  q->ops = &queue_ops; q->mem_ops = &vb2_dma_contig_memops;
  q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
  q->lock = &ctx->dev->lock; q->dev = ctx->dev->dev;
  ret = vb2_queue_init(q); if (ret) return ret;
 }
 return 0;
}
static void free_context_buffers(struct context *ctx)
{
 int i;
 /* Hardware fault pins all possibly referenced DMA until reboot. */
 if (ctx->dev->faulted) return;
 for (i = 0; i < ARRAY_SIZE(ctx->work); i++)
  if (i != 1) free_buffer(ctx->dev, &ctx->work[i]);
}
static int set_control(struct v4l2_ctrl *ctrl)
{
 struct context *ctx = container_of(ctrl->handler, struct context, controls);
 if (ctrl->id == V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME)
  atomic_set(&ctx->force_keyframe, 1);
 return 0;
}
static const struct v4l2_ctrl_ops control_ops = { .s_ctrl = set_control };
static int open_video(struct file *file)
{
 struct probe *p = video_drvdata(file);
 struct context *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
 int ret;
 if (!ctx) return -ENOMEM;
 spin_lock_init(&ctx->drain_lock);
 ctx->dev = p; INIT_WORK(&ctx->job, run_job);
 ctx->work[1] = p->work[1];
 ctx->mode = &modes[0]; ctx->input_format = V4L2_PIX_FMT_YUV420;
 v4l2_fh_init(&ctx->fh, &p->video); file->private_data = &ctx->fh;
 v4l2_ctrl_handler_init(&ctx->controls, 2);
 ctx->bitrate = v4l2_ctrl_new_std(&ctx->controls, &control_ops, V4L2_CID_MPEG_VIDEO_BITRATE,
                                100000, 4000000, 1000, 1000000);
 v4l2_ctrl_new_std(&ctx->controls, &control_ops, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 0, 0, 0, 0);
 if (ctx->controls.error) {
  ret = ctx->controls.error; v4l2_ctrl_handler_free(&ctx->controls);
  v4l2_fh_exit(&ctx->fh); kfree(ctx); return ret;
 }
 ctx->fh.ctrl_handler = &ctx->controls;
 mutex_lock(&p->lock);
 ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(p->m2m, ctx, init_queues);
 if (IS_ERR(ctx->fh.m2m_ctx)) {
  ret = PTR_ERR(ctx->fh.m2m_ctx); v4l2_ctrl_handler_free(&ctx->controls); mutex_unlock(&p->lock); v4l2_fh_exit(&ctx->fh); free_context_buffers(ctx); kfree(ctx); return ret;
 }
 v4l2_fh_add(&ctx->fh, file); mutex_unlock(&p->lock);
 return 0;
}
static int close_video(struct file *file)
{
 struct context *ctx = file_context(file);
 mutex_lock(&ctx->dev->lock);
 v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
 cancel_work_sync(&ctx->job);
 v4l2_ctrl_handler_free(&ctx->controls);
 v4l2_fh_del(&ctx->fh, file); v4l2_fh_exit(&ctx->fh);
 free_context_buffers(ctx);
 mutex_unlock(&ctx->dev->lock); kfree(ctx);
 return 0;
}
static int query_cap(struct file *f, void *priv, struct v4l2_capability *cap)
{
 strscpy(cap->driver, "r1-venc", sizeof(cap->driver));
 strscpy(cap->card, "Rabbit R1 H264 encoder", sizeof(cap->card));
 strscpy(cap->bus_info, "platform:17020000.venc", sizeof(cap->bus_info));
 return 0;
}
static int enum_format(struct file *f, void *priv, struct v4l2_fmtdesc *fmt)
{
 static const unsigned int formats[] = {V4L2_PIX_FMT_YUV420, V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_NV21};
 if (V4L2_TYPE_IS_OUTPUT(fmt->type)) {
  if (fmt->index >= ARRAY_SIZE(formats)) return -EINVAL;
  fmt->pixelformat = formats[fmt->index];
 } else { if (fmt->index) return -EINVAL; fmt->pixelformat = V4L2_PIX_FMT_H264; }
 return 0;
}
static int fill_format(struct context *ctx, const struct mode *mode, struct v4l2_format *fmt)
{
 struct v4l2_pix_format_mplane *pix = &fmt->fmt.pix_mp;
 memset(pix, 0, sizeof(*pix));
 pix->width = mode->width; pix->height = mode->height; pix->field = V4L2_FIELD_NONE;
 pix->pixelformat = V4L2_TYPE_IS_OUTPUT(fmt->type) ? ctx->input_format : V4L2_PIX_FMT_H264;
 pix->colorspace = mode->height >= 720 ? V4L2_COLORSPACE_REC709 : V4L2_COLORSPACE_470_SYSTEM_BG;
 pix->ycbcr_enc = mode->height >= 720 ? V4L2_YCBCR_ENC_709 : V4L2_YCBCR_ENC_601;
 pix->quantization = V4L2_QUANTIZATION_LIM_RANGE; pix->xfer_func = V4L2_XFER_FUNC_709;
 pix->num_planes = 1; pix->plane_fmt[0].sizeimage = (V4L2_TYPE_IS_OUTPUT(fmt->type) ? mode->width * mode->stride_height * 3 / 2 : OUTPUT_SIZE + 64);
 pix->plane_fmt[0].bytesperline = V4L2_TYPE_IS_OUTPUT(fmt->type) ? mode->width : 0;
 return 0;
}
static const struct mode *find_mode(unsigned int width, unsigned int height)
{
 const struct mode *best = &modes[0];
 unsigned int i, distance = UINT_MAX;
 for (i = 0; i < ARRAY_SIZE(modes); i++) {
  unsigned int d = abs((int)min(width, 65535U) - (int)modes[i].width) +
                   abs((int)min(height, 65535U) - (int)modes[i].height);
  if (d < distance) { best = &modes[i]; distance = d; }
 }
 return best;
}
static int get_format(struct file *f, void *priv, struct v4l2_format *fmt)
{
 struct context *ctx = file_context(f);
 return fill_format(ctx, ctx->mode, fmt);
}
static int try_format(struct file *f, void *priv, struct v4l2_format *fmt)
{
 struct context *ctx = file_context(f);
 const struct mode *mode = V4L2_TYPE_IS_OUTPUT(fmt->type) ?
  find_mode(fmt->fmt.pix_mp.width, fmt->fmt.pix_mp.height) : ctx->mode;
 unsigned int format = fmt->fmt.pix_mp.pixelformat;
 int ret = fill_format(ctx, mode, fmt);
 if (V4L2_TYPE_IS_OUTPUT(fmt->type) && (format == V4L2_PIX_FMT_NV12 || format == V4L2_PIX_FMT_NV21))
  fmt->fmt.pix_mp.pixelformat = format;
 return ret;
}
static int set_format(struct file *f, void *priv, struct v4l2_format *fmt)
{
 struct context *ctx = file_context(f);
 if (vb2_is_busy(v4l2_m2m_get_vq(ctx->fh.m2m_ctx, fmt->type))) return -EBUSY;
 if (V4L2_TYPE_IS_OUTPUT(fmt->type)) {
  if (vb2_is_busy(v4l2_m2m_get_vq(ctx->fh.m2m_ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE))) return -EBUSY;
  ctx->input_format = (fmt->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV12 ||
    fmt->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV21) ? fmt->fmt.pix_mp.pixelformat : V4L2_PIX_FMT_YUV420;
  ctx->mode = find_mode(fmt->fmt.pix_mp.width, fmt->fmt.pix_mp.height);
  v4l2_ctrl_modify_range(ctx->bitrate, 100000,
   ctx->mode->level <= 22 ? 4000000 : ctx->mode->level <= 30 ? 10000000 :
   ctx->mode->level <= 31 ? 14000000 : 20000000, 1000, 1000000);
 }
 return get_format(f, priv, fmt);
}
static int enum_framesizes(struct file *f, void *priv, struct v4l2_frmsizeenum *e)
{
 if (e->index >= ARRAY_SIZE(modes) ||
     (e->pixel_format != V4L2_PIX_FMT_YUV420 && e->pixel_format != V4L2_PIX_FMT_NV12 &&
      e->pixel_format != V4L2_PIX_FMT_NV21 && e->pixel_format != V4L2_PIX_FMT_H264)) return -EINVAL;
 e->type = V4L2_FRMSIZE_TYPE_DISCRETE;
 e->discrete.width = modes[e->index].width; e->discrete.height = modes[e->index].height;
 return 0;
}
static int stream_parameters(struct file *f, void *priv, struct v4l2_streamparm *parm)
{
 if (parm->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) return -EINVAL;
 memset(&parm->parm.output, 0, sizeof(parm->parm.output));
 parm->parm.output.capability = V4L2_CAP_TIMEPERFRAME;
 parm->parm.output.timeperframe.numerator = 1;
 parm->parm.output.timeperframe.denominator = 30;
 return 0;
}
static int enum_intervals(struct file *f, void *priv, struct v4l2_frmivalenum *e)
{
 const struct mode *mode = find_mode(e->width, e->height);
 if (e->index || mode->width != e->width || mode->height != e->height ||
     (e->pixel_format != V4L2_PIX_FMT_YUV420 && e->pixel_format != V4L2_PIX_FMT_NV12 &&
      e->pixel_format != V4L2_PIX_FMT_NV21 && e->pixel_format != V4L2_PIX_FMT_H264)) return -EINVAL;
 e->type = V4L2_FRMIVAL_TYPE_DISCRETE;
 e->discrete.numerator = 1; e->discrete.denominator = 30;
 return 0;
}
static int encoder_command(struct file *f, void *priv, struct v4l2_encoder_cmd *cmd)
{
 struct context *ctx = file_context(f);
 unsigned long flags;
 int ret = v4l2_m2m_ioctl_try_encoder_cmd(f, priv, cmd);
 if (ret) return ret;
 if (!vb2_is_streaming(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx)) ||
     !vb2_is_streaming(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx))) return 0;
 spin_lock_irqsave(&ctx->drain_lock, flags);
 ret = v4l2_m2m_encoder_cmd(f, ctx->fh.m2m_ctx, cmd);
 if (!ret && cmd->cmd == V4L2_ENC_CMD_STOP && v4l2_m2m_has_stopped(ctx->fh.m2m_ctx))
  v4l2_event_queue_fh(&ctx->fh, &eos_event);
 spin_unlock_irqrestore(&ctx->drain_lock, flags);
 if (!ret && cmd->cmd == V4L2_ENC_CMD_START) {
  vb2_clear_last_buffer_dequeued(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx));
  atomic_set(&ctx->force_keyframe, 1);
  v4l2_m2m_try_schedule(ctx->fh.m2m_ctx);
 }
 return ret;
}
static int subscribe_event(struct v4l2_fh *fh, const struct v4l2_event_subscription *sub)
{
 if (sub->type == V4L2_EVENT_EOS) return v4l2_event_subscribe(fh, sub, 1, NULL);
 return v4l2_ctrl_subscribe_event(fh, sub);
}
static const struct v4l2_ioctl_ops ioctl_ops = {
 .vidioc_encoder_cmd = encoder_command, .vidioc_try_encoder_cmd = v4l2_m2m_ioctl_try_encoder_cmd,
 .vidioc_g_parm = stream_parameters, .vidioc_s_parm = stream_parameters,
 .vidioc_enum_frameintervals = enum_intervals,
 .vidioc_subscribe_event = subscribe_event, .vidioc_unsubscribe_event = v4l2_event_unsubscribe,
 .vidioc_querycap = query_cap, .vidioc_enum_framesizes = enum_framesizes,
 .vidioc_enum_fmt_vid_cap = enum_format, .vidioc_enum_fmt_vid_out = enum_format,
 .vidioc_g_fmt_vid_cap_mplane = get_format, .vidioc_g_fmt_vid_out_mplane = get_format,
 .vidioc_try_fmt_vid_cap_mplane = try_format, .vidioc_try_fmt_vid_out_mplane = try_format,
 .vidioc_s_fmt_vid_cap_mplane = set_format, .vidioc_s_fmt_vid_out_mplane = set_format,
 .vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs, .vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
 .vidioc_qbuf = v4l2_m2m_ioctl_qbuf, .vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
 .vidioc_streamon = v4l2_m2m_ioctl_streamon, .vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
};
static const struct v4l2_file_operations video_ops = { .owner = THIS_MODULE,
 .open = open_video, .release = close_video, .poll = v4l2_m2m_fop_poll,
 .mmap = v4l2_m2m_fop_mmap, .unlocked_ioctl = video_ioctl2 };
static int register_video(struct probe *p)
{
 int ret = v4l2_device_register(p->dev, &p->v4l2);
 if (ret) return ret;
 p->m2m = v4l2_m2m_init(&m2m_ops);
 if (IS_ERR(p->m2m)) { ret = PTR_ERR(p->m2m); goto unregister; }
 strscpy(p->video.name, "r1-venc", sizeof(p->video.name));
 p->video.v4l2_dev = &p->v4l2; p->video.fops = &video_ops; p->video.ioctl_ops = &ioctl_ops;
 p->video.release = video_device_release_empty; p->video.lock = &p->lock;
 p->video.vfl_dir = VFL_DIR_M2M;
 p->video.device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
 video_set_drvdata(&p->video, p);
 ret = video_register_device(&p->video, VFL_TYPE_VIDEO, -1);
 if (!ret) return 0;
 v4l2_m2m_release(p->m2m);
unregister:
 v4l2_device_unregister(&p->v4l2); return ret;
}
