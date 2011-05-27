/* drivers/video/msm_fb/mdp.c
 *
 * MSM MDP Interface (used by framebuffer core)
 *
 * Copyright (C) 2007 QUALCOMM Incorporated
 * Copyright (C) 2007 Google Incorporated
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/fb.h>
#include <linux/msm_mdp.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/clk.h>
#include <linux/file.h>
#include <linux/android_pmem.h>
#include <linux/major.h>
#include <linux/slab.h>

#include <mach/msm_iomap.h>
#include <mach/msm_fb.h>
#include <linux/platform_device.h>

#include "mdp_hw.h"
#include "mdp_ppp.h"

struct class *mdp_class;

#define MDP_CMD_DEBUG_ACCESS_BASE (0x10000)

static unsigned int mdp_irq_mask;
struct clk *mdp_clk_to_disable_later = 0;

static int locked_enable_mdp_irq(struct mdp_info *mdp, uint32_t mask)
{
	BUG_ON(!mask);

	/* if the mask bits are already set return an error, this interrupt
	 * is already enabled */
	if (mdp_irq_mask & mask) {
		pr_err("mdp irq already on %x %x\n", mdp_irq_mask, mask);
		return -1;
	}
	/* if the mdp irq is not already enabled enable it */
	if (!mdp_irq_mask) {
		clk_set_rate(mdp->ebi1_clk, 128000000);
		clk_enable(mdp->clk);
		enable_irq(mdp->irq);
	}

	/* clear out any previous irqs for the requested mask*/
	mdp_writel(mdp, mask, MDP_INTR_CLEAR);

	/* update the irq mask to reflect the fact that the interrupt is
	 * enabled */
	mdp_irq_mask |= mask;
	mdp_writel(mdp, mdp_irq_mask, MDP_INTR_ENABLE);
	return 0;
}

static int enable_mdp_irq(struct mdp_info *mdp, uint32_t mask)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&mdp->lock, flags);
	ret = locked_enable_mdp_irq(mdp, mask);
	spin_unlock_irqrestore(&mdp->lock, flags);
	return ret;
}

static int locked_disable_mdp_irq(struct mdp_info *mdp, uint32_t mask)
{
	/* this interrupt is already disabled! */
	if (!(mdp_irq_mask & mask)) {
		printk(KERN_ERR "mdp irq already off %x %x\n",
		       mdp_irq_mask, mask);
		return -1;
	}
	/* update the irq mask to reflect the fact that the interrupt is
	 * disabled */
	mdp_irq_mask &= ~(mask);
	mdp_writel(mdp, mdp_irq_mask, MDP_INTR_ENABLE);

	/* if no one is waiting on the interrupt, disable it */
	if (!mdp_irq_mask) {
		disable_irq_nosync(mdp->irq);
		if (mdp->clk)
			clk_disable(mdp->clk);
		clk_set_rate(mdp->ebi1_clk, 0);
	}
	return 0;
}

int disable_mdp_irq(struct mdp_info *mdp, uint32_t mask)
{
	unsigned long irq_flags;
	int ret;

	spin_lock_irqsave(&mdp->lock, irq_flags);
	ret = locked_disable_mdp_irq(mdp, mask);
	spin_unlock_irqrestore(&mdp->lock, irq_flags);
	return ret;
}

static irqreturn_t mdp_isr(int irq, void *data)
{
	uint32_t status;
	unsigned long irq_flags;
	struct mdp_info *mdp = data;
	int i;

	spin_lock_irqsave(&mdp->lock, irq_flags);

	status = mdp_readl(mdp, MDP_INTR_STATUS);
	mdp_writel(mdp, status, MDP_INTR_CLEAR);

//	pr_info("%s: status=%08x (irq_mask=%08x)\n", __func__, status,
//		mdp_irq_mask);
	status &= mdp_irq_mask;

	for (i = 0; i < MSM_MDP_NUM_INTERFACES; ++i) {
		struct mdp_out_interface *out_if = &mdp->out_if[i];
		if (status & out_if->dma_mask) {
			if (out_if->dma_cb) {
				out_if->dma_cb->func(out_if->dma_cb);
				out_if->dma_cb = NULL;
			}
			wake_up(&out_if->dma_waitqueue);
		}
		if (status & out_if->irq_mask) {
			out_if->irq_cb->func(out_if->irq_cb);
			out_if->irq_cb = NULL;
		}
	}

	mdp_ppp_handle_isr(mdp, status);

	if (status)
		locked_disable_mdp_irq(mdp, status);

	spin_unlock_irqrestore(&mdp->lock, irq_flags);
	return IRQ_HANDLED;
}

static uint32_t mdp_check_mask(struct mdp_info *mdp, uint32_t mask)
{
	uint32_t ret;
	unsigned long irq_flags;

	spin_lock_irqsave(&mdp->lock, irq_flags);
	ret = mdp_irq_mask & mask;
	spin_unlock_irqrestore(&mdp->lock, irq_flags);
	return ret;
}

int mdp_wait(struct mdp_info *mdp, uint32_t mask, wait_queue_head_t *wq)
{
	int ret = 0;
	unsigned long irq_flags;

//	pr_info("%s: WAITING for 0x%x\n", __func__, mask);
	wait_event_timeout(*wq, !mdp_check_mask(mdp, mask),
			   msecs_to_jiffies(1000));

	spin_lock_irqsave(&mdp->lock, irq_flags);
	if (mdp_irq_mask & mask) {
		pr_warning("%s: timeout waiting for mdp to complete 0x%x\n",
			   __func__, mask);
		pr_info("GLBL_CLK_ENA: %08X\n",
			readl(MSM_CLK_CTL_BASE + 0x0000));
		pr_info("GLBL_CLK_STATE: %08X\n",
			readl(MSM_CLK_CTL_BASE + 0x0004));
		pr_info("GLBL_SLEEP_EN: %08X\n",
			readl(MSM_CLK_CTL_BASE + 0x001C));
		pr_info("GLBL_CLK_ENA_2: %08X\n",
			readl(MSM_CLK_CTL_BASE + 0x0220));
		pr_info("GLBL_CLK_STATE_2: %08X\n",
			readl(MSM_CLK_CTL_BASE + 0x0224));
		pr_info("GLBL_CLK_SLEEP_EN_2: %08X\n",
			readl(MSM_CLK_CTL_BASE + 0x023C));
		locked_disable_mdp_irq(mdp, mask);
		ret = -ETIMEDOUT;
	} else {
//		pr_info("%s: SUCCESS waiting for 0x%x\n", __func__, mask);
	}
	spin_unlock_irqrestore(&mdp->lock, irq_flags);

	return ret;
}

static void mdp_dma_wait(struct mdp_device *mdp_dev, int interface)
{
#define MDP_MAX_TIMEOUTS 20
	static int timeout_count;
	struct mdp_info *mdp = container_of(mdp_dev, struct mdp_info, mdp_dev);
	unsigned int mask = 0;
	wait_queue_head_t *wq;

	switch (interface) {
	case MSM_MDDI_PMDH_INTERFACE:
	case MSM_MDDI_EMDH_INTERFACE:
	case MSM_LCDC_INTERFACE:
		BUG_ON(!mdp->out_if[interface].registered);
		mask = mdp->out_if[interface].dma_mask;
		wq = &mdp->out_if[interface].dma_waitqueue;
		break;
	default:
		pr_err("%s: Unknown interface %d\n", __func__, interface);
		BUG();
	}

	if (mdp_wait(mdp, mask, wq) == -ETIMEDOUT)
		timeout_count++;
	else
		timeout_count = 0;

	if (timeout_count > MDP_MAX_TIMEOUTS) {
		printk(KERN_ERR "mdp: dma failed %d times, somethings wrong!\n",
		       MDP_MAX_TIMEOUTS);
		BUG();
	}
}

static void mdp_dma_to_mddi(void *priv, uint32_t addr, uint32_t stride,
			    uint32_t width, uint32_t height, uint32_t x,
			    uint32_t y)
{
	struct mdp_info *mdp = priv;
	uint32_t dma2_cfg;
	uint16_t ld_param = 0; /* 0=PRIM, 1=SECD, 2=EXT */

	dma2_cfg = DMA_PACK_TIGHT |
		DMA_PACK_ALIGN_LSB |
		DMA_PACK_PATTERN_RGB |
		DMA_OUT_SEL_AHB |
		DMA_IBUF_NONCONTIGUOUS;

	dma2_cfg |= mdp->format;

	dma2_cfg |= DMA_OUT_SEL_MDDI;

	dma2_cfg |= DMA_MDDI_DMAOUT_LCD_SEL_PRIMARY;

	dma2_cfg |= DMA_DITHER_EN;

	/* 666 18BPP */
	dma2_cfg |= DMA_DSTC0G_6BITS | DMA_DSTC1B_6BITS | DMA_DSTC2R_6BITS;

#ifdef CONFIG_MSM_MDP22
	/* setup size, address, and stride */
	mdp_writel(mdp, (height << 16) | (width),
		   MDP_CMD_DEBUG_ACCESS_BASE + 0x0184);
	mdp_writel(mdp, addr, MDP_CMD_DEBUG_ACCESS_BASE + 0x0188);
	mdp_writel(mdp, stride, MDP_CMD_DEBUG_ACCESS_BASE + 0x018C);

	/* set y & x offset and MDDI transaction parameters */
	mdp_writel(mdp, (y << 16) | (x), MDP_CMD_DEBUG_ACCESS_BASE + 0x0194);
	mdp_writel(mdp, ld_param, MDP_CMD_DEBUG_ACCESS_BASE + 0x01a0);
	mdp_writel(mdp, (MDDI_VDO_PACKET_DESC << 16) | MDDI_VDO_PACKET_PRIM,
		   MDP_CMD_DEBUG_ACCESS_BASE + 0x01a4);

	mdp_writel(mdp, dma2_cfg, MDP_CMD_DEBUG_ACCESS_BASE + 0x0180);

	/* start DMA2 */
	mdp_writel(mdp, 0, MDP_CMD_DEBUG_ACCESS_BASE + 0x0044);
#else
	/* setup size, address, and stride */
	mdp_writel(mdp, (height << 16) | (width), MDP_DMA_P_SIZE);
	mdp_writel(mdp, addr, MDP_DMA_P_IBUF_ADDR);
	mdp_writel(mdp, stride, MDP_DMA_P_IBUF_Y_STRIDE);

	/* set y & x offset and MDDI transaction parameters */
	mdp_writel(mdp, (y << 16) | (x), MDP_DMA_P_OUT_XY);
	mdp_writel(mdp, ld_param, MDP_MDDI_PARAM_WR_SEL);
	mdp_writel(mdp, (MDDI_VDO_PACKET_DESC << 16) | MDDI_VDO_PACKET_PRIM,
		   MDP_MDDI_PARAM);

	mdp_writel(mdp, dma2_cfg, MDP_DMA_P_CONFIG);
	mdp_writel(mdp, 0, MDP_DMA_P_START);
#endif
}

static void mdp_dma(struct mdp_device *mdp_dev, uint32_t addr, uint32_t stride,
	     uint32_t width, uint32_t height, uint32_t x, uint32_t y,
	     struct msmfb_callback *callback, int interface)
{
	struct mdp_info *mdp = container_of(mdp_dev, struct mdp_info, mdp_dev);
	struct mdp_out_interface *out_if;
	unsigned long flags;

	if (interface < 0 || interface > MSM_MDP_NUM_INTERFACES ||
	    !mdp->out_if[interface].registered) {
		pr_err("%s: Unknown interface: %d\n", __func__, interface);
		BUG();
	}
	out_if = &mdp->out_if[interface];

	spin_lock_irqsave(&mdp->lock, flags);
	if (locked_enable_mdp_irq(mdp, out_if->dma_mask)) {
		pr_err("%s: busy\n", __func__);
		goto done;
	}

	out_if->dma_cb = callback;
	out_if->dma_start(out_if->priv, addr, stride, width, height, x, y);
done:
	spin_unlock_irqrestore(&mdp->lock, flags);
}

void mdp_configure_dma(struct mdp_device *mdp_dev)
{
	struct mdp_info *mdp = container_of(mdp_dev, struct mdp_info, mdp_dev);
	uint32_t dma_cfg;

	if (!mdp->dma_config_dirty)
		return;
	dma_cfg = mdp_readl(mdp, MDP_DMA_P_CONFIG);
	dma_cfg &= ~DMA_IBUF_FORMAT_MASK;
	dma_cfg &= ~DMA_PACK_PATTERN_MASK;
	dma_cfg |= (mdp->format | mdp->pack_pattern);
	mdp_writel(mdp, dma_cfg, MDP_DMA_P_CONFIG);
	mdp->dma_config_dirty = false;

	return;
}

int mdp_check_output_format(struct mdp_device *mdp_dev, int bpp)
{
	switch (bpp) {
	case 16:
	case 24:
	case 32:
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int mdp_set_output_format(struct mdp_device *mdp_dev, int bpp)
{
	struct mdp_info *mdp = container_of(mdp_dev, struct mdp_info, mdp_dev);
	uint32_t format, pack_pattern;

	switch (bpp) {
	case 16:
		format = DMA_IBUF_FORMAT_RGB565;
		pack_pattern = DMA_PACK_PATTERN_RGB;
		break;
#ifdef CONFIG_MSM_MDP22
	case 24:
	case 32:
		format = DMA_IBUF_FORMAT_RGB888_OR_ARGB8888;
		break;
#else
	case 24:
		format = DMA_IBUF_FORMAT_RGB888;
		pack_pattern = DMA_PACK_PATTERN_BGR;
		break;
	case 32:
		format = DMA_IBUF_FORMAT_XRGB8888;
		pack_pattern = DMA_PACK_PATTERN_BGR;
		break;
#endif
	default:
		return -EINVAL;
	}
	if (format != mdp->format || pack_pattern != mdp->pack_pattern) {
		mdp->format = format;
		mdp->pack_pattern = pack_pattern;
		mdp->dma_config_dirty = true;
	}

	return 0;
}

int mdp_blit(struct mdp_device *mdp_dev, struct fb_info *fb,
	     struct mdp_blit_req *req)
{
	struct mdp_info *mdp = container_of(mdp_dev, struct mdp_info, mdp_dev);
	return mdp_ppp_blit(mdp, fb, req);
}

void mdp_set_grp_disp(struct mdp_device *mdp_dev, unsigned disp_id)
{
	struct mdp_info *mdp = container_of(mdp_dev, struct mdp_info, mdp_dev);

	disp_id &= 0xf;
	mdp_writel(mdp, disp_id, MDP_FULL_BYPASS_WORD43);
}

/* used by output interface drivers like mddi and lcdc */
int mdp_out_if_register(struct mdp_device *mdp_dev, int interface,
			void *private_data, uint32_t dma_mask,
			mdp_dma_start_func_t dma_start)
{
	struct mdp_info *mdp = container_of(mdp_dev, struct mdp_info, mdp_dev);
	unsigned long flags;
	int ret = 0;

	if (interface < 0 || interface >= MSM_MDP_NUM_INTERFACES) {
		pr_err("%s: invalid interface (%d)\n", __func__, interface);
		return -EINVAL;
	}

	spin_lock_irqsave(&mdp->lock, flags);

	if (mdp->out_if[interface].registered) {
		pr_err("%s: interface (%d) already registered\n", __func__,
		       interface);
		ret = -EINVAL;
		goto done;
	}

	init_waitqueue_head(&mdp->out_if[interface].dma_waitqueue);
	mdp->out_if[interface].registered = 1;
	mdp->out_if[interface].priv = private_data;
	mdp->out_if[interface].dma_mask = dma_mask;
	mdp->out_if[interface].dma_start = dma_start;
	mdp->out_if[interface].dma_cb = NULL;

done:
	spin_unlock_irqrestore(&mdp->lock, flags);
	return ret;
}

int mdp_out_if_req_irq(struct mdp_device *mdp_dev, int interface,
		       uint32_t mask, struct msmfb_callback *cb)
{
	struct mdp_info *mdp = container_of(mdp_dev, struct mdp_info, mdp_dev);
	unsigned long flags;
	int ret = 0;

	if (interface < 0 || interface >= MSM_MDP_NUM_INTERFACES) {
		pr_err("%s: invalid interface (%d)\n", __func__, interface);
		BUG();
	} else if (!mdp->out_if[interface].registered) {
		pr_err("%s: interface (%d) not registered\n", __func__,
		       interface);
		BUG();
	}

	spin_lock_irqsave(&mdp->lock, flags);

	if (mask) {
		ret = locked_enable_mdp_irq(mdp, mask);
		if (ret) {
			pr_err("%s: busy\n", __func__);
			goto done;
		}
		mdp->out_if[interface].irq_mask = mask;
		mdp->out_if[interface].irq_cb = cb;
	} else {
		locked_disable_mdp_irq(mdp, mask);
		mdp->out_if[interface].irq_mask = 0;
		mdp->out_if[interface].irq_cb = NULL;
	}

done:
	spin_unlock_irqrestore(&mdp->lock, flags);
	return ret;
}

int register_mdp_client(struct class_interface *cint)
{
	if (!mdp_class) {
		pr_err("mdp: no mdp_class when registering mdp client\n");
		return -ENODEV;
	}
	cint->class = mdp_class;
	return class_interface_register(cint);
}

#include "mdp_csc_table.h"

void mdp_hw_init(struct mdp_info *mdp)
{
	int n;

	mdp_irq_mask = 0;

	mdp_writel(mdp, 0, MDP_INTR_ENABLE);

	/* debug interface write access */
	mdp_writel(mdp, 1, 0x60);
	mdp_writel(mdp, 1, MDP_EBI2_PORTMAP_MODE);

#ifndef CONFIG_MSM_MDP22
	/* disable lcdc */
	mdp_writel(mdp, 0, MDP_LCDC_EN);
	/* enable auto clock gating for all blocks by default */
	mdp_writel(mdp, 0xffffffff, MDP_CGC_EN);
	/* reset color/gamma correct parms */
	mdp_writel(mdp, 0, MDP_DMA_P_COLOR_CORRECT_CONFIG);
#endif

	mdp_writel(mdp, 0, MDP_CMD_DEBUG_ACCESS_BASE + 0x01f8);
	mdp_writel(mdp, 0, MDP_CMD_DEBUG_ACCESS_BASE + 0x01fc);
	mdp_writel(mdp, 1, 0x60);

	for (n = 0; n < ARRAY_SIZE(csc_color_lut); n++)
		mdp_writel(mdp, csc_color_lut[n].val, csc_color_lut[n].reg);

	/* clear up unused fg/main registers */
	/* comp.plane 2&3 ystride */
	mdp_writel(mdp, 0, MDP_CMD_DEBUG_ACCESS_BASE + 0x0120);

	/* unpacked pattern */
	mdp_writel(mdp, 0, MDP_CMD_DEBUG_ACCESS_BASE + 0x012c);
	mdp_writel(mdp, 0, MDP_CMD_DEBUG_ACCESS_BASE + 0x0130);
	mdp_writel(mdp, 0, MDP_CMD_DEBUG_ACCESS_BASE + 0x0134);
	mdp_writel(mdp, 0, MDP_CMD_DEBUG_ACCESS_BASE + 0x0158);
	mdp_writel(mdp, 0, MDP_CMD_DEBUG_ACCESS_BASE + 0x015c);
	mdp_writel(mdp, 0, MDP_CMD_DEBUG_ACCESS_BASE + 0x0160);
	mdp_writel(mdp, 0, MDP_CMD_DEBUG_ACCESS_BASE + 0x0170);
	mdp_writel(mdp, 0, MDP_CMD_DEBUG_ACCESS_BASE + 0x0174);
	mdp_writel(mdp, 0, MDP_CMD_DEBUG_ACCESS_BASE + 0x017c);

	/* comp.plane 2 & 3 */
	mdp_writel(mdp, 0, MDP_CMD_DEBUG_ACCESS_BASE + 0x0114);
	mdp_writel(mdp, 0, MDP_CMD_DEBUG_ACCESS_BASE + 0x0118);

	/* clear unused bg registers */
	mdp_writel(mdp, 0, MDP_CMD_DEBUG_ACCESS_BASE + 0x01c8);
	mdp_writel(mdp, 0, MDP_CMD_DEBUG_ACCESS_BASE + 0x01d0);
	mdp_writel(mdp, 0, MDP_CMD_DEBUG_ACCESS_BASE + 0x01dc);
	mdp_writel(mdp, 0, MDP_CMD_DEBUG_ACCESS_BASE + 0x01e0);
	mdp_writel(mdp, 0, MDP_CMD_DEBUG_ACCESS_BASE + 0x01e4);

	for (n = 0; n < ARRAY_SIZE(csc_matrix_config_table); n++)
		mdp_writel(mdp, csc_matrix_config_table[n].val,
			   csc_matrix_config_table[n].reg);

	mdp_ppp_init_scale(mdp);

#ifndef CONFIG_MSM_MDP31
	mdp_writel(mdp, 0x04000400, MDP_COMMAND_CONFIG);
#endif
}

int mdp_probe(struct platform_device *pdev)
{
	struct resource *resource;
	int ret;
	struct mdp_info *mdp;

	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource) {
		pr_err("mdp: can not get mdp mem resource!\n");
		return -ENOMEM;
	}

	mdp = kzalloc(sizeof(struct mdp_info), GFP_KERNEL);
	if (!mdp)
		return -ENOMEM;

	spin_lock_init(&mdp->lock);

	mdp->irq = platform_get_irq(pdev, 0);
	if (mdp->irq < 0) {
		pr_err("mdp: can not get mdp irq\n");
		ret = mdp->irq;
		goto error_get_irq;
	}

	mdp->base = ioremap(resource->start,
			    resource->end - resource->start);
	if (mdp->base == 0) {
		printk(KERN_ERR "msmfb: cannot allocate mdp regs!\n");
		ret = -ENOMEM;
		goto error_ioremap;
	}

	mdp->mdp_dev.dma = mdp_dma;
	mdp->mdp_dev.dma_wait = mdp_dma_wait;
	mdp->mdp_dev.blit = mdp_blit;
	mdp->mdp_dev.set_grp_disp = mdp_set_grp_disp;
	mdp->mdp_dev.set_output_format = mdp_set_output_format;
	mdp->mdp_dev.check_output_format = mdp_check_output_format;
	mdp->mdp_dev.configure_dma = mdp_configure_dma;

	mdp->enable_irq = enable_mdp_irq;
	mdp->disable_irq = disable_mdp_irq;

	ret = mdp_out_if_register(&mdp->mdp_dev, MSM_MDDI_PMDH_INTERFACE, mdp,
				  MDP_DMA_P_DONE, mdp_dma_to_mddi);
	if (ret)
		goto error_mddi_pmdh_register;

	mdp->clk = clk_get(&pdev->dev, "mdp_clk");
	if (IS_ERR(mdp->clk)) {
		printk(KERN_INFO "mdp: failed to get mdp clk");
		ret = PTR_ERR(mdp->clk);
		goto error_get_mdp_clk;
	}

	mdp->ebi1_clk = clk_get(NULL, "ebi1_clk");
	if (IS_ERR(mdp->ebi1_clk)) {
		pr_err("mdp: failed to get ebi1 clk\n");
		ret = PTR_ERR(mdp->ebi1_clk);
		goto error_get_ebi1_clk;
	}


	ret = request_irq(mdp->irq, mdp_isr, IRQF_DISABLED, "msm_mdp", mdp);
	if (ret)
		goto error_request_irq;
	disable_irq(mdp->irq);

	clk_enable(mdp->clk);
	mdp_clk_to_disable_later = mdp->clk;
	mdp_hw_init(mdp);

	/* register mdp device */
	mdp->mdp_dev.dev.parent = &pdev->dev;
	mdp->mdp_dev.dev.class = mdp_class;
	dev_set_name(&mdp->mdp_dev.dev, "mdp%d", pdev->id);

	/* if you can remove the platform device you'd have to implement
	 * this:
	mdp_dev.release = mdp_class; */

	ret = device_register(&mdp->mdp_dev.dev);
	if (ret)
		goto error_device_register;

	pr_info("%s: initialized\n", __func__);

	return 0;

error_device_register:
	free_irq(mdp->irq, mdp);
error_request_irq:
	clk_put(mdp->ebi1_clk);
error_get_ebi1_clk:
	clk_put(mdp->clk);
error_get_mdp_clk:
error_mddi_pmdh_register:
	iounmap(mdp->base);
error_ioremap:
error_get_irq:
	kfree(mdp);
	return ret;
}

static struct platform_driver msm_mdp_driver = {
	.probe = mdp_probe,
	.driver = {.name = "msm_mdp"},
};

static int __init mdp_lateinit(void)
{
	if (mdp_clk_to_disable_later)
		clk_disable(mdp_clk_to_disable_later);
	return 0;
}

static int __init mdp_init(void)
{
	mdp_class = class_create(THIS_MODULE, "msm_mdp");
	if (IS_ERR(mdp_class)) {
		printk(KERN_ERR "Error creating mdp class\n");
		return PTR_ERR(mdp_class);
	}
	return platform_driver_register(&msm_mdp_driver);
}

subsys_initcall(mdp_init);
late_initcall(mdp_lateinit);
