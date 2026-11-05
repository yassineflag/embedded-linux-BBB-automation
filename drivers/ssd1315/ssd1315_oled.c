// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yassine El H. @yassineflag
 *
 * SSD1315 128x64 monochrome OLED, I2C driver exposing /dev/ssd1315.
 *
 * Target: BeagleBone Black, I2C2 (P9.19 SCL, P9.20 SDA), address 0x3c.
 *
 * Differences from the SSD1306 init sequence:
 *  - Charge pump 0x8d, 0x95: 9.0 V (SSD1306 uses 0x14, 7.5 V)
 *  - Clock divide / oscillator 0xd5, 0xf0: max oscillator, divide by 1
 *    (SSD1306 commonly 0x80)
 *
 * Userspace ABI: see ssd1315_ioctl.h.
 */

#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "ssd1315_ioctl.h"

#define SSD1315_PAGES			(SSD1315_HEIGHT / 8)

/* I2C control byte: Co = 0, D/C# selects command or GDDRAM data stream */
#define SSD1315_CTRL_CMD		0x00
#define SSD1315_CTRL_DATA		0x40

#define SSD1315_CMD_SET_MEM_ADDR	0x20
#define SSD1315_MEM_ADDR_HORIZ		0x00
#define SSD1315_CMD_SET_COL_ADDR	0x21
#define SSD1315_CMD_SET_PAGE_ADDR	0x22
#define SSD1315_CMD_SET_START_LINE	0x40
#define SSD1315_CMD_SET_CONTRAST	0x81
#define SSD1315_CMD_CHARGE_PUMP		0x8d
#define SSD1315_CHARGE_PUMP_9V		0x95
#define SSD1315_CMD_SEG_REMAP		0xa1
#define SSD1315_CMD_DISPLAY_RAM		0xa4
#define SSD1315_CMD_NORMAL_DISPLAY	0xa6
#define SSD1315_CMD_SET_MUX_RATIO	0xa8
#define SSD1315_CMD_DISPLAY_OFF		0xae
#define SSD1315_CMD_DISPLAY_ON		0xaf
#define SSD1315_CMD_COM_SCAN_DEC	0xc8
#define SSD1315_CMD_SET_DISP_OFFSET	0xd3
#define SSD1315_CMD_SET_DISP_CLOCK	0xd5
#define SSD1315_DISP_CLOCK_MAX		0xf0
#define SSD1315_CMD_SET_PRECHARGE	0xd9
#define SSD1315_CMD_SET_COM_PINS	0xda
#define SSD1315_CMD_SET_VCOMH		0xdb

struct ssd1315_dev {
	struct kref		ref;
	struct mutex		lock;		/* protects client and txbuf */
	struct i2c_client	*client;	/* NULL once the device is unbound */
	struct miscdevice	miscdev;
	/* control byte followed by one full frame, sent in a single transfer */
	u8			txbuf[1 + SSD1315_FB_SIZE];
};

static int ssd1315_send(struct i2c_client *client, const u8 *buf, int len)
{
	int ret = i2c_master_send(client, buf, len);

	if (ret < 0)
		return ret;
	return ret == len ? 0 : -EIO;
}

static int ssd1315_cmd(struct i2c_client *client, u8 cmd)
{
	const u8 buf[] = { SSD1315_CTRL_CMD, cmd };

	return ssd1315_send(client, buf, sizeof(buf));
}

/* Caller holds ssd->lock, frame data is already in txbuf[1..] */
static int ssd1315_flush(struct ssd1315_dev *ssd)
{
	static const u8 window[] = {
		SSD1315_CTRL_CMD,
		SSD1315_CMD_SET_COL_ADDR, 0, SSD1315_WIDTH - 1,
		SSD1315_CMD_SET_PAGE_ADDR, 0, SSD1315_PAGES - 1,
	};
	int ret;

	ret = ssd1315_send(ssd->client, window, sizeof(window));
	if (ret)
		return ret;
	return ssd1315_send(ssd->client, ssd->txbuf, sizeof(ssd->txbuf));
}

static int ssd1315_init_display(struct ssd1315_dev *ssd)
{
	/* One command stream: a single control byte, then all commands */
	static const u8 init_seq[] = {
		SSD1315_CTRL_CMD,
		SSD1315_CMD_DISPLAY_OFF,
		SSD1315_CMD_SET_DISP_CLOCK, SSD1315_DISP_CLOCK_MAX,
		SSD1315_CMD_SET_MUX_RATIO, SSD1315_HEIGHT - 1,
		SSD1315_CMD_SET_DISP_OFFSET, 0x00,
		SSD1315_CMD_SET_START_LINE | 0x00,
		SSD1315_CMD_CHARGE_PUMP, SSD1315_CHARGE_PUMP_9V,
		SSD1315_CMD_SET_MEM_ADDR, SSD1315_MEM_ADDR_HORIZ,
		SSD1315_CMD_SEG_REMAP,
		SSD1315_CMD_COM_SCAN_DEC,
		SSD1315_CMD_SET_COM_PINS, 0x12,
		SSD1315_CMD_SET_CONTRAST, 0xcf,
		SSD1315_CMD_SET_PRECHARGE, 0xf1,
		SSD1315_CMD_SET_VCOMH, 0x20,
		SSD1315_CMD_DISPLAY_RAM,
		SSD1315_CMD_NORMAL_DISPLAY,
	};
	int ret;

	ret = ssd1315_send(ssd->client, init_seq, sizeof(init_seq));
	if (ret)
		return ret;

	/* Clear GDDRAM before the panel is switched on: no garbage flash */
	memset(ssd->txbuf + 1, 0, SSD1315_FB_SIZE);
	ret = ssd1315_flush(ssd);
	if (ret)
		return ret;

	return ssd1315_cmd(ssd->client, SSD1315_CMD_DISPLAY_ON);
}

static void ssd1315_free(struct kref *ref)
{
	kfree(container_of(ref, struct ssd1315_dev, ref));
}

/*
 * misc_open() calls this with misc_mtx held and file->private_data set to
 * our miscdevice. misc_deregister() takes the same mutex, so the reference
 * taken here cannot race with ssd1315_remove().
 */
static int ssd1315_open(struct inode *inode, struct file *file)
{
	struct ssd1315_dev *ssd = container_of(file->private_data,
					       struct ssd1315_dev, miscdev);

	kref_get(&ssd->ref);
	file->private_data = ssd;
	return nonseekable_open(inode, file);
}

static int ssd1315_release(struct inode *inode, struct file *file)
{
	struct ssd1315_dev *ssd = file->private_data;

	kref_put(&ssd->ref, ssd1315_free);
	return 0;
}

static ssize_t ssd1315_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct ssd1315_dev *ssd = file->private_data;
	int ret;

	if (count != SSD1315_FB_SIZE)
		return -EINVAL;

	mutex_lock(&ssd->lock);
	if (!ssd->client)
		ret = -ENODEV;
	else if (copy_from_user(ssd->txbuf + 1, buf, SSD1315_FB_SIZE))
		ret = -EFAULT;
	else
		ret = ssd1315_flush(ssd);
	mutex_unlock(&ssd->lock);

	return ret ? ret : count;
}

static long ssd1315_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	struct ssd1315_dev *ssd = file->private_data;
	long ret;

	mutex_lock(&ssd->lock);
	if (!ssd->client) {
		ret = -ENODEV;
		goto out;
	}

	switch (cmd) {
	case SSD1315_IOCTL_CLEAR:
		memset(ssd->txbuf + 1, 0, SSD1315_FB_SIZE);
		ret = ssd1315_flush(ssd);
		break;
	case SSD1315_IOCTL_ON:
		ret = ssd1315_cmd(ssd->client, SSD1315_CMD_DISPLAY_ON);
		break;
	case SSD1315_IOCTL_OFF:
		ret = ssd1315_cmd(ssd->client, SSD1315_CMD_DISPLAY_OFF);
		break;
	default:
		ret = -ENOTTY;
	}
out:
	mutex_unlock(&ssd->lock);
	return ret;
}

static const struct file_operations ssd1315_fops = {
	.owner		= THIS_MODULE,
	.open		= ssd1315_open,
	.release	= ssd1315_release,
	.write		= ssd1315_write,
	.unlocked_ioctl	= ssd1315_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static int ssd1315_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ssd1315_dev *ssd;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENODEV;

	/* Not devm: the object must outlive unbind while files are open */
	ssd = kzalloc(sizeof(*ssd), GFP_KERNEL);
	if (!ssd)
		return -ENOMEM;

	kref_init(&ssd->ref);
	mutex_init(&ssd->lock);
	ssd->client = client;
	ssd->txbuf[0] = SSD1315_CTRL_DATA;
	i2c_set_clientdata(client, ssd);

	ret = ssd1315_init_display(ssd);
	if (ret) {
		dev_err_probe(dev, ret, "display init failed\n");
		goto err_free;
	}

	ssd->miscdev.minor  = MISC_DYNAMIC_MINOR;
	ssd->miscdev.name   = "ssd1315";
	ssd->miscdev.fops   = &ssd1315_fops;
	ssd->miscdev.parent = dev;

	ret = misc_register(&ssd->miscdev);
	if (ret) {
		dev_err(dev, "misc_register failed: %d\n", ret);
		goto err_off;
	}

	dev_info(dev, "SSD1315 128x64 ready at 0x%02x, /dev/%s\n",
		 client->addr, ssd->miscdev.name);
	return 0;

err_off:
	ssd1315_cmd(client, SSD1315_CMD_DISPLAY_OFF);
err_free:
	kfree(ssd);
	return ret;
}

static void ssd1315_remove(struct i2c_client *client)
{
	struct ssd1315_dev *ssd = i2c_get_clientdata(client);

	/* No new opens after this returns */
	misc_deregister(&ssd->miscdev);

	/* Detach from the hardware. Open files now get -ENODEV */
	mutex_lock(&ssd->lock);
	ssd1315_cmd(client, SSD1315_CMD_DISPLAY_OFF);
	ssd->client = NULL;
	mutex_unlock(&ssd->lock);

	kref_put(&ssd->ref, ssd1315_free);
}

static const struct i2c_device_id ssd1315_id[] = {
	{ "ssd1315" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ssd1315_id);

static const struct of_device_id ssd1315_of_match[] = {
	{ .compatible = "solomon,ssd1315" },
	{ }
};
MODULE_DEVICE_TABLE(of, ssd1315_of_match);

static struct i2c_driver ssd1315_driver = {
	.driver = {
		.name		= "ssd1315",
		.of_match_table	= ssd1315_of_match,
	},
	.probe		= ssd1315_probe,
	.remove		= ssd1315_remove,
	.id_table	= ssd1315_id,
};
module_i2c_driver(ssd1315_driver);

MODULE_AUTHOR("Yassine El H. <yassineflag>");
MODULE_DESCRIPTION("SSD1315 OLED I2C driver (/dev/ssd1315)");
MODULE_LICENSE("GPL");
