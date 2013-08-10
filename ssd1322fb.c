#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/spi/spi.h>
#include <linux/delay.h>

#include "fbtft.h"

#define DRVNAME     "ssd1322fb"
#define WIDTH       256
#define HEIGHT      64
#define OFFSET      28
#define TXBUFLEN    (WIDTH/2*HEIGHT)
#define GAMMA       "7 1 1 1 1 2 2 3 3 4 4 5 5 6 6"

int init[] = { 			// Initialization for LM560G-256064 5.6" OLED display
	-1, 0xFD, 0x12,		// Unlock OLED driver IC
	-1, 0xAE,		// Display OFF (blank)
	-1, 0xB3, 0xF3,		// Display divide clockratio/frequency
	-1, 0xCA, 0x3F,		// Multiplex ratio, 1/64, 64 COMS enabled
	-1, 0xA2, 0x00,		// Set offset, the display map starting line is COM0
	-1, 0xA1, 0x00,		// Set start line position
	-1, 0xA0, 0x14, 0x11,	// Set remap, horiz address increment, disable colum address remap,
				//  enable nibble remap, scan from com[N-1] to COM0, disable COM split odd even
	-1, 0xAB, 0x01,		// Select external VDD
	-1, 0xB4, 0xA0, 0xFD,	// Display enhancement A, external VSL, enhanced low GS display quality
	-1, 0xC1, 0xFF,		// Contrast current, 256 steps, default is 0x7F
	-1, 0xC7, 0x0F,		// Master contrast current, 16 steps, default is 0x0F
	-1, 0xB1, 0xF0,		// Phase Length
	-1, 0xD1, 0x82, 0x20	// Display enhancement B
	-1, 0xBB, 0x0D,		// Pre-charge voltage
	-1, 0xBE, 0x00,		// Set VCOMH
	-1, 0xA6,		// Normal display
	-1, 0xAF,		// Display ON
	-3 };

/* Module Parameter: debug  (also available through sysfs) */
MODULE_PARM_DEBUG;


void ssd1322fb_set_addr_win(struct fbtft_par *par, int xs, int ys, int xe, int ye)
{
	fbtft_fbtft_dev_dbg(DEBUG_SET_ADDR_WIN, par, par->info->device, "%s(xs=%d, ys=%d, xe=%d, ye=%d)\n", __func__, xs, ys, xe, ye);

	write_cmd(par, 0x15);
	write_data(par, OFFSET);
	write_data(par, OFFSET+WIDTH/4-1);

	write_cmd(par, 0x75);
	write_data(par, ys);
	write_data(par, ye);

	write_cmd(par, 0x5c);
}

/*
	Grayscale Lookup Table
	GS1 - GS15
	The "Gamma curve" contains the relative values between the entries in the Lookup table.

	From datasheet:
	The next 63 data bytes define Gray Scale (GS) Table by 
	setting the gray scale pulse width in unit of DCLK's 
	(ranges from 0d ~ 180d) 

	0 = Setting of GS1 < Setting of GS2 < Setting of GS3..... < Setting of GS14 < Setting of GS15

*/
static int ssd1322fb_set_gamma(struct fbtft_par *par, unsigned long *curves)
{
	int i, acc = 0;

	fbtft_dev_dbg(DEBUG_INIT_DISPLAY, par->info->device, "%s()\n", __func__);

	/* verify lookup table */
	for (i=0;i<15;i++) {
		acc += curves[i];
		if (acc > 180) {
			dev_err(par->info->device, "Illegal value(s) in Grayscale Lookup Table. At index=%d, the accumulated value has exceeded 180\n", i);
			return -EINVAL;
		}
		if (curves[i] == 0) {
			dev_err(par->info->device, "Illegal value in Grayscale Lookup Table. Value can't be zero\n");
			return -EINVAL;
		}
	}

	acc = 0;
	write_cmd(par, 0xB8);
	for (i=0;i<15;i++) {
		acc += curves[i];
		write_data(par, acc);
	}

	return 0;

	write_cmd(par, 0x00); // Enable Grayscale Lookup
}

static int ssd1322fb_blank(struct fbtft_par *par, bool on)
{
	fbtft_dev_dbg(DEBUG_BLANK, par->info->device, "%s(blank=%s)\n", __func__, on ? "true" : "false");
	if (on)
		write_cmd(par, 0xAE);
	else
		write_cmd(par, 0xAF);
	return 0;
}


#define CYR     613    // 2.392
#define CYG     601    // 2.348
#define CYB     233    // 0.912

static unsigned int ssd1322fb_rgb565_to_y(unsigned int rgb)
{
	return CYR * (rgb >> 11) + CYG * (rgb >> 5 & 0x3F) + CYB * (rgb & 0x1F);
}

static int ssd1322fb_write_vmem(struct fbtft_par *par)
{
	u16 *vmem16 = (u16 *)(par->info->screen_base);
	u8 *buf = par->txbuf.buf;
	size_t offset;
	int y, x, bl_height, bl_width;
	int ret = 0;

	/* Set data line beforehand */
	gpio_set_value(par->gpio.dc, 1);

	offset = par->dirty_lines_start * par->info->var.xres;
	bl_width = par->info->var.xres;
	bl_height = par->dirty_lines_end - par->dirty_lines_start+1;

	fbtft_fbtft_dev_dbg(DEBUG_WRITE_VMEM, par, par->info->device, "%s(offset=0x%x bl_width=%d bl_height=%d)\n", __func__, offset, bl_width, bl_height);

	for (y=0;y<bl_height;y++) {
		for (x=0;x<bl_width/2;x++) {
			*buf = ssd1322fb_rgb565_to_y(vmem16[offset++]) >> 8 & 0xF0;
			*buf++ |= ssd1322fb_rgb565_to_y(vmem16[offset++]) >> 12;
		}
	}

	/* Write data */
	ret = par->fbtftops.write(par, par->txbuf.buf, bl_width/2*bl_height);
	if (ret < 0)
		dev_err(par->info->device, "%s: write failed and returned: %d\n", __func__, ret);

	return ret;
}

struct fbtft_display ssd1322fb_display = {
	.width = WIDTH,
	.height = HEIGHT,
	.txbuflen = TXBUFLEN,
	.gamma_num = 1,
	.gamma_len = 15,
	.gamma = GAMMA,
	.init_sequence = init,
	.fbtftops = {
		.write_vmem = ssd1322fb_write_vmem,
		.set_addr_win  = ssd1322fb_set_addr_win,
		.blank = ssd1322fb_blank,
		.set_gamma = ssd1322fb_set_gamma,
	},
};

static int ssd1322fb_probe(struct spi_device *spi)
{
	return fbtft_probe_common(&ssd1322fb_display, spi, NULL);
}

static int ssd1322fb_remove(struct spi_device *spi)
{
	struct fb_info *info = spi_get_drvdata(spi);

	fbtft_dev_dbg(DEBUG_DRIVER_INIT_FUNCTIONS, &spi->dev, "%s()\n", __func__);

	if (info) {
		fbtft_unregister_framebuffer(info);
		fbtft_framebuffer_release(info);
	}

	return 0;
}

static struct spi_driver ssd1322fb_driver = {
	.driver = {
		.name   = DRVNAME,
		.owner  = THIS_MODULE,
	},
	.probe  = ssd1322fb_probe,
	.remove = ssd1322fb_remove,
};

static int __init ssd1322fb_init(void)
{
	fbtft_pr_debug("\n\n"DRVNAME": %s()\n", __func__);
	return spi_register_driver(&ssd1322fb_driver);
}

static void __exit ssd1322fb_exit(void)
{
	fbtft_pr_debug(DRVNAME": %s()\n", __func__);
	spi_unregister_driver(&ssd1322fb_driver);
}

module_init(ssd1322fb_init);
module_exit(ssd1322fb_exit);

MODULE_DESCRIPTION("SSD1322 OLED Driver");
MODULE_AUTHOR("Ryan Press");
MODULE_LICENSE("GPL");
