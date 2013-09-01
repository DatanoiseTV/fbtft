#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/delay.h>

#include "fbtft.h"

#define DRVNAME     "fb_gu39xx"
#define WIDTH       256
#define HEIGHT      64
#define ADDRESS     0

#define CMD_STX          0x02
#define CMD_BRIGHTNESS   0x58
#define CMD_BITIMAGE     0x46

int init[] = { -3 };


/* this does nothing, we set the address window in write_vmem */
static void set_addr_win(struct fbtft_par *par, int xs, int ys, int xe, int ye)
{
}

static int blank(struct fbtft_par *par, bool on)
{
	fbtft_par_dbg(DEBUG_BLANK, par, "%s(blank=%s)\n", __func__, on ? "true" : "false");
	if (on)
		/* set brightness to 100% */
		write_reg(par, CMD_STX, 0x44, ADDRESS, CMD_BRIGHTNESS, 0x10 + 4);
	else
		/* set brightness to 0% */
		write_reg(par, CMD_STX, 0x44, ADDRESS, CMD_BRIGHTNESS, 0x10 + 0);
	return 0;
}

static int write(struct fbtft_par *par, void *buf, size_t len)
{
	u8 data;
	int i;
	static u8 prev_data;

	fbtft_par_dbg_hex(DEBUG_WRITE, par, par->info->device, u8, buf, len,
		"%s(len=%d): ", __func__, len);

	while (len--) {
		data = *(u8 *) buf;

		/* wait for ready line to be asserted */
		while(gpio_get_value(par->gpio.aux[0]) == 0)
			yield();

		/* Start writing by pulling down /WR */
		gpio_set_value(par->gpio.wr, 0);

		/* Set data */
		if (data == prev_data) {
			gpio_set_value(par->gpio.wr, 0); /* used as delay */
		} else {
			for (i = 0; i < 8; i++) {
				if ((data & 1) != (prev_data & 1))
					gpio_set_value(par->gpio.db[i],
								(data & 1));
				data >>= 1;
				prev_data >>= 1;
			}
		}

		/* Pullup /WR */
		gpio_set_value(par->gpio.wr, 1);

		prev_data = *(u8 *) buf;
		buf++;
	}

	return 0;
}

#define CYR     613    /* 2.392 */
#define CYG     601    /* 2.348 */
#define CYB     233    /* 0.912 */

#define THRESH  ((int)(65536 * 0.4))

static unsigned int rgb565_to_m(unsigned int rgb)
{
	unsigned int value;

	rgb = cpu_to_le16(rgb);
	value = CYR * (rgb >> 11) + CYG * (rgb >> 5 & 0x3F) + CYB * (rgb & 0x1F);

	return (value > THRESH) ? 1 : 0;
}

static int write_vmem(struct fbtft_par *par, size_t offset, size_t len)
{
	u16 *vmem16 = (u16 *)(par->info->screen_base);
	u8 *buf = par->txbuf.buf;
	uint16_t wr_addr, wr_len;
	int y, x, bl_height, bl_width;
	unsigned int offs;
	int ret = 0;

	bl_width = par->info->var.xres;
	bl_height = par->info->var.yres;

	fbtft_par_dbg(DEBUG_WRITE_VMEM, par,
		"%s(offset=0x%x bl_width=%d bl_height=%d)\n", __func__, offset, bl_width, bl_height);

	for (x = 0; x < bl_width; x ++) {
		for (y = 0; y < bl_height; y += 8) {
			offs = (y * bl_width) + x;
			/* each byte contains 8 pixels in incrementing height */
			*buf =    rgb565_to_m(vmem16[offs]) << 7;
			offs += bl_width;
			*buf |=   rgb565_to_m(vmem16[offs]) << 6;
			offs += bl_width;
			*buf |=   rgb565_to_m(vmem16[offs]) << 5;
			offs += bl_width;
			*buf |=   rgb565_to_m(vmem16[offs]) << 4;
			offs += bl_width;
			*buf |=   rgb565_to_m(vmem16[offs]) << 3;
			offs += bl_width;
			*buf |=   rgb565_to_m(vmem16[offs]) << 2;
			offs += bl_width;
			*buf |=   rgb565_to_m(vmem16[offs]) << 1;
			offs += bl_width;
			*buf++ |= rgb565_to_m(vmem16[offs]) << 0;
		}
	}

	/* Write data */
	wr_addr = cpu_to_le16(0);
	wr_len = cpu_to_le16(bl_width * bl_height / 8);
	write_reg(par, CMD_STX, 0x44, ADDRESS, CMD_BITIMAGE, wr_addr, wr_addr >> 8, wr_len, wr_len >> 8);
	ret = par->fbtftops.write(par, par->txbuf.buf, wr_len);
	if (ret < 0)
		dev_err(par->info->device, "%s: write failed and returned: %d\n", __func__, ret);

	return ret;
}


static int verify_gpios(struct fbtft_par *par)
{
	int i;

	if (par->gpio.aux[0] < 0) {
		dev_err(par->info->device, "Missing info about 'ready' gpio.  Aborting.\n");
		return -EINVAL;
	}

	if (par->gpio.wr < 0) {
		dev_err(par->info->device, "Missing info about 'wr' gpio.  Aborting.\n");
		return -EINVAL;
	}

	for (i = 0; i < 8; i ++) {
		if (par->gpio.db[i] < 0) {
			dev_err(par->info->device, "Missing info about 'db%02d' gpio.  Aborting.\n", i);
			return -EINVAL;
		}
	}

	return 0;
}

static unsigned long request_gpios_match(struct fbtft_par *par, const struct fbtft_gpio *gpio)
{
	if (strcasecmp(gpio->name, "ready") == 0) {
		par->gpio.aux[0] = gpio->gpio;
		return GPIOF_IN;
	}

	return FBTFT_GPIO_NO_MATCH;
}

static struct fbtft_display display = {
	.regwidth = 8,
	.width = WIDTH,
	.height = HEIGHT,
	.txbuflen = WIDTH*HEIGHT/8,
	.init_sequence = init,
	.fbtftops = {
		.write = write,
		.write_vmem = write_vmem,
		.set_addr_win  = set_addr_win,
//		.blank = blank,
		.request_gpios_match = request_gpios_match,
		.verify_gpios = verify_gpios,
	},
};
FBTFT_REGISTER_DRIVER(DRVNAME, &display);

MODULE_ALIAS("platform:" DRVNAME);

MODULE_DESCRIPTION("Noritake GU-39xx VFD Driver");
MODULE_AUTHOR("Ryan Press");
MODULE_LICENSE("GPL");
