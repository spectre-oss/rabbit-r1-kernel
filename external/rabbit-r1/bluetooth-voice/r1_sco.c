// SPDX-License-Identifier: GPL-2.0
/* MT6765 BTCVSD SRAM -> H4 voice transport. Packet layout derives from
 * MediaTek's mtk-btcvsd.c (KaiChieh Chuang, copyright 2019 MediaTek).
 * Poll only during an established transparent eSCO connection. The stock
 * AXI-BUS reserves the whole CONSYS aperture; map the DT subresources as
 * the stock driver does. Never route voice to the phone speaker or mic.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/unaligned.h>
#define START _IOW('R', 1, unsigned int)
#define STOP _IO('R', 2)
#define Q 64
#define CLEAR (BIT(21) | BIT(22) | BIT(23) | BIT(24) | BIT(31))
struct packet {
	unsigned int len;
	u8 data[64];
};
struct sco {
	struct miscdevice misc;
	void __iomem *regs, *sram;
	u32 offsets[5];
	struct mutex lock;
	struct delayed_work work;
	wait_queue_head_t wait;
	bool opened, active;
	u16 handle;
	struct packet rx[Q];
	unsigned int r, w;
	u8 tx[Q][60];
	unsigned int tr, tw, completed;
	u64 polls, frames, invalid, drops, written;
	u32 last_control;
};
static const unsigned int sizes[6][2] = { { 30, 6 },  { 60, 3 },  { 90, 2 },
					  { 120, 1 }, { 10, 18 }, { 20, 9 } };
static const u32 masks[6][6] = { { 1, 2, 4, 8, 16, 32 },
				 { 1, 1, 2, 2, 4, 4 },
				 { 1, 1, 1, 2, 2, 2 },
				 { 1, 1, 1, 1, 0, 0 },
				 { 7, 56, 448, 3584, 28672, 229376 },
				 { 3, 6, 24, 48, 192, 384 } };
/* One correctly encoded silent mSBC frame; H2 sequence is filled per frame. */
static const u8 silence[60] = {
	1,    8,    0xad, 0,	0,    0xc5, 0,	  0,	0,    0,    0x77, 0x6d,
	0xb6, 0xdd, 0xdb, 0x6d, 0xb7, 0x76, 0xdb, 0x6d, 0xdd, 0xb6, 0xdb, 0x77,
	0x6d, 0xb6, 0xdd, 0xdb, 0x6d, 0xb7, 0x76, 0xdb, 0x6d, 0xdd, 0xb6, 0xdb,
	0x77, 0x6d, 0xb6, 0xdd, 0xdb, 0x6d, 0xb7, 0x76, 0xdb, 0x6d, 0xdd, 0xb6,
	0xdb, 0x77, 0x6d, 0xb6, 0xdd, 0xdb, 0x6d, 0xb7, 0x76, 0xdb, 0x6c, 0
};
static void enqueue(struct sco *s, const u8 *p, unsigned int n)
{
	if (s->w - s->r == Q) {
		s->drops++;
		return;
	}
	s->rx[s->w % Q].len = n;
	memcpy(s->rx[s->w % Q].data, p, n);
	s->w++;
}
static void transfer(struct sco *s, u8 *buf, u32 offset, unsigned int size,
		     unsigned int count, bool tx)
{
	unsigned int block, i,
		pos = 0,
		step = size +
		       ((size == 60 || size == 120 || size == 20) ? 0 : 2);
	for (block = 0; block < count; block++)
		for (i = 0; i < size; i += 2, pos += 2) {
			void __iomem *p = s->sram + offset + block * step + i;
			if (tx)
				writew(get_unaligned_le16(buf + pos), p);
			else
				put_unaligned_le16(readw(p), buf + pos);
		}
}
static void tick(struct work_struct *work)
{
	struct sco *s = container_of(to_delayed_work(work), struct sco, work);
	u8 data[180], out[180], p[64];
	u32 ctl, rx, tx, type, size, num, span;
	unsigned int i, done = 0;
	mutex_lock(&s->lock);
	if (!s->active)
		goto end;
	s->polls++;
	ctl = readl(s->regs + s->offsets[4]);
	s->last_control = ctl;
	if (ctl == 0xdeadfeed || !(ctl & BIT(31)))
		goto again;
	type = (ctl >> 18) & 7;
	if (type >= 6) {
		s->invalid++;
		goto ack;
	}
	size = sizes[type][0];
	num = sizes[type][1];
	span = (size + ((size == 60 || size == 120 || size == 20) ? 0 : 2)) *
	       num;
	rx = readl(s->regs + s->offsets[2]);
	tx = readl(s->regs + s->offsets[3]);
	if (rx == 0xdeadfeed || tx == 0xdeadfeed) {
		s->invalid++;
		goto again;
	}
	rx &= 0xffff;
	tx &= 0xffff;
	if (rx + span > 0x10000 || tx + span > 0x10000 || (rx | tx) & 1) {
		s->invalid++;
		goto ack;
	}
	transfer(s, data, rx, size, num, false);
	for (i = 0; i < size * num / 60; i++) {
		u32 valid = masks[type][2 * i] | masks[type][2 * i + 1];
		p[0] = 3;
		put_unaligned_le16(
			s->handle | ((ctl & valid) == valid ? 0 : 0x2000),
			p + 1);
		p[3] = 60;
		memcpy(p + 4, data + i * 60, 60);
		enqueue(s, p, 64);
		s->frames++;
		if ((ctl & valid) != valid)
			s->invalid++;
		if (s->tr != s->tw) {
			memcpy(out + i * 60, s->tx[s->tr++ % Q], 60);
			done++;
			s->written++;
		} else {
			memcpy(out + i * 60, silence, 60);
			out[i * 60 + 1] =
				(u8[]){ 8, 0x38, 0xc8, 0xf8 }[s->frames % 4];
		}
	}
	transfer(s, out, tx, size, num, true);
	/* Credits correspond to packets actually consumed by the SRAM transport. */
	s->completed += done;
	wmb();
ack:
	writel(ctl & ~CLEAR, s->regs + s->offsets[4]);
	wake_up_interruptible(&s->wait);
again:
	schedule_delayed_work(&s->work, msecs_to_jiffies(2));
end:
	mutex_unlock(&s->lock);
}
static int open_sco(struct inode *ino, struct file *f)
{
	struct sco *s = container_of(f->private_data, struct sco, misc);
	int ret = 0;
	mutex_lock(&s->lock);
	if (s->opened)
		ret = -EBUSY;
	else {
		s->opened = true;
		f->private_data = s;
	}
	mutex_unlock(&s->lock);
	return ret;
}
static void stop_sco(struct sco *s)
{
	mutex_lock(&s->lock);
	s->active = false;
	mutex_unlock(&s->lock);
	cancel_delayed_work_sync(&s->work);
	mutex_lock(&s->lock);
	s->r = s->w = s->tr = s->tw = s->completed = 0;
	mutex_unlock(&s->lock);
}
static int close_sco(struct inode *ino, struct file *f)
{
	struct sco *s = f->private_data;
	stop_sco(s);
	mutex_lock(&s->lock);
	s->opened = false;
	mutex_unlock(&s->lock);
	return 0;
}
static long ioctl_sco(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct sco *s = f->private_data;
	unsigned int handle;
	if (cmd == STOP) {
		stop_sco(s);
		return 0;
	}
	if (cmd != START)
		return -ENOTTY;
	if (copy_from_user(&handle, (void __user *)arg, sizeof(handle)))
		return -EFAULT;
	if (handle > 0xfff)
		return -EINVAL;
	stop_sco(s);
	mutex_lock(&s->lock);
	s->handle = handle;
	s->active = true;
	schedule_delayed_work(&s->work, 0);
	mutex_unlock(&s->lock);
	return 0;
}
static ssize_t read_sco(struct file *f, char __user *buf, size_t n, loff_t *pos)
{
	struct sco *s = f->private_data;
	struct packet *p;
	ssize_t ret;
	mutex_lock(&s->lock);
	if (s->completed) {
		u8 complete[] = { 4, 0x13, 5, 1, 0, 0, 0, 0 };
		if (n < sizeof(complete)) {
			ret = -EMSGSIZE;
			goto out;
		}
		put_unaligned_le16(s->handle, complete + 4);
		put_unaligned_le16(s->completed, complete + 6);
		ret = copy_to_user(buf, complete, sizeof(complete)) ?
			      -EFAULT :
			      sizeof(complete);
		if (ret > 0)
			s->completed = 0;
		goto out;
	}
	if (s->r == s->w) {
		ret = -EAGAIN;
		goto out;
	}
	p = &s->rx[s->r % Q];
	if (n < p->len) {
		ret = -EMSGSIZE;
		goto out;
	}
	ret = copy_to_user(buf, p->data, p->len) ? -EFAULT : p->len;
	if (ret > 0)
		s->r++;
out:
	mutex_unlock(&s->lock);
	return ret;
}
static ssize_t write_sco(struct file *f, const char __user *buf, size_t n,
			 loff_t *pos)
{
	struct sco *s = f->private_data;
	u8 p[64];
	ssize_t ret = n;
	if (n != 64)
		return -EMSGSIZE;
	if (copy_from_user(p, buf, n))
		return -EFAULT;
	mutex_lock(&s->lock);
	if (!s->active || p[0] != 3 || p[3] != 60 ||
	    (get_unaligned_le16(p + 1) & 0xfff) != s->handle)
		ret = -EINVAL;
	else if (s->tw - s->tr == Q)
		ret = -EAGAIN;
	else
		memcpy(s->tx[s->tw++ % Q], p + 4, 60);
	mutex_unlock(&s->lock);
	return ret;
}
static __poll_t poll_sco(struct file *f, poll_table *wait)
{
	struct sco *s = f->private_data;
	__poll_t flags = 0;
	poll_wait(f, &s->wait, wait);
	mutex_lock(&s->lock);
	if (s->r != s->w || s->completed)
		flags |= EPOLLIN | EPOLLRDNORM;
	if (s->active && s->tw - s->tr < Q)
		flags |= EPOLLOUT | EPOLLWRNORM;
	mutex_unlock(&s->lock);
	return flags;
}
static const struct file_operations ops = { .owner = THIS_MODULE,
					    .open = open_sco,
					    .release = close_sco,
					    .read = read_sco,
					    .write = write_sco,
					    .poll = poll_sco,
					    .unlocked_ioctl = ioctl_sco };
static ssize_t stats_show(struct device *d, struct device_attribute *a,
			  char *buf)
{
	struct sco *s = dev_get_drvdata(d);
	ssize_t n;
	mutex_lock(&s->lock);
	n = sysfs_emit(
		buf,
		"active=%u polls=%llu frames=%llu invalid=%llu drops=%llu written=%llu control=%08x rxq=%u txq=%u\n",
		s->active, s->polls, s->frames, s->invalid, s->drops,
		s->written, s->last_control, s->w - s->r, s->tw - s->tr);
	mutex_unlock(&s->lock);
	return n;
}
static DEVICE_ATTR_RO(stats);
static int probe(struct platform_device *pdev)
{
	struct sco *s;
	struct resource *r;
	int ret;
	s = devm_kzalloc(&pdev->dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!r || r->start != 0x18050000 || resource_size(r) != 0x1000)
		return -EINVAL;
	s->regs = devm_ioremap(&pdev->dev, r->start, resource_size(r));
	if (!s->regs)
		return -ENOMEM;
	r = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!r || r->start != 0x18080000 || resource_size(r) != 0x10000)
		return -EINVAL;
	s->sram = devm_ioremap(&pdev->dev, r->start, resource_size(r));
	if (!s->sram)
		return -ENOMEM;
	ret = of_property_read_u32_array(pdev->dev.of_node, "mediatek,offset",
					 s->offsets, 5);
	if (ret)
		return ret;
	if (s->offsets[2] != 0x140 || s->offsets[3] != 0x144 ||
	    s->offsets[4] != 0x148)
		return -EINVAL;
	mutex_init(&s->lock);
	init_waitqueue_head(&s->wait);
	INIT_DELAYED_WORK(&s->work, tick);
	s->misc = (struct miscdevice){ .minor = MISC_DYNAMIC_MINOR,
				       .name = "r1-sco",
				       .fops = &ops,
				       .parent = &pdev->dev,
				       .mode = 0600 };
	platform_set_drvdata(pdev, s);
	ret = misc_register(&s->misc);
	if (ret)
		return ret;
	ret = device_create_file(&pdev->dev, &dev_attr_stats);
	if (ret)
		misc_deregister(&s->misc);
	return ret;
}
static void remove_sco(struct platform_device *pdev)
{
	struct sco *s = platform_get_drvdata(pdev);
	misc_deregister(&s->misc);
	stop_sco(s);
	device_remove_file(&pdev->dev, &dev_attr_stats);
}
static const struct of_device_id ids[] = {
	{ .compatible = "mediatek,mtk-btcvsd-snd" },
	{}
};
static struct platform_driver drv = { .probe = probe,
				      .remove = remove_sco,
				      .driver = { .name = "r1-sco",
						  .of_match_table = ids,
						  .suppress_bind_attrs =
							  true } };
module_platform_driver(drv);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rabbit R1 transparent eSCO SRAM transport");
