/*
 * (C) Copyright 2008-2016 Fuzhou Rockchip Electronics Co., Ltd
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <config.h>
#include <common.h>
#include <errno.h>
#include <malloc.h>
#include <fdtdec.h>
#include <fdt_support.h>
#include <resource.h>
#include <asm/arch/rkplat.h>
#include <asm/unaligned.h>
#include <linux/list.h>

#include "rockchip_display.h"
#include "rockchip_crtc.h"
#include "rockchip_connector.h"
#include "rockchip_phy.h"
#include "rockchip_mipi_dsi.h"

#define MSEC_PER_SEC    1000L
#define USEC_PER_SEC	1000000L

#define BIT(x) (1 << (x))

#define readx_poll_timeout(op, addr, val, cond, sleep_us, timeout_us)	\
({ \
	int try = 100; \
	for (;;) { \
		(val) = op(addr); \
		if (cond) \
			break; \
		try--; \
		if (!try) \
			break; \
		if (sleep_us) \
			udelay(sleep_us >> 2); \
	} \
	(cond) ? 0 : -ETIMEDOUT; \
})

#define RK3288_GRF_SOC_CON6		0x025c
#define RK3288_DSI0_SEL_VOP_LIT		BIT(6)
#define RK3288_DSI1_SEL_VOP_LIT		BIT(9)

#define RK3399_GRF_SOC_CON19		0x6250
#define RK3399_DSI0_SEL_VOP_LIT		BIT(0)
#define RK3399_DSI1_SEL_VOP_LIT		BIT(4)

/* disable turnrequest, turndisable, forcetxstopmode, forcerxmode */
#define RK3399_GRF_SOC_CON22		0x6258
#define RK3399_GRF_DSI_MODE		0xffff0000

#define DSI_VERSION			0x00
#define DSI_PWR_UP			0x04
#define RESET				0
#define POWERUP				BIT(0)

#define DSI_CLKMGR_CFG			0x08
#define TO_CLK_DIVIDSION(div)		(((div) & 0xff) << 8)
#define TX_ESC_CLK_DIVIDSION(div)	(((div) & 0xff) << 0)

#define DSI_DPI_VCID			0x0c
#define DPI_VID(vid)			(((vid) & 0x3) << 0)

#define DSI_DPI_COLOR_CODING		0x10
#define EN18_LOOSELY			BIT(8)
#define DPI_COLOR_CODING_16BIT_1	0x0
#define DPI_COLOR_CODING_16BIT_2	0x1
#define DPI_COLOR_CODING_16BIT_3	0x2
#define DPI_COLOR_CODING_18BIT_1	0x3
#define DPI_COLOR_CODING_18BIT_2	0x4
#define DPI_COLOR_CODING_24BIT		0x5

#define DSI_DPI_CFG_POL			0x14
#define COLORM_ACTIVE_LOW		BIT(4)
#define SHUTD_ACTIVE_LOW		BIT(3)
#define HSYNC_ACTIVE_LOW		BIT(2)
#define VSYNC_ACTIVE_LOW		BIT(1)
#define DATAEN_ACTIVE_LOW		BIT(0)

#define DSI_DPI_LP_CMD_TIM		0x18
#define OUTVACT_LPCMD_TIME(p)		(((p) & 0xff) << 16)
#define INVACT_LPCMD_TIME(p)		((p) & 0xff)

#define DSI_DBI_CFG			0x20
#define DSI_DBI_CMDSIZE			0x28

#define DSI_PCKHDL_CFG			0x2c
#define EN_CRC_RX			BIT(4)
#define EN_ECC_RX			BIT(3)
#define EN_BTA				BIT(2)
#define EN_EOTP_RX			BIT(1)
#define EN_EOTP_TX			BIT(0)

#define DSI_MODE_CFG			0x34
#define ENABLE_VIDEO_MODE		0
#define ENABLE_CMD_MODE			BIT(0)

#define DSI_VID_MODE_CFG		0x38
#define FRAME_BTA_ACK			BIT(14)
#define ENABLE_LOW_POWER		(0x3f << 8)
#define ENABLE_LOW_POWER_MASK		(0x3f << 8)
#define VID_MODE_TYPE_BURST_SYNC_PULSES		0x2
#define VID_MODE_TYPE_MASK			0x3

#define DSI_VID_PKT_SIZE		0x3c
#define VID_PKT_SIZE(p)			(((p) & 0x3fff) << 0)
#define VID_PKT_MAX_SIZE		0x3fff

#define DSI_VID_HSA_TIME		0x48
#define DSI_VID_HBP_TIME		0x4c
#define DSI_VID_HLINE_TIME		0x50
#define DSI_VID_VSA_LINES		0x54
#define DSI_VID_VBP_LINES		0x58
#define DSI_VID_VFP_LINES		0x5c
#define DSI_VID_VACTIVE_LINES		0x60
#define DSI_CMD_MODE_CFG		0x68
#define MAX_RD_PKT_SIZE_LP		BIT(24)
#define DCS_LW_TX_LP			BIT(19)
#define DCS_SR_0P_TX_LP			BIT(18)
#define DCS_SW_1P_TX_LP			BIT(17)
#define DCS_SW_0P_TX_LP			BIT(16)
#define GEN_LW_TX_LP			BIT(14)
#define GEN_SR_2P_TX_LP			BIT(13)
#define GEN_SR_1P_TX_LP			BIT(12)
#define GEN_SR_0P_TX_LP			BIT(11)
#define GEN_SW_2P_TX_LP			BIT(10)
#define GEN_SW_1P_TX_LP			BIT(9)
#define GEN_SW_0P_TX_LP			BIT(8)
#define EN_ACK_RQST			BIT(1)
#define EN_TEAR_FX			BIT(0)

#define CMD_MODE_ALL_LP			(MAX_RD_PKT_SIZE_LP | \
					 DCS_LW_TX_LP | \
					 DCS_SR_0P_TX_LP | \
					 DCS_SW_1P_TX_LP | \
					 DCS_SW_0P_TX_LP | \
					 GEN_LW_TX_LP | \
					 GEN_SR_2P_TX_LP | \
					 GEN_SR_1P_TX_LP | \
					 GEN_SR_0P_TX_LP | \
					 GEN_SW_2P_TX_LP | \
					 GEN_SW_1P_TX_LP | \
					 GEN_SW_0P_TX_LP)

#define DSI_GEN_HDR			0x6c
#define GEN_HDATA(data)			(((data) & 0xffff) << 8)
#define GEN_HDATA_MASK			(0xffff << 8)
#define GEN_HTYPE(type)			(((type) & 0xff) << 0)
#define GEN_HTYPE_MASK			0xff

#define DSI_GEN_PLD_DATA		0x70

#define DSI_CMD_PKT_STATUS		0x74
#define GEN_CMD_EMPTY			BIT(0)
#define GEN_CMD_FULL			BIT(1)
#define GEN_PLD_W_EMPTY			BIT(2)
#define GEN_PLD_W_FULL			BIT(3)
#define GEN_PLD_R_EMPTY			BIT(4)
#define GEN_PLD_R_FULL			BIT(5)
#define GEN_RD_CMD_BUSY			BIT(6)

#define DSI_TO_CNT_CFG			0x78
#define HSTX_TO_CNT(p)			(((p) & 0xffff) << 16)
#define LPRX_TO_CNT(p)			((p) & 0xffff)

#define DSI_BTA_TO_CNT			0x8c
#define DSI_LPCLK_CTRL			0x94
#define AUTO_CLKLANE_CTRL		BIT(1)
#define PHY_TXREQUESTCLKHS		BIT(0)

#define DSI_PHY_TMR_LPCLK_CFG		0x98
#define PHY_CLKHS2LP_TIME(lbcc)		(((lbcc) & 0x3ff) << 16)
#define PHY_CLKLP2HS_TIME(lbcc)		((lbcc) & 0x3ff)

#define DSI_PHY_TMR_CFG			0x9c
#define PHY_HS2LP_TIME(lbcc)		(((lbcc) & 0xff) << 24)
#define PHY_LP2HS_TIME(lbcc)		(((lbcc) & 0xff) << 16)
#define MAX_RD_TIME(lbcc)		((lbcc) & 0x7fff)

#define DSI_PHY_RSTZ			0xa0
#define PHY_DISFORCEPLL			0
#define PHY_ENFORCEPLL			BIT(3)
#define PHY_DISABLECLK			0
#define PHY_ENABLECLK			BIT(2)
#define PHY_RSTZ			0
#define PHY_UNRSTZ			BIT(1)
#define PHY_SHUTDOWNZ			0
#define PHY_UNSHUTDOWNZ			BIT(0)

#define DSI_PHY_IF_CFG			0xa4
#define N_LANES(n)			((((n) - 1) & 0x3) << 0)
#define PHY_STOP_WAIT_TIME(cycle)	(((cycle) & 0xff) << 8)

#define DSI_PHY_STATUS			0xb0
#define LOCK				BIT(0)
#define STOP_STATE_CLK_LANE		BIT(2)

#define DSI_PHY_TST_CTRL0		0xb4
#define PHY_TESTCLK			BIT(1)
#define PHY_UNTESTCLK			0
#define PHY_TESTCLR			BIT(0)
#define PHY_UNTESTCLR			0

#define DSI_PHY_TST_CTRL1		0xb8
#define PHY_TESTEN			BIT(16)
#define PHY_UNTESTEN			0
#define PHY_TESTDOUT(n)			(((n) & 0xff) << 8)
#define PHY_TESTDIN(n)			(((n) & 0xff) << 0)

#define DSI_INT_ST0			0xbc
#define DSI_INT_ST1			0xc0
#define DSI_INT_MSK0			0xc4
#define DSI_INT_MSK1			0xc8

#define PHY_STATUS_TIMEOUT_US		10000
#define CMD_PKT_STATUS_TIMEOUT_US	20000

#define BYPASS_VCO_RANGE	BIT(7)
#define VCO_RANGE_CON_SEL(val)	(((val) & 0x7) << 3)
#define VCO_IN_CAP_CON_DEFAULT	(0x0 << 1)
#define VCO_IN_CAP_CON_LOW	(0x1 << 1)
#define VCO_IN_CAP_CON_HIGH	(0x2 << 1)
#define REF_BIAS_CUR_SEL	BIT(0)

#define CP_CURRENT_3MA		BIT(3)
#define CP_PROGRAM_EN		BIT(7)
#define LPF_PROGRAM_EN		BIT(6)
#define LPF_RESISTORS_20_KOHM	0

#define HSFREQRANGE_SEL(val)	(((val) & 0x3f) << 1)

#define INPUT_DIVIDER(val)	((val - 1) & 0x7f)
#define LOW_PROGRAM_EN		0
#define HIGH_PROGRAM_EN		BIT(7)
#define LOOP_DIV_LOW_SEL(val)	((val - 1) & 0x1f)
#define LOOP_DIV_HIGH_SEL(val)	(((val - 1) >> 5) & 0x1f)
#define PLL_LOOP_DIV_EN		BIT(5)
#define PLL_INPUT_DIV_EN	BIT(4)

#define POWER_CONTROL		BIT(6)
#define INTERNAL_REG_CURRENT	BIT(3)
#define BIAS_BLOCK_ON		BIT(2)
#define BANDGAP_ON		BIT(0)

#define TER_RESISTOR_HIGH	BIT(7)
#define	TER_RESISTOR_LOW	0
#define LEVEL_SHIFTERS_ON	BIT(6)
#define TER_CAL_DONE		BIT(5)
#define SETRD_MAX		(0x7 << 2)
#define POWER_MANAGE		BIT(1)
#define TER_RESISTORS_ON	BIT(0)

#define BIASEXTR_SEL(val)	((val) & 0x7)
#define BANDGAP_SEL(val)	((val) & 0x7)
#define TLP_PROGRAM_EN		BIT(7)
#define THS_PRE_PROGRAM_EN	BIT(7)
#define THS_ZERO_PROGRAM_EN	BIT(6)

enum {
	BANDGAP_97_07,
	BANDGAP_98_05,
	BANDGAP_99_02,
	BANDGAP_100_00,
	BANDGAP_93_17,
	BANDGAP_94_15,
	BANDGAP_95_12,
	BANDGAP_96_10,
};

enum {
	BIASEXTR_87_1,
	BIASEXTR_91_5,
	BIASEXTR_95_9,
	BIASEXTR_100,
	BIASEXTR_105_94,
	BIASEXTR_111_88,
	BIASEXTR_118_8,
	BIASEXTR_127_7,
};

struct dw_mipi_dsi_plat_data {
	u32 dsi0_en_bit;
	u32 dsi1_en_bit;
	u32 grf_switch_reg;
	u32 grf_dsi0_mode;
	u32 grf_dsi0_mode_reg;
	unsigned int max_data_lanes;
	u32 max_bit_rate_per_lane;
	bool has_vop_sel;
};

struct dw_mipi_dsi {
	void *base;

	unsigned int lane_mbps; /* per lane */
	u32 channel;
	u32 lanes;
	u32 format;
	u32 mode_flags;
	u16 input_div;
	u16 feedback_div;
	struct drm_display_mode *mode;

	const struct dw_mipi_dsi_plat_data *pdata;
};

enum dw_mipi_dsi_mode {
	DW_MIPI_DSI_CMD_MODE,
	DW_MIPI_DSI_VID_MODE,
};

struct dphy_pll_testdin_map {
	unsigned int max_mbps;
	u8 testdin;
};

/* The table is based on 27MHz DPHY pll reference clock. */
static const struct dphy_pll_testdin_map dptdin_map[] = {
	{  90, 0x00}, { 100, 0x10}, { 110, 0x20}, { 130, 0x01},
	{ 140, 0x11}, { 150, 0x21}, { 170, 0x02}, { 180, 0x12},
	{ 200, 0x22}, { 220, 0x03}, { 240, 0x13}, { 250, 0x23},
	{ 270, 0x04}, { 300, 0x14}, { 330, 0x05}, { 360, 0x15},
	{ 400, 0x25}, { 450, 0x06}, { 500, 0x16}, { 550, 0x07},
	{ 600, 0x17}, { 650, 0x08}, { 700, 0x18}, { 750, 0x09},
	{ 800, 0x19}, { 850, 0x29}, { 900, 0x39}, { 950, 0x0a},
	{1000, 0x1a}, {1050, 0x2a}, {1100, 0x3a}, {1150, 0x0b},
	{1200, 0x1b}, {1250, 0x2b}, {1300, 0x3b}, {1350, 0x0c},
	{1400, 0x1c}, {1450, 0x2c}, {1500, 0x3c}
};

static int max_mbps_to_testdin(unsigned int max_mbps)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(dptdin_map); i++)
		if (dptdin_map[i].max_mbps > max_mbps)
			return dptdin_map[i].testdin;

	return -EINVAL;
}

/*
 * The controller should generate 2 frames before
 * preparing the peripheral.
 */
static void dw_mipi_dsi_wait_for_two_frames(struct dw_mipi_dsi *dsi)
{
	int refresh, two_frames;

	refresh = drm_mode_vrefresh(dsi->mode);
	two_frames = DIV_ROUND_UP(MSEC_PER_SEC, refresh) * 2;
	mdelay(two_frames);
}

static inline void dsi_write(struct dw_mipi_dsi *dsi, u32 reg, u32 val)
{
	writel(val, dsi->base + reg);
}

static inline u32 dsi_read(struct dw_mipi_dsi *dsi, u32 reg)
{
	return readl(dsi->base + reg);
}

static void dw_mipi_dsi_phy_write(struct dw_mipi_dsi *dsi, u8 test_code,
				 u8 test_data)
{
	/*
	 * With the falling edge on TESTCLK, the TESTDIN[7:0] signal content
	 * is latched internally as the current test code. Test data is
	 * programmed internally by rising edge on TESTCLK.
	 */
	dsi_write(dsi, DSI_PHY_TST_CTRL0, PHY_TESTCLK | PHY_UNTESTCLR);

	dsi_write(dsi, DSI_PHY_TST_CTRL1, PHY_TESTEN | PHY_TESTDOUT(0) |
					  PHY_TESTDIN(test_code));

	dsi_write(dsi, DSI_PHY_TST_CTRL0, PHY_UNTESTCLK | PHY_UNTESTCLR);

	dsi_write(dsi, DSI_PHY_TST_CTRL1, PHY_UNTESTEN | PHY_TESTDOUT(0) |
					  PHY_TESTDIN(test_data));

	dsi_write(dsi, DSI_PHY_TST_CTRL0, PHY_TESTCLK | PHY_UNTESTCLR);
}

static int dw_mipi_dsi_phy_init(struct dw_mipi_dsi *dsi)
{
	int ret, testdin, vco, val;

	vco = (dsi->lane_mbps < 200) ? 0 : (dsi->lane_mbps + 100) / 200;

	testdin = max_mbps_to_testdin(dsi->lane_mbps);
	if (testdin < 0) {
		printf("failed to get testdin for %dmbps lane clock\n",
		       dsi->lane_mbps);
		return testdin;
	}

	dsi_write(dsi, DSI_PWR_UP, POWERUP);

	dw_mipi_dsi_phy_write(dsi, 0x10, BYPASS_VCO_RANGE |
					 VCO_RANGE_CON_SEL(vco) |
					 VCO_IN_CAP_CON_LOW |
					 REF_BIAS_CUR_SEL);

	dw_mipi_dsi_phy_write(dsi, 0x11, CP_CURRENT_3MA);
	dw_mipi_dsi_phy_write(dsi, 0x12, CP_PROGRAM_EN | LPF_PROGRAM_EN |
					 LPF_RESISTORS_20_KOHM);

	dw_mipi_dsi_phy_write(dsi, 0x44, HSFREQRANGE_SEL(testdin));

	dw_mipi_dsi_phy_write(dsi, 0x19, PLL_LOOP_DIV_EN | PLL_INPUT_DIV_EN);
	dw_mipi_dsi_phy_write(dsi, 0x17, INPUT_DIVIDER(dsi->input_div));
	dw_mipi_dsi_phy_write(dsi, 0x18, LOOP_DIV_LOW_SEL(dsi->feedback_div) |
					 LOW_PROGRAM_EN);
	dw_mipi_dsi_phy_write(dsi, 0x18, LOOP_DIV_HIGH_SEL(dsi->feedback_div) |
					 HIGH_PROGRAM_EN);

	dw_mipi_dsi_phy_write(dsi, 0x20, POWER_CONTROL | INTERNAL_REG_CURRENT |
					 BIAS_BLOCK_ON | BANDGAP_ON);

	dw_mipi_dsi_phy_write(dsi, 0x21, TER_RESISTOR_LOW | TER_CAL_DONE |
					 SETRD_MAX | TER_RESISTORS_ON);
	dw_mipi_dsi_phy_write(dsi, 0x21, TER_RESISTOR_HIGH | LEVEL_SHIFTERS_ON |
					 SETRD_MAX | POWER_MANAGE |
					 TER_RESISTORS_ON);

	dw_mipi_dsi_phy_write(dsi, 0x22, LOW_PROGRAM_EN |
					 BIASEXTR_SEL(BIASEXTR_127_7));
	dw_mipi_dsi_phy_write(dsi, 0x22, HIGH_PROGRAM_EN |
					 BANDGAP_SEL(BANDGAP_96_10));

	dw_mipi_dsi_phy_write(dsi, 0x70, TLP_PROGRAM_EN | 0xf);
	dw_mipi_dsi_phy_write(dsi, 0x71, THS_PRE_PROGRAM_EN | 0x55);
	dw_mipi_dsi_phy_write(dsi, 0x72, THS_ZERO_PROGRAM_EN | 0xa);

	dsi_write(dsi, DSI_PHY_RSTZ, PHY_ENFORCEPLL | PHY_ENABLECLK |
				     PHY_UNRSTZ | PHY_UNSHUTDOWNZ);

	ret = readx_poll_timeout(readl, dsi->base + DSI_PHY_STATUS,
				 val, val & LOCK, 1000, PHY_STATUS_TIMEOUT_US);
	if (ret < 0) {
		printf("failed to wait for phy lock state\n");
		return ret;
	}

	ret = readx_poll_timeout(readl, dsi->base + DSI_PHY_STATUS,
				 val, val & STOP_STATE_CLK_LANE, 1000,
				 PHY_STATUS_TIMEOUT_US);
	if (ret < 0)
		printf("failed to wait for phy clk lane stop state\n");

	return ret;
}

static int dw_mipi_dsi_get_lane_bps(struct dw_mipi_dsi *dsi)
{
	unsigned int i, pre;
	unsigned long mpclk, pllref, tmp;
	unsigned int m = 1, n = 1, target_mbps = 1000;
	unsigned int max_mbps = dptdin_map[ARRAY_SIZE(dptdin_map) - 1].max_mbps;
	int bpp;

	bpp = mipi_dsi_pixel_format_to_bpp(dsi->format);
	if (bpp < 0) {
		printf("failed to get bpp for pixel format %d\n", dsi->format);
		return bpp;
	}

	mpclk = DIV_ROUND_UP(dsi->mode->clock, MSEC_PER_SEC);
	if (mpclk) {
		/* take 1 / 0.9, since mbps must big than bandwidth of RGB */
		tmp = mpclk * (bpp / dsi->lanes) * 10 / 9;
		if (tmp < max_mbps)
			target_mbps = tmp;
		else
			printf("DPHY clock frequency is out of range\n");
	}

	pllref = DIV_ROUND_UP(24 * MHZ, USEC_PER_SEC);
	tmp = pllref;

	for (i = 1; i < 6; i++) {
		pre = pllref / i;
		if ((tmp > (target_mbps % pre)) && (target_mbps / pre < 512)) {
			tmp = target_mbps % pre;
			n = i;
			m = target_mbps / pre;
		}
		if (tmp == 0)
			break;
	}

	dsi->lane_mbps = pllref / n * m;
	dsi->input_div = n;
	dsi->feedback_div = m;

	return 0;
}

static int dw_mipi_dsi_gen_pkt_hdr_write(struct dw_mipi_dsi *dsi, u32 val)
{
	int ret;
	int sts = 0;

	ret = readx_poll_timeout(readl, dsi->base + DSI_CMD_PKT_STATUS,
				 sts, !(sts & GEN_CMD_FULL), 1000,
				 CMD_PKT_STATUS_TIMEOUT_US);
	if (ret < 0) {
		printf("failed to get available command FIFO\n");
		return ret;
	}

	dsi_write(dsi, DSI_GEN_HDR, val);

	ret = readx_poll_timeout(readl, dsi->base + DSI_CMD_PKT_STATUS,
				 sts, sts & (GEN_CMD_EMPTY | GEN_PLD_W_EMPTY),
				 1000, CMD_PKT_STATUS_TIMEOUT_US);
	if (ret < 0) {
		printf("failed to write command FIFO\n");
		return ret;
	}

	return 0;
}

static int dw_mipi_dsi_short_write(struct dw_mipi_dsi *dsi,
				   const struct mipi_dsi_msg *msg)
{
	const u16 *tx_buf = msg->tx_buf;
	u32 val = GEN_HDATA(*tx_buf) | GEN_HTYPE(msg->type);

	if (msg->tx_len > 2) {
		printf("too long tx buf length %zu for short write\n",
		       msg->tx_len);
		return -EINVAL;
	}

	return dw_mipi_dsi_gen_pkt_hdr_write(dsi, val);
}

static int dw_mipi_dsi_long_write(struct dw_mipi_dsi *dsi,
				  const struct mipi_dsi_msg *msg)
{
	const u32 *tx_buf = msg->tx_buf;
	int len = msg->tx_len, pld_data_bytes = sizeof(*tx_buf), ret;
	u32 val = GEN_HDATA(msg->tx_len) | GEN_HTYPE(msg->type);
	u32 remainder = 0;
	u32 sts = 0;

	if (msg->tx_len < 3) {
		printf("wrong tx buf length %zu for long write\n",
		       msg->tx_len);
		return -EINVAL;
	}

	while (DIV_ROUND_UP(len, pld_data_bytes)) {
		if (len < pld_data_bytes) {
			memcpy(&remainder, tx_buf, len);
			dsi_write(dsi, DSI_GEN_PLD_DATA, remainder);
			len = 0;
		} else {
			dsi_write(dsi, DSI_GEN_PLD_DATA, *tx_buf);
			tx_buf++;
			len -= pld_data_bytes;
		}

		ret = readx_poll_timeout(readl, dsi->base + DSI_CMD_PKT_STATUS,
					 sts, !(sts & GEN_PLD_W_FULL), 1000,
					 CMD_PKT_STATUS_TIMEOUT_US);
		if (ret < 0) {
			printf("failed to get available write payload FIFO\n");
			return ret;
		}
	}

	return dw_mipi_dsi_gen_pkt_hdr_write(dsi, val);
}

static ssize_t rockchip_dw_mipi_dsi_transfer(struct display_state *state,
					     const struct mipi_dsi_msg *msg)
{
	struct connector_state *conn_state = &state->conn_state;
	struct dw_mipi_dsi *dsi = conn_state->private;
	int ret;

	switch (msg->type) {
	case MIPI_DSI_DCS_SHORT_WRITE:
	case MIPI_DSI_DCS_SHORT_WRITE_PARAM:
	case MIPI_DSI_GENERIC_SHORT_WRITE_0_PARAM:
	case MIPI_DSI_GENERIC_SHORT_WRITE_1_PARAM:
	case MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM:
	case MIPI_DSI_SET_MAXIMUM_RETURN_PACKET_SIZE:
		ret = dw_mipi_dsi_short_write(dsi, msg);
		break;
	case MIPI_DSI_DCS_LONG_WRITE:
	case MIPI_DSI_GENERIC_LONG_WRITE:
		ret = dw_mipi_dsi_long_write(dsi, msg);
		break;
	default:
		printf("unsupport packet data type: %d\n", msg->type);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static void dw_mipi_dsi_video_mode_config(struct dw_mipi_dsi *dsi)
{
	u32 val;

	val = VID_MODE_TYPE_BURST_SYNC_PULSES | ENABLE_LOW_POWER;

	dsi_write(dsi, DSI_VID_MODE_CFG, val);
}

static void dw_mipi_dsi_set_mode(struct dw_mipi_dsi *dsi,
				 enum dw_mipi_dsi_mode mode)
{
	if (mode == DW_MIPI_DSI_CMD_MODE) {
		dsi_write(dsi, DSI_PWR_UP, RESET);
		dsi_write(dsi, DSI_MODE_CFG, ENABLE_CMD_MODE);
		dsi_write(dsi, DSI_PWR_UP, POWERUP);
	} else {
		dsi_write(dsi, DSI_PWR_UP, RESET);
		dsi_write(dsi, DSI_MODE_CFG, ENABLE_VIDEO_MODE);
		dw_mipi_dsi_video_mode_config(dsi);
		dsi_write(dsi, DSI_PWR_UP, POWERUP);
	}
}

static void dw_mipi_dsi_disable(struct dw_mipi_dsi *dsi)
{
	dsi_write(dsi, DSI_PWR_UP, RESET);
	dsi_write(dsi, DSI_PHY_RSTZ, PHY_RSTZ);
}

static void dw_mipi_dsi_init(struct dw_mipi_dsi *dsi)
{
	dsi_write(dsi, DSI_PWR_UP, RESET);
	dsi_write(dsi, DSI_PHY_RSTZ, PHY_DISFORCEPLL | PHY_DISABLECLK
		  | PHY_RSTZ | PHY_SHUTDOWNZ);
	dsi_write(dsi, DSI_CLKMGR_CFG, TO_CLK_DIVIDSION(10) |
		  TX_ESC_CLK_DIVIDSION(7));
	dsi_write(dsi, DSI_LPCLK_CTRL, PHY_TXREQUESTCLKHS);
}

static void dw_mipi_dsi_dpi_config(struct dw_mipi_dsi *dsi,
				   struct drm_display_mode *mode)
{
	u32 val = 0, color = 0;

	switch (dsi->format) {
	case MIPI_DSI_FMT_RGB888:
		color = DPI_COLOR_CODING_24BIT;
		break;
	case MIPI_DSI_FMT_RGB666:
		color = DPI_COLOR_CODING_18BIT_2 | EN18_LOOSELY;
		break;
	case MIPI_DSI_FMT_RGB666_PACKED:
		color = DPI_COLOR_CODING_18BIT_1;
		break;
	case MIPI_DSI_FMT_RGB565:
		color = DPI_COLOR_CODING_16BIT_1;
		break;
	}

	if (!(mode->flags & DRM_MODE_FLAG_PVSYNC))
		val |= VSYNC_ACTIVE_LOW;
	if (!(mode->flags & DRM_MODE_FLAG_PHSYNC))
		val |= HSYNC_ACTIVE_LOW;

	dsi_write(dsi, DSI_DPI_VCID, DPI_VID(dsi->channel));
	dsi_write(dsi, DSI_DPI_COLOR_CODING, color);
	dsi_write(dsi, DSI_DPI_CFG_POL, val);
	dsi_write(dsi, DSI_DPI_LP_CMD_TIM, OUTVACT_LPCMD_TIME(4)
		  | INVACT_LPCMD_TIME(4));
}

static void dw_mipi_dsi_packet_handler_config(struct dw_mipi_dsi *dsi)
{
	dsi_write(dsi, DSI_PCKHDL_CFG, EN_CRC_RX | EN_ECC_RX | EN_BTA);
}

static void dw_mipi_dsi_video_packet_config(struct dw_mipi_dsi *dsi,
					    struct drm_display_mode *mode)
{
	dsi_write(dsi, DSI_VID_PKT_SIZE, VID_PKT_SIZE(mode->hdisplay));
}

static void dw_mipi_dsi_command_mode_config(struct dw_mipi_dsi *dsi)
{
	dsi_write(dsi, DSI_TO_CNT_CFG, HSTX_TO_CNT(1000) | LPRX_TO_CNT(1000));
	dsi_write(dsi, DSI_BTA_TO_CNT, 0xd00);
	dsi_write(dsi, DSI_CMD_MODE_CFG, CMD_MODE_ALL_LP);
	dsi_write(dsi, DSI_MODE_CFG, ENABLE_CMD_MODE);
}

/* Get lane byte clock cycles. */
static u32 dw_mipi_dsi_get_hcomponent_lbcc(struct dw_mipi_dsi *dsi,
					   u32 hcomponent)
{
	u32 frac, lbcc;

	lbcc = hcomponent * dsi->lane_mbps * MSEC_PER_SEC / 8;

	frac = lbcc % dsi->mode->clock;
	lbcc = lbcc / dsi->mode->clock;
	if (frac)
		lbcc++;

	return lbcc;
}

static void dw_mipi_dsi_line_timer_config(struct dw_mipi_dsi *dsi)
{
	u32 htotal, hsa, hbp, lbcc;
	struct drm_display_mode *mode = dsi->mode;

	htotal = mode->htotal;
	hsa = mode->hsync_end - mode->hsync_start;
	hbp = mode->htotal - mode->hsync_end;

	lbcc = dw_mipi_dsi_get_hcomponent_lbcc(dsi, htotal);
	dsi_write(dsi, DSI_VID_HLINE_TIME, lbcc);

	lbcc = dw_mipi_dsi_get_hcomponent_lbcc(dsi, hsa);
	dsi_write(dsi, DSI_VID_HSA_TIME, lbcc);

	lbcc = dw_mipi_dsi_get_hcomponent_lbcc(dsi, hbp);
	dsi_write(dsi, DSI_VID_HBP_TIME, lbcc);
}

static void dw_mipi_dsi_vertical_timing_config(struct dw_mipi_dsi *dsi)
{
	u32 vactive, vsa, vfp, vbp;
	struct drm_display_mode *mode = dsi->mode;

	vactive = mode->vdisplay;
	vsa = mode->vsync_end - mode->vsync_start;
	vfp = mode->vsync_start - mode->vdisplay;
	vbp = mode->vtotal - mode->vsync_end;

	dsi_write(dsi, DSI_VID_VACTIVE_LINES, vactive);
	dsi_write(dsi, DSI_VID_VSA_LINES, vsa);
	dsi_write(dsi, DSI_VID_VFP_LINES, vfp);
	dsi_write(dsi, DSI_VID_VBP_LINES, vbp);
}

static void dw_mipi_dsi_dphy_timing_config(struct dw_mipi_dsi *dsi)
{
	dsi_write(dsi, DSI_PHY_TMR_CFG, PHY_HS2LP_TIME(0x14)
		  | PHY_LP2HS_TIME(0x10) | MAX_RD_TIME(10000));

	dsi_write(dsi, DSI_PHY_TMR_LPCLK_CFG, PHY_CLKHS2LP_TIME(0x40)
		  | PHY_CLKLP2HS_TIME(0x40));
}

static void dw_mipi_dsi_dphy_interface_config(struct dw_mipi_dsi *dsi)
{
	dsi_write(dsi, DSI_PHY_IF_CFG, PHY_STOP_WAIT_TIME(0x20) |
		  N_LANES(dsi->lanes));
}

static void dw_mipi_dsi_clear_err(struct dw_mipi_dsi *dsi)
{
	dsi_read(dsi, DSI_INT_ST0);
	dsi_read(dsi, DSI_INT_ST1);
	dsi_write(dsi, DSI_INT_MSK0, 0);
	dsi_write(dsi, DSI_INT_MSK1, 0);
}

const struct dw_mipi_dsi_plat_data rk3288_mipi_dsi_drv_data = {
	.dsi0_en_bit = RK3288_DSI0_SEL_VOP_LIT,
	.dsi1_en_bit = RK3288_DSI1_SEL_VOP_LIT,
	.grf_switch_reg = RK3288_GRF_SOC_CON6,
	.max_data_lanes = 4,
	.max_bit_rate_per_lane = 1500000000,
	.has_vop_sel = true,
};

const struct dw_mipi_dsi_plat_data rk3368_mipi_dsi_drv_data = {
	.max_bit_rate_per_lane = 1000000000,
	.max_data_lanes = 4,
};

const struct dw_mipi_dsi_plat_data rk3399_mipi_dsi_drv_data = {
	.dsi0_en_bit = RK3399_DSI0_SEL_VOP_LIT,
	.dsi1_en_bit = RK3399_DSI1_SEL_VOP_LIT,
	.grf_switch_reg = RK3399_GRF_SOC_CON19,
	.grf_dsi0_mode = RK3399_GRF_DSI_MODE,
	.grf_dsi0_mode_reg = RK3399_GRF_SOC_CON22,
	.max_data_lanes = 4,
	.max_bit_rate_per_lane = 1500000000,
	.has_vop_sel = true,
};

static int dw_mipi_dsi_clk_enable(struct dw_mipi_dsi *dsi)
{
	return 0;
}

static int rockchip_dw_mipi_dsi_init(struct display_state *state)
{
	struct connector_state *conn_state = &state->conn_state;
	const struct rockchip_connector *connector = conn_state->connector;
	const struct dw_mipi_dsi_plat_data *pdata = connector->data;
	int mipi_node = conn_state->node;
	struct dw_mipi_dsi *dsi;
	int panel;

	dsi = malloc(sizeof(*dsi));
	if (!dsi)
		return -ENOMEM;

	dsi->base = (void *)fdtdec_get_addr_size_auto_noparent(state->blob,
						mipi_node, "reg", 0, NULL);
	dsi->pdata = pdata;
	conn_state->private = dsi;
	conn_state->type = DRM_MODE_CONNECTOR_DSI;
	conn_state->output_mode = ROCKCHIP_OUT_MODE_P888;

	panel = fdt_subnode_offset(state->blob, mipi_node, "panel");
	if (panel < 0) {
		printf("failed to find panel node\n");
		return -1;
	}

#define FDT_GET_INT(val, name) \
	val = fdtdec_get_int(state->blob, panel, name, -1); \
	if (val < 0) { \
		printf("Can't get %s\n", name); \
		return -1; \
	}

	FDT_GET_INT(dsi->lanes, "dsi,lanes");
	FDT_GET_INT(dsi->format, "dsi,format");
	FDT_GET_INT(dsi->mode_flags, "dsi,mode_flags");
	FDT_GET_INT(dsi->channel, "reg");

	return 0;
}

static void rockchip_dw_mipi_dsi_deinit(struct display_state *state)
{
	struct connector_state *conn_state = &state->conn_state;
	struct dw_mipi_dsi *dsi = conn_state->private;

	free(dsi);
}

static int rockchip_dw_mipi_dsi_prepare(struct display_state *state)
{
	struct connector_state *conn_state = &state->conn_state;
	struct dw_mipi_dsi *dsi = conn_state->private;
	int ret;

	dsi->mode = &conn_state->mode;

	dw_mipi_dsi_clk_enable(dsi);

	rockchip_phy_power_on(state);

	if (conn_state->phy) {
		dsi->lane_mbps = rockchip_phy_get_data(state);
	} else {
		ret = dw_mipi_dsi_get_lane_bps(dsi);
		if (ret < 0)
			return ret;
	}

	dw_mipi_dsi_init(dsi);
	dw_mipi_dsi_dpi_config(dsi, dsi->mode);
	dw_mipi_dsi_packet_handler_config(dsi);
	dw_mipi_dsi_video_mode_config(dsi);
	dw_mipi_dsi_video_packet_config(dsi, dsi->mode);
	dw_mipi_dsi_command_mode_config(dsi);
	dw_mipi_dsi_line_timer_config(dsi);
	dw_mipi_dsi_vertical_timing_config(dsi);
	dw_mipi_dsi_dphy_timing_config(dsi);
	dw_mipi_dsi_dphy_interface_config(dsi);
	dw_mipi_dsi_clear_err(dsi);

	return 0;
}

static int rockchip_dw_mipi_dsi_enable(struct display_state *state)
{
	struct connector_state *conn_state = &state->conn_state;
	struct crtc_state *crtc_state = &state->crtc_state;
	const struct rockchip_connector *connector = conn_state->connector;
	const struct dw_mipi_dsi_plat_data *pdata = connector->data;
	struct dw_mipi_dsi *dsi = conn_state->private;
	u32 val;

	if (pdata->grf_dsi0_mode_reg)
		writel(pdata->grf_dsi0_mode,
		       RKIO_GRF_PHYS + pdata->grf_dsi0_mode_reg);

	if (!conn_state->phy)
		dw_mipi_dsi_phy_init(dsi);
	dw_mipi_dsi_wait_for_two_frames(dsi);

	dw_mipi_dsi_set_mode(dsi, DW_MIPI_DSI_VID_MODE);

	if (!pdata->has_vop_sel)
		return 0;

	if (pdata->grf_switch_reg) {
		if (crtc_state->crtc_id)
			val = pdata->dsi0_en_bit | (pdata->dsi0_en_bit << 16);
		else
			val = pdata->dsi0_en_bit << 16;

		writel(val, RKIO_GRF_PHYS + pdata->grf_switch_reg);
	}
	debug("vop %s output to dsi0\n", (crtc_state->crtc_id) ? "LIT" : "BIG");

	return 0;
}

static int rockchip_dw_mipi_dsi_disable(struct display_state *state)
{
	struct connector_state *conn_state = &state->conn_state;
	struct dw_mipi_dsi *dsi = conn_state->private;

	dw_mipi_dsi_set_mode(dsi, DW_MIPI_DSI_CMD_MODE);
	dw_mipi_dsi_set_mode(dsi, DW_MIPI_DSI_VID_MODE);

	/*
	 * This is necessary to make sure the peripheral will be driven
	 * normally when the display is enabled again later.
	 */
	mdelay(120);

	dw_mipi_dsi_set_mode(dsi, DW_MIPI_DSI_CMD_MODE);
	dw_mipi_dsi_disable(dsi);
	return 0;
}

const struct rockchip_connector_funcs rockchip_dw_mipi_dsi_funcs = {
	.init = rockchip_dw_mipi_dsi_init,
	.deinit = rockchip_dw_mipi_dsi_deinit,
	.prepare = rockchip_dw_mipi_dsi_prepare,
	.enable = rockchip_dw_mipi_dsi_enable,
	.disable = rockchip_dw_mipi_dsi_disable,
	.transfer = rockchip_dw_mipi_dsi_transfer,
};
