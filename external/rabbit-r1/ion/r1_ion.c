// SPDX-License-Identifier: GPL-2.0
/* Modern ION allocation/query ABI backed by the Linux 7.1 system-heap
 * allocator and DMA-buf operations. No physical/secure/MTK custom heaps.
 */
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/uaccess.h>
#include "r1_system_heap.c"

struct r1_ion_alloc { __u64 len; __u32 heap_mask, flags, fd, unused; };
struct r1_ion_query { __u32 count, reserved0; __u64 heaps; __u32 reserved1, reserved2; };
struct r1_ion_heap { char name[32]; __u32 type, heap_id, reserved[3]; };
#define R1_ION_ALLOC _IOWR('I', 0, struct r1_ion_alloc)
#define R1_ION_QUERY _IOWR('I', 8, struct r1_ion_query)
static long r1_ion_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *user = (void __user *)arg;
	if (cmd == R1_ION_QUERY) {
		struct r1_ion_query q;
		struct r1_ion_heap h = {.name = "system", .type = 0, .heap_id = 0};
		if (copy_from_user(&q, user, sizeof(q))) return -EFAULT;
		if (q.reserved0 || q.reserved1 || q.reserved2) return -EINVAL;
		if (q.heaps && q.count && copy_to_user(u64_to_user_ptr(q.heaps), &h, sizeof(h)))
			return -EFAULT;
		q.count = 1;
		return copy_to_user(user, &q, sizeof(q)) ? -EFAULT : 0;
	}
	if (cmd == R1_ION_ALLOC) {
		struct r1_ion_alloc a;
		struct dma_buf *buf;
		unsigned long len;
		int fd;
		if (copy_from_user(&a, user, sizeof(a))) return -EFAULT;
		/* Only ordinary cached system RAM. Never pretend a secure,
		 * contiguous or MediaTek multimedia heap is a system heap.
		 */
		if (a.heap_mask != 1 || a.flags != 1 || a.unused) return -EOPNOTSUPP;
		if (!a.len || a.len > SZ_64M) return -EINVAL;
		len = PAGE_ALIGN(a.len);
		buf = system_heap_allocate(NULL, len, O_RDWR | O_CLOEXEC, 0);
		if (IS_ERR(buf)) return PTR_ERR(buf);
		fd = get_unused_fd_flags(O_CLOEXEC);
		if (fd < 0) {dma_buf_put(buf); return fd;}
		a.fd = fd;
		if (copy_to_user(user, &a, sizeof(a))) {
			put_unused_fd(fd); dma_buf_put(buf); return -EFAULT;
		}
		fd_install(fd, buf->file);
		return 0;
	}
	/* libion's legacy detection deliberately expects ENOTTY for FREE. */
	if (cmd != _IOWR('I', 1, __u32))
		pr_info_ratelimited("r1-ion: unsupported ioctl %#x\n", cmd);
	return -ENOTTY;
}
static const struct file_operations r1_ion_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = r1_ion_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};
static struct miscdevice r1_ion_device = {
	.minor = MISC_DYNAMIC_MINOR, .name = "ion", .fops = &r1_ion_fops, .mode = 0600,
};
static int __init r1_ion_init(void) {return misc_register(&r1_ion_device);}
static void __exit r1_ion_exit(void) {misc_deregister(&r1_ion_device);}
module_init(r1_ion_init);
module_exit(r1_ion_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("R1 ION system heap ABI using real Linux DMA-buf allocation");
MODULE_IMPORT_NS("DMA_BUF");
