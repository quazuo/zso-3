#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/ioctl.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/cdev.h>

MODULE_LICENSE("GPL");

// miscellaneous constants
#define DICEDEV_MAX_DEVICES 			256

// ioctl constants
#define DICEDEV_IOCTL_CREATE_SET 		_IO(0xDD, 0xFC)
#define DICEDEV_IOCTL_RUN 			_IO(0xDD, 0xFD)
#define DICEDEV_IOCTL_WAIT 			_IO(0xDD, 0xFE)
#define DICEDEV_IOCTL_ENABLE_SEED_INCREMENT 	_IO(0xDD, 0xFF)

struct dicedev_device {
	struct pci_dev *pdev;
	struct cdev cdev;
	int idx;
	struct device *dev;
	void __iomem *bar;
	// todo -- maybe smth more
};

static dev_t dicedev_major;
static struct dicedev_device *dicedev_devices[DICEDEV_MAX_DEVICES];
static size_t dicedev_devcount = 0;
static struct class dicedev_class = {
	.name = "dicedev",
	.owner = THIS_MODULE,
};

static const struct pci_device_id dicedev_pci_ids[] = {
	{ PCI_DEVICE(DICEDEV_VENDOR_ID, DICEDEV_DEVICE_ID) },
	{ 0 }
};

// file operations

static int dicedev_open(struct inode *inode, struct file *file) {
	printk(KERN_WARNING "Hello from open\n");
	// todo
	return 0;
}

static int dicedev_release(struct inode *inode, struct file *file) {
	// todo
	return 0;
}

static int dicedev_ioctl(struct file *file, unsigned int cmd) {
	switch (cmd) {
	case DICEDEV_IOCTL_CREATE_SET:
		break; // todo
	case DICEDEV_IOCTL_RUN:
		break; // todo
	case DICEDEV_IOCTL_WAIT:
		break; // todo
	case DICEDEV_IOCTL_ENABLE_SEED_INCREMENT:
		break; // todo
	default:
		return -ENOTTY;
	}

	return 0;
}

static struct file_operations dicedev_fops = {
	.owner = THIS_MODULE,
	.open = dicedev_open,
	.release = dicedev_release,
	.unlocked_ioctl = dicedev_ioctl,
	.compat_ioctl = dicedev_ioctl
};

// pci operations

static int dicedev_probe(struct pci_dev *pdev, const struct pci_device_id *pci_id) {
	int err, i;

	// allocate our structure
	struct dicedev_device *dev = kzalloc(sizeof *dev, GFP_KERNEL);
	if (!dev) {
		err = -ENOMEM;
		goto out_alloc;
	}
	pci_set_drvdata(pdev, dev);
	dev->pdev = pdev;

	// allocate free index
	for (i = 0; i < DICEDEV_MAX_DEVICES; i++) {
		if (!dicedev_devices[i]) break;
	}

	if (i == DICEDEV_MAX_DEVICES) {
		err = -ENOSPC;
		goto out_slot;
	}

	dicedev_devices[i] = dev;
	dev->idx = i;

	// enable hardware resources
	err = pci_enable_device(pdev);
	if (err) goto out_enable;

	err = pci_set_dma_mask(pdev, DMA_BIT_MASK(32));
	if (err) goto out_mask;
	err = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(32));
	if (err) goto out_mask;

	pci_set_master(pdev);

	err = pci_request_regions(pdev, "dicedev");
	if (err) goto out_regions;

	// map the BAR
	dev->bar = pci_iomap(pdev, 0, 0);
	if (!dev->bar) {
		err = -ENOMEM;
		goto out_bar;
	}

	// todo -- allocate buffers, do irq stuff, etc(?)

	// export the cdev
	cdev_init(&dev->cdev, &dicedev_fops);
	err = cdev_add(&dev->cdev, dicedev_major + dev->idx, 1);
	if (err) goto out_cdev;

	// register it in sysfs
	dev->dev = device_create(&dicedev_class, &dev->pdev->dev,
				 dicedev_major + dev->idx, dev,
				 "dice%d", dev->idx);

	if (IS_ERR(dev->dev)) {
		printk(KERN_ERR "dicedev: failed to register subdevice\n");
		dev->dev = NULL;
	}

	return 0;


out_cdev:
	pci_iounmap(pdev, dev->bar);
out_bar:
	pci_release_regions(pdev);
out_regions:
out_mask:
	pci_disable_device(pdev);
out_enable:
	dicedev_devices[dev->idx] = 0;
out_slot:
	kfree(dev);
out_alloc:
	return err;
}

static void dicedev_remove(struct pci_dev *pdev) {
	struct adlerdev_device *dev = pci_get_drvdata(pdev);

	if (dev->dev) {
		device_destroy(&dicedev_class, dicedev_major + dev->idx);
	}

	cdev_del(&dev->cdev);
	pci_iounmap(pdev, dev->bar);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	dicedev_devices[dev->idx] = 0;
	kfree(dev);
}

static int dicedev_suspend(struct pci_dev *dev, pm_message_t state) {
	// todo
	return 0;
}

static int dicedev_resume(struct pci_dev *dev) {
	// todo
	return 0;
}

static void dicedev_shutdown(struct pci_dev *dev) {
	// todo
}

struct pci_driver dicedev_pci_drv = {
	.name = "dicedev",
	.id_table = dicedev_pci_ids,
	.probe = dicedev_probe,
	.remove = dicedev_remove,
	.suspend = dicedev_suspend,
	.resume = dicedev_resume,
	.shutdown = dicedev_shutdown
};

// module init/exit

static int dicedev_init(void) {
	int err;

	err = alloc_chrdev_region(&dicedev_major, 0, MAX_DEV_COUNT, "dicedev");
	if (err) goto err_alloc;

	err = class_register(&dicedev_class);
	if (err) goto err_class;

	err = pci_register_driver(&dicedev_pci_drv);
	if (err) goto err_pci;

	return 0;

err_pci:
	class_unregister(&dicedev_class);
err_class:
	unregister_chrdev_region(dicedev_major, 2);
err_alloc:
	return 0;
}

static int dicedev_exit(void) {
	pci_unregister_driver(&dicedev_pci_drv);
	class_unregister(&dicedev_class);
	unregister_chrdev_region(dicedev_major, 2);
	return 0;
}

module_init(dicedev_init);
module_exit(dicedev_exit);