/*
 * Copyright (C) 2017 Loongson Technology Corporation Limited
 *
 * Author: Juxin Gao <gaojuxin@loongson.cn>
 * License terms: GNU General Public License (GPL)
 */

#include <linux/acpi.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <linux/interrupt.h>
#include <linux/irq.h>

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/delay.h>

/* counter offest */
#define LOW_BUFFER  0x004
#define FULL_BUFFER 0x008
#define CTRL		0x00c

/* CTRL counter each bit */
#define CTRL_EN		BIT(0)
#define CTRL_OE		BIT(3)
#define CTRL_SINGLE	BIT(4)
#define CTRL_INTE	BIT(5)
#define CTRL_INT	BIT(6)
#define CTRL_RST	BIT(7)
#define CTRL_CAPTE	BIT(8)
#define CTRL_INVERT	BIT(9)
#define CTRL_DZONE	BIT(10)

#define to_ls_pwm_chip(_chip)		container_of(_chip, struct ls_pwm_chip, chip)
#define NS_IN_HZ (1000000000UL)
#define CPU_FRQ_PWM (50000000UL)

#define SPI_MAX_ID 5

struct ls_pwm_chip{
	struct pwm_chip chip;
	void __iomem		*mmio_base;
	/* following registers used for suspend/resume */
	u32	ctrl_reg;
	u32	low_buffer_reg;
	u32	full_buffer_reg;
	u64	clock_frequency;
};

static int ls_pwm_set_polarity(struct pwm_chip *chip,
                               struct pwm_device *pwm,
                               enum pwm_polarity polarity)
{
	struct ls_pwm_chip *ls_pwm = to_ls_pwm_chip(chip);
	u16 val;

	val = readl(ls_pwm->mmio_base + CTRL);
        switch (polarity) {
        case PWM_POLARITY_NORMAL:
                val &= ~CTRL_INVERT;
                break;
        case PWM_POLARITY_INVERSED:
                val |= CTRL_INVERT;
                break;
        default:
                break;
        }
	writel(val, ls_pwm->mmio_base + CTRL);
	return 0;
}

static void ls_pwm_disable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct ls_pwm_chip *ls_pwm = to_ls_pwm_chip(chip);
	u32 ret;

	/*
	 * when disbale we let output is 0 duty_cycle state
	 * so normal polarity output high voltage
	 * and inversed polarity output low voltage
	 * just because led pwm and pwm backlight
	 *
	 * about ls pwm, low buffer just need 1 is 0 duty cycle
	 * and if init so that period just 0
	 * we need put a num which bigger than 1 to full buufer
	 * so it just will run as 0 duty cycle
	 */
	writel(1, ls_pwm->mmio_base + LOW_BUFFER);
	if (!(ls_pwm->full_buffer_reg))
		writel(10000, ls_pwm->mmio_base + FULL_BUFFER);

	/*
	 * setup 0 duty cycle
	 * bu ls pwm have low_level full_pulse which can't contorl
	 * low_level from LOW_BUFFER, full_pulse from FULL_BUFFER
	 * and pwm output state depend on low_level and full_pulse
	 * no BUFFER
	 * so we need reset low_level and full_pulse to let 0 duty cycle setup token effect
	 */
	ret = readl(ls_pwm->mmio_base + CTRL);
	ret |= CTRL_RST;
	writel(ret, ls_pwm->mmio_base + CTRL);
	ret &= ~CTRL_RST;
	writel(ret, ls_pwm->mmio_base + CTRL);

	/*
	 * stop count
	 * but dont setup CTRL_OE
	 * it will output low voltage under normal polarity or inversed polarity
	 */
	ret = readl(ls_pwm->mmio_base + CTRL);
	ret &= ~CTRL_EN;
	writel(ret, ls_pwm->mmio_base + CTRL);
}

static int ls_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct ls_pwm_chip *ls_pwm = to_ls_pwm_chip(chip);
	int ret;

	writel(ls_pwm->low_buffer_reg, ls_pwm->mmio_base + LOW_BUFFER);
	writel(ls_pwm->full_buffer_reg, ls_pwm->mmio_base + FULL_BUFFER);

	ret = readl(ls_pwm->mmio_base + CTRL);
	ret |= CTRL_EN;
	writel(ret, ls_pwm->mmio_base + CTRL);
	return 0;
}

static int ls_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
			int duty_ns, int period_ns)
{
	struct ls_pwm_chip *ls_pwm = to_ls_pwm_chip(chip);
	unsigned int period, duty;
	unsigned long long val0,val1;

	if (period_ns > NS_IN_HZ || duty_ns > NS_IN_HZ)
		return -ERANGE;

	val0 = ls_pwm->clock_frequency * period_ns;
	do_div(val0, NSEC_PER_SEC);
	if (val0 < 1)
		val0 = 1;
	period = val0;

	val1 = ls_pwm->clock_frequency * duty_ns;
	do_div(val1, NSEC_PER_SEC);
	if (val1 < 1)
		val1 = 1;
	duty = val1;

	writel(duty,ls_pwm->mmio_base + LOW_BUFFER);
	writel(period,ls_pwm->mmio_base + FULL_BUFFER);
	ls_pwm->low_buffer_reg = duty;
	ls_pwm->full_buffer_reg = period;

	/*
	 * delay 1 of period_ns to ensure pwm setup enable
	 * in pwm-led, if not delay, when set 0 duty_cycle and set pwm disable right now
	 * it maybe setup error, such as max-brightness is 1024
	 * cur brightness is 100, and set brightness 1024, led will off, this is not right.
	 * just because when setup 1024, will setup 0 duty_cycle and disable pwm
	 * but loongson pwm set CTRL_EN 0 will keep last output
	 * so if not delay, last output more will be high(led off), because 100 brightness more time is high
	 */
	ndelay(period_ns);

	return 0;
}


void ls_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
		struct pwm_state *state)
{
	struct ls_pwm_chip *ls_pwm = to_ls_pwm_chip(chip);
	unsigned long long val0, val1;
	u32 ctrl_reg;

	val0 = readl(ls_pwm->mmio_base + FULL_BUFFER) * NSEC_PER_SEC;
	do_div(val0, ls_pwm->clock_frequency);
	state->period = val0;

	val1 = readl(ls_pwm->mmio_base + LOW_BUFFER) * NSEC_PER_SEC;
	do_div(val1, ls_pwm->clock_frequency);
	state->duty_cycle = val1;

	ctrl_reg = readl(ls_pwm->mmio_base + CTRL);
	state->polarity = (ctrl_reg & CTRL_INVERT) ? PWM_POLARITY_INVERSED
		: PWM_POLARITY_NORMAL;
	state->enabled = (ctrl_reg & CTRL_EN) ? true : false;

	ls_pwm->low_buffer_reg = readl(ls_pwm->mmio_base + LOW_BUFFER);
	ls_pwm->full_buffer_reg = readl(ls_pwm->mmio_base + FULL_BUFFER);
}

static const struct pwm_ops ls_pwm_ops = {
	.config = ls_pwm_config,
	.set_polarity = ls_pwm_set_polarity,
	.enable = ls_pwm_enable,
	.disable = ls_pwm_disable,
	.get_state = ls_pwm_get_state,
	.owner = THIS_MODULE,
};

static void pwm_clock_frequency_get(struct platform_device *pdev, struct device_node *np, struct ls_pwm_chip *pwm)
{
	int err;
	u32 clk;
	struct clk *clk_unit;

	if (!(of_property_read_u32(np, "clock-frequency", &clk)))
		pwm->clock_frequency = clk;
	else {
		clk_unit = devm_clk_get(&pdev->dev, "pwm-clk");

		if(IS_ERR(clk_unit))
			goto err_def;

		err = clk_prepare_enable(clk_unit);
		if (err)
			goto err_def;

		clk = clk_get_rate(clk_unit);
		if (!clk) {
			clk_disable_unprepare(clk_unit);
			goto err_def;
		}

		pwm->clock_frequency = clk;
		return;
err_def:
		pwm->clock_frequency = CPU_FRQ_PWM;
		clk_unit = NULL;
	}
}

static int ls_pwm_probe(struct platform_device *pdev)
{
	struct ls_pwm_chip *pwm;
	struct resource *mem;
	int err;
	struct device_node *np = pdev->dev.of_node;
	u32 polarity;

	if (pdev->id > SPI_MAX_ID)
		dev_err(&pdev->dev, "Currently only six PWM controller is implemented,namely, 0,1,2,3,4,5.\n");
	pwm = devm_kzalloc(&pdev->dev, sizeof(*pwm), GFP_KERNEL);
	if(!pwm){
		dev_err(&pdev->dev, "failed to allocate memory\n");
		return -ENOMEM;
	}

	pwm->chip.dev = &pdev->dev;
	pwm->chip.ops = &ls_pwm_ops;
	pwm->chip.base = -1;
	pwm->chip.npwm = 1;

	pwm_clock_frequency_get(pdev, np, pwm);
	dev_info(&pdev->dev, "pwm->clock_frequency=%llu", pwm->clock_frequency);

	mem = platform_get_resource(pdev,IORESOURCE_MEM, 0);
	if(!mem){
		dev_err(&pdev->dev, "no mem resource?\n");
		return -ENODEV;
	}
	pwm->mmio_base = devm_ioremap_resource(&pdev->dev, mem);
	if(!pwm->mmio_base){
		dev_err(&pdev->dev, "mmio_base is null\n");
		return -ENOMEM;
	}
	err = pwmchip_add(&pwm->chip);
	if(err < 0){
		dev_err(&pdev->dev, "pwmchip_add() failed: %d\n",err);
		return err;
	}

	platform_set_drvdata(pdev, pwm);
	dev_dbg(&pdev->dev, "pwm probe successful\n");

	if (!(of_property_read_u32(np, "polarity", &polarity)))
		ls_pwm_set_polarity(&pwm->chip, pwm->chip.pwms, polarity);

	//disable pwm (in /sys/class enable is 0)
	ls_pwm_disable(&pwm->chip, pwm->chip.pwms);

	return 0;
}

static int ls_pwm_remove(struct platform_device *pdev)
{
	struct ls_pwm_chip *pwm = platform_get_drvdata(pdev);
	int err;
	if (!pwm)
		return -ENODEV;
	err = pwmchip_remove(&pwm->chip);
	if(err < 0)
		return err;

	return 0;
}

#ifdef CONFIG_OF
static struct of_device_id ls_pwm_id_table[] = {
	{.compatible = "loongson,ls7a-pwm"},
	{.compatible = "loongson,ls-pwm"},
	{.compatible = "loongson,ls2k-pwm"},
	{},
};
MODULE_DEVICE_TABLE(of, ls_pwm_id_table);
#endif

#ifdef CONFIG_PM_SLEEP
static int ls_pwm_suspend(struct device *dev)
{
	struct ls_pwm_chip *ls_pwm = dev_get_drvdata(dev);

	ls_pwm->ctrl_reg = readl(ls_pwm->mmio_base + CTRL);
	ls_pwm->low_buffer_reg = readl(ls_pwm->mmio_base + LOW_BUFFER);
	ls_pwm->full_buffer_reg = readl(ls_pwm->mmio_base + FULL_BUFFER);

	return 0;
}

static int ls_pwm_resume(struct device *dev)
{
	struct ls_pwm_chip *ls_pwm = dev_get_drvdata(dev);

	writel(ls_pwm->ctrl_reg, ls_pwm->mmio_base + CTRL);
	writel(ls_pwm->low_buffer_reg, ls_pwm->mmio_base + LOW_BUFFER);
	writel(ls_pwm->full_buffer_reg, ls_pwm->mmio_base + FULL_BUFFER);

	return 0;
}

#endif

static SIMPLE_DEV_PM_OPS(ls_pwm_pm_ops, ls_pwm_suspend, ls_pwm_resume);

static const struct acpi_device_id loongson_pwm_acpi_match[] = {
	{"LOON0006"},
	{}
};
MODULE_DEVICE_TABLE(acpi, loongson_pwm_acpi_match);

static struct platform_driver ls_pwm_driver = {
	.driver = {
		.name = "ls-pwm",
		.owner = THIS_MODULE,
		.bus = &platform_bus_type,
		.pm = &ls_pwm_pm_ops,
#ifdef CONFIG_OF
		.of_match_table = of_match_ptr(ls_pwm_id_table),
#endif
		.acpi_match_table = ACPI_PTR(loongson_pwm_acpi_match),
	},
	.probe = ls_pwm_probe,
	.remove = ls_pwm_remove,
};
module_platform_driver(ls_pwm_driver);

MODULE_AUTHOR("Juxin Gao <gaojuxin@loongson.com>");
MODULE_DESCRIPTION("Loongson Pwm Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:ls-pwm");
