// SPDX-License-Identifier: GPL-2.0
/*
 * SSD1315 OLED Display — Linux I2C Kernel Driver
 *
 * Target : BeagleBone Black, I2C2 (P9.19=SCL, P9.20=SDA), addr 0x3C
 * Display: 128×64 monochrome OLED (SSD1315 controller)
 *
 * SSD1315 vs SSD1306 differences implemented here:
 *  - Charge pump command: 0x8D + 0x95 (9V) instead of 0x8D + 0x14 (7.5V)
 *    The SSD1315 benefits from 9V for better display stability at 3.3V supply.
 *  - Clock divider/oscillator: 0xD5 + 0xF0 (higher freq) vs 0x80
 *    SSD1315 oscillator runs at 620–756 kHz; 0xF0 sets max freq, divide=1.
 *  - VCOMH deselect: 0xDB + 0x20 (0.77×VCC) — same as SSD1306 but explicit.
 *  - Compatible strings updated: "ssd1315-oled"
 *  - Device node: /dev/ssd1315
 *  - Everything else (128×64, horizontal addr, page structure) is identical.
 *
 * Wiring:
 *   SSD1315 VCC  → P9.3 (3.3V)
 *   SSD1315 GND  → P9.1 (GND)
 *   SSD1315 SCL  → P9.19 (I2C2_SCL)
 *   SSD1315 SDA  → P9.20 (I2C2_SDA)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/delay.h>

/* -------------------------------------------------------------------------
 * SSD1315 constants
 * ------------------------------------------------------------------------- */
#define SSD1315_DISPLAY_WIDTH       128
#define SSD1315_DISPLAY_HEIGHT       64
#define SSD1315_PAGES               (SSD1315_DISPLAY_HEIGHT / 8)
#define SSD1315_FRAMEBUF_SIZE       (SSD1315_DISPLAY_WIDTH * SSD1315_PAGES)

#define SSD1315_CTRL_CMD            0x00
#define SSD1315_CTRL_DATA           0x40

/* Commands (shared with SSD1306 unless noted) */
#define SSD1315_CMD_DISPLAY_OFF     0xAE
#define SSD1315_CMD_DISPLAY_ON      0xAF
#define SSD1315_CMD_SET_CONTRAST    0x81
#define SSD1315_CMD_ENTIRE_DISP_ON  0xA4
#define SSD1315_CMD_NORMAL_DISPLAY  0xA6
#define SSD1315_CMD_SET_MEM_ADDR    0x20
#define SSD1315_CMD_MEM_ADDR_HORIZ  0x00
#define SSD1315_CMD_SET_COL_ADDR    0x21
#define SSD1315_CMD_SET_PAGE_ADDR   0x22
#define SSD1315_CMD_SET_MUX_RATIO   0xA8
#define SSD1315_CMD_SET_DISP_OFFSET 0xD3
#define SSD1315_CMD_SET_START_LINE  0x40
#define SSD1315_CMD_CHARGE_PUMP     0x8D
#define SSD1315_CMD_CHARGE_PUMP_ON  0x95  /* SSD1315: 9V (vs 0x14=7.5V on SSD1306) */
#define SSD1315_CMD_SEG_REMAP       0xA1
#define SSD1315_CMD_COM_SCAN_DEC    0xC8
#define SSD1315_CMD_SET_COM_PINS    0xDA
#define SSD1315_CMD_SET_PRECHARGE   0xD9
#define SSD1315_CMD_SET_VCOMH       0xDB
/* SSD1315-specific: clock divider — 0xF0 = max oscillator freq, divide ratio=1 */
#define SSD1315_CMD_SET_DISP_CLOCK  0xD5
#define SSD1315_CLOCK_DIV           0xF0  /* SSD1315: 0xF0 vs SSD1306: 0x80 */

/* -------------------------------------------------------------------------
 * Driver private data
 * ------------------------------------------------------------------------- */
struct ssd1315_dev {
    struct i2c_client   *client;
    struct miscdevice    miscdev;
    struct mutex         lock;
    u8                   framebuf[SSD1315_FRAMEBUF_SIZE];
};

/* -------------------------------------------------------------------------
 * I2C helpers
 * ------------------------------------------------------------------------- */
static int ssd1315_write_cmd(struct i2c_client *client, u8 cmd)
{
    u8 buf[2] = { SSD1315_CTRL_CMD, cmd };
    int ret = i2c_master_send(client, buf, 2);
    if (ret < 0)
        dev_err(&client->dev, "cmd write failed: %d (cmd=0x%02x)\n", ret, cmd);
    return ret < 0 ? ret : 0;
}

static int ssd1315_write_cmd2(struct i2c_client *client, u8 cmd, u8 arg)
{
    u8 buf[3] = { SSD1315_CTRL_CMD, cmd, arg };
    int ret = i2c_master_send(client, buf, 3);
    if (ret < 0)
        dev_err(&client->dev, "cmd2 write failed: %d (cmd=0x%02x)\n", ret, cmd);
    return ret < 0 ? ret : 0;
}

static int ssd1315_write_data(struct i2c_client *client,
                               const u8 *data, size_t len)
{
    u8 *txbuf;
    int ret;

    txbuf = kmalloc(len + 1, GFP_KERNEL);
    if (!txbuf)
        return -ENOMEM;

    txbuf[0] = SSD1315_CTRL_DATA;
    memcpy(txbuf + 1, data, len);
    ret = i2c_master_send(client, txbuf, len + 1);
    kfree(txbuf);

    return ret < 0 ? ret : 0;
}

/* -------------------------------------------------------------------------
 * SSD1315 initialisation sequence
 *
 * Key differences from SSD1306:
 *   Step 2: clock 0xF0  (higher oscillator for SSD1315)
 *   Step 6: charge pump 0x95 (9V instead of 7.5V)
 * ------------------------------------------------------------------------- */
static int ssd1315_init_display(struct i2c_client *client)
{
    int ret = 0;
#define CHK(expr) do { ret = (expr); if (ret) return ret; } while (0)

    CHK(ssd1315_write_cmd(client,  SSD1315_CMD_DISPLAY_OFF));

    /* [SSD1315 diff] Higher oscillator frequency for stable refresh */
    CHK(ssd1315_write_cmd2(client, SSD1315_CMD_SET_DISP_CLOCK, SSD1315_CLOCK_DIV));

    CHK(ssd1315_write_cmd2(client, SSD1315_CMD_SET_MUX_RATIO,   0x3F));
    CHK(ssd1315_write_cmd2(client, SSD1315_CMD_SET_DISP_OFFSET, 0x00));
    CHK(ssd1315_write_cmd(client,  SSD1315_CMD_SET_START_LINE | 0x00));

    /* [SSD1315 diff] Charge pump at 9V for better stability with 3.3V supply */
    CHK(ssd1315_write_cmd2(client, SSD1315_CMD_CHARGE_PUMP,
                            SSD1315_CMD_CHARGE_PUMP_ON));

    CHK(ssd1315_write_cmd2(client, SSD1315_CMD_SET_MEM_ADDR,
                            SSD1315_CMD_MEM_ADDR_HORIZ));
    CHK(ssd1315_write_cmd(client,  SSD1315_CMD_SEG_REMAP));
    CHK(ssd1315_write_cmd(client,  SSD1315_CMD_COM_SCAN_DEC));
    CHK(ssd1315_write_cmd2(client, SSD1315_CMD_SET_COM_PINS,  0x12));
    CHK(ssd1315_write_cmd2(client, SSD1315_CMD_SET_CONTRAST,  0xCF));
    CHK(ssd1315_write_cmd2(client, SSD1315_CMD_SET_PRECHARGE, 0xF1));
    CHK(ssd1315_write_cmd2(client, SSD1315_CMD_SET_VCOMH,     0x20));
    CHK(ssd1315_write_cmd(client,  SSD1315_CMD_ENTIRE_DISP_ON));
    CHK(ssd1315_write_cmd(client,  SSD1315_CMD_NORMAL_DISPLAY));

    {
        u8 col[]  = { SSD1315_CTRL_CMD, SSD1315_CMD_SET_COL_ADDR,  0x00, 0x7F };
        u8 page[] = { SSD1315_CTRL_CMD, SSD1315_CMD_SET_PAGE_ADDR, 0x00, 0x07 };
        ret = i2c_master_send(client, col,  sizeof(col));  if (ret < 0) return ret;
        ret = i2c_master_send(client, page, sizeof(page)); if (ret < 0) return ret;
    }

    CHK(ssd1315_write_cmd(client, SSD1315_CMD_DISPLAY_ON));

#undef CHK
    dev_info(&client->dev, "SSD1315 initialised (128x64, horizontal addr, 9V pump)\n");
    return 0;
}

static int ssd1315_flush(struct ssd1315_dev *ssd)
{
    struct i2c_client *client = ssd->client;
    int ret;
    u8 col[]  = { SSD1315_CTRL_CMD, SSD1315_CMD_SET_COL_ADDR,  0x00, 0x7F };
    u8 page[] = { SSD1315_CTRL_CMD, SSD1315_CMD_SET_PAGE_ADDR, 0x00, 0x07 };

    ret = i2c_master_send(client, col,  sizeof(col));  if (ret < 0) return ret;
    ret = i2c_master_send(client, page, sizeof(page)); if (ret < 0) return ret;
    return ssd1315_write_data(client, ssd->framebuf, SSD1315_FRAMEBUF_SIZE);
}

/* -------------------------------------------------------------------------
 * File operations — /dev/ssd1315
 * ------------------------------------------------------------------------- */
#define SSD1315_IOCTL_CLEAR  _IO('S', 0)
#define SSD1315_IOCTL_ON     _IO('S', 1)
#define SSD1315_IOCTL_OFF    _IO('S', 2)

static ssize_t ssd1315_fops_write(struct file *file,
                                   const char __user *buf,
                                   size_t count, loff_t *ppos)
{
    struct ssd1315_dev *ssd =
        container_of(file->private_data, struct ssd1315_dev, miscdev);
    int ret;

    if (count != SSD1315_FRAMEBUF_SIZE) {
        dev_err(&ssd->client->dev,
                "write: expected %d bytes, got %zu\n",
                SSD1315_FRAMEBUF_SIZE, count);
        return -EINVAL;
    }
    if (copy_from_user(ssd->framebuf, buf, SSD1315_FRAMEBUF_SIZE))
        return -EFAULT;

    mutex_lock(&ssd->lock);
    ret = ssd1315_flush(ssd);
    mutex_unlock(&ssd->lock);

    return ret < 0 ? ret : (ssize_t)count;
}

static long ssd1315_fops_ioctl(struct file *file, unsigned int cmd,
                                 unsigned long arg)
{
    struct ssd1315_dev *ssd =
        container_of(file->private_data, struct ssd1315_dev, miscdev);
    int ret = 0;

    mutex_lock(&ssd->lock);
    switch (cmd) {
    case SSD1315_IOCTL_CLEAR:
        memset(ssd->framebuf, 0x00, SSD1315_FRAMEBUF_SIZE);
        ret = ssd1315_flush(ssd);
        break;
    case SSD1315_IOCTL_ON:
        ret = ssd1315_write_cmd(ssd->client, SSD1315_CMD_DISPLAY_ON);
        break;
    case SSD1315_IOCTL_OFF:
        ret = ssd1315_write_cmd(ssd->client, SSD1315_CMD_DISPLAY_OFF);
        break;
    default:
        ret = -ENOTTY;
    }
    mutex_unlock(&ssd->lock);
    return ret;
}

static const struct file_operations ssd1315_fops = {
    .owner          = THIS_MODULE,
    .write          = ssd1315_fops_write,
    .unlocked_ioctl = ssd1315_fops_ioctl,
};

/* -------------------------------------------------------------------------
 * I2C probe / remove
 * ------------------------------------------------------------------------- */
static int ssd1315_probe(struct i2c_client *client,
                          const struct i2c_device_id *id)
{
    struct ssd1315_dev *ssd;
    int ret;

    dev_info(&client->dev, "SSD1315 OLED probed at I2C addr 0x%02x\n",
             client->addr);

    if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
        return -ENODEV;

    ssd = devm_kzalloc(&client->dev, sizeof(*ssd), GFP_KERNEL);
    if (!ssd)
        return -ENOMEM;

    ssd->client = client;
    mutex_init(&ssd->lock);
    i2c_set_clientdata(client, ssd);

    ret = ssd1315_init_display(client);
    if (ret) {
        dev_err(&client->dev, "init failed: %d\n", ret);
        return ret;
    }

    memset(ssd->framebuf, 0x00, SSD1315_FRAMEBUF_SIZE);
    ret = ssd1315_flush(ssd);
    if (ret)
        return ret;

    ssd->miscdev.minor  = MISC_DYNAMIC_MINOR;
    ssd->miscdev.name   = "ssd1315";
    ssd->miscdev.fops   = &ssd1315_fops;
    ssd->miscdev.parent = &client->dev;

    ret = misc_register(&ssd->miscdev);
    if (ret) {
        dev_err(&client->dev, "misc_register failed: %d\n", ret);
        return ret;
    }

    dev_info(&client->dev, "/dev/ssd1315 registered\n");
    return 0;
}

static void ssd1315_remove(struct i2c_client *client)
{
    struct ssd1315_dev *ssd = i2c_get_clientdata(client);
    ssd1315_write_cmd(client, SSD1315_CMD_DISPLAY_OFF);
    misc_deregister(&ssd->miscdev);
    dev_info(&client->dev, "SSD1315 removed\n");
}

/* -------------------------------------------------------------------------
 * Driver registration
 * ------------------------------------------------------------------------- */
static const struct i2c_device_id ssd1315_id[] = {
    { "ssd1315", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, ssd1315_id);

static const struct of_device_id ssd1315_of_match[] = {
    { .compatible = "ssd1315-oled" },
    { .compatible = "solomon,ssd1315" },
    { }
};
MODULE_DEVICE_TABLE(of, ssd1315_of_match);

static struct i2c_driver ssd1315_driver = {
    .driver = {
        .name           = "ssd1315",
        .of_match_table = ssd1315_of_match,
    },
    .probe    = ssd1315_probe,
    .remove   = ssd1315_remove,
    .id_table = ssd1315_id,
};

module_i2c_driver(ssd1315_driver);

MODULE_AUTHOR("Embedded Linux Project");
MODULE_DESCRIPTION("SSD1315 OLED I2C driver — BeagleBone Black");
MODULE_LICENSE("GPL v2");
