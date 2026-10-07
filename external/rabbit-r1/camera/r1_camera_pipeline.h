/* SPDX-License-Identifier: GPL-2.0-only */
/* CAM-only bring-up, stock clk-mt6765-pg.c sequence; all waits bounded. */
#include <linux/iopoll.h>
static unsigned pipeline_stage;
module_param(pipeline_stage,uint,0400);
static void __iomem *cam_spm,*cam_infra,*cam_smi,*sen,*phy,*sv;
static struct clk *cam_clks[14];
static unsigned cam_enabled;
static bool cam_powered;
static void rmw(void __iomem *b,u32 o,u32 clear,u32 set) {writel((readl(b+o)&~clear)|set,b+o);}
static int pollbits(void __iomem *b,u32 o,u32 mask,u32 expect) {
 u32 v;return readl_poll_timeout(b+o,v,(v&mask)==expect,10,100000);
}
static struct clk *camera_clk(const char *path,u32 id) {
 struct of_phandle_args a={.args_count=1,.args={id}};struct clk *c;
 a.np=of_find_node_by_path(path);
 if(!a.np)return ERR_PTR(-ENODEV);
 c=of_clk_get_from_provider(&a);of_node_put(a.np);return c;
}
static int camera_power_on(void) {
 static const struct {const char *path;u32 id;} c[]={
 {"/r1-display/clock-controller@14000000",CLK_MM_SMI_COMMON},
 {"/r1-display/clock-controller@14000000",CLK_MM_SMI_COMM0},
 {"/r1-display/clock-controller@14000000",CLK_MM_SMI_COMM1},
 {"/r1-display/clock-controller@14000000",CLK_MM_SMI_CAM},
 {"/camsys@1a000000",CLK_CAM_LARB3},
 {"/camsys@1a000000",CLK_CAM_SENINF},
 {"/camsys@1a000000",CLK_CAMSV0},
 {"/camsys@1a000000",CLK_CAM},
 {"/camsys@1a000000",CLK_CAMTG},
 {"/camsys@1a000000",CLK_CAM_DFP_VAD},
 {"/camsys@1a000000",CLK_CAM_CCU},
 {"/apmixed@1000c000",CLK_APMIXED_MIPIC0_26M},
 {"/mipi_rx_ana_csi0a@11c10000",CLK_MIPI0A_CSR_CSI_EN_0A},
 {"/apmixed@1000c000",CLK_APMIXED_MIPIC1_26M}};
 int i,r;
 cam_spm=ioremap(0x10006000,0x1000);cam_infra=ioremap(0x10001000,0x1000);cam_smi=ioremap(0x14002000,0x1000);
 if(!cam_spm||!cam_infra||!cam_smi)return -ENOMEM;
 if((readl(cam_spm+0x180)|readl(cam_spm+0x184))&BIT(25))return -EBUSY;
 for(i=0;i<ARRAY_SIZE(c);i++) {
  cam_clks[i]=camera_clk(c[i].path,c[i].id);
  if(IS_ERR(cam_clks[i])){r=PTR_ERR(cam_clks[i]);pr_err("r1-camera-pipeline: clock %u failed %d\n",i,r);cam_clks[i]=NULL;return r;}
 }
 /* Retain shared bus gates before power transitions. */
 for(i=0;i<4;i++){r=clk_prepare_enable(cam_clks[i]);
 if(r)return r;
 cam_enabled++;}
 rmw(cam_spm,0x344,0,BIT(2));rmw(cam_spm,0x344,0,BIT(3));cam_powered=true;
 r=pollbits(cam_spm,0x180,BIT(25),BIT(25));
 if(r)return r;
 r=pollbits(cam_spm,0x184,BIT(25),BIT(25));
 if(r)return r;
 rmw(cam_spm,0x344,BIT(4),0);rmw(cam_spm,0x344,BIT(1),0);rmw(cam_spm,0x344,0,BIT(0));
 for(i=4;i<ARRAY_SIZE(c);i++){r=clk_prepare_enable(cam_clks[i]);
 if(r)return r;
 cam_enabled++;}
 rmw(cam_spm,0x344,BIT(8),0);r=pollbits(cam_spm,0x344,BIT(12),0);
 if(r)return r;
 rmw(cam_spm,0x344,BIT(9),0);r=pollbits(cam_spm,0x344,BIT(13),0);
 if(r)return r;
 writel(BIT(20),cam_infra+0x2a4);writel(BIT(3),cam_smi+0x3c8);writel(BIT(19)|BIT(21),cam_infra+0x2ac);
 sen=ioremap(0x1a040000,0x8000);phy=ioremap(0x11c10000,0x6000);sv=ioremap(0x1a050000,0x1000);
 if(!sen||!phy||!sv)return -ENOMEM;
 /* Stock SeninfDrvImp::setMclk fixed timing and clock output enables. */
 writel(0x67,sen+0x614);rmw(sen,0x200,0,1);rmw(sen,0x204,0,0x40);
 pr_info("r1-camera-pipeline: CAM on=%08x SENINF=%08x CAMSV-date=%08x proj=%08x\n",readl(cam_spm+0x344),readl(sen),readl(sv+0x3c),readl(sv+0x40));
 return 0;
}
static int camera_power_off(void) {
 int r=0,i;
 if(cam_powered) {
  writel(BIT(19)|BIT(21),cam_infra+0x2a8);
  r=pollbits(cam_infra,0x258,BIT(19)|BIT(21),BIT(19)|BIT(21));
 if(r)goto retain;
  writel(BIT(20),cam_infra+0x2a0);r=pollbits(cam_infra,0x228,BIT(20),BIT(20));
 if(r)goto retain;
  writel(BIT(3),cam_smi+0x3c4);r=pollbits(cam_smi,0x3c0,BIT(3),BIT(3));
 if(r)goto retain;
  rmw(cam_spm,0x344,0,BIT(8)|BIT(9));r=pollbits(cam_spm,0x344,BIT(12)|BIT(13),BIT(12)|BIT(13));
 if(r)goto retain;
  while(cam_enabled>4)clk_disable_unprepare(cam_clks[--cam_enabled]);
  rmw(cam_spm,0x344,0,BIT(1));rmw(cam_spm,0x344,0,BIT(4));rmw(cam_spm,0x344,BIT(0),0);
  rmw(cam_spm,0x344,BIT(2),0);rmw(cam_spm,0x344,BIT(3),0);
  r=pollbits(cam_spm,0x180,BIT(25),0);
 if(r)goto retain;
  r=pollbits(cam_spm,0x184,BIT(25),0);
 if(r)goto retain;
  cam_powered=false;
  pr_info("r1-camera-pipeline: CAM off=%08x\n",readl(cam_spm+0x344));
 }
 while(cam_enabled)clk_disable_unprepare(cam_clks[--cam_enabled]);
 for(i=ARRAY_SIZE(cam_clks)-1;i>=0;i--)if(cam_clks[i]){clk_put(cam_clks[i]);cam_clks[i]=NULL;}
 if(sv)iounmap(sv);
 if(phy)iounmap(phy);
 if(sen)iounmap(sen);
 if(cam_smi)iounmap(cam_smi);
 if(cam_infra)iounmap(cam_infra);
 if(cam_spm)iounmap(cam_spm);
 return r;
retain:
 /* Keep clock references on unverified power shutdown; no DMA is used. */
 pr_err("r1-camera-pipeline: shutdown unverified %d; clocks retained; ctl=%08x protect=%08x/%08x clamp=%08x; normal reboot required\n",r,readl(cam_spm+0x344),readl(cam_infra+0x258),readl(cam_infra+0x228),readl(cam_smi+0x3c0));
 return r;
}

/* Reference power-sequence attribution:
 * Copyright (c) 2020 MediaTek Inc.
 * Author: Owen Chen <owen.chen@mediatek.com>
 * Reference: drivers/clk/mediatek/clk-mt6765-pg.c, GPL-2.0.
 */
