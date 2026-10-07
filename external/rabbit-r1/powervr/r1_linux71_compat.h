/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/string.h>
static inline size_t r1_rgx_strlcpy(char *dst, const char *src, size_t size)
{
    size_t length = strlen(src);
    if (size) {
        size_t count = length < size - 1 ? length : size - 1;
        memcpy(dst, src, count);
        dst[count] = '\0';
    }
    return length;
}
#define strlcpy r1_rgx_strlcpy

#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/dma-buf.h>
#include <linux/iosys-map.h>
typedef unsigned long pfn_t;
#define pfn_to_pfn_t(pfn) (pfn)
#define phys_to_pfn_t(phys, flags) ((unsigned long)((phys) >> PAGE_SHIFT))
#define pfn_t_to_pfn(pfn) (pfn)
#define pfn_t_to_page(pfn) pfn_to_page(pfn)
#define pfn_t_valid(pfn) pfn_valid(pfn)
#define page_to_pfn_t(page) page_to_pfn(page)
static inline void *r1_rgx_dma_buf_vmap(struct dma_buf *buf)
{
    struct iosys_map map = IOSYS_MAP_INIT_VADDR(NULL);
    int ret = dma_buf_vmap_unlocked(buf, &map);
    if (ret)
        return NULL;
    if (map.is_iomem) {
        dma_buf_vunmap_unlocked(buf, &map);
        return NULL;
    }
    return map.vaddr;
}
static inline void r1_rgx_dma_buf_vunmap(struct dma_buf *buf, void *addr)
{
    struct iosys_map map = IOSYS_MAP_INIT_VADDR(addr);
    dma_buf_vunmap_unlocked(buf, &map);
}
#define dma_buf_vmap(buf) r1_rgx_dma_buf_vmap(buf)
#define dma_buf_vunmap(buf, addr) r1_rgx_dma_buf_vunmap(buf, addr)
/* Preserve the requested cache attributes with vmap, not the new
 * three-argument vm_map_ram which cannot take pgprot. */
static inline void *r1_rgx_vm_map_ram(struct page **pages, unsigned int count,
                                    int node, pgprot_t prot)
{
    return vmap(pages, count, VM_MAP, prot);
}
#define vm_map_ram(pages, count, node, prot) r1_rgx_vm_map_ram(pages, count, node, prot)
#define vm_unmap_ram(addr, count) vunmap(addr)
