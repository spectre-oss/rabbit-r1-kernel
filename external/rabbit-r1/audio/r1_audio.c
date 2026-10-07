// SPDX-License-Identifier: GPL-2.0
/* MT6765 DL1/VUL -> ADDA/MTKAIF -> MT6357 -> AW87390/microphones.
 * Register sequences derived from shipped MT6765/MT6357 sources.
 * Fixed 48 kHz S16 stereo; hardware pointer polled, no guessed IRQ setup.
 */
#include "r1-audio-analog.h"
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/workqueue.h>
#include <linux/regulator/consumer.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <dt-bindings/clock/mt6765-clk.h>

static bool speaker;
module_param(speaker, bool, 0644);
MODULE_PARM_DESC(speaker, "Enable speaker on next PCM prepare (default muted)");
static bool capture_sgen;
module_param(capture_sgen, bool, 0644);
MODULE_PARM_DESC(capture_sgen, "Route internal PMIC sine table to capture for diagnostics");
static bool capture_dmic;
module_param(capture_dmic, bool, 0644);
MODULE_PARM_DESC(capture_dmic, "Use the stock MT6357 digital-microphone path for diagnostics");
static bool capture_acc;
module_param(capture_acc, bool, 0644);
MODULE_PARM_DESC(capture_acc, "Use the stock MT6357 ACC analog-microphone path for diagnostics");
static unsigned int capture_ecm;
module_param(capture_ecm, uint, 0644);
MODULE_PARM_DESC(capture_ecm, "Stock DCC bias coupling: 0 normal, 1 differential ECM, 2 single-ended ECM");
static bool capture_ain1;
module_param(capture_ain1, bool, 0644);
MODULE_PARM_DESC(capture_ain1, "Diagnostic stock AIN1/MICBIAS1 path on left ADC");

struct r1_pcm {
 struct snd_pcm_substream *sub;
 unsigned bytes, period, last_pos, accumulated;
 unsigned long periods, advances, last_progress;
 bool running;
};

struct r1_audio {
 struct amp amp;
 void __iomem *afe;
 struct clk *clks[8];
 struct regulator *vaud28;
 struct gpio_desc *pins[6];
 struct snd_card *card;
 struct r1_pcm st[2];
 struct hrtimer timer;
 struct delayed_work play_guard, cap_guard;
 struct mutex power_lock;
 bool powered, mic_powered;
};

static void upd(struct r1_audio *a, u32 reg, u32 mask, u32 val)
{
 writel((readl(a->afe + reg) & ~mask) | (val & mask), a->afe + reg);
}
static void pin_mode(struct r1_audio *a, unsigned pin, unsigned mode)
{
 unsigned off = 0x300 + (pin / 8) * 0x10, shift = (pin % 8) * 4;
 writel(0xf << shift, a->amp.regs + off + 8);
 writel(mode << shift, a->amp.regs + off + 4);
}
static bool any_running(struct r1_audio *a)
{
 return READ_ONCE(a->st[SNDRV_PCM_STREAM_PLAYBACK].running) ||
        READ_ONCE(a->st[SNDRV_PCM_STREAM_CAPTURE].running);
}
static void afe_off_if_idle(struct r1_audio *a)
{
 if (any_running(a))
  return;
 upd(a, 0x124, BIT(0), 0);
 upd(a, 0x10, BIT(0), 0);
}
static void digital_stop(struct r1_audio *a)
{
 WRITE_ONCE(a->st[SNDRV_PCM_STREAM_PLAYBACK].running, false);
 upd(a, 0x10, BIT(1), 0); /* DL1 */
 upd(a, 0x108, BIT(0), 0); /* DL SRC */
 afe_off_if_idle(a);
}
static void capture_stop(struct r1_audio *a)
{
 WRITE_ONCE(a->st[SNDRV_PCM_STREAM_CAPTURE].running, false);
 upd(a, 0x10, BIT(3), 0); /* VUL */
 upd(a, 0x114, BIT(0), 0); /* ADDA UL SRC */
 afe_off_if_idle(a);
}
static int mic_on(struct r1_audio *a)
{
 struct amp *p = &a->amp;
 int ret;

 if (capture_ecm > 2) return -EINVAL;
 /* Mark partial initialization for cleanup if any PMIC write fails. */
 a->mic_powered = true;
 /* Shipped phone-mic default is DCC: main on AIN0, reference on AIN2. */
 if ((ret = pmic_upd(p, MT6357_DCXO_CW14, BIT(13), BIT(13)))) return ret;
 if ((ret = pmic_upd(p, MT6357_AUDDEC_ANA_CON11, 0, BIT(4)))) return ret;
 if ((ret = pmic_upd(p, MT6357_AUDENC_ANA_CON6, 1, 1))) return ret;
 if ((ret = pmic_upd(p, MT6357_AUD_TOP_CKPDN_CON0, 0, 0x66))) return ret;
 if ((ret = pmic_wr(p, MT6357_GPIO_MODE3, 0x0249))) return ret;
 if ((ret = pmic_upd(p, MT6357_SMT_CON1, 0x0ff0, 0x0ff0))) return ret;
 if (capture_dmic) {
  if ((ret = pmic_wr(p, MT6357_AUDENC_ANA_CON8, 0x0021))) return ret;
  if ((ret = pmic_upd(p, MT6357_AUDENC_ANA_CON9, 0, BIT(12)))) return ret;
  if ((ret = pmic_wr(p, MT6357_AUDENC_ANA_CON7, 0x0005))) return ret;
  goto digital;
 }
 if ((ret = pmic_upd(p, MT6357_AUDDEC_ANA_CON11, BIT(5), BIT(5)))) return ret;
 if ((ret = pmic_wr(p, MT6357_AUDENC_ANA_CON3, 0))) return ret;
 if ((ret = pmic_upd(p, MT6357_AUDDEC_ANA_CON12, 0x0100, 0x2500))) return ret;
 if ((ret = pmic_upd(p, MT6357_AUDDEC_ANA_CON12, 0x2500, 0x2500))) return ret;
 if (!capture_acc) {
  /* Stock DCC clock startup, including its deliberate repeated first write. */
  if ((ret = pmic_wr(p, MT6357_AFE_DCCLK_CFG0, 0x0402))) return ret;
  if ((ret = pmic_wr(p, MT6357_AFE_DCCLK_CFG0, 0x0402))) return ret;
  if ((ret = pmic_wr(p, MT6357_AFE_DCCLK_CFG0, 0x0400))) return ret;
  if ((ret = pmic_wr(p, MT6357_AFE_DCCLK_CFG0, 0x0401))) return ret;
  if ((ret = pmic_wr(p, MT6357_AFE_DCCLK_CFG1, 0x0100))) return ret;
 }
 if ((ret = pmic_wr(p, MT6357_AUDENC_ANA_CON8, 0x0021 |
                   (capture_ecm == 1 ? 0x7700 : capture_ecm == 2 ? 0x1100 : 0)))) return ret;
 if (capture_ain1 && (ret = pmic_upd(p, MT6357_AUDENC_ANA_CON9, 1, 1))) return ret;
 /* 24 dB PGA, then enable left/main and right/reference ADCs. */
 if (!capture_acc) {
  if ((ret = pmic_upd(p, MT6357_AUDENC_ANA_CON0, BIT(1), BIT(1)))) return ret;
  if ((ret = pmic_upd(p, MT6357_AUDENC_ANA_CON1, BIT(1), BIT(1)))) return ret;
  if ((ret = pmic_upd(p, MT6357_AUDENC_ANA_CON0, BIT(2), BIT(2)))) return ret;
 }
 if ((ret = pmic_upd(p, MT6357_AUDENC_ANA_CON0, capture_ain1 ? 0x0081 : 0x0041, 0x00c1))) return ret;
 if ((ret = pmic_upd(p, MT6357_AUDENC_ANA_CON0, 4 << 8, 0x0700))) return ret;
 if ((ret = pmic_upd(p, MT6357_AUDENC_ANA_CON1, 4 << 8, 0x0700))) return ret;
 usleep_range(1000, 1100);
 if ((ret = pmic_upd(p, MT6357_AUDENC_ANA_CON0, 0x5000, 0xf000))) return ret;
 /* The shipped codec resets either ADC when its RC status is stuck at an edge. */
 usleep_range(500, 520);
 {
  unsigned int rc;
  if (!regmap_read(p->pmic, MT6357_AUDENC_ANA_CON11, &rc)) {
   if ((rc & 0x1f) == 0 || (rc & 0x1f) == 0x1f) {
    pmic_upd(p, MT6357_AUDENC_ANA_CON0, 0, BIT(12));
    pmic_upd(p, MT6357_AUDENC_ANA_CON0, BIT(12), BIT(12));
   }
  }
 }
 usleep_range(500, 520);
 if (!capture_acc) {
  if ((ret = pmic_upd(p, MT6357_AUDENC_ANA_CON0, 0, BIT(2)))) return ret;
 }
digital:
 if ((ret = pmic_upd(p, MT6357_AUDIO_TOP_CON0, 0x8000, 0xdfbf))) return ret;
 if ((ret = pmic_wr(p, MT6357_AFE_TOP_CON0, 0))) return ret;
 if ((ret = pmic_upd(p, MT6357_AFE_UL_DL_CON0, 1, 1))) return ret;
 if ((ret = pmic_wr(p, MT6357_AFE_ADDA_MTKAIF_CFG0, 0))) return ret;
 if ((ret = pmic_upd(p, MT6357_AFE_AUD_PAD_TOP, 0x3100, 0xff00))) return ret;
 if ((ret = pmic_wr(p, MT6357_AFE_UL_SRC_CON0_H,
                    capture_dmic ? 0x0080 : 0x0000))) return ret;
 if ((ret = pmic_wr(p, MT6357_AFE_UL_SRC_CON0_L,
                    capture_dmic ? 0x0003 : 0x0001))) return ret;
 /* Stock ALSA powers ADC1 (including digital uplink), then ADC2. */
 if (!capture_dmic) {
  unsigned int rc;
  if (!capture_acc && (ret = pmic_upd(p, MT6357_AUDENC_ANA_CON1, BIT(2), BIT(2)))) return ret;
  if ((ret = pmic_upd(p, MT6357_AUDENC_ANA_CON1, 0x00c1, 0x00c1))) return ret;
  usleep_range(1000, 1020);
  if ((ret = pmic_upd(p, MT6357_AUDENC_ANA_CON1, 0x5000, 0xf000))) return ret;
  usleep_range(500, 520);
  if ((ret = regmap_read(p->pmic, MT6357_AUDENC_ANA_CON11, &rc))) return ret;
  if (((rc >> 8) & 0x1f) == 0 || ((rc >> 8) & 0x1f) == 0x1f) {
   if ((ret = pmic_upd(p, MT6357_AUDENC_ANA_CON1, 0, BIT(12)))) return ret;
   if ((ret = pmic_upd(p, MT6357_AUDENC_ANA_CON1, BIT(12), BIT(12)))) return ret;
  }
  usleep_range(500, 520);
  if (!capture_acc && (ret = pmic_upd(p, MT6357_AUDENC_ANA_CON1, 0, BIT(2)))) return ret;
 }
 if (capture_sgen) {
  if ((ret = pmic_upd(p, MT6357_AFE_TOP_CON0, BIT(1), BIT(1)))) return ret;
  if ((ret = pmic_wr(p, MT6357_AFE_SGEN_CFG0, 0x0080))) return ret;
  if ((ret = pmic_wr(p, MT6357_AFE_SGEN_CFG1, 0x0101))) return ret;
 }
 {
  unsigned int c0, c1, bias, dc0, dc1, top, ulh, ull;
  regmap_read(p->pmic, MT6357_AUDENC_ANA_CON0, &c0);
  regmap_read(p->pmic, MT6357_AUDENC_ANA_CON1, &c1);
  regmap_read(p->pmic, MT6357_AUDENC_ANA_CON8, &bias);
  regmap_read(p->pmic, MT6357_AFE_DCCLK_CFG0, &dc0);
  regmap_read(p->pmic, MT6357_AFE_DCCLK_CFG1, &dc1);
  regmap_read(p->pmic, MT6357_AUDIO_TOP_CON0, &top);
  regmap_read(p->pmic, MT6357_AFE_UL_SRC_CON0_H, &ulh);
  regmap_read(p->pmic, MT6357_AFE_UL_SRC_CON0_L, &ull);
  dev_info(p->dev, "mic state dmic=%u acc=%u c0=%04x c1=%04x bias=%04x dc=%04x/%04x top=%04x ul=%04x/%04x\n",
           capture_dmic, capture_acc, c0, c1, bias, dc0, dc1, top, ulh, ull);
 }
 a->mic_powered = true;
 return 0;
}
static void mic_off(struct r1_audio *a)
{
 struct amp *p = &a->amp;
 if (!a->mic_powered) return;
 pmic_upd(p, MT6357_AFE_UL_SRC_CON0_L, 0, 1);
 pmic_upd(p, MT6357_AFE_TOP_CON0, 0, BIT(1));
 pmic_wr(p, MT6357_AFE_SGEN_CFG0, 0);
 pmic_upd(p, MT6357_AFE_AUD_PAD_TOP, 0x3000, 0xff00);
 pmic_upd(p, MT6357_AUDENC_ANA_CON0, 0, 0xffff);
 pmic_upd(p, MT6357_AUDENC_ANA_CON1, 0, 0xffff);
 pmic_wr(p, MT6357_AUDENC_ANA_CON7, 0);
 pmic_wr(p, MT6357_AUDENC_ANA_CON8, 0);
 pmic_upd(p, MT6357_AUDENC_ANA_CON9, 0, 1);
 pmic_wr(p, MT6357_AFE_DCCLK_CFG0, 0x2060);
 pmic_wr(p, MT6357_AFE_DCCLK_CFG0, 0x2062);
 pmic_upd(p, MT6357_AUDDEC_ANA_CON12, 0, 0x2500);
 pmic_upd(p, MT6357_AUDDEC_ANA_CON11, 0, BIT(5));
 pmic_wr(p, MT6357_GPIO_MODE3, 0);
 if (!a->powered) {
  pmic_upd(p, MT6357_GPIO_DIR0, 0, 0xf000);
  pmic_upd(p, MT6357_SMT_CON1, 0, 0x0ff0);
  pmic_upd(p, MT6357_AUD_TOP_CKPDN_CON0, 0x66, 0x66);
  pmic_upd(p, MT6357_AUDENC_ANA_CON6, 0, 1);
  pmic_upd(p, MT6357_AUDDEC_ANA_CON11, BIT(4), BIT(4));
  pmic_upd(p, MT6357_DCXO_CW14, 0, BIT(13));
 }
 a->mic_powered = false;
 dev_info(a->amp.dev, "microphones off periods=%lu advances=%lu\n",
          a->st[SNDRV_PCM_STREAM_CAPTURE].periods,
          a->st[SNDRV_PCM_STREAM_CAPTURE].advances);
}
static void power_off(struct r1_audio *a)
{
 mutex_lock(&a->power_lock);
 if (a->powered) {
  i2c_write(&a->amp, REG_SYSCTRL, 0);
  i2c_read(&a->amp, REG_SYSCTRL, &a->amp.off_sysctrl);
  /* Keep shared PMIC clocks if capture is still using them. */
  if (!a->mic_powered)
   codec_off(&a->amp);
  a->powered = false;
  dev_info(a->amp.dev, "speaker off SYSCTRL=%02x periods=%lu advances=%lu\n",
           a->amp.off_sysctrl, a->st[SNDRV_PCM_STREAM_PLAYBACK].periods,
           a->st[SNDRV_PCM_STREAM_PLAYBACK].advances);
 }
 mutex_unlock(&a->power_lock);
}
static void play_guard_fn(struct work_struct *work)
{
 struct r1_audio *a = container_of(to_delayed_work(work), struct r1_audio, play_guard);
 if (READ_ONCE(a->st[SNDRV_PCM_STREAM_PLAYBACK].running) &&
     time_before(jiffies, READ_ONCE(a->st[SNDRV_PCM_STREAM_PLAYBACK].last_progress) + 5 * HZ)) {
  schedule_delayed_work(&a->play_guard, 120 * HZ);
  return;
 }
 digital_stop(a);
 power_off(a);
 dev_warn(a->amp.dev, "playback power guard expired\n");
}
static void cap_guard_fn(struct work_struct *work)
{
 struct r1_audio *a = container_of(to_delayed_work(work), struct r1_audio, cap_guard);
 if (READ_ONCE(a->st[SNDRV_PCM_STREAM_CAPTURE].running) &&
     time_before(jiffies, READ_ONCE(a->st[SNDRV_PCM_STREAM_CAPTURE].last_progress) + 5 * HZ)) {
  schedule_delayed_work(&a->cap_guard, 30 * HZ);
  return;
 }
 capture_stop(a);
 mic_off(a);
 dev_warn(a->amp.dev, "capture power guard expired\n");
}
static void tick_stream(struct r1_audio *a, int stream, u32 cur_reg)
{
 struct r1_pcm *s = &a->st[stream];
 unsigned cur, pos, delta;
 if (!READ_ONCE(s->running) || !s->sub || !s->sub->runtime)
  return;
 cur = readl(a->afe + cur_reg);
 if (cur < lower_32_bits(s->sub->runtime->dma_addr) ||
     cur >= lower_32_bits(s->sub->runtime->dma_addr) + s->bytes)
  return;
 pos = cur - lower_32_bits(s->sub->runtime->dma_addr);
 delta = (pos + s->bytes - s->last_pos) % s->bytes;
 if (delta) {
  s->advances++;
  WRITE_ONCE(s->last_progress, jiffies);
 }
 s->last_pos = pos;
 s->accumulated += delta;
 if (s->accumulated >= s->period) {
  s->accumulated %= s->period;
  s->periods++;
  snd_pcm_period_elapsed(s->sub);
 }
}
static enum hrtimer_restart poll_dma(struct hrtimer *timer)
{
 struct r1_audio *a = container_of(timer, struct r1_audio, timer);
 if (!any_running(a))
  return HRTIMER_NORESTART;
 tick_stream(a, SNDRV_PCM_STREAM_PLAYBACK, 0x44);
 tick_stream(a, SNDRV_PCM_STREAM_CAPTURE, 0x8c);
 if (!any_running(a))
  return HRTIMER_NORESTART;
 hrtimer_forward_now(timer, ms_to_ktime(1));
 return HRTIMER_RESTART;
}
static const struct snd_pcm_hardware pcm_hw = {
 .info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
         SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID,
 .formats = SNDRV_PCM_FMTBIT_S16_LE,
 .rates = SNDRV_PCM_RATE_48000, .rate_min = 48000, .rate_max = 48000,
 .channels_min = 2, .channels_max = 2,
 .buffer_bytes_max = 65536, .period_bytes_min = 1024,
 .period_bytes_max = 16384, .periods_min = 2, .periods_max = 64,
};
static int pcm_open(struct snd_pcm_substream *sub)
{
 struct r1_audio *a = snd_pcm_substream_chip(sub);
 struct r1_pcm *s = &a->st[sub->stream];
 if (s->sub)
  return -EBUSY;
 sub->runtime->hw = pcm_hw;
 s->sub = sub;
 return snd_pcm_hw_constraint_integer(sub->runtime, SNDRV_PCM_HW_PARAM_PERIODS);
}
static int pcm_close(struct snd_pcm_substream *sub)
{
 struct r1_audio *a = snd_pcm_substream_chip(sub);
 if (sub->stream == SNDRV_PCM_STREAM_CAPTURE) {
  capture_stop(a);
  cancel_delayed_work_sync(&a->cap_guard);
  mic_off(a);
 } else {
  digital_stop(a);
  cancel_delayed_work_sync(&a->play_guard);
  power_off(a);
 }
 if (!any_running(a))
  hrtimer_cancel(&a->timer);
 a->st[sub->stream].sub = NULL;
 return 0;
}
static int pcm_free(struct snd_pcm_substream *sub)
{
 struct r1_audio *a = snd_pcm_substream_chip(sub);
 if (sub->stream == SNDRV_PCM_STREAM_CAPTURE)
  capture_stop(a);
 else
  digital_stop(a);
 if (!any_running(a))
  hrtimer_cancel(&a->timer);
 return 0;
}
static int pcm_prepare(struct snd_pcm_substream *sub)
{
 struct r1_audio *a = snd_pcm_substream_chip(sub);
 struct snd_pcm_runtime *r = sub->runtime;
 struct r1_pcm *s = &a->st[sub->stream];
 int ret = 0;
 /* A previous prepare must not leave a guard racing the new stream. */
 cancel_delayed_work_sync(sub->stream == SNDRV_PCM_STREAM_CAPTURE ?
                          &a->cap_guard : &a->play_guard);
 if (sub->stream == SNDRV_PCM_STREAM_CAPTURE)
  capture_stop(a);
 else
  digital_stop(a);
 s->bytes = snd_pcm_lib_buffer_bytes(sub);
 s->period = snd_pcm_lib_period_bytes(sub);
 s->last_pos = s->accumulated = 0;
 s->periods = s->advances = 0;
 WRITE_ONCE(s->last_progress, jiffies);
 if (upper_32_bits(r->dma_addr) || upper_32_bits(r->dma_addr + s->bytes - 1))
  return -ERANGE;
 if (sub->stream == SNDRV_PCM_STREAM_CAPTURE) {
  pin_mode(a, 140, 1);
  /* VUL captures stereo main/reference mics at 48 kHz S16. */
  writel(lower_32_bits(r->dma_addr), a->afe + 0x80);
  writel(lower_32_bits(r->dma_addr) + s->bytes - 1, a->afe + 0x88);
  writel(0, a->afe + 0xb30); writel(0, a->afe + 0xb38);
  upd(a, 0x14, (0xf << 16) | BIT(27), 10 << 16);
  upd(a, 0x3f8, 3 << 6, 0); upd(a, 0x3fc, BIT(3) | BIT(19), 0);
  upd(a, 0xcc, BIT(3), 0); upd(a, 0x6c, BIT(9) | BIT(10), 0);
  writel(BIT(3), a->afe + 0x440); writel(BIT(4), a->afe + 0x444);
  writel(0x31, a->afe + 0xe40); /* stock receiver pads before ADC enable */
  writel(0, a->afe + 0xe00); /* protocol 1, no loopback */
  /* Match set_chip_adc_in/set_ap_dmic: select internal ADC and explicitly
   * select AMIC/DMIC receiver format. PMIC mode alone is insufficient. */
  upd(a, 0x120, BIT(0), 0);
  upd(a, 0xe20, BIT(0) | (0xf << 20), capture_dmic ? BIT(0) : 0);
  /* 35 Hz input high-pass coefficients verified in shipped set_chip_adc_in. */
  writel(0, a->afe + 0x290);
  writel(0x3fb8, a->afe + 0x294);
  writel(0x3fb80000, a->afe + 0x298);
  writel(0x3fb80000, a->afe + 0x29c);
  writel(0xc048, a->afe + 0x2a0);
  writel((3 << 17) | BIT(10) | (capture_dmic ? ((3 << 21) | BIT(1)) : 0),
         a->afe + 0x114); /* 48k; optional stock AP-DMIC clock mode */
  /* Stock pcm_prepare enables ADDA/UL before HAL powers the PMIC ADCs. */
  upd(a, 0x10, BIT(0), BIT(0));
  upd(a, 0x124, BIT(0), BIT(0));
  upd(a, 0x114, BIT(0), BIT(0));
  mutex_lock(&a->power_lock); ret = mic_on(a); mutex_unlock(&a->power_lock);
  if (ret) { capture_stop(a); mic_off(a); return ret; }
  writel(0x31, a->afe + 0xe40);
  schedule_delayed_work(&a->cap_guard, 30 * HZ);
  dev_info(a->amp.dev, "capture prepare DMA=%pad bytes=%u main+reference 24dB\n", &r->dma_addr, s->bytes);
  return 0;
 }
 /* 48k stereo, 16-bit packed DL1; explicit low-address DMA. */
 writel(lower_32_bits(r->dma_addr), a->afe + 0x40);
 writel(lower_32_bits(r->dma_addr) + s->bytes - 1, a->afe + 0x48);
 writel(0, a->afe + 0xb00);
 writel(0, a->afe + 0xb08);
 upd(a, 0x14, 0xf | BIT(21), 10);
 upd(a, 0x3f8, 3, 0);
 upd(a, 0x3fc, BIT(0) | BIT(16), 0);
 upd(a, 0xcc, BIT(0), 0);
 upd(a, 0x6c, BIT(3) | BIT(4), 0);
 /* MEM_DL1 I05/I06 to ADDA O03/O04 (MT6765 connection table). */
 writel(BIT(5), a->afe + 0x2c);
 writel(BIT(6), a->afe + 0x30);
 writel(0, a->afe + 0x260);
 writel(0, a->afe + 0x264);
 writel(0x83001802, a->afe + 0x108); /* 48k SRC, gain enabled */
 writel(0xf74f0000, a->afe + 0x10c); /* vendor -0.3 dB */
 upd(a, 0xc50, 0x3f | BIT(8), 0x1d); /* SDM normal, no DC injection */
 writel(0, a->afe + 0xe00); /* MTKAIF protocol 1 */
 mutex_lock(&a->power_lock);
 if (speaker) {
  a->powered = true;
  a->amp.io_error = 0;
  ret = codec_on(&a->amp);
  if (!ret) ret = amp_apply(&a->amp, music_acf, sizeof(music_acf));
 }
 mutex_unlock(&a->power_lock);
 if (ret) { power_off(a); return ret; }
 schedule_delayed_work(&a->play_guard, 120 * HZ);
 dev_info(a->amp.dev, "prepare DMA=%pad bytes=%u speaker=%d\n", &r->dma_addr, s->bytes, speaker);
 return 0;
}
static int pcm_trigger(struct snd_pcm_substream *sub, int cmd)
{
 struct r1_audio *a = snd_pcm_substream_chip(sub);
 switch (cmd) {
 case SNDRV_PCM_TRIGGER_START:
  if (sub->stream == SNDRV_PCM_STREAM_CAPTURE) {
   upd(a, 0x10, BIT(0), BIT(0)); upd(a, 0x124, BIT(0), BIT(0));
   upd(a, 0x114, BIT(0), BIT(0)); upd(a, 0x10, BIT(3), BIT(3));
   WRITE_ONCE(a->st[SNDRV_PCM_STREAM_CAPTURE].running, true);
   hrtimer_start(&a->timer, ms_to_ktime(1), HRTIMER_MODE_REL);
   return 0;
  }
  upd(a, 0x10, BIT(0), BIT(0));
  upd(a, 0x124, BIT(0), BIT(0));
  upd(a, 0x108, BIT(0), BIT(0));
  upd(a, 0x10, BIT(1), BIT(1));
  WRITE_ONCE(a->st[SNDRV_PCM_STREAM_PLAYBACK].running, true);
  hrtimer_start(&a->timer, ms_to_ktime(1), HRTIMER_MODE_REL);
  return 0;
 case SNDRV_PCM_TRIGGER_STOP:
  if (sub->stream == SNDRV_PCM_STREAM_CAPTURE) capture_stop(a); else digital_stop(a);
  return 0;
 default: return -EINVAL;
 }
}
static snd_pcm_uframes_t pcm_pointer(struct snd_pcm_substream *sub)
{
 struct r1_audio *a = snd_pcm_substream_chip(sub);
 struct r1_pcm *s = &a->st[sub->stream];
 u32 cur = readl(a->afe + (sub->stream == SNDRV_PCM_STREAM_CAPTURE ? 0x8c : 0x44));
 u32 base = lower_32_bits(sub->runtime->dma_addr);
 if (cur < base || cur >= base + s->bytes) return 0;
 return bytes_to_frames(sub->runtime, cur - base);
}
static const struct snd_pcm_ops pcm_ops = {
 .open = pcm_open, .close = pcm_close, .ioctl = snd_pcm_lib_ioctl,
 .hw_free = pcm_free, .prepare = pcm_prepare,
 .trigger = pcm_trigger, .pointer = pcm_pointer,
};
static struct clk *get_clock(const char *compat, unsigned id)
{
 struct of_phandle_args spec = { .args_count = 1, .args = {id} };
 struct clk *clk;
 spec.np = of_find_compatible_node(NULL, NULL, compat);
 if (!spec.np) return ERR_PTR(-ENODEV);
 clk = of_clk_get_from_provider(&spec);
 of_node_put(spec.np);
 return clk;
}
static ssize_t status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
 struct r1_audio *a = dev_get_drvdata(dev);
 return sysfs_emit(buf, "running=%u powered=%u mic_powered=%u off_sysctrl=%02x periods=%lu advances=%lu cap_periods=%lu cap_advances=%lu cur=%08x vul_cur=%08x dac=%08x src=%08x ul_src=%08x\n",
  any_running(a), a->powered, a->mic_powered, a->amp.off_sysctrl,
  a->st[SNDRV_PCM_STREAM_PLAYBACK].periods, a->st[SNDRV_PCM_STREAM_PLAYBACK].advances,
  a->st[SNDRV_PCM_STREAM_CAPTURE].periods, a->st[SNDRV_PCM_STREAM_CAPTURE].advances,
  readl(a->afe+0x44), readl(a->afe+0x8c), readl(a->afe+0x10), readl(a->afe+0x108), readl(a->afe+0x114));
}
static DEVICE_ATTR_RO(status);
static struct attribute *attrs_attrs[] = { &dev_attr_status.attr, NULL };
ATTRIBUTE_GROUPS(attrs);
static void clocks_off(void *arg)
{
 struct r1_audio *a = arg;
 int i;
 for (i = ARRAY_SIZE(a->clks)-1; i >= 0; i--)
  if (!IS_ERR_OR_NULL(a->clks[i])) { clk_disable_unprepare(a->clks[i]); clk_put(a->clks[i]); }
}
static void regulator_off(void *arg)
{
 struct r1_audio *a = arg;
 regulator_disable(a->vaud28);
 regulator_put(a->vaud28);
}
static int audio_probe(struct platform_device *pdev)
{
 struct r1_audio *a;
 struct device_node *np;
 struct platform_device *pmic;
 struct mt6397_chip *chip;
 struct snd_pcm *pcm;
 int ret, i;
 u8 id;
 const unsigned pins[] = {136, 138, 139, 140, 142, 143};
 const unsigned modes[] = {1, 1, 1, 1, 1, 1};
 const char *names[] = {"mosi-clk", "mosi0", "mosi1", "miso-clk", "miso0", "miso1"};
 const char *compat[] = {"mediatek,mt6765-infracfg", "mediatek,mt6765-infracfg",
  "mediatek,mt6765-topckgen", "mediatek,mt6765-topckgen", "mediatek,mt6765-audsys",
  "mediatek,mt6765-audsys", "mediatek,mt6765-audsys", "mediatek,mt6765-audsys"};
 const unsigned ids[] = {CLK_IFR_AUDIO, CLK_IFR_AUDIO_26M_BCLK, CLK_TOP_AUDIO_SEL,
  CLK_TOP_AUD_INTBUS_SEL, CLK_AUDIO_AFE, CLK_AUDIO_DAC, CLK_AUDIO_DAC_PREDIS,
  CLK_AUDIO_ADC};
 a = devm_kzalloc(&pdev->dev, sizeof(*a), GFP_KERNEL);
 if (!a) return -ENOMEM;
 a->amp.dev = &pdev->dev;
 mutex_init(&a->power_lock);
 hrtimer_setup(&a->timer, poll_dma, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
 INIT_DELAYED_WORK(&a->play_guard, play_guard_fn);
 INIT_DELAYED_WORK(&a->cap_guard, cap_guard_fn);
 a->amp.regs = devm_ioremap(&pdev->dev, GPIO_PA, 0x1000);
 if (!a->amp.regs) return -ENOMEM;
 a->amp.scl = devm_gpiod_get(&pdev->dev, "scl", GPIOD_IN);
 if (IS_ERR(a->amp.scl)) return PTR_ERR(a->amp.scl);
 a->amp.sda = devm_gpiod_get(&pdev->dev, "sda", GPIOD_IN);
 if (IS_ERR(a->amp.sda)) return PTR_ERR(a->amp.sda);
 np = of_find_compatible_node(NULL, NULL, "mediatek,mt6357");
 if (!np) return -ENODEV;
 pmic = of_find_device_by_node(np); of_node_put(np);
 if (!pmic) return -EPROBE_DEFER;
 chip = dev_get_drvdata(&pmic->dev);
 a->amp.pmic = chip ? chip->regmap : NULL;
 put_device(&pmic->dev);
 if (!a->amp.pmic) return -EPROBE_DEFER;
 /* The upstream MT6357 codec requires VAUD28 before codec register access. */
 a->vaud28 = regulator_get(NULL, "VAUD28");
 if (IS_ERR(a->vaud28))
  return dev_err_probe(&pdev->dev, PTR_ERR(a->vaud28), "VAUD28\n");
 ret = regulator_enable(a->vaud28);
 if (ret) { regulator_put(a->vaud28); return ret; }
 ret = devm_add_action_or_reset(&pdev->dev, regulator_off, a);
 if (ret) return ret;
 gpio_mode0(&a->amp, PIN_SCL); gpio_mode0(&a->amp, PIN_SDA);
 od(a->amp.scl, 1); od(a->amp.sda, 1);
 ret = i2c_read(&a->amp, REG_ID, &id);
 if (ret || id != CHIP_ID) return ret ? ret : -ENODEV;
 ret = i2c_write(&a->amp, REG_SYSCTRL, 0);
 if (ret) return ret;
 for (i=0; i<ARRAY_SIZE(pins); i++) {
  a->pins[i] = devm_gpiod_get(&pdev->dev, names[i], GPIOD_IN);
  if (IS_ERR(a->pins[i])) return PTR_ERR(a->pins[i]);
  pin_mode(a, pins[i], modes[i]);
 }
 ret = devm_add_action_or_reset(&pdev->dev, clocks_off, a);
 if (ret) return ret;
 for (i=0; i<ARRAY_SIZE(ids); i++) {
  a->clks[i] = get_clock(compat[i], ids[i]);
  if (IS_ERR(a->clks[i])) return dev_err_probe(&pdev->dev, PTR_ERR(a->clks[i]), "clock %d\n", i);
  ret = clk_prepare_enable(a->clks[i]);
  if (ret) { clk_put(a->clks[i]); a->clks[i] = NULL; return ret; }
 }
 a->afe = devm_ioremap(&pdev->dev, 0x11220000, 0x1000);
 if (!a->afe) return -ENOMEM;
 dev_info(&pdev->dev, "AFE clocked: TOP=%08x DAC=%08x ID=%02x; speaker off\n", readl(a->afe), readl(a->afe+0x10), id);
 digital_stop(a);
 ret = dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
 if (ret) return ret;
 ret = snd_card_new(&pdev->dev, -1, "R1", THIS_MODULE, 0, &a->card);
 if (ret) return ret;
 strscpy(a->card->driver, "R1-MT6765");
 strscpy(a->card->shortname, "Rabbit R1 Speaker");
 strscpy(a->card->longname, "Rabbit R1 MT6765 MT6357 AW87390");
 ret = snd_pcm_new(a->card, "MT6765 audio", 0, 1, 1, &pcm);
 if (ret) goto fail;
 pcm->private_data = a;
 strscpy(pcm->name, "MT6765 DL1 speaker");
 snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &pcm_ops);
 snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &pcm_ops);
 ret = snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_DEV, &pdev->dev, 65536, 65536);
 if (ret) goto fail;
 platform_set_drvdata(pdev, a);
 ret = snd_card_register(a->card);
 if (ret) goto fail;
 return 0;
fail:
 snd_card_free(a->card);
 return ret;
}
static void audio_remove(struct platform_device *pdev)
{
 struct r1_audio *a = platform_get_drvdata(pdev);
 snd_card_disconnect(a->card);
 digital_stop(a);
 capture_stop(a);
 hrtimer_cancel(&a->timer);
 cancel_delayed_work_sync(&a->play_guard);
 cancel_delayed_work_sync(&a->cap_guard);
 power_off(a);
 mic_off(a);
 snd_card_free(a->card);
 pin_mode(a,136,0);pin_mode(a,138,0);pin_mode(a,139,0);
 pin_mode(a,140,0);pin_mode(a,142,0);pin_mode(a,143,0);
}
static void audio_shutdown(struct platform_device *pdev)
{
 struct r1_audio *a = platform_get_drvdata(pdev);
 digital_stop(a);
 capture_stop(a);
 hrtimer_cancel(&a->timer);
 cancel_delayed_work_sync(&a->play_guard);
 cancel_delayed_work_sync(&a->cap_guard);
 power_off(a);
 mic_off(a);
}
static struct gpiod_lookup_table audio_lookup = {
 .dev_id = "r1-audio", .table = {
 GPIO_LOOKUP("pinctrl_paris",83,"scl",GPIO_ACTIVE_HIGH),
 GPIO_LOOKUP("pinctrl_paris",82,"sda",GPIO_ACTIVE_HIGH),
 GPIO_LOOKUP("pinctrl_paris",136,"mosi-clk",GPIO_ACTIVE_HIGH),
 GPIO_LOOKUP("pinctrl_paris",138,"mosi0",GPIO_ACTIVE_HIGH),
 GPIO_LOOKUP("pinctrl_paris",139,"mosi1",GPIO_ACTIVE_HIGH),
 GPIO_LOOKUP("pinctrl_paris",140,"miso-clk",GPIO_ACTIVE_HIGH),
 GPIO_LOOKUP("pinctrl_paris",142,"miso0",GPIO_ACTIVE_HIGH),
 GPIO_LOOKUP("pinctrl_paris",143,"miso1",GPIO_ACTIVE_HIGH), {}
 }
};
static struct platform_driver audio_driver = {
 .probe=audio_probe, .remove=audio_remove, .shutdown=audio_shutdown,
 .driver={ .name="r1-audio", .dev_groups=attrs_groups }
};
static struct platform_device *audio_pdev;
static int __init audio_init(void)
{
 int ret;
 gpiod_add_lookup_table(&audio_lookup);
 ret=platform_driver_register(&audio_driver);
 if (ret) goto fail;
 audio_pdev=platform_device_register_simple("r1-audio",-1,NULL,0);
 if (!IS_ERR(audio_pdev)) return 0;
 ret=PTR_ERR(audio_pdev);
 platform_driver_unregister(&audio_driver);
fail:
 gpiod_remove_lookup_table(&audio_lookup);
 return ret;
}
static void __exit audio_exit(void)
{
 platform_device_unregister(audio_pdev);
 platform_driver_unregister(&audio_driver);
 gpiod_remove_lookup_table(&audio_lookup);
}
module_init(audio_init);
module_exit(audio_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rabbit R1 MT6765 DL1 PCM speaker bring-up");

/* Vendor reference attribution retained for MT6357/MT6765 sequences:
 * Copyright (c) 2019 MediaTek Inc.
 * Author: Michael Hsiao <michael.hsiao@mediatek.com>
 * Codec reference also credits Chipeng Chang.
 * See Documentation/rabbit-r1/external-sources.json for reference hashes.
 */
