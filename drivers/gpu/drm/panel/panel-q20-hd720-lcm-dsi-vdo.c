// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include <drm/drm_modes.h>
#include <linux/delay.h>
#include <drm/drm_device.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>
#include <linux/backlight.h>

#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>
#include <video/of_videomode.h>
#include <video/videomode.h>

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>

#define CONFIG_MTK_PANEL_EXT
#if defined(CONFIG_MTK_PANEL_EXT)
#include "../mediatek/mediatek_v2/mtk_drm_graphics_base.h"
#include "../mediatek/mediatek_v2/mtk_log.h"
#include "../mediatek/mediatek_v2/mtk_panel_ext.h"
#include "include/panel-q20-hd720-lcm-dsi-vdo.h"
#endif

/*add by hodafone begin*/
#include <linux/proc_fs.h> 
#include <linux/seq_file.h>
#include <linux/of_gpio.h>
#include <linux/of.h>
#define PROC_NAME	"hodafone_lcm"
static struct proc_dir_entry *lcm_proc_entry;
/*add by hodafone end*/

#ifdef CONFIG_MTK_ROUND_CORNER_SUPPORT
#include "../mediatek/mtk_corner_pattern/mtk_data_hw_roundedpattern.h"
#endif

#define LCM_NAME "panel-q20_hd720_lcm_dsi_vdo"

struct hodafone_lcm {
	struct device *dev;
	struct drm_panel panel;
	struct backlight_device *backlight;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *avdd;

	bool prepared;
	bool enabled;
	
	char *board_name;

	int error;
};

static struct hodafone_lcm *mlocalctx = NULL;

#define lcm_dcs_write_seq(ctx, seq...)                                         \
	({                                                                     \
		const u8 d[] = {seq};                                          \
		BUILD_BUG_ON_MSG(ARRAY_SIZE(d) > 64,                           \
				 "DCS sequence too big for stack");            \
		lcm_dcs_write(ctx, d, ARRAY_SIZE(d));                          \
	})

#define lcm_dcs_write_seq_static(ctx, seq...)                                  \
	({                                                                     \
		static const u8 d[] = {seq};                                   \
		lcm_dcs_write(ctx, d, ARRAY_SIZE(d));                          \
	})

static inline struct hodafone_lcm *panel_to_hodafone_lcm(struct drm_panel *panel)
{
	return container_of(panel, struct hodafone_lcm, panel);
}

#define HODAFONE_Q20_V12_BOARD_NAME "q20_v12"
int is_board_q20_v12(struct hodafone_lcm *ctx) {
	if (ctx->board_name && (0 == strcmp(HODAFONE_Q20_V12_BOARD_NAME, ctx->board_name))) {
		
		return 1;
	}

	return 0;
}

#if 0
static void lcm_dcs_write(struct hodafone_lcm *ctx, const void *data, size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	ssize_t ret;
	char *addr;

	if (ctx->error < 0)
		return;

	addr = (char *)data;
	if ((int)*addr < 0xB0)
		ret = mipi_dsi_dcs_write_buffer(dsi, data, len);
	else
		ret = mipi_dsi_generic_write(dsi, data, len);
	if (ret < 0) {
		dev_err(ctx->dev, "error %zd writing seq: %ph\n", ret, data);
		ctx->error = ret;
	}
}
#endif

static void lcm_panel_init(struct hodafone_lcm *ctx)
{
#if 0
	lcm_dcs_write_seq_static(ctx, 0x00,0x00);
	lcm_dcs_write_seq_static(ctx, 0xFF,0x12,0x82,0x01);
	lcm_dcs_write_seq_static(ctx, 0x00,0x80);
	lcm_dcs_write_seq_static(ctx, 0xFF,0x12,0x82);
	lcm_dcs_write_seq_static(ctx, 0x00,0x90);
	lcm_dcs_write_seq_static(ctx, 0xB3,0x64,0x04,0x50);
	lcm_dcs_write_seq_static(ctx, 0x00,0x90);
	lcm_dcs_write_seq_static(ctx, 0xC1,0xBB);
	lcm_dcs_write_seq_static(ctx, 0x00,0x80);
	lcm_dcs_write_seq_static(ctx, 0xC0,0x00,0x90,0x00,0x0A,0x0A,0x00,0x8A,0x0A,0x0A,0x00,0x8A,0x00,0x0A,0x0A); 
	lcm_dcs_write_seq_static(ctx, 0x00,0xA0);
	lcm_dcs_write_seq_static(ctx, 0xC0,0x00,0x00,0x00,0x02,0x00,0x25,0x03,0x00,0x00,0x00,0x00,0x00);
	lcm_dcs_write_seq_static(ctx, 0x00,0x80);
	lcm_dcs_write_seq_static(ctx, 0xC2,0x82,0x02,0x00,0x00,0x00,0x00,0x00,0x00);
	lcm_dcs_write_seq_static(ctx, 0x00,0x90);
	lcm_dcs_write_seq_static(ctx, 0xC2,0x87,0x0C,0x01,0x04,0x04,0x86,0x0C,0x01,0x04,0x04,0x85,0x0D,0x01,0x04,0x04);
	lcm_dcs_write_seq_static(ctx, 0x00,0xA0);
	lcm_dcs_write_seq_static(ctx, 0xC2,0x84,0x0D,0x01,0x04,0x04,0x87,0x0B,0x01,0x04,0x04,0x86,0x0B,0x01,0x04,0x04);
	lcm_dcs_write_seq_static(ctx, 0x00,0xB0);
	lcm_dcs_write_seq_static(ctx, 0xC2,0x85,0x0D,0x01,0x04,0x04,0x84,0x0D,0x01,0x04,0x04);
	lcm_dcs_write_seq_static(ctx, 0x00,0x00);
	lcm_dcs_write_seq_static(ctx, 0x1C,0x20);
	lcm_dcs_write_seq_static(ctx, 0x00,0xB0);
	lcm_dcs_write_seq_static(ctx, 0xCA,0x04,0x04,0x5F,0x50);
	lcm_dcs_write_seq_static(ctx, 0x00,0x00);
	lcm_dcs_write_seq_static(ctx, 0xE1,0x0F,0x23,0x2D,0x3A,0x43,0x4B,0x57,0x6A,0x75,0x87,0x92,0x9B,0x5F,0x5A,0x57,0x4D,0x3F,0x32,0x2A,0x24,0x1F,0x1B,0x1A,0x18);
	lcm_dcs_write_seq_static(ctx, 0x00,0x00);
	lcm_dcs_write_seq_static(ctx, 0xE2,0x0F,0x23,0x2D,0x3A,0x43,0x4B,0x57,0x6A,0x75,0x87,0x92,0x9B,0x5F,0x5A,0x57,0x4D,0x3F,0x32,0x2A,0x24,0x1F,0x1B,0x1A,0x18);
	lcm_dcs_write_seq_static(ctx, 0x00,0x00);
	lcm_dcs_write_seq_static(ctx, 0xE3,0x0F,0x23,0x2D,0x3A,0x43,0x4B,0x57,0x6A,0x75,0x87,0x92,0x9B,0x5F,0x5A,0x57,0x4D,0x3F,0x32,0x2A,0x24,0x1F,0x1B,0x1A,0x18);
	lcm_dcs_write_seq_static(ctx, 0x00,0x00);
	lcm_dcs_write_seq_static(ctx, 0xE4,0x0F,0x23,0x2D,0x3A,0x43,0x4B,0x57,0x6A,0x75,0x87,0x92,0x9B,0x5F,0x5A,0x57,0x4D,0x3F,0x32,0x2A,0x24,0x1F,0x1B,0x1A,0x18);
	lcm_dcs_write_seq_static(ctx, 0x00,0x00);
	lcm_dcs_write_seq_static(ctx, 0xE5,0x0F,0x23,0x2D,0x3A,0x43,0x4B,0x57,0x6A,0x75,0x87,0x92,0x9B,0x5F,0x5A,0x57,0x4D,0x3F,0x32,0x2A,0x24,0x1F,0x1B,0x1A,0x18);
	lcm_dcs_write_seq_static(ctx, 0x00,0x00);
	lcm_dcs_write_seq_static(ctx, 0xE6,0x0F,0x23,0x2D,0x3A,0x43,0x4B,0x57,0x6A,0x75,0x87,0x92,0x9B,0x5F,0x5A,0x57,0x4D,0x3F,0x32,0x2A,0x24,0x1F,0x1B,0x1A,0x18);
	lcm_dcs_write_seq_static(ctx, 0x59,0x03);
	lcm_dcs_write_seq_static(ctx, 0x00,0xA0);
	lcm_dcs_write_seq_static(ctx, 0xD6,0x01,0x4D,0x01,0x4D,0x01,0xA0,0x01,0xF3,0x01,0xF3,0x01,0xF3);
	lcm_dcs_write_seq_static(ctx, 0x00,0xB0);
	lcm_dcs_write_seq_static(ctx, 0xD6,0x01,0xF3,0x01,0xF3,0x01,0xF3,0x01,0xF3,0x01,0xF3,0x01,0xF3);
	lcm_dcs_write_seq_static(ctx, 0x00,0xC0);
	lcm_dcs_write_seq_static(ctx, 0xD6,0x6B,0x11,0x33,0xA2,0x11,0xA2,0xA2,0x11,0xA2,0xA2,0x11,0xA2);
	lcm_dcs_write_seq_static(ctx, 0x00,0xD0);
	lcm_dcs_write_seq_static(ctx, 0xD6,0xA2,0x11,0xA2,0xA2,0x11,0xA2);
	lcm_dcs_write_seq_static(ctx, 0x00,0xE0);
	lcm_dcs_write_seq_static(ctx, 0xD6,0x51,0x11,0x51,0x51,0x11,0x51,0x51,0x11,0x51,0x51,0x11,0x51);
	lcm_dcs_write_seq_static(ctx, 0x00,0xF0);
	lcm_dcs_write_seq_static(ctx, 0xD6,0x51,0x11,0x51,0x51,0x11,0x51); 
	lcm_dcs_write_seq_static(ctx, 0x59,0x00);
	lcm_dcs_write_seq_static(ctx, 0x00,0x80);
	lcm_dcs_write_seq_static(ctx, 0xFF,0x00,0x00);
	lcm_dcs_write_seq_static(ctx, 0x00,0x00);
	lcm_dcs_write_seq_static(ctx, 0xFF,0x00,0x00,0x00);
	lcm_dcs_write_seq_static(ctx, 0x51,0xFF);
	lcm_dcs_write_seq_static(ctx, 0x53,0x2C);
	lcm_dcs_write_seq_static(ctx, 0x55,0x90);
	lcm_dcs_write_seq_static(ctx, 0x35,0x00);
	lcm_dcs_write_seq_static(ctx, 0x44,0x01,0xD9);	
#endif
}

#ifdef PANEL_SUPPORT_READBACK
static int lcm_dcs_read(struct hodafone_lcm *ctx, u8 cmd, void *data, size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	ssize_t ret;

	if (ctx->error < 0)
		return 0;

	ret = mipi_dsi_dcs_read(dsi, cmd, data, len);
	if (ret < 0) {
		dev_err(ctx->dev, "error %d reading dcs seq:(%#x)\n", ret, cmd);
		ctx->error = ret;
	}

	return ret;
}

static void lcm_panel_get_data(struct hodafone_lcm *ctx)
{
	u8 buffer[3] = {0};
	static int ret;

	if (ret == 0) {
		ret = lcm_dcs_read(ctx, 0x0A, buffer, 1);
		dev_err(ctx->dev, "return %d data(0x%08x) to dsi engine\n",
			 ret, buffer[0] | (buffer[1] << 8));
	}
}
#endif

static int lcm_disable(struct drm_panel *panel)
{
	struct hodafone_lcm *ctx = panel_to_hodafone_lcm(panel);

	//pr_err("%s\n", __func__);
	if (!ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(ctx->backlight);
	}

	ctx->enabled = false;

	return 0;
}

static int lcm_unprepare(struct drm_panel *panel)
{
	struct hodafone_lcm *ctx = panel_to_hodafone_lcm(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret;

	//pr_err("%s\n", __func__);
	if (!ctx->prepared)
		return 0;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret) {
		dev_err(ctx->dev, "failed to turn display off (%d)\n", ret);
		return ret;
	}
	usleep_range(10000, 20000);

	/* Enter sleep mode */
	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret) {
		dev_err(ctx->dev, "failed to enter sleep mode (%d)\n", ret);
		return ret;
	}
		
	if (is_board_q20_v12(ctx)) {
		dev_err(ctx->dev, "Q20 V12 board not poweroff\n");
	} else {
		dev_err(ctx->dev, "Q20 V1 board poweroff\n");
		gpiod_set_value(ctx->reset_gpio, 0);
		gpiod_set_value(ctx->avdd, 0);
	}

	ctx->error = 0;
	ctx->prepared = false;
	
	return 0;
}

static int lcm_prepare(struct drm_panel *panel)
{
	struct hodafone_lcm *ctx = panel_to_hodafone_lcm(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret;
	

	//pr_info("%s\n", __func__);
	if (ctx->prepared)
		return 0;
	
	gpiod_set_value(ctx->avdd, 1);
	mdelay(15);
	gpiod_set_value(ctx->reset_gpio, 1);
	mdelay(1);
	gpiod_set_value(ctx->reset_gpio, 0);
	mdelay(10);
	gpiod_set_value(ctx->reset_gpio, 1);
	mdelay(120);
	
	lcm_panel_init(ctx);
	
	/* Exit sleep mode */
	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret) {
		dev_err(ctx->dev, "failed to exit sleep mode (%d)\n", ret);
		return ret;
	}
	/* Up to 120 ms */
	usleep_range(120000, 150000);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret) {
		dev_err(ctx->dev, "failed to turn display on (%d)\n", ret);
		return ret;
	}
	/* Some 10 ms */
	usleep_range(10000, 20000);

	ret = ctx->error;
	if (ret < 0)
		lcm_unprepare(panel);

	ctx->prepared = true;
#if defined(CONFIG_MTK_PANEL_EXT)
	mtk_panel_tch_rst(panel);
#endif
#ifdef PANEL_SUPPORT_READBACK
	lcm_panel_get_data(ctx);
#endif

	return ret;
}

static int lcm_enable(struct drm_panel *panel)
{
	struct hodafone_lcm *ctx = panel_to_hodafone_lcm(panel);

	//pr_err("%s\n", __func__);
	if (ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(ctx->backlight);
	}

	ctx->enabled = true;

	return 0;
}


#define PCLK_MODE0_IN_KHZ \
    ((FRAME_WIDTH + MODE_0_HFP + HSA + HBP)*(FRAME_HEIGHT + MODE_0_VFP + VSA + VBP)*(60)/1000)

static const struct drm_display_mode default_mode = {
	.clock = PCLK_MODE0_IN_KHZ,
	.hdisplay = FRAME_WIDTH,
	.hsync_start = FRAME_WIDTH + MODE_0_HFP,
	.hsync_end = FRAME_WIDTH + MODE_0_HFP + HSA,
	.htotal = FRAME_WIDTH + MODE_0_HFP + HSA + HBP,
	.vdisplay = FRAME_HEIGHT,
	.vsync_start = FRAME_HEIGHT + MODE_0_VFP,
	.vsync_end = FRAME_HEIGHT + MODE_0_VFP + VSA,
	.vtotal = FRAME_HEIGHT + MODE_0_VFP + VSA + VBP,
};


#if defined(CONFIG_MTK_PANEL_EXT)

static struct mtk_panel_params ext_params = {
	.pll_clk = DATA_RATE/2,
	.data_rate = DATA_RATE,
	.data_rate_khz = DATA_RATE*1000,
	.vfp_low_power = 750,
	.cust_esd_check = 0,
	.esd_check_enable = 0,
	.lcm_esd_check_table[0] = {
		.cmd = 0x0A, .count = 1, .para_list[0] = 0x9C,
	},
	.physical_width_um = PHYSICAL_WIDTH,
	.physical_height_um = PHYSICAL_HEIGHT,
};

static int panel_ext_reset(struct drm_panel *panel, int on)
{
	struct hodafone_lcm *ctx = panel_to_hodafone_lcm(panel);

	gpiod_set_value(ctx->reset_gpio, on);

	return 0;
}

static int panel_ata_check(struct drm_panel *panel)
{
	/* Customer test by own ATA tool */
	return 1;
}






static struct mtk_panel_funcs ext_funcs = {
	.reset = panel_ext_reset,
	.ata_check = panel_ata_check,
};
#endif

static int lcm_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &default_mode);
	if (!mode) {
		dev_info(connector->dev->dev, "failed to add mode %ux%ux@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			drm_mode_vrefresh(&default_mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = 62;
	connector->display_info.height_mm = 62;

	return 1;
}

static const struct drm_panel_funcs lcm_drm_funcs = {
	.disable = lcm_disable,
	.unprepare = lcm_unprepare,
	.prepare = lcm_prepare,
	.enable = lcm_enable,
	.get_modes = lcm_get_modes,
};


/*add by hodafone begin*/
static int lcm_debug_read(struct seq_file *m, void *v)                     
{
	seq_printf(m, "%s\n", LCM_NAME);
	return 0;
}

static int proc_hodafone_lcm_open(struct inode *inode, struct file *file)
{
	return single_open(file, lcm_debug_read, NULL);
};

static struct proc_ops hodafone_lcm_fops = {
	.proc_open  = proc_hodafone_lcm_open,
	.proc_read  = seq_read,
};

/*add by hodafone end*/

static int lcm_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct hodafone_lcm *ctx;
	struct device_node *backlight;
	int ret;
	struct device_node *dsi_node, *remote_node = NULL, *endpoint = NULL;

	dsi_node = of_get_parent(dev->of_node);
	if (dsi_node) {
		endpoint = of_graph_get_next_endpoint(dsi_node, NULL);
		if (endpoint) {
			remote_node = of_graph_get_remote_port_parent(endpoint);
			if (!remote_node) {
				pr_info("No panel connected,skip probe lcm\n");
				return -ENODEV;
			}
			pr_info("device node name:%s\n", remote_node->name);
		}
	}
	if (remote_node != dev->of_node) {
		pr_info("%s+ skip probe due to not current lcm\n", __func__);
		return -ENODEV;
	}

	ctx = devm_kzalloc(dev, sizeof(struct hodafone_lcm), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, ctx);

	mlocalctx = ctx;
	ctx->dev = dev;
	dsi->lanes = 2;
	dsi->format = MIPI_DSI_FMT_RGB888;
#ifdef LCM_DSI_CMD_MODE
	dsi->mode_flags = MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_EOT_PACKET |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS;
#else
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_EOT_PACKET |
			MIPI_DSI_CLOCK_NON_CONTINUOUS;	
#endif
	backlight = of_parse_phandle(dev->of_node, "backlight", 0);

	if (backlight) {
		ctx->backlight = of_find_backlight_by_node(backlight);
		of_node_put(backlight);

		if (!ctx->backlight)
			return -EPROBE_DEFER;
	}

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);

	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(dev, "cannot get reset-gpios %ld\n",
			PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}

	ctx->avdd = devm_gpiod_get(dev, "avdd", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->avdd)) {
		dev_err(dev, "cannot get avdd %ld\n",
			PTR_ERR(ctx->avdd));
		return PTR_ERR(ctx->avdd);
	}
	
	ctx->board_name = (char *)of_get_property(dev->of_node, "board_name", NULL);
	
	ctx->prepared = true;
	ctx->enabled = true;

	drm_panel_init(&ctx->panel, dev, &lcm_drm_funcs, DRM_MODE_CONNECTOR_DSI);
	ctx->panel.dev = dev;
	ctx->panel.funcs = &lcm_drm_funcs;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);

	if (ret < 0)
		drm_panel_remove(&ctx->panel);

#if defined(CONFIG_MTK_PANEL_EXT)
	mtk_panel_tch_handle_reg(&ctx->panel);
	ret = mtk_panel_ext_create(dev, &ext_params, &ext_funcs, &ctx->panel);
	if (ret < 0)
		return ret;
#endif

	/*add by hodafone begin*/
	lcm_proc_entry = proc_create(PROC_NAME, 0777, NULL,&hodafone_lcm_fops);
	/*add by hodafone end*/
	
	pr_info("%s-\n", __func__);

	return ret;
}

static int lcm_remove(struct mipi_dsi_device *dsi)
{
	struct hodafone_lcm *ctx = mipi_dsi_get_drvdata(dsi);
#if defined(CONFIG_MTK_PANEL_EXT)
	struct mtk_panel_ctx *ext_ctx = find_panel_ctx(&ctx->panel);
#endif

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
#if defined(CONFIG_MTK_PANEL_EXT)
	mtk_panel_detach(ext_ctx);
	mtk_panel_remove(ext_ctx);
#endif
	/*add by hodafone begin*/
	if (lcm_proc_entry)
		remove_proc_entry(PROC_NAME, NULL);
	/*add by hodafone end*/
	
	return 0;
}

static const struct of_device_id lcm_of_match[] = {
	{ .compatible = "mediatek,q20_panel_general,vdo", },
	{ }
};

MODULE_DEVICE_TABLE(of, lcm_of_match);

static struct mipi_dsi_driver q20_lcm_driver = {
	.probe = lcm_probe,
	.remove = lcm_remove,
	.driver = {
		.name = LCM_NAME,
		.owner = THIS_MODULE,
		.of_match_table = lcm_of_match,
	},
};

module_mipi_dsi_driver(q20_lcm_driver);

MODULE_AUTHOR("Tai-Hua Tseng <tai-hua.tseng@mediatek.com>");
MODULE_DESCRIPTION("hodafonehw_panel_general vdo Panel Driver");
MODULE_LICENSE("GPL v2");
