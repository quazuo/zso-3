#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/ioctl.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/pci.h>
#include <linux/anon_inodes.h>
#include <linux/wait.h>
#include <linux/file.h>
#include <linux/kref.h>
#include <linux/interrupt.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>

#include "dicedev.h"

MODULE_LICENSE("GPL");

struct dicedev_device {
	struct pci_dev *pdev;
	struct cdev cdev;
	int idx;
	struct device *dev;
	void __iomem *bar;
	spinlock_t slock;
	struct mutex mutex;
	struct dicedev_buf *slots[DICEDEV_BUF_SLOT_COUNT];
	uint32_t last_fence; // last fence sent to the device
	uint32_t last_completed; // last fence completed, as evidenced by CMD_FENCE_LAST
	struct dicedev_ctx *running_ctx;
};

#define DICEDEV_CTX_CMD_QUEUE_SIZE DICEDEV_MANUAL_FEED_SIZE

struct dicedev_ctx {
	struct dicedev_device *dicedev;
	bool burnt;
	enum processing_state state;
	struct cmd_queue {
		uint32_t cmd_no[DICEDEV_CTX_CMD_QUEUE_SIZE];
		size_t begin, end;
	} queue;
};

struct dma_buf {
	void *buf;
	dma_addr_t dma_handle;
};

struct dicedev_buf {
	size_t size;
	struct p_table {
		struct dma_buf table;
		struct dma_buf pages[DICEDEV_PTABLE_ENTRY_COUNT];
		size_t p_count;
	} p_table;
	uint64_t allowed;
	uint32_t seed;
	uint32_t slot;
	bool bound;
	struct dicedev_ctx *owner;
	loff_t read_off; // for io_uring purposes
};

static dev_t dicedev_major;
static struct dicedev_device *dicedev_devices[DICEDEV_MAX_DEVICES];
static struct class dicedev_class = {
	.name = "dicedev",
	.owner = THIS_MODULE,
};

static const struct pci_device_id dicedev_pci_ids[] = {
	{ PCI_DEVICE(DICEDEV_VENDOR_ID, DICEDEV_DEVICE_ID) },
	{ 0 }
};

/// cmd utils

static bool dicedev_is_cmd(uint32_t cmd, int cmd_type)
{
	return (cmd & DICEDEV_CMD_TYPE_MASK) == cmd_type;
}

static void dicedev_get_die_vars(uint32_t cmd, uint32_t *num, uint32_t *out_type,
				 uint32_t *slot)
{
	uint32_t num_mask = 0xFFFF << 4;
	uint32_t out_type_mask = 0xF << 20;
	uint32_t slot_mask = 0xF << 24;

	if (num)
		*num = (cmd & num_mask) >> 4;
	if (out_type)
		*out_type = (cmd & out_type_mask) >> 20;
	if (slot)
		*slot = (cmd & slot_mask) >> 24;
}

static uint32_t dicedev_update_cmd(uint32_t cmd, uint32_t buf_slot)
{
	if (dicedev_is_cmd(cmd, DICEDEV_USER_CMD_TYPE_GET_DIE)) {
		uint32_t num, out_type;
		dicedev_get_die_vars(cmd, &num, &out_type, NULL);
		return DICEDEV_USER_CMD_GET_DIE_HEADER_WSLOT(num, out_type, buf_slot);
	}

	if (dicedev_is_cmd(cmd, DICEDEV_USER_CMD_NEW_SET)) {
		return DICEDEV_USER_CMD_NEW_SET_WSLOT(buf_slot);
	}

	return cmd;
}

/// misc stuff

static uint32_t dicedev_ior(struct dicedev_device *dicedev, uint32_t reg)
{
	return ioread32(dicedev->bar + reg);
}

static void dicedev_iow(struct dicedev_device *dicedev, uint32_t reg,
			uint32_t val)
{
	iowrite32(val, dicedev->bar + reg);
}

static void dicedev_iocmd(struct dicedev_ctx *ctx, uint32_t cmd)
{
	uint32_t queue_free;

	do {
		queue_free = dicedev_ior(ctx->dicedev, CMD_MANUAL_FREE);
	} while (queue_free == 0);

	// update state
	if (ctx->state == NONE) {
		if (dicedev_is_cmd(cmd, DICEDEV_USER_CMD_TYPE_BIND_SLOT))
			ctx->state = BIND_SLOT_0;
		else if (dicedev_is_cmd(cmd, DICEDEV_USER_CMD_TYPE_GET_DIE))
			ctx->state = GET_DIE_0;

	} else if (ctx->state == BIND_SLOT_0) {
		ctx->state = BIND_SLOT_1;

	} else if (ctx->state == BIND_SLOT_1) {
		ctx->state = BIND_SLOT_2;

	} else {
		ctx->state = NONE;
	}

	dicedev_iow(ctx->dicedev, CMD_MANUAL_FEED, cmd);
}

static void dicedev_user_iocmd(struct dicedev_ctx *ctx, struct dicedev_buf *buf,
			       uint32_t cmd)
{
	uint32_t queue_free;
	uint32_t fence_no;
	uint32_t fence_cmd;

	if (ctx->state == GET_DIE_0) {
		if ((buf->allowed & cmd) != cmd)
			ctx->burnt = true;
	}

	dicedev_iocmd(ctx, cmd);

	if (ctx->state != NONE)
		return;

	do {
		queue_free = dicedev_ior(ctx->dicedev, CMD_MANUAL_FREE);
	} while (queue_free == 0);

	ctx->dicedev->last_fence++;
	ctx->dicedev->last_fence %= 1 << 28;

	fence_no = ctx->dicedev->last_fence;
	fence_cmd = DICEDEV_USER_CMD_FENCE_HEADER(fence_no);

	dicedev_iow(ctx->dicedev, CMD_MANUAL_FEED, fence_cmd);

	ctx->queue.cmd_no[ctx->queue.end] = fence_no;

	ctx->queue.end++;
	ctx->queue.end %= DICEDEV_CTX_CMD_QUEUE_SIZE;

	if (ctx->queue.begin == ctx->queue.end) {
		ctx->queue.begin++;
		ctx->queue.begin %= DICEDEV_CTX_CMD_QUEUE_SIZE;
	}
}

static int dicedev_enable(struct pci_dev *pdev)
{
	uint8_t enabled_intrs;
	struct dicedev_device *dicedev = pci_get_drvdata(pdev);

	if (!dicedev)
		return -ENOENT;

	dicedev_iow(dicedev, DICEDEV_INTR, 0);

	enabled_intrs = DICEDEV_INTR_FENCE_WAIT | DICEDEV_INTR_FEED_ERROR |
			DICEDEV_INTR_CMD_ERROR | DICEDEV_INTR_MEM_ERROR |
			DICEDEV_INTR_SLOT_ERROR;
	dicedev_iow(dicedev, DICEDEV_INTR_ENABLE, enabled_intrs);

	dicedev_iow(dicedev, DICEDEV_ENABLE, 1);
	dicedev_iow(dicedev, DICEDEV_CMD_FENCE_LAST, 0);
	dicedev_iow(dicedev, DICEDEV_CMD_FENCE_WAIT, 0);

	return 0;
}

static int dicedev_disable(struct pci_dev *pdev)
{
	struct dicedev_device *dicedev = pci_get_drvdata(pdev);
	if (!dicedev)
		return -ENOENT;

	dicedev_iow(dicedev, DICEDEV_ENABLE, 0);
	dicedev_iow(dicedev, DICEDEV_INTR_ENABLE, 0);

	return 0;
}

static void dicedev_burn_ctx(struct dicedev_device *dicedev, uint32_t cmd_no)
{
	// find the ctx that submitted the cmd with cmd_no fence id

	for (size_t i = 0; i < DICEDEV_BUF_SLOT_COUNT; i++) {
		struct dicedev_ctx *owner;
		size_t ndx;

		if (!dicedev->slots[i])
			continue;

		owner = dicedev->slots[i]->owner;
		ndx = owner->queue.begin;

		while (ndx != owner->queue.end) {
			if (owner->queue.cmd_no[ndx] == cmd_no) {
				owner->burnt = true;
				return;
			}

			ndx++;
			ndx %= DICEDEV_CTX_CMD_QUEUE_SIZE;
		}

		if (owner->queue.cmd_no[ndx] == cmd_no) {
			owner->burnt = true;
			return;
		}
	}
}

static void dicedev_update_fence(struct dicedev_device *dicedev)
{
	dicedev->last_completed = dicedev_ior(dicedev, DICEDEV_CMD_FENCE_LAST);
}

static struct dma_buf dicedev_dma_alloc(struct device *dev, size_t size)
{
	struct dma_buf buf;
	buf.buf = dma_alloc_coherent(dev, size, &buf.dma_handle, GFP_KERNEL);
	return buf;
}

static void dicedev_free_ptable(struct dicedev_ctx *ctx, struct dicedev_buf *buf)
{
	struct device *dev = &ctx->dicedev->pdev->dev;
	struct p_table *p_table = &buf->p_table;

	dma_free_coherent(dev, DICEDEV_PAGE_SIZE, p_table->table.buf,
			  p_table->table.dma_handle);

	for (int i = 0; i < DICEDEV_PTABLE_ENTRY_COUNT; i++) {
		struct dma_buf *page = &p_table->pages[i];
		if (page->buf) {
			dma_free_coherent(dev, DICEDEV_PAGE_SIZE, page->buf,
					  page->dma_handle);
		}
	}
}

static int dicedev_alloc_ptable(struct dicedev_ctx *ctx, struct dicedev_buf *buf)
{
	struct device *dev = &ctx->dicedev->pdev->dev;
	size_t page_count = buf->size / DICEDEV_PAGE_SIZE +
			    (buf->size % DICEDEV_PAGE_SIZE ? 1 : 0);
	struct p_table *p_table = &buf->p_table;
	size_t i;

	p_table->table = dicedev_dma_alloc(dev, DICEDEV_PAGE_SIZE);
	if (!p_table->table.buf)
		return -ENOMEM;

	for (i = 0; i < page_count; i++) {
		struct dma_buf *page = &p_table->pages[i];
		uint32_t *entry;

		*page = dicedev_dma_alloc(dev, DICEDEV_PAGE_SIZE);
		if (!page->buf)
			goto err_page_alloc;

		memset(page->buf, 0, DICEDEV_PAGE_SIZE);

		entry = p_table->table.buf + (i * DICEDEV_PTABLE_ENTRY_SIZE);
		*entry = DICEDEV_PTABLE_MAKE_ENTRY(1, page->dma_handle);
	}

	return 0;

err_page_alloc:
	dicedev_free_ptable(ctx, buf);
	return -ENOMEM;
}

/// irq handler

static irqreturn_t dicedev_isr(int irq, void *opaque)
{
	struct dicedev_device *dicedev = opaque;
	uint32_t intr, val;
	unsigned long slock_flags;

	if (dicedev->pdev->irq != irq)
		return IRQ_NONE;

	spin_lock_irqsave(&dicedev->slock, slock_flags);

	intr = dicedev_ior(dicedev, DICEDEV_INTR);

	if (intr & DICEDEV_INTR_FENCE_WAIT)
		dicedev_update_fence(dicedev);

	if (intr & DICEDEV_INTR_CMD_ERROR || intr & DICEDEV_INTR_MEM_ERROR) {
		if (dicedev->running_ctx) {
			dicedev->running_ctx->burnt = true;
		} else {
			uint32_t last_completed =
				dicedev_ior(dicedev, DICEDEV_CMD_FENCE_LAST);
			dicedev_burn_ctx(dicedev, last_completed);
		}
	}

	val = DICEDEV_INTR_FENCE_WAIT | DICEDEV_INTR_FEED_ERROR |
	      DICEDEV_INTR_CMD_ERROR | DICEDEV_INTR_MEM_ERROR |
	      DICEDEV_INTR_SLOT_ERROR;

	dicedev_iow(dicedev, DICEDEV_INTR, val);

	spin_unlock_irqrestore(&dicedev->slock, slock_flags);

	return IRQ_HANDLED;
}

/// buffer file operations

static int dicedev_bind_slot(struct dicedev_ctx *ctx, struct dicedev_buf *buf);
static void dicedev_unbind_slot(struct dicedev_device *dicedev, uint32_t slot);

static ssize_t dicedev_buf_read(struct file *file, char __user *buf,
				size_t size, loff_t *off)
{
	struct dicedev_buf *dicedev_buf;
	loff_t read_bytes = 0;
	char *temp_buf;
	int err;

	if (size % sizeof(struct dice))
		return -EINVAL;

	dicedev_buf = file->private_data;
	if (!dicedev_buf)
		return -EINVAL;

	temp_buf = kmalloc(size, GFP_KERNEL);
	if (!temp_buf)
		return -ENOMEM;

	mutex_lock(&dicedev_buf->owner->dicedev->mutex);

	while (read_bytes < size) {
		size_t bytes_left = size - read_bytes;
		pgoff_t page_ndx = dicedev_buf->read_off / DICEDEV_PAGE_SIZE;
		loff_t page_off = dicedev_buf->read_off % DICEDEV_PAGE_SIZE;
		void *page = dicedev_buf->p_table.pages[page_ndx].buf;

		size_t curr_read_n = DICEDEV_PAGE_SIZE - page_off;
		if (bytes_left < curr_read_n)
			curr_read_n = bytes_left;

		memcpy(temp_buf + read_bytes, page + page_off, curr_read_n);

		read_bytes += curr_read_n;
		dicedev_buf->read_off += curr_read_n;
	}

	mutex_unlock(&dicedev_buf->owner->dicedev->mutex);

	err = copy_to_user(buf, temp_buf, size);
	if (err) {
		kfree(temp_buf);
		return -EFAULT;
	}

	if (off)
		*off += size;

	kfree(temp_buf);
	return size;
}

static ssize_t dicedev_buf_write(struct file *file, const char __user *buf,
				 size_t size, loff_t *off)
{
	int ret;
	struct dicedev_buf *dicedev_buf;
	struct dicedev_ctx *ctx;
	uint32_t *cmd_buf;
	int buf_slot;

	if (size % 4)
		return -EINVAL;

	dicedev_buf = file->private_data;
	if (!buf)
		return -EINVAL;

	ctx = dicedev_buf->owner;

	if (ctx->burnt)
		return -EIO;

	cmd_buf = kmalloc(size, GFP_KERNEL);
	if (!cmd_buf)
		return -ENOMEM;

	ret = copy_from_user(cmd_buf, buf, size);
	if (ret)
		goto copy_err;

	mutex_lock(&ctx->dicedev->mutex);

	if (dicedev_buf->bound) {
		buf_slot = dicedev_buf->slot;
	} else {
		buf_slot = dicedev_bind_slot(ctx, dicedev_buf);
		if (buf_slot == -1) {
			mutex_unlock(&ctx->dicedev->mutex);
			return -EINVAL;
		}
	}

	ctx->dicedev->running_ctx = ctx;

	for (size_t i = 0; i < size / 4; i++) {
		uint32_t cmd = dicedev_update_cmd(cmd_buf[i], buf_slot);
		dicedev_user_iocmd(ctx, dicedev_buf, cmd);

		if (ctx->burnt)
			break;
	}

	ctx->dicedev->running_ctx = NULL;
	mutex_unlock(&ctx->dicedev->mutex);

	if(off) *off += size;
	ret = size;

copy_err:
	kfree(cmd_buf);
	return ret;
}

static long dicedev_buf_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	struct dicedev_buf *buf = file->private_data;
	struct dicedev_ioctl_seed _arg;
	int err;

	if (!buf)
		return -EINVAL;

	if (cmd != DICEDEV_BUFFER_IOCTL_SEED)
		return -ENOTTY;

	err = copy_from_user(&_arg, (void *)arg, sizeof(_arg));
	if (err)
		return -EFAULT;

	buf->seed = _arg.seed;

	return 0;
}

static vm_fault_t dicedev_buf_mmap_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct file *file = vma->vm_file;
	struct dicedev_buf *buf = file->private_data;
	struct page *page;

	if (vmf->pgoff * DICEDEV_PAGE_SIZE >= buf->size)
		return VM_FAULT_SIGBUS;

	page = virt_to_page(buf->p_table.pages[vmf->pgoff].buf);
	get_page(page);
	vmf->page = page;
	return 0;
}

struct vm_operations_struct dicedev_buf_vmops = {
	.fault = dicedev_buf_mmap_fault
};

static int dicedev_buf_mmap(struct file *file, struct vm_area_struct *vma)
{
	vma->vm_ops = &dicedev_buf_vmops;
	return 0;
}

static int dicedev_buf_release(struct inode *inode, struct file *file)
{
	struct dicedev_buf *buf = file->private_data;

	if (buf) {
		if (buf->slot)
			dicedev_unbind_slot(buf->owner->dicedev, buf->slot);

		dicedev_free_ptable(buf->owner, buf);
		kfree(buf);
	}

	return 0;
}

struct file_operations dicedev_buf_fops = {
	.owner = THIS_MODULE,
	.read = dicedev_buf_read,
	.write = dicedev_buf_write,
	.unlocked_ioctl = dicedev_buf_ioctl,
	.compat_ioctl = dicedev_buf_ioctl,
	.mmap = dicedev_buf_mmap,
	.release = dicedev_buf_release
};

/// file operations

static int dicedev_open(struct inode *inode, struct file *file)
{
	struct dicedev_device *dicedev =
		container_of(inode->i_cdev, struct dicedev_device, cdev);

	struct dicedev_ctx *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dicedev = dicedev;
	ctx->burnt = false;
	file->private_data = ctx;
	return 0;
}

static int dicedev_release(struct inode *inode, struct file *file)
{
	struct dicedev_ctx *ctx = file->private_data;

	if (!file->private_data)
		return -EINVAL;

	for (int i = 0; i < DICEDEV_BUF_SLOT_COUNT; i++) {
		struct dicedev_buf *buf = ctx->dicedev->slots[i];

		if (buf && buf->owner == ctx) {
			dicedev_unbind_slot(ctx->dicedev, i);
		}
	}

	kfree(ctx);
	return 0;
}

static long dicedev_ioctl_crtset(struct dicedev_ctx *ctx, unsigned long arg)
{
	int err, fd;
	struct dicedev_ioctl_create_set _arg;
	struct dicedev_buf *buf;

	err = copy_from_user(&_arg, (void *)arg, sizeof(_arg));
	if (err)
		return -EFAULT;

	buf = kzalloc(sizeof(struct dicedev_buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf->allowed = _arg.allowed;
	buf->size = _arg.size;
	buf->seed = DICEDEV_BUF_INIT_SEED;
	buf->owner = ctx;

	if (buf->size > DICEDEV_BUF_MAX_SIZE) {
		err = -EINVAL;
		goto err_bufsize;
	}

	// allocate the buffer and its page table
	err = dicedev_alloc_ptable(ctx, buf);
	if (err)
		goto err_ptable;

	// make it a file and get the file descriptor
	fd = anon_inode_getfd("dicedev", &dicedev_buf_fops, buf, O_RDWR);
	if (fd < 0) {
		err = fd;
		goto err_file;
	}

	return fd;

err_file:
	dicedev_free_ptable(ctx, buf);
err_ptable:
err_bufsize:
	kfree(buf);
	return err;
}

static void dicedev_unbind_slot(struct dicedev_device *dicedev, uint32_t slot)
{
	struct dicedev_buf *buf = dicedev->slots[slot];
	uint32_t cmd = DICEDEV_USER_CMD_UNBIND_SLOT_HEADER(slot);
	dicedev_iow(dicedev, CMD_MANUAL_FEED, cmd);

	if (buf)
		buf->bound = false;

	dicedev->slots[slot] = NULL;
}

static int dicedev_bind_slot(struct dicedev_ctx *ctx, struct dicedev_buf *buf)
{
	uint32_t i, cmd;
	uint64_t pa;
	struct dicedev_device *dicedev = ctx->dicedev;

	if (buf->bound) {
		printk(KERN_WARNING "buf already bound\n");
		return -1;
	}

	for (i = 0; i < DICEDEV_BUF_SLOT_COUNT; i++) {
		if (!dicedev->slots[i])
			break;
	}

	if (i == DICEDEV_BUF_SLOT_COUNT) {
		for (i = 0; i < DICEDEV_BUF_SLOT_COUNT; i++) {
			if (dicedev->slots[i] != buf)
				break;
		}

		dicedev_unbind_slot(dicedev, i);
	}

	cmd = DICEDEV_USER_CMD_BIND_SLOT_HEADER(i, buf->seed);
	dicedev_iocmd(ctx, cmd);

	cmd = buf->allowed;
	dicedev_iocmd(ctx, cmd);

	pa = buf->p_table.table.dma_handle;

	cmd = pa;
	dicedev_iocmd(ctx, cmd);

	cmd = pa >> 32;
	dicedev_iocmd(ctx, cmd);

	buf->slot = i;
	buf->bound = true;
	dicedev->slots[i] = buf;

	return buf->slot;
}

static long dicedev_ioctl_run(struct dicedev_ctx *ctx, unsigned long arg)
{
	int err;
	struct dicedev_ioctl_run _arg;
	struct fd f;
	struct dicedev_buf *in_buf, *out_buf;
	uint32_t out_buf_slot;

	if (ctx->burnt)
		return -EIO;

	err = copy_from_user(&_arg, (void *)arg, sizeof(_arg));
	if (err)
		return -EFAULT;

	if (_arg.addr % 4 || _arg.size % 4)
		return -EINVAL;

	f = fdget(_arg.cfd);
	if (!f.file || !f.file->private_data || f.file->f_op != &dicedev_buf_fops)
		return -EINVAL;

	in_buf = f.file->private_data;

	f = fdget(_arg.bfd);
	if (!f.file || !f.file->private_data || f.file->f_op != &dicedev_buf_fops)
		return -EINVAL;

	out_buf = f.file->private_data;

	if (in_buf->owner != ctx || out_buf->owner != ctx)
		return -EINVAL;

	mutex_lock(&ctx->dicedev->mutex);

	if (out_buf->bound) {
		out_buf_slot = out_buf->slot;
	} else {
		out_buf_slot = dicedev_bind_slot(ctx, out_buf);
		if (out_buf_slot == -1) {
			mutex_unlock(&ctx->dicedev->mutex);
			return -EINVAL;
		}
	}

	ctx->dicedev->running_ctx = ctx;

	for (size_t off = 0; off < _arg.size; off += sizeof(uint32_t)) {
		pgoff_t page_ndx = (_arg.addr + off) / DICEDEV_PAGE_SIZE;
		loff_t page_off = (_arg.addr + off) % DICEDEV_PAGE_SIZE;
		uint32_t *cmd = in_buf->p_table.pages[page_ndx].buf + page_off;

		if (dicedev_is_cmd(*cmd, DICEDEV_USER_CMD_TYPE_GET_DIE)) {
			uint32_t num_mask = 0xFFFF << 4;
			uint32_t num = (*cmd & num_mask) >> 4;

			uint32_t out_type_mask = 0xF << 20;
			uint32_t out_type = (*cmd & out_type_mask) >> 20;

			*cmd = DICEDEV_USER_CMD_GET_DIE_HEADER_WSLOT(num, out_type, out_buf_slot);
		}

		dicedev_user_iocmd(ctx, in_buf, *cmd);

		if (ctx->burnt)
			break;
	}

	ctx->dicedev->running_ctx = NULL;
	mutex_unlock(&ctx->dicedev->mutex);

	return 0;
}

static long dicedev_ioctl_wait(struct dicedev_ctx *ctx, unsigned long arg)
{
	int err;
	struct dicedev_ioctl_wait _arg;
	uint32_t awaited_cmd_ndx, awaited_cmd_no;

	err = copy_from_user(&_arg, (void *)arg, sizeof(_arg));
	if (err)
		return -EFAULT;

	if (_arg.cnt > DICEDEV_CTX_CMD_QUEUE_SIZE) {
		// if we're trying to wait for more commands than the queue
		// can hold, then there's definitely nothing to wait for.
		return 0;
	}

	awaited_cmd_ndx = ctx->queue.end + DICEDEV_CTX_CMD_QUEUE_SIZE - _arg.cnt;
	awaited_cmd_ndx %= DICEDEV_CTX_CMD_QUEUE_SIZE;
	awaited_cmd_no = ctx->queue.cmd_no[awaited_cmd_ndx];

	// now we actually have to wait.
	while (dicedev_ior(ctx->dicedev, DICEDEV_CMD_FENCE_LAST) < awaited_cmd_no) { }

	if (ctx->burnt)
		return -EIO;

	return 0;
}

static long dicedev_ioctl_seedincr(struct dicedev_ctx *ctx, unsigned long arg)
{
	struct dicedev_device *dicedev = ctx->dicedev;
	uint32_t curr_incr_seed;

	if (!dicedev)
		return -ENOENT;

	curr_incr_seed = dicedev_ior(dicedev, DICEDEV_INCREMENT_SEED);
	dicedev_iow(dicedev, DICEDEV_INCREMENT_SEED, 1 - curr_incr_seed);

	return 0;
}

static long dicedev_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	int err = 0;
	struct dicedev_ctx *ctx = file->private_data;
	if (!ctx)
		return -EINVAL;

	dicedev_update_fence(ctx->dicedev);

	switch (cmd) {
	case DICEDEV_IOCTL_CREATE_SET:
		err = dicedev_ioctl_crtset(ctx, arg);
		break;
	case DICEDEV_IOCTL_RUN:
		err = dicedev_ioctl_run(ctx, arg);
		break;
	case DICEDEV_IOCTL_WAIT:
		err = dicedev_ioctl_wait(ctx, arg);
		break;
	case DICEDEV_IOCTL_ENABLE_SEED_INCREMENT:
		err = dicedev_ioctl_seedincr(ctx, arg);
		break;
	default:
		return -ENOTTY;
	}

	return err;
}

static struct file_operations dicedev_fops = {
	.owner = THIS_MODULE,
	.open = dicedev_open,
	.release = dicedev_release,
	.unlocked_ioctl = dicedev_ioctl,
	.compat_ioctl = dicedev_ioctl
};

/// pci operations

static int dicedev_probe(struct pci_dev *pdev,
			 const struct pci_device_id *pci_id)
{
	int err, i;

	// allocate our structure
	struct dicedev_device *dev = kzalloc(sizeof *dev, GFP_KERNEL);
	if (!dev) {
		err = -ENOMEM;
		goto out_alloc;
	}
	pci_set_drvdata(pdev, dev);
	dev->pdev = pdev;

	// init locks
	spin_lock_init(&dev->slock);
	mutex_init(&dev->mutex);

	// allocate free index
	for (i = 0; i < DICEDEV_MAX_DEVICES; i++) {
		if (!dicedev_devices[i])
			break;
	}

	if (i == DICEDEV_MAX_DEVICES) {
		err = -ENOSPC;
		goto out_slot;
	}

	dicedev_devices[i] = dev;
	dev->idx = i;

	// enable hardware resources
	err = pci_enable_device(pdev);
	if (err)
		goto out_enable;

	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(40));
	if (err)
		goto out_mask;

	pci_set_master(pdev);

	err = pci_request_regions(pdev, "dicedev");
	if (err)
		goto out_regions;

	// map the BAR
	dev->bar = pci_iomap(pdev, 0, 0);
	if (!dev->bar) {
		err = -ENOMEM;
		goto out_bar;
	}

	// connect the IRQ line
	err = request_irq(pdev->irq, dicedev_isr, IRQF_SHARED, "dicedev", dev);
	if (err)
		goto out_irq;

	// export the cdev
	cdev_init(&dev->cdev, &dicedev_fops);
	err = cdev_add(&dev->cdev, dicedev_major + dev->idx, 1);
	if (err)
		goto out_cdev;

	// register it in sysfs
	dev->dev = device_create(&dicedev_class, &pdev->dev,
				 dicedev_major + dev->idx, dev, "dice%d",
				 dev->idx);
	if (IS_ERR(dev->dev)) {
		printk(KERN_ERR "dicedev: failed to register subdevice\n");
		dev->dev = NULL;
	}

	// enable the device
	dicedev_enable(pdev);

	for (uint32_t i = 0; i < DICEDEV_BUF_SLOT_COUNT; i++)
		dicedev_unbind_slot(dev, i);

	return 0;

out_cdev:
	free_irq(pdev->irq, dev);
out_irq:
	pci_iounmap(pdev, dev->bar);
out_bar:
	pci_release_regions(pdev);
out_regions:
out_mask:
	pci_disable_device(pdev);
out_enable:
	dicedev_devices[dev->idx] = NULL;
out_slot:
	kfree(dev);
out_alloc:
	return err;
}

static void dicedev_remove(struct pci_dev *pdev)
{
	struct dicedev_device *dev = pci_get_drvdata(pdev);

	dicedev_disable(pdev);

	if (dev->dev) {
		device_destroy(&dicedev_class, dicedev_major + dev->idx);
	}

	cdev_del(&dev->cdev);
	free_irq(pdev->irq, dev);
	pci_iounmap(pdev, dev->bar);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	dicedev_devices[dev->idx] = 0;
	kfree(dev);
}

static int dicedev_suspend(struct pci_dev *pdev, pm_message_t state)
{
	return 0;
}

static int dicedev_resume(struct pci_dev *pdev)
{
	return 0;
}

static void dicedev_shutdown(struct pci_dev *pdev)
{
	dicedev_remove(pdev);
}

struct pci_driver dicedev_pci_drv = { .name = "dicedev",
				      .id_table = dicedev_pci_ids,
				      .probe = dicedev_probe,
				      .remove = dicedev_remove,
				      .suspend = dicedev_suspend,
				      .resume = dicedev_resume,
				      .shutdown = dicedev_shutdown };

/// module init/exit

static int dicedev_init(void)
{
	int err;

	err = alloc_chrdev_region(&dicedev_major, 0, DICEDEV_MAX_DEVICES,
				  "dicedev");
	if (err)
		goto err_alloc;

	err = class_register(&dicedev_class);
	if (err)
		goto err_class;

	err = pci_register_driver(&dicedev_pci_drv);
	if (err)
		goto err_pci;

	return 0;

err_pci:
	class_unregister(&dicedev_class);
err_class:
	unregister_chrdev_region(dicedev_major, DICEDEV_MAX_DEVICES);
err_alloc:
	return 0;
}

static void dicedev_exit(void)
{
	pci_unregister_driver(&dicedev_pci_drv);
	class_unregister(&dicedev_class);
	unregister_chrdev_region(dicedev_major, DICEDEV_MAX_DEVICES);
}

module_init(dicedev_init);
module_exit(dicedev_exit);