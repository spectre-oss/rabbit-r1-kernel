/* SPDX-License-Identifier: GPL-2.0-only */
/* Fixed test vectors only. Do not use these defaults for submitted streams. */
static void fixture_job(struct decoder *d, struct r1_h264_job *job)
{
 /* The diagnostic still supplies fixed syntax; the conversion accepts
  * the standard V4L2 SPS structure used by the future request interface. */
 struct v4l2_ctrl_h264_sps sps = {
  .profile_idc = 66, .chroma_format_idc = 1,
  .pic_order_cnt_type = 2,
  .max_num_ref_frames = compressed && !sequence ? 0 : 1,
  .pic_width_in_mbs_minus1 = fixture_width / 16 - 1,
  .pic_height_in_map_units_minus1 = fixture_height / 16 - 1,
  .flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY |
           V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE,
 };
 struct v4l2_ctrl_h264_pps pps = {
  .pic_init_qp_minus26 = compressed || sequence ? -2 : 0,
  .flags = V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT,
 };
 struct v4l2_ctrl_h264_slice_params slice = {
  .slice_type = sequence && d->frame ? V4L2_H264_SLICE_TYPE_P : V4L2_H264_SLICE_TYPE_I,
  .slice_qp_delta = sequence && d->frame ? 0 : (compressed || sequence) ? -3 : 0,
  .disable_deblocking_filter_idc = compressed || sequence ? 0 : 1,
 };
 struct v4l2_ctrl_h264_decode_params dec = {
  .frame_num = sequence && d->frame ? 1 : 0,
  .nal_ref_idc = sequence && d->frame ? 2 : 3,
  .flags = sequence && d->frame ? 0 : V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC,
 };
 job->sps=sps;job->pps=pps;job->slice=slice;job->dec=dec;
}
