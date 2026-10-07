/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef R1_H264_LIMITS_H
#define R1_H264_LIMITS_H
/* 1080 visible lines occupy1088 coded lines. Stock scratch allocations are
 * fixed0xf000/0x1e000; tiled pixels and per-picture MV grow with geometry. */
#define R1_H264_MAX_WIDTH 1936U
#define R1_H264_MAX_HEIGHT 1088U
#define R1_H264_PIXEL_STORAGE (4U*1024U*1024U)
#define R1_H264_MV_STORAGE (512U*1024U)
static inline int r1_h264_geometry_fits(unsigned int width,unsigned int height)
{
 unsigned int y,mv;
 if(!width || !height || (width|height)&15 || width>R1_H264_MAX_WIDTH || height>R1_H264_MAX_HEIGHT)return 0;
 y=(width/16)*((height+31)/32)*512;
 mv=width*4*(height/16)+512;
 return y*3/2<=R1_H264_PIXEL_STORAGE && mv<=R1_H264_MV_STORAGE;
}
#endif
