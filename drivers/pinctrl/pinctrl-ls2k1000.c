// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <asm/io.h>
#include "core.h"
#include "pinctrl-utils.h"

/*
 useful groups:
 "sata_led", "gmac1", "dvo0_lio_uart", "dvo1_camera", "can0", "can1",
 "hda_i2s", "i2c0", "i2c1", "uart0", "uart1", "uart2", "nand",
 "pwm0", "pwm1", "pwm2", "pwm3", "sdio"
 */

/*
 useful functions:
 "sata_led", "gpio", "gmac1", "dvo0", "lio",
 "uart1_4", "uart1_2", "uart1_1", "uart2_4", "uart2_2", "uart2_1",
 "dvo1", "camera", "can0", "can1", "hda", "i2s", "i2c0", "i2c1",
 "uart0_4", "uart0_2", "uart0_1",
 "nand", "pwm0", "pwm1", "pwm2", "pwm3", "sdio"
 */

/*
 * usage:
	uart2_4_default:uart2_4 {
		mux {
			groups = "uart2";
			function = "uart2_4";
		};
	};

	----------------------------------

	uart_group1: bus1 {
				compatible = "simple-pm-bus";
				#address-cells = <1>;
				#size-cells = <1>;
				ranges;

				pinctrl-names = "default";
				pinctrl-0 = <&uart1_4_default>;
	groups 能用什么 functions 见于 使用FUNCTION_GROUP_MAP_CREATE宏的代码
	一份dts里面理论上不能出现同样的 groups的使用。像上面的声明可以有相同的组
	但是在 类似 uart_group1: bus1 里面 pinctrl-0这样就不能用到同样的 groups
 */

#define PMX_GROUP(group_name)					\
	{								\
		.name = #group_name,						\
		.pins = group_name ## _pins,					\
		.num_pins = ARRAY_SIZE(group_name ## _pins),			\
		.bit_size = ARRAY_SIZE(group_name ## _reg),						\
		.reg = group_name ## _reg,						\
		.bit = group_name ## _bit,						\
		.bit_width = group_name ## _bit_width,						\
	}

#define FUNCTION_GROUP_MAP_CREATE(function, single_groups)						\
	static const char * const function##_function_groups[] = {			\
		single_groups							\
	}

#define FUNCTION(fn, handle_func, value, mask)							\
	{								\
		.name = #fn,						\
		.groups = fn ## _function_groups,				\
		.num_groups = ARRAY_SIZE(fn ## _function_groups),		\
		.mul_bit_zone_handle_func = handle_func, \
		.bit_value = value,\
		.bit_mask = mask,\
	}

struct ls2k_pinctrl {
	struct device *dev;
	struct pinctrl_dev *pcdev;
	struct pinctrl_desc desc;
	struct device_node *of_node;
	raw_spinlock_t lock;
	void * __iomem reg_base;
};

struct ls2k_pmx_group {
	const char *name;
	const unsigned int *pins;
	unsigned int num_pins;
	unsigned char bit_size;
	unsigned int* reg;
	unsigned int* bit;
	unsigned char* bit_width;
};

struct ls2k_pmx_func {
	const char *name;
	const char * const *groups;
	unsigned int num_groups;

	int (*mul_bit_zone_handle_func)(unsigned long, unsigned int);
	unsigned int* bit_value;
	unsigned char* bit_mask;
};

/*
 * register pin desc but not so important
 * because ls2k1000 pin No is compilcate such as Y22
 * so dont match to all pin
 */
#define LS2K_PIN(pin_num, groups) PINCTRL_PIN(pin_num, groups)
static const struct pinctrl_pin_desc ls2k_pctrl_pins[] = {
	LS2K_PIN(1, "SATA_LEDN"),
	LS2K_PIN(2, "GMAC1_14_pin"),
	LS2K_PIN(17, "uart1_dvo0_pin"),
	LS2K_PIN(3, "uart2_dvo0_pin"),
	LS2K_PIN(4, "dvo1_camera_pin"),
	LS2K_PIN(5, "can0"), LS2K_PIN(6, "can1"),
	LS2K_PIN(7, "hda_i2s"),
	LS2K_PIN(8, "i2c0"), LS2K_PIN(9, "i2c1"),
	LS2K_PIN(10, "uart0"),
	LS2K_PIN(11, "nand"),
	LS2K_PIN(12, "pwm0"), LS2K_PIN(13, "pwm1"), LS2K_PIN(14, "pwm2"), LS2K_PIN(15, "pwm3"),
	LS2K_PIN(16, "sdio"),
};

/*
 * 下面的数组 可以观测得知 一共分为 _pins _reg _bit _bit_width四大组
 * 然后每个数组的前缀都是groups里面的一个 比如 sata_led uart等等
 * 如果需要添加，那么希望按照这个规则添加
 * 因为这些都是为了契合 PMX_GROUP 这个宏
 * 而这个宏的目的 就是为了 填充 ls2k_pmx_group 变量
 * group_name ## _pins 这样是把宏参数和_pins拼接，那么就明白为什么数组的名字要这样命名。
 * _pins 数组是groups包括的引脚的物理编号的集合，呼应ls2k_pctrl_pins，事实上如果为空或者乱填也没关系。
 * _reg 数组是配置寄存器相对于基地址的偏移值的集合 有多少个要设置的位域就要有多少个元素
 * _bit 数组是配置寄存器中要设置的位域的最低位的偏移值 从0开始算，芯片手册的配置寄存器可知。
 * _bit_width 数组是配置寄存器中要设置的位域的位宽，和 _bit 数组就能描述这个位域的位置。
 */
static const unsigned int sata_led_pins[]={1};
static const unsigned int gmac1_pins[]={2};
static const unsigned int dvo0_lio_uart_pins[]={3, 17};
static const unsigned int uart1_pins[]={17};
static const unsigned int uart2_pins[]={3};
static const unsigned int dvo1_camera_pins[]={4};
static const unsigned int can0_pins[]={5};
static const unsigned int can1_pins[]={6};
static const unsigned int hda_i2s_pins[]={7};
static const unsigned int i2c0_pins[]={8};
static const unsigned int i2c1_pins[]={9};
static const unsigned int uart0_pins[]={10};
static const unsigned int nand_pins[]={11};
static const unsigned int pwm0_pins[]={12};
static const unsigned int pwm1_pins[]={13};
static const unsigned int pwm2_pins[]={14};
static const unsigned int pwm3_pins[]={15};
static const unsigned int sdio_pins[]={16};

static unsigned int sata_led_reg[] = {0};
static unsigned int gmac1_reg[] = {0};
static unsigned int dvo0_lio_uart_reg[] = {0x10, 0x8, 0x8, 0, 0x8, 0x8};
static unsigned int uart1_reg[] = {0x10, 0x8, 0x8, 0, 0x8, 0x8};
static unsigned int uart2_reg[] = {0x10, 0x8, 0x8, 0, 0x8, 0x8};
static unsigned int dvo1_camera_reg[] = {0x10, 0x10};
static unsigned int can0_reg[] = {0};
static unsigned int can1_reg[] = {0};
static unsigned int hda_i2s_reg[] = {0, 0};
static unsigned int i2c0_reg[] = {0};
static unsigned int i2c1_reg[] = {0};
static unsigned int uart0_reg[] = {0x8};
static unsigned int nand_reg[] = {0};
static unsigned int pwm0_reg[] = {0};
static unsigned int pwm1_reg[] = {0};
static unsigned int pwm2_reg[] = {0};
static unsigned int pwm3_reg[] = {0};
static unsigned int sdio_reg[] = {0};

static unsigned int sata_led_bit[] = {8};
static unsigned int gmac1_bit[] = {3};
static unsigned int dvo0_lio_uart_bit[] = {1, 12, 13, 7, 4, 8};
static unsigned int uart1_bit[] = {1, 12, 13, 7, 4, 8};
static unsigned int uart2_bit[] = {1, 12, 13, 7, 4, 8};
static unsigned int dvo1_camera_bit[] = {4, 5};
static unsigned int can0_bit[] = {16};
static unsigned int can1_bit[] = {17};
static unsigned int hda_i2s_bit[] = {6, 4};
static unsigned int i2c0_bit[] = {10};
static unsigned int i2c1_bit[] = {11};
static unsigned int uart0_bit[] = {0};
static unsigned int nand_bit[] = {9};
static unsigned int pwm0_bit[] = {12};
static unsigned int pwm1_bit[] = {13};
static unsigned int pwm2_bit[] = {14};
static unsigned int pwm3_bit[] = {15};
static unsigned int sdio_bit[] = {20};

static unsigned char sata_led_bit_width[] = {1};
static unsigned char gmac1_bit_width[] = {1};
static unsigned char dvo0_lio_uart_bit_width[] = {1, 1, 1, 1, 4, 4};
static unsigned char uart1_bit_width[] = {1, 1, 1, 1, 4, 4};
static unsigned char uart2_bit_width[] = {1, 1, 1, 1, 4, 4};
static unsigned char dvo1_camera_bit_width[] = {1, 1};
static unsigned char can0_bit_width[] = {1};
static unsigned char can1_bit_width[] = {1};
static unsigned char hda_i2s_bit_width[] = {1, 1};
static unsigned char i2c0_bit_width[] = {1};
static unsigned char i2c1_bit_width[] = {1};
static unsigned char uart0_bit_width[] = {4};
static unsigned char nand_bit_width[] = {1};
static unsigned char pwm0_bit_width[] = {1};
static unsigned char pwm1_bit_width[] = {1};
static unsigned char pwm2_bit_width[] = {1};
static unsigned char pwm3_bit_width[] = {1};
static unsigned char sdio_bit_width[] = {1};

//上面的数组就是在这里用的。
static struct ls2k_pmx_group ls2k_pmx_groups[] = {
	PMX_GROUP(sata_led),
	PMX_GROUP(gmac1),
	PMX_GROUP(dvo0_lio_uart),
	PMX_GROUP(uart1),
	PMX_GROUP(uart2),
	PMX_GROUP(dvo1_camera),
	PMX_GROUP(can0),
	PMX_GROUP(can1),
	PMX_GROUP(hda_i2s),
	PMX_GROUP(i2c0),
	PMX_GROUP(i2c1),
	PMX_GROUP(uart0),
	PMX_GROUP(nand),
	PMX_GROUP(pwm0),
	PMX_GROUP(pwm1),
	PMX_GROUP(pwm2),
	PMX_GROUP(pwm3),
	PMX_GROUP(sdio),
};

/*
 * 这里管理的是function
 * 包括 function所属的groups(这个groups可以设置的functions) 和 怎么设置配置寄存器让function生效
 * FUNCTION_GROUP_MAP_CREATE 做的工作其实就是声明一个 _function_groups数组
 * 这个数组里面的字符串就是宏参数。
 * 数组前缀是function的名字 数组里面的内容是groups的名字
 * 这里的含义就是 function所属的groups(这个groups可以设置的functions) 的声明
 * 接着下面的那些数字的数组，就是functions生效的那些配置寄存器的位域的值
 * 那些数组还要对应上面的_bit _bit_width 数组 见芯片手册
 * 比如 dvo0 lio uart12 这样的复用 要控制6个位域 所以对应有6个值
 * 但是uart1 和 uart2 是能共存的 所以在 uart1_4_bit_mask 和类似的数组 里面 会有0
 * 0 代表对应的位域不设置，这样就能实现共存。
 *
 * FUNCTION 宏代表的是 ls2k_pmx_func 变量的填充
 * 第1个参数是 fucntion的名字 会用在填充 对应 _function_groups 数组到对应的变量中
 * 第2个 是指代function有多个 groups的情况 比如 gpio 这个function
 * 可以发现很多个groups都是可以用的，那么如果还是用给定数组来设置位域，那么将会多很多代码
 * 所以把这个过程 使用一个回调函数来实现 见于 ls2k_pmx_set_mux_handle_gpio 的实现
 * 如果 满足通用的设置方法 那么 填入 NULL 并 写好 第3 4参数
 * 第 3 和 第4个参数 就是 位域的值的数组填充，使用另外一套生效办法 这套办法是通用的。
 */

//create const char * const[] name {func}_function_groups to use in ls2k_pmx_functions
FUNCTION_GROUP_MAP_CREATE(sata_led, "sata_led");
static const char * const gpio_function_groups[] = {
	"sdio", "can1", "can0", "pwm3", "pwm2", "pwm1", "pwm0", "i2c1",
	"i2c0", "nand", "sata_led", "i2s", "hda"
};
FUNCTION_GROUP_MAP_CREATE(gmac1, "gmac1");
FUNCTION_GROUP_MAP_CREATE(dvo0, "dvo0_lio_uart");
FUNCTION_GROUP_MAP_CREATE(lio, "dvo0_lio_uart");
FUNCTION_GROUP_MAP_CREATE(uart1_4, "uart1");
FUNCTION_GROUP_MAP_CREATE(uart1_2, "uart1");
FUNCTION_GROUP_MAP_CREATE(uart1_1, "uart1");
FUNCTION_GROUP_MAP_CREATE(uart2_4, "uart2");
FUNCTION_GROUP_MAP_CREATE(uart2_2, "uart2");
FUNCTION_GROUP_MAP_CREATE(uart2_1, "uart2");
FUNCTION_GROUP_MAP_CREATE(dvo1, "dvo1_camera");
FUNCTION_GROUP_MAP_CREATE(camera, "dvo1_camera");
FUNCTION_GROUP_MAP_CREATE(can0, "can0");
FUNCTION_GROUP_MAP_CREATE(can1, "can1");
FUNCTION_GROUP_MAP_CREATE(hda, "hda_i2s");
FUNCTION_GROUP_MAP_CREATE(i2s, "hda_i2s");
FUNCTION_GROUP_MAP_CREATE(i2c0, "i2c0");
FUNCTION_GROUP_MAP_CREATE(i2c1, "i2c1");
FUNCTION_GROUP_MAP_CREATE(uart0_4, "uart0");
FUNCTION_GROUP_MAP_CREATE(uart0_2, "uart0");
FUNCTION_GROUP_MAP_CREATE(uart0_1, "uart0");
FUNCTION_GROUP_MAP_CREATE(nand, "nand");
FUNCTION_GROUP_MAP_CREATE(pwm0, "pwm0");
FUNCTION_GROUP_MAP_CREATE(pwm1, "pwm1");
FUNCTION_GROUP_MAP_CREATE(pwm2, "pwm2");
FUNCTION_GROUP_MAP_CREATE(pwm3, "pwm3");
FUNCTION_GROUP_MAP_CREATE(sdio, "sdio");

static unsigned int single_bit_1_on_value[] = {1};
static unsigned int dvo0_bit_value[] = {1, 0, 0, 0, 0, 0};
static unsigned int lio_bit_value[] = {0, 0, 0, 1, 0, 0};
static unsigned int uart1_4_bit_value[] = {0, 1, 0, 0, 0xf, 0};
static unsigned int uart1_2_bit_value[] = {0, 1, 0, 0, 0x3, 0};
static unsigned int uart1_1_bit_value[] = {0, 1, 0, 0, 0x1, 0};
static unsigned int uart2_4_bit_value[] = {0, 0, 1, 0, 0, 0xf};
static unsigned int uart2_2_bit_value[] = {0, 0, 1, 0, 0, 0x3};
static unsigned int uart2_1_bit_value[] = {0, 0, 1, 0, 0, 0x1};
static unsigned int dvo1_bit_value[] = {1, 0};
static unsigned int camera_bit_value[] = {0, 1};
static unsigned int hda_bit_value[] = {0, 1};
static unsigned int i2s_bit_value[] = {1, 0};
static unsigned int gen_uart_mode4_value[] = {0xf};
static unsigned int gen_uart_mode2_value[] = {0x3};
static unsigned int gen_uart_mode1_value[] = {0x1};

static unsigned char dvo0_bit_mask[] = {1, 1, 1, 1, 0, 0};
static unsigned char lio_bit_mask[] = {1, 1, 1, 1, 0, 0};
static unsigned char uart1_4_bit_mask[] = {1, 1, 0, 1, 1, 0};
static unsigned char uart1_2_bit_mask[] = {1, 1, 0, 1, 1, 0};
static unsigned char uart1_1_bit_mask[] = {1, 1, 0, 1, 1, 0};
static unsigned char uart2_4_bit_mask[] = {1, 0, 1, 1, 0, 1};
static unsigned char uart2_2_bit_mask[] = {1, 0, 1, 1, 0, 1};
static unsigned char uart2_1_bit_mask[] = {1, 0, 1, 1, 0, 1};

static int ls2k_pmx_set_mux_handle_gpio(unsigned long reg_base, unsigned int group_num);
static struct ls2k_pmx_func ls2k_pmx_functions[] = {
	FUNCTION(sata_led, NULL, single_bit_1_on_value, NULL),
	FUNCTION(gmac1, NULL, single_bit_1_on_value, NULL),
	FUNCTION(gpio, ls2k_pmx_set_mux_handle_gpio, NULL, NULL),
	FUNCTION(dvo0, NULL, dvo0_bit_value, dvo0_bit_mask),
	FUNCTION(lio, NULL, lio_bit_value, lio_bit_mask),
	FUNCTION(uart1_4, NULL, uart1_4_bit_value, uart1_4_bit_mask),
	FUNCTION(uart1_2, NULL, uart1_2_bit_value, uart1_2_bit_mask),
	FUNCTION(uart1_1, NULL, uart1_1_bit_value, uart1_1_bit_mask),
	FUNCTION(uart2_4, NULL, uart2_4_bit_value, uart2_4_bit_mask),
	FUNCTION(uart2_2, NULL, uart2_2_bit_value, uart2_2_bit_mask),
	FUNCTION(uart2_1, NULL, uart2_1_bit_value, uart2_1_bit_mask),
	FUNCTION(dvo1, NULL, dvo1_bit_value, NULL),
	FUNCTION(camera, NULL, camera_bit_value, NULL),
	FUNCTION(can0, NULL, single_bit_1_on_value, NULL),
	FUNCTION(can1, NULL, single_bit_1_on_value, NULL),
	FUNCTION(hda, NULL, hda_bit_value, NULL),
	FUNCTION(i2s, NULL, i2s_bit_value, NULL),
	FUNCTION(i2c0, NULL, single_bit_1_on_value, NULL),
	FUNCTION(i2c1, NULL, single_bit_1_on_value, NULL),
	FUNCTION(uart0_4, NULL, gen_uart_mode4_value, NULL),
	FUNCTION(uart0_2, NULL, gen_uart_mode2_value, NULL),
	FUNCTION(uart0_1, NULL, gen_uart_mode1_value, NULL),
	FUNCTION(nand, NULL, single_bit_1_on_value, NULL),
	FUNCTION(pwm0, NULL, single_bit_1_on_value, NULL),
	FUNCTION(pwm1, NULL, single_bit_1_on_value, NULL),
	FUNCTION(pwm2, NULL, single_bit_1_on_value, NULL),
	FUNCTION(pwm3, NULL, single_bit_1_on_value, NULL),
	FUNCTION(sdio, NULL, single_bit_1_on_value, NULL),
};

static int ls2k_get_groups_count(struct pinctrl_dev *pcdev)
{
	return ARRAY_SIZE(ls2k_pmx_groups);
}

static const char *ls2k_get_group_name(struct pinctrl_dev *pcdev,
					unsigned int selector)
{
	return ls2k_pmx_groups[selector].name;
}

static int ls2k_get_group_pins(struct pinctrl_dev *pcdev, unsigned int selector,
			const unsigned int **pins, unsigned int *num_pins)
{
	*pins = ls2k_pmx_groups[selector].pins;
	*num_pins = ls2k_pmx_groups[selector].num_pins;

	return 0;
}

static void ls2k_pin_dbg_show(struct pinctrl_dev *pcdev, struct seq_file *s,
			       unsigned int offset)
{
	seq_printf(s, " %s", dev_name(pcdev->dev));
}

static const struct pinctrl_ops ls2k_pctrl_ops = {
	.get_groups_count	= ls2k_get_groups_count,
	.get_group_name		= ls2k_get_group_name,
	.get_group_pins		= ls2k_get_group_pins,
	.dt_node_to_map		= pinconf_generic_dt_node_to_map_all,
	.dt_free_map		= pinctrl_utils_free_map,
	.pin_dbg_show		= ls2k_pin_dbg_show,
};

static int ls2k_pmx_set_mux_handle_gpio(unsigned long reg_base, unsigned int group_num)
{
	int i;
	unsigned char bit_size = ls2k_pmx_groups[group_num].bit_size;
	unsigned int* reg_offset = ls2k_pmx_groups[group_num].reg;
	unsigned int* bit = ls2k_pmx_groups[group_num].bit;

	unsigned long reg_temp;

	unsigned int val;

	for(i = 0; i < bit_size; ++i) {
		reg_temp = reg_base + reg_offset[i];

		val = readl((void *)reg_temp);
		val &= ~(1 << bit[i]);  //use mask clear bit filed
		val |= 1 << bit[i]; //value fix bit filed
		writel(val, (void *)reg_temp);
	}
	return 0;
}

static int ls2k_pmx_set_mux(struct pinctrl_dev *pcdev, unsigned int func_num,
			      unsigned int group_num)
{
	int ret;
	int i;
	struct ls2k_pinctrl *pctrl = pinctrl_dev_get_drvdata(pcdev);

	int (*handle_func)(unsigned long, unsigned int) = ls2k_pmx_functions[func_num].mul_bit_zone_handle_func;
	unsigned int* bit_value = ls2k_pmx_functions[func_num].bit_value;
	unsigned char* bit_mask = ls2k_pmx_functions[func_num].bit_mask;

	unsigned char bit_size = ls2k_pmx_groups[group_num].bit_size;
	unsigned int* reg_offset = ls2k_pmx_groups[group_num].reg;
	unsigned int* bit = ls2k_pmx_groups[group_num].bit;
	unsigned char* bit_width = ls2k_pmx_groups[group_num].bit_width;

	unsigned long reg_base = (unsigned long)pctrl->reg_base;
	unsigned long reg_temp;
	unsigned int bit_value_temp;

	unsigned int val;
	unsigned int val_mask;

	unsigned long flags;

	raw_spin_lock_irqsave(&pctrl->lock, flags);
	if(handle_func)
		ret = handle_func(reg_base, group_num);
	else {
		for(i = 0; i < bit_size; ++i) {
			reg_temp = reg_base + reg_offset[i];
			if(!bit_mask || bit_mask[i]) {
				bit_value_temp = bit_value[i];
				bit_value_temp <<= bit[i];

				//make bit filed mask
				val_mask = 1 << bit_width[i];
				--val_mask;
				val_mask <<= bit[i];

				val = readl((void *)reg_temp);
				val &= ~val_mask;  //use mask clear bit filed
				val |= bit_value_temp; //value fix bit filed
				writel(val, (void *)reg_temp);
			}
		}
		ret = 0;
	}
	raw_spin_unlock_irqrestore(&pctrl->lock, flags);

	return ret;
}

int ls2k_pmx_get_funcs_count(struct pinctrl_dev *pcdev)
{
	return ARRAY_SIZE(ls2k_pmx_functions);
}

const char *ls2k_pmx_get_func_name(struct pinctrl_dev *pcdev,
				    unsigned int selector)
{
	return ls2k_pmx_functions[selector].name;
}

int ls2k_pmx_get_groups(struct pinctrl_dev *pcdev, unsigned int selector,
			 const char * const **groups,
			 unsigned int * const num_groups)
{
	*groups = ls2k_pmx_functions[selector].groups;
	*num_groups = ls2k_pmx_functions[selector].num_groups;

	return 0;
}

const struct pinmux_ops ls2k_pmx_ops = {
	.set_mux = ls2k_pmx_set_mux,
	.get_functions_count = ls2k_pmx_get_funcs_count,
	.get_function_name = ls2k_pmx_get_func_name,
	.get_function_groups = ls2k_pmx_get_groups,
};

int ls2k_pinctrl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ls2k_pinctrl *pctrl;
	struct resource *res;

	pctrl = devm_kzalloc(dev, sizeof(struct ls2k_pinctrl), GFP_KERNEL);
	if (!pctrl)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pctrl->reg_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(pctrl->reg_base))
		return PTR_ERR(pctrl->reg_base);

	raw_spin_lock_init(&pctrl->lock);

	pctrl->dev = dev;
	pctrl->desc.name	= "pinctrl-ls2k";
	pctrl->desc.owner	= THIS_MODULE;
	pctrl->desc.pctlops	= &ls2k_pctrl_ops;
	pctrl->desc.pmxops	= &ls2k_pmx_ops;
	pctrl->desc.confops	= NULL;
	pctrl->desc.pins	= ls2k_pctrl_pins;
	pctrl->desc.npins	= ARRAY_SIZE(ls2k_pctrl_pins);

	pctrl->pcdev = devm_pinctrl_register(pctrl->dev, &pctrl->desc, pctrl);
	if (IS_ERR(pctrl->pcdev)) {
		dev_err(pctrl->dev, "can't register pinctrl device");
		return PTR_ERR(pctrl->pcdev);
	}

	return 0;
}

static const struct of_device_id ls2k_pinctrl_dt_match[] = {
	{
		.compatible = "loongson,2k1000-pinctrl",
	},
	{ },
};

static struct platform_driver ls2k_pinctrl_driver = {
	.probe		= ls2k_pinctrl_probe,
	.driver = {
		.name	= "ls2k-pinctrl",
		.of_match_table = ls2k_pinctrl_dt_match,
	},
};

static int __init ls2k_pinctrl_init(void)
{
	return platform_driver_register(&ls2k_pinctrl_driver);
}
arch_initcall(ls2k_pinctrl_init);

static void __exit ls2k_pinctrl_exit(void)
{
	platform_driver_unregister(&ls2k_pinctrl_driver);
}
module_exit(ls2k_pinctrl_exit);
