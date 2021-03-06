/*
 * Copyright (C) 2015 Freescale Semiconductor, Inc.
 *
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regulator/consumer.h>
#include <linux/irqchip/arm-gic.h>
#include "common.h"
#include "hardware.h"

#define IMR_NUM			4
#define GPC_LPCR_A7_BSC		0x0
#define GPC_LPCR_A7_AD		0x4
#define GPC_LPCR_M4		0x8
#define GPC_SLPCR		0x14
#define GPC_MLPCR		0x20
#define GPC_PGC_ACK_SEL_A7	0x24
#define GPC_MISC		0x2c
#define GPC_IMR1_CORE0		0x30
#define GPC_IMR1_CORE1		0x40
#define GPC_SLOT0_CFG		0xb0
#define GPC_PGC_CPU_MAPPING	0xec
#define GPC_CPU_PGC_SW_PUP_REQ	0xf0
#define GPC_PU_PGC_SW_PUP_REQ	0xf8
#define GPC_CPU_PGC_SW_PDN_REQ	0xfc
#define GPC_PU_PGC_SW_PDN_REQ	0x104
#define GPC_GTOR		0x124
#define GPC_PGC_C0		0x800
#define GPC_PGC_SCU_TIMING	0x890
#define GPC_PGC_C1		0x840
#define GPC_PGC_SCU		0x880
#define GPC_PGC_FM		0xa00
#define GPC_PGC_MIPI_PHY	0xc00
#define GPC_PGC_PCIE_PHY	0xc40
#define GPC_PGC_USB_OTG1_PHY	0xc80
#define GPC_PGC_USB_OTG2_PHY	0xcc0
#define GPC_PGC_USB_HSIC_PHY	0xd00

#define BM_LPCR_A7_BSC_IRQ_SRC_A7_WAKEUP	0x70000000
#define BM_LPCR_A7_BSC_CPU_CLK_ON_LPM		0x4000
#define BM_LPCR_A7_BSC_LPM1			0xc
#define BM_LPCR_A7_BSC_LPM0			0x3
#define BP_LPCR_A7_BSC_LPM1			2
#define BP_LPCR_A7_BSC_LPM0			0
#define BM_LPCR_M4_MASK_DSM_TRIGGER		0x80000000
#define BM_SLPCR_EN_DSM				0x80000000
#define BM_SLPCR_RBC_EN				0x40000000
#define BM_SLPCR_VSTBY				0x4
#define BM_SLPCR_SBYOS				0x2
#define BM_SLPCR_BYPASS_PMIC_READY		0x1
#define BM_SLPCR_EN_A7_FASTWUP_WAIT_MODE	0x10000
#define BM_LPCR_A7_AD_L2PGE			0x10000
#define BM_LPCR_A7_AD_EN_C1_PUP			0x800
#define BM_LPCR_A7_AD_EN_C1_IRQ_PUP		0x400
#define BM_LPCR_A7_AD_EN_C0_PUP			0x200
#define BM_LPCR_A7_AD_EN_C0_IRQ_PUP		0x100
#define BM_LPCR_A7_AD_EN_PLAT_PDN		0x10
#define BM_LPCR_A7_AD_EN_C1_PDN			0x8
#define BM_LPCR_A7_AD_EN_C1_WFI_PDN		0x4
#define BM_LPCR_A7_AD_EN_C0_PDN			0x2
#define BM_LPCR_A7_AD_EN_C0_WFI_PDN		0x1

#define BM_CPU_PGC_SW_PDN_PUP_REQ_CORE1_A7	0x2

#define BM_GPC_PGC_ACK_SEL_A7_DUMMY_PUP_ACK	0x80000000
#define BM_GPC_PGC_ACK_SEL_A7_DUMMY_PDN_ACK	0x8000
#define BM_GPC_MLPCR_MEMLP_CTL_DIS		0x1

#define BP_LPCR_A7_BSC_IRQ_SRC			28

#define MAX_SLOT_NUMBER				10
#define A7_LPM_WAIT				0x5
#define A7_LPM_STOP				0xa

enum imx_gpc_slot {
	CORE0_A7,
	CORE1_A7,
	SCU_A7,
	FAST_MEGA_MIX,
	MIPI_PHY,
	PCIE_PHY,
	USB_OTG1_PHY,
	USB_OTG2_PHY,
	USB_HSIC_PHY,
	CORE0_M4,
};

static void __iomem *gpc_base;
static u32 gpcv2_wake_irqs[IMR_NUM];
static u32 gpcv2_saved_imrs[IMR_NUM];
static u32 gpcv2_mf_irqs[IMR_NUM];
static u32 gpcv2_mf_request_on[IMR_NUM];
static DEFINE_SPINLOCK(gpcv2_lock);
static struct notifier_block nb_pcie, nb_mipi;

void imx_gpcv2_set_slot_ack(u32 index, enum imx_gpc_slot m_core,
				bool mode, bool ack)
{
	u32 val;

	if (index >= MAX_SLOT_NUMBER)
		pr_err("Invalid slot index!\n");
	/* set slot */
	writel_relaxed((mode + 1) << (m_core * 2), gpc_base +
		GPC_SLOT0_CFG + index * 4);

	if (ack) {
		/* set ack */
		val = readl_relaxed(gpc_base + GPC_PGC_ACK_SEL_A7);
		/* clear dummy ack */
		val &= ~(1 << (15 + (mode ? 16 : 0)));
		val |= 1 << (m_core + (mode ? 16 : 0));
		writel_relaxed(val, gpc_base + GPC_PGC_ACK_SEL_A7);
	}
}

void imx_gpcv2_irq_unmask(struct irq_data *d)
{
	void __iomem *reg;
	u32 val;

	/* Sanity check for SPI irq */
	if (d->irq < 32)
		return;
	reg = gpc_base + GPC_IMR1_CORE0 + (d->irq / 32 - 1) * 4;
	val = readl_relaxed(reg);
	val &= ~(1 << d->irq % 32);
	writel_relaxed(val, reg);
}

void imx_gpcv2_irq_mask(struct irq_data *d)
{
	void __iomem *reg;
	u32 val;

	/* Sanity check for SPI irq */
	if (d->irq < 32)
		return;
	reg = gpc_base + GPC_IMR1_CORE0 + (d->irq / 32 - 1) * 4;
	val = readl_relaxed(reg);
	val |= 1 << (d->irq % 32);
	writel_relaxed(val, reg);
}

void imx_gpcv2_set_lpm_mode(enum mxc_cpu_pwr_mode mode)
{
	unsigned long flags;
	u32 val1, val2;
	struct irq_desc *iomuxc_irq_desc;

	spin_lock_irqsave(&gpcv2_lock, flags);

	val1 = readl_relaxed(gpc_base + GPC_LPCR_A7_BSC);
	val2 = readl_relaxed(gpc_base + GPC_SLPCR);

	/* all cores' LPM settings must be same */
	val1 &= ~(BM_LPCR_A7_BSC_LPM0 | BM_LPCR_A7_BSC_LPM1);

	val1 |= BM_LPCR_A7_BSC_CPU_CLK_ON_LPM;

	val2 &= ~(BM_SLPCR_EN_DSM | BM_SLPCR_VSTBY | BM_SLPCR_RBC_EN |
		BM_SLPCR_SBYOS | BM_SLPCR_BYPASS_PMIC_READY);
	/*
	 * GPC: When improper low-power sequence is used,
	 * the SoC enters low power mode before the ARM core executes WFI.
	 *
	 * Software workaround:
	 * 1) Software should trigger IRQ #32 (IOMUX) to be always pending
	 *    by setting IOMUX_GPR1_IRQ.
	 * 2) Software should then unmask IRQ #32 in GPC before setting GPC
	 *    Low-Power mode.
	 * 3) Software should mask IRQ #32 right after GPC Low-Power mode
	 *    is set.
	 */
	iomuxc_irq_desc = irq_to_desc(32);

	switch (mode) {
	case WAIT_CLOCKED:
		imx_gpcv2_irq_unmask(&iomuxc_irq_desc->irq_data);
		break;
	case WAIT_UNCLOCKED:
		val1 |= A7_LPM_WAIT << BP_LPCR_A7_BSC_LPM0;
		val1 &= ~BM_LPCR_A7_BSC_CPU_CLK_ON_LPM;
		imx_gpcv2_irq_mask(&iomuxc_irq_desc->irq_data);
		break;
	case STOP_POWER_ON:
		val1 |= A7_LPM_STOP << BP_LPCR_A7_BSC_LPM0;
		val1 &= ~BM_LPCR_A7_BSC_CPU_CLK_ON_LPM;
		val2 |= BM_SLPCR_EN_DSM;
		val2 |= BM_SLPCR_RBC_EN;
		val2 |= BM_SLPCR_BYPASS_PMIC_READY;
		imx_gpcv2_irq_mask(&iomuxc_irq_desc->irq_data);
		break;
	case STOP_POWER_OFF:
		val1 |= A7_LPM_STOP << BP_LPCR_A7_BSC_LPM0;
		val1 &= ~BM_LPCR_A7_BSC_CPU_CLK_ON_LPM;
		val2 |= BM_SLPCR_EN_DSM;
		val2 |= BM_SLPCR_RBC_EN;
		val2 |= BM_SLPCR_SBYOS;
		val2 |= BM_SLPCR_VSTBY;
		val2 |= BM_SLPCR_BYPASS_PMIC_READY;
		imx_gpcv2_irq_mask(&iomuxc_irq_desc->irq_data);
		break;
	default:
		return;
	}
	writel_relaxed(val1, gpc_base + GPC_LPCR_A7_BSC);
	writel_relaxed(val2, gpc_base + GPC_SLPCR);

	spin_unlock_irqrestore(&gpcv2_lock, flags);
}

void imx_gpcv2_set_plat_power_gate_by_lpm(bool pdn)
{
	u32 val = readl_relaxed(gpc_base + GPC_LPCR_A7_AD);

	val &= ~(BM_LPCR_A7_AD_EN_PLAT_PDN | BM_LPCR_A7_AD_L2PGE);
	if (pdn)
		val |= BM_LPCR_A7_AD_EN_PLAT_PDN | BM_LPCR_A7_AD_L2PGE;

	writel_relaxed(val, gpc_base + GPC_LPCR_A7_AD);
}

void imx_gpcv2_set_m_core_pgc(bool enable, u32 offset)
{
	writel_relaxed(enable, gpc_base + offset);
}

void imx_gpcv2_set_core1_pdn_pup_by_software(bool pdn)
{
	u32 val = readl_relaxed(gpc_base + (pdn ?
		GPC_CPU_PGC_SW_PDN_REQ : GPC_CPU_PGC_SW_PUP_REQ));

	imx_gpcv2_set_m_core_pgc(true, GPC_PGC_C1);
	val |= BM_CPU_PGC_SW_PDN_PUP_REQ_CORE1_A7;
	writel_relaxed(val, gpc_base + (pdn ?
		GPC_CPU_PGC_SW_PDN_REQ : GPC_CPU_PGC_SW_PUP_REQ));

	while ((readl_relaxed(gpc_base + (pdn ?
		GPC_CPU_PGC_SW_PDN_REQ : GPC_CPU_PGC_SW_PUP_REQ)) &
		BM_CPU_PGC_SW_PDN_PUP_REQ_CORE1_A7) != 0)
		;
	imx_gpcv2_set_m_core_pgc(false, GPC_PGC_C1);
}

void imx_gpcv2_set_cpu_power_gate_by_wfi(u32 cpu, bool pdn)
{
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&gpcv2_lock, flags);
	val = readl_relaxed(gpc_base + GPC_LPCR_A7_AD);

	if (cpu == 0) {
		if (pdn) {
			imx_gpcv2_set_m_core_pgc(true, GPC_PGC_C0);
			val |= BM_LPCR_A7_AD_EN_C0_WFI_PDN |
				BM_LPCR_A7_AD_EN_C0_IRQ_PUP;
		} else {
			imx_gpcv2_set_m_core_pgc(false, GPC_PGC_C0);
			val &= ~(BM_LPCR_A7_AD_EN_C0_WFI_PDN |
				BM_LPCR_A7_AD_EN_C0_IRQ_PUP);
		}
	}
	if (cpu == 1) {
		if (pdn) {
			imx_gpcv2_set_m_core_pgc(true, GPC_PGC_C1);
			val |= BM_LPCR_A7_AD_EN_C1_WFI_PDN |
				BM_LPCR_A7_AD_EN_C1_IRQ_PUP;
		} else {
			imx_gpcv2_set_m_core_pgc(false, GPC_PGC_C1);
			val &= ~(BM_LPCR_A7_AD_EN_C1_WFI_PDN |
				BM_LPCR_A7_AD_EN_C1_IRQ_PUP);
		}
	}
	writel_relaxed(val, gpc_base + GPC_LPCR_A7_AD);
	spin_unlock_irqrestore(&gpcv2_lock, flags);
}

void imx_gpcv2_set_cpu_power_gate_by_lpm(u32 cpu, bool pdn)
{
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&gpcv2_lock, flags);

	val = readl_relaxed(gpc_base + GPC_LPCR_A7_AD);
	if (cpu == 0) {
		if (pdn)
			val |= BM_LPCR_A7_AD_EN_C0_PDN |
				BM_LPCR_A7_AD_EN_C0_PUP;
		else
			val &= ~(BM_LPCR_A7_AD_EN_C0_PDN |
				BM_LPCR_A7_AD_EN_C0_PUP);
	}
	if (cpu == 1) {
		if (pdn)
			val |= BM_LPCR_A7_AD_EN_C1_PDN |
				BM_LPCR_A7_AD_EN_C1_PUP;
		else
			val &= ~(BM_LPCR_A7_AD_EN_C1_PDN |
				BM_LPCR_A7_AD_EN_C1_PUP);
	}

	writel_relaxed(val, gpc_base + GPC_LPCR_A7_AD);
	spin_unlock_irqrestore(&gpcv2_lock, flags);
}

void imx_gpcv2_set_cpu_power_gate_in_idle(bool pdn)
{
	unsigned long flags;
	u32 cpu;

	for_each_possible_cpu(cpu)
		imx_gpcv2_set_cpu_power_gate_by_lpm(cpu, pdn);

	spin_lock_irqsave(&gpcv2_lock, flags);

	imx_gpcv2_set_m_core_pgc(pdn, GPC_PGC_C0);
	imx_gpcv2_set_m_core_pgc(pdn, GPC_PGC_C1);
	imx_gpcv2_set_m_core_pgc(pdn, GPC_PGC_SCU);
	imx_gpcv2_set_plat_power_gate_by_lpm(pdn);

	if (pdn) {
		imx_gpcv2_set_slot_ack(0, CORE0_A7, false, false);
		imx_gpcv2_set_slot_ack(1, CORE1_A7, false, false);
		imx_gpcv2_set_slot_ack(2, SCU_A7, false, true);
		imx_gpcv2_set_slot_ack(6, SCU_A7, true, false);
		imx_gpcv2_set_slot_ack(7, CORE0_A7, true, false);
		imx_gpcv2_set_slot_ack(8, CORE1_A7, true, true);
	} else {
		writel_relaxed(0x0, gpc_base + GPC_SLOT0_CFG + 0 * 0x4);
		writel_relaxed(0x0, gpc_base + GPC_SLOT0_CFG + 1 * 0x4);
		writel_relaxed(0x0, gpc_base + GPC_SLOT0_CFG + 2 * 0x4);
		writel_relaxed(0x0, gpc_base + GPC_SLOT0_CFG + 6 * 0x4);
		writel_relaxed(0x0, gpc_base + GPC_SLOT0_CFG + 7 * 0x4);
		writel_relaxed(0x0, gpc_base + GPC_SLOT0_CFG + 8 * 0x4);
		writel_relaxed(BM_GPC_PGC_ACK_SEL_A7_DUMMY_PUP_ACK |
			BM_GPC_PGC_ACK_SEL_A7_DUMMY_PDN_ACK,
			gpc_base + GPC_PGC_ACK_SEL_A7);
	}
	spin_unlock_irqrestore(&gpcv2_lock, flags);
}

void imx_gpcv2_set_mix_phy_gate_by_lpm(u32 pdn_index, u32 pup_index)
{
	/* set power down slot */
	writel_relaxed(1 << (FAST_MEGA_MIX * 2),
		gpc_base + GPC_SLOT0_CFG + pdn_index * 4);

	/* set power up slot */
	writel_relaxed(1 << (FAST_MEGA_MIX * 2 + 1),
		gpc_base + GPC_SLOT0_CFG + pup_index * 4);
}

unsigned int imx_gpcv2_is_mf_mix_off(void)
{
	return readl_relaxed(gpc_base + GPC_PGC_FM);
}

static void imx_gpcv2_mf_mix_off(void)
{
	int i;

	for (i = 0; i < IMR_NUM; i++)
		if (((gpcv2_wake_irqs[i] | gpcv2_mf_request_on[i]) &
						gpcv2_mf_irqs[i]) != 0)
			return;

	pr_info("Turn off Mega/Fast mix in DSM\n");
	imx_gpcv2_set_mix_phy_gate_by_lpm(1, 5);
	imx_gpcv2_set_m_core_pgc(true, GPC_PGC_FM);
}

int imx_gpcv2_mf_power_on(unsigned int irq, unsigned int on)
{
	unsigned int idx = irq / 32 - 1;
	unsigned long flags;
	u32 mask;

	mask = 1 << (irq % 32);
	spin_lock_irqsave(&gpcv2_lock, flags);
	gpcv2_mf_request_on[idx] = on ? gpcv2_mf_request_on[idx] | mask :
				  gpcv2_mf_request_on[idx] & ~mask;
	spin_unlock_irqrestore(&gpcv2_lock, flags);

	return 0;
}

void imx_gpcv2_pre_suspend(bool arm_power_off)
{
	void __iomem *reg_imr1 = gpc_base + GPC_IMR1_CORE0;
	int i;

	if (arm_power_off) {
		imx_gpcv2_set_lpm_mode(STOP_POWER_OFF);
		/* enable core0 power down/up with low power mode */
		imx_gpcv2_set_cpu_power_gate_by_lpm(0, true);
		/* enable plat power down with low power mode */
		imx_gpcv2_set_plat_power_gate_by_lpm(true);

		/*
		 * To avoid confuse, we use slot 0~4 for power down,
		 * slot 5~9 for power up.
		 *
		 * Power down slot sequence:
		 * Slot0 -> CORE0
		 * Slot1 -> Mega/Fast MIX
		 * Slot2 -> SCU
		 *
		 * Power up slot sequence:
		 * Slot5 -> Mega/Fast MIX
		 * Slot6 -> SCU
		 * Slot7 -> CORE0
		 */
		imx_gpcv2_set_slot_ack(0, CORE0_A7, false, false);
		imx_gpcv2_set_slot_ack(2, SCU_A7, false, true);

		imx_gpcv2_mf_mix_off();

		imx_gpcv2_set_slot_ack(6, SCU_A7, true, false);
		imx_gpcv2_set_slot_ack(7, CORE0_A7, true, true);

		/* enable core0, scu */
		imx_gpcv2_set_m_core_pgc(true, GPC_PGC_C0);
		imx_gpcv2_set_m_core_pgc(true, GPC_PGC_SCU);
	} else {
		imx_gpcv2_set_lpm_mode(STOP_POWER_ON);
	}

	for (i = 0; i < IMR_NUM; i++) {
		gpcv2_saved_imrs[i] = readl_relaxed(reg_imr1 + i * 4);
		writel_relaxed(~gpcv2_wake_irqs[i], reg_imr1 + i * 4);
	}
}

void imx_gpcv2_post_resume(void)
{
	void __iomem *reg_imr1 = gpc_base + GPC_IMR1_CORE0;
	int i;

	for (i = 0; i < IMR_NUM; i++)
		writel_relaxed(gpcv2_saved_imrs[i], reg_imr1 + i * 4);

	imx_gpcv2_set_lpm_mode(WAIT_CLOCKED);
	imx_gpcv2_set_cpu_power_gate_by_lpm(0, false);
	imx_gpcv2_set_plat_power_gate_by_lpm(false);

	imx_gpcv2_set_m_core_pgc(false, GPC_PGC_C0);
	imx_gpcv2_set_m_core_pgc(false, GPC_PGC_SCU);
	imx_gpcv2_set_m_core_pgc(false, GPC_PGC_FM);
	for (i = 0; i < MAX_SLOT_NUMBER; i++)
		writel_relaxed(0x0, gpc_base + GPC_SLOT0_CFG + i * 0x4);
	writel_relaxed(BM_GPC_PGC_ACK_SEL_A7_DUMMY_PUP_ACK |
		BM_GPC_PGC_ACK_SEL_A7_DUMMY_PDN_ACK,
		gpc_base + GPC_PGC_ACK_SEL_A7);
}

static int imx_gpcv2_irq_set_wake(struct irq_data *d, unsigned int on)
{
	unsigned int idx = d->irq / 32 - 1;
	unsigned long flags;
	u32 mask;

	/* Sanity check for SPI irq */
	if (d->irq < 32)
		return -EINVAL;

	mask = 1 << d->irq % 32;
	spin_lock_irqsave(&gpcv2_lock, flags);
	gpcv2_wake_irqs[idx] = on ? gpcv2_wake_irqs[idx] | mask :
				  gpcv2_wake_irqs[idx] & ~mask;
	spin_unlock_irqrestore(&gpcv2_lock, flags);

	return 0;
}

void imx_gpcv2_mask_all(void)
{
	void __iomem *reg_imr1 = gpc_base + GPC_IMR1_CORE0;
	int i;

	for (i = 0; i < IMR_NUM; i++) {
		gpcv2_saved_imrs[i] = readl_relaxed(reg_imr1 + i * 4);
		writel_relaxed(~0, reg_imr1 + i * 4);
	}
}

void imx_gpcv2_restore_all(void)
{
	void __iomem *reg_imr1 = gpc_base + GPC_IMR1_CORE0;
	int i;

	for (i = 0; i < IMR_NUM; i++)
		writel_relaxed(gpcv2_saved_imrs[i], reg_imr1 + i * 4);
}

static int imx_pcie_regulator_notify(struct notifier_block *nb,
					unsigned long event,
					void *ignored)
{
	u32 val = 0;

	switch (event) {
	case REGULATOR_EVENT_PRE_ENABLE:
		val = readl_relaxed(gpc_base + GPC_PGC_CPU_MAPPING);
		writel_relaxed(val | BIT(3), gpc_base + GPC_PGC_CPU_MAPPING);

		val = readl_relaxed(gpc_base + GPC_PU_PGC_SW_PUP_REQ);
		writel_relaxed(val | BIT(1), gpc_base + GPC_PU_PGC_SW_PUP_REQ);
		break;
	case REGULATOR_EVENT_PRE_DISABLE:
		val = readl_relaxed(gpc_base + GPC_PU_PGC_SW_PDN_REQ);
		writel_relaxed(val | BIT(1), gpc_base + GPC_PU_PGC_SW_PDN_REQ);

		val = readl_relaxed(gpc_base + GPC_PGC_PCIE_PHY);
		writel_relaxed(val | BIT(0), gpc_base + GPC_PGC_PCIE_PHY);

		val = readl_relaxed(gpc_base + GPC_PGC_CPU_MAPPING);
		writel_relaxed(val & ~BIT(3), gpc_base + GPC_PGC_CPU_MAPPING);
		break;
	default:
		break;
	}

	return NOTIFY_OK;
}

static int imx_mipi_regulator_notify(struct notifier_block *nb,
					unsigned long event,
					void *ignored)
{
	u32 val = 0;

	switch (event) {
	case REGULATOR_EVENT_PRE_ENABLE:
		val = readl_relaxed(gpc_base + GPC_PGC_CPU_MAPPING);
		writel_relaxed(val | BIT(2), gpc_base + GPC_PGC_CPU_MAPPING);

		val = readl_relaxed(gpc_base + GPC_PU_PGC_SW_PUP_REQ);
		writel_relaxed(val | BIT(0), gpc_base + GPC_PU_PGC_SW_PUP_REQ);
		break;
	case REGULATOR_EVENT_PRE_DISABLE:
		val = readl_relaxed(gpc_base + GPC_PU_PGC_SW_PDN_REQ);
		writel_relaxed(val | BIT(0), gpc_base + GPC_PU_PGC_SW_PDN_REQ);

		val = readl_relaxed(gpc_base + GPC_PGC_MIPI_PHY);
		writel_relaxed(val | BIT(0), gpc_base + GPC_PGC_MIPI_PHY);

		val = readl_relaxed(gpc_base + GPC_PGC_CPU_MAPPING);
		writel_relaxed(val & ~BIT(2), gpc_base + GPC_PGC_CPU_MAPPING);
		break;
	default:
		break;
	}

	return NOTIFY_OK;
}

void __init imx_gpcv2_init(void)
{
	struct device_node *np;
	int i, val;

	np = of_find_compatible_node(NULL, NULL, "fsl,imx7d-gpc");
	gpc_base = of_iomap(np, 0);
	WARN_ON(!gpc_base);

	/* Initially mask all interrupts */
	for (i = 0; i < IMR_NUM; i++) {
		writel_relaxed(~0, gpc_base + GPC_IMR1_CORE0 + i * 4);
		writel_relaxed(~0, gpc_base + GPC_IMR1_CORE1 + i * 4);
	}
	/*
	 * Due to hardware design requirement, need to make sure GPR
	 * interrupt(#32) is unmasked during RUN mode to avoid entering
	 * DSM by mistake.
	 */
	writel_relaxed(~0x1, gpc_base + GPC_IMR1_CORE0);

	/* Read supported wakeup source in M/F domain */
	if (cpu_is_imx7d()) {
		of_property_read_u32_index(np, "fsl,mf-mix-wakeup-irq", 0,
			&gpcv2_mf_irqs[0]);
		of_property_read_u32_index(np, "fsl,mf-mix-wakeup-irq", 1,
			&gpcv2_mf_irqs[1]);
		of_property_read_u32_index(np, "fsl,mf-mix-wakeup-irq", 2,
			&gpcv2_mf_irqs[2]);
		of_property_read_u32_index(np, "fsl,mf-mix-wakeup-irq", 3,
			&gpcv2_mf_irqs[3]);
		if (!(gpcv2_mf_irqs[0] | gpcv2_mf_irqs[1] |
			gpcv2_mf_irqs[2] | gpcv2_mf_irqs[3]))
			pr_info("No wakeup source in Mega/Fast domain found!\n");
	}

	/* only external IRQs to wake up LPM and core 0/1 */
	val = readl_relaxed(gpc_base + GPC_LPCR_A7_BSC);
	val |= BM_LPCR_A7_BSC_IRQ_SRC_A7_WAKEUP;
	writel_relaxed(val, gpc_base + GPC_LPCR_A7_BSC);
	/* mask m4 dsm trigger */
	writel_relaxed(readl_relaxed(gpc_base + GPC_LPCR_M4) |
		BM_LPCR_M4_MASK_DSM_TRIGGER, gpc_base + GPC_LPCR_M4);
	/* set mega/fast mix in A7 domain */
	writel_relaxed(0x1, gpc_base + GPC_PGC_CPU_MAPPING);
	/* set SCU timing */
	writel_relaxed((0x59 << 10) | 0x5B | (0x51 << 20),
		gpc_base + GPC_PGC_SCU_TIMING);
	writel_relaxed(BM_GPC_PGC_ACK_SEL_A7_DUMMY_PUP_ACK |
		BM_GPC_PGC_ACK_SEL_A7_DUMMY_PDN_ACK,
		gpc_base + GPC_PGC_ACK_SEL_A7);

	val = readl_relaxed(gpc_base + GPC_SLPCR);
	val &= ~(BM_SLPCR_EN_DSM | BM_SLPCR_VSTBY | BM_SLPCR_RBC_EN |
		BM_SLPCR_SBYOS | BM_SLPCR_BYPASS_PMIC_READY);
	val |= BM_SLPCR_EN_A7_FASTWUP_WAIT_MODE;
	writel_relaxed(val, gpc_base + GPC_SLPCR);

	/* disable memory low power mode */
	val = readl_relaxed(gpc_base + GPC_MLPCR);
	val |= BM_GPC_MLPCR_MEMLP_CTL_DIS;
	writel_relaxed(val, gpc_base + GPC_MLPCR);

	/* Register GPC as the secondary interrupt controller behind GIC */
	gic_arch_extn.irq_mask = imx_gpcv2_irq_mask;
	gic_arch_extn.irq_unmask = imx_gpcv2_irq_unmask;
	gic_arch_extn.irq_set_wake = imx_gpcv2_irq_set_wake;
}

static int imx_gpcv2_probe(struct platform_device *pdev)
{
	int ret;
	struct regulator *pcie_reg, *mipi_reg;

	if (cpu_is_imx7d()) {
		pcie_reg = devm_regulator_get(&pdev->dev, "pcie-phy");
		if (IS_ERR(pcie_reg)) {
			ret = PTR_ERR(pcie_reg);
			dev_info(&pdev->dev, "pcie regulator not ready.\n");
			return ret;
		}
		nb_pcie.notifier_call = &imx_pcie_regulator_notify;

		ret = regulator_register_notifier(pcie_reg, &nb_pcie);
		if (ret) {
			dev_err(&pdev->dev,
				"pcie regulator notifier request failed\n");
			return ret;
		}

		mipi_reg = devm_regulator_get(&pdev->dev, "mipi-phy");
		if (IS_ERR(mipi_reg)) {
			ret = PTR_ERR(mipi_reg);
			dev_info(&pdev->dev, "mipi regulator not ready.\n");
			return ret;
		}
		nb_mipi.notifier_call = &imx_mipi_regulator_notify;

		ret = regulator_register_notifier(mipi_reg, &nb_mipi);
		if (ret) {
			dev_err(&pdev->dev,
				"mipi regulator notifier request failed.\n");
			return ret;
		}
	}
	return 0;
}

static struct of_device_id imx_gpcv2_dt_ids[] = {
	{ .compatible = "fsl,imx7d-gpc" },
	{ }
};

static struct platform_driver imx_gpcv2_driver = {
	.driver = {
		.name = "imx-gpcv2",
		.owner = THIS_MODULE,
		.of_match_table = imx_gpcv2_dt_ids,
	},
	.probe = imx_gpcv2_probe,
};

static int __init imx_pgcv2_init(void)
{
	return platform_driver_register(&imx_gpcv2_driver);
}
subsys_initcall(imx_pgcv2_init);
