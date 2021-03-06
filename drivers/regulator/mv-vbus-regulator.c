/*
 * Driver for Marvell USB VBUS power regulator for Armada 380 SoCs.
 *
 * Copyright (C) 2015 Marvell
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/gpio.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regulator/machine.h>

/**
 * struct mv_vbus_config
 * @supply_name:	Name of the regulator supply
 * @input_supply:	Name of the input regulator supply
 * @microvolts:		Output voltage of regulator
 * @gpio:		GPIO to use for enable control
 *			set to EINVAL if not used
 * @startup_delay:	Start-up time in microseconds
 * @enable_high:	Polarity of enable GPIO
 *			1 = Active high, 0 = Active low
 * @enabled_at_boot:	Whether regulator has been enabled at
 *			boot or not. 1 = Yes, 0 = No This is
 *			used to keep the regulator at the
 *			default state
 * @init_data:		regulator_init_data
 *
 * This structure contains vbus voltage regulator configuration
 * information that must be passed by platform code to the
 * voltage regulator driver.
 */
struct mv_vbus_config {
	const char *supply_name;
	const char *input_supply;
	int microvolts;
	int gpio;
	unsigned startup_delay;
	unsigned enable_high:1;
	unsigned enabled_at_boot:1;
	struct regulator_init_data *init_data;
};

struct mv_vbus_data {
	struct regulator_desc desc;
	struct regulator_dev *dev;
	int gpio;
	unsigned int ena_gpio_invert:1;
	int microvolts;
};

/**
 * of_get_mv_vbus_config - extract mv_vbus_config structure
 * @dev: device requesting for mv_vbus_config
 *
 * Populates mv_vbus_config structure by extracting data from
 * device tree node, returns a pointer to the populated
 * structure of NULL if memory alloc fails.
 */
static struct mv_vbus_config *
of_get_mv_vbus_config(struct device *dev)
{
	struct mv_vbus_config *config;
	struct device_node *np = dev->of_node;
	const __be32 *delay;
	struct regulator_init_data *init_data;

	config = devm_kzalloc(dev, sizeof(struct mv_vbus_config), GFP_KERNEL);
	if (!config)
		return ERR_PTR(-ENOMEM);

	config->init_data = of_get_regulator_init_data(dev, dev->of_node);
	if (!config->init_data)
		return ERR_PTR(-EINVAL);

	init_data = config->init_data;
	init_data->constraints.apply_uV = 0;

	config->supply_name = init_data->constraints.name;
	if (init_data->constraints.min_uV == init_data->constraints.max_uV) {
		config->microvolts = init_data->constraints.min_uV;
	} else {
		dev_err(dev,
			"Fixed regulator specified with variable voltages\n");
		return ERR_PTR(-EINVAL);
	}

	if (init_data->constraints.boot_on)
		config->enabled_at_boot = true;

	config->gpio = of_get_named_gpio(np, "gpio", 0);

	if ((config->gpio == -ENODEV) || (config->gpio == -EPROBE_DEFER))
		return ERR_PTR(-EPROBE_DEFER);

	delay = of_get_property(np, "startup-delay-us", NULL);
	if (delay)
		config->startup_delay = be32_to_cpu(*delay);

	if (of_find_property(np, "enable-active-high", NULL))
		config->enable_high = true;

	return config;
}

static int mv_vbus_get_voltage(struct regulator_dev *dev)
{
	struct mv_vbus_data *data = rdev_get_drvdata(dev);

	if (data->microvolts)
		return data->microvolts;
	else
		return -EINVAL;
}

static int mv_vbus_list_voltage(struct regulator_dev *dev,
			     unsigned selector)
{
	struct mv_vbus_data *data = rdev_get_drvdata(dev);

	if (selector != 0)
		return -EINVAL;

	return data->microvolts;
}

static int mv_vbus_gpio_set(struct regulator_dev *rdev, bool enable)
{
	struct mv_vbus_data *drvdata = (struct mv_vbus_data *)rdev->reg_data;
	unsigned int gpio_val;

	if (!drvdata)
		return -EINVAL;

	gpio_val = (enable) ? !drvdata->ena_gpio_invert : drvdata->ena_gpio_invert;

	gpio_set_value_cansleep(drvdata->gpio, gpio_val);

	return 0;
}

static int mv_vbus_enable_supply(struct regulator_dev *rdev)
{
	return mv_vbus_gpio_set(rdev, true);
}

static int mv_vbus_disable_supply(struct regulator_dev *rdev)
{
	return mv_vbus_gpio_set(rdev, false);
}

static int mv_vbus_suspend_disable(struct regulator_dev *rdev)
{
	return mv_vbus_disable_supply(rdev);
}

static struct regulator_ops vbus_ops = {
	.enable	= mv_vbus_enable_supply,
	.disable = mv_vbus_disable_supply,
	.set_suspend_disable = mv_vbus_suspend_disable,
	.get_voltage = mv_vbus_get_voltage,
	.list_voltage = mv_vbus_list_voltage,
};

static int mv_vbus_reg_probe(struct platform_device *pdev)
{
	struct mv_vbus_config *config;
	struct mv_vbus_data *drvdata;
	struct regulator_config cfg = { };
	int ret;

	if (pdev->dev.of_node) {
		config = of_get_mv_vbus_config(&pdev->dev);
		if (IS_ERR(config))
			return PTR_ERR(config);
	} else {
		config = pdev->dev.platform_data;
	}

	if (!config)
		return -ENOMEM;

	drvdata = devm_kzalloc(&pdev->dev, sizeof(struct mv_vbus_data),
			       GFP_KERNEL);
	if (drvdata == NULL) {
		dev_err(&pdev->dev, "Failed to allocate device data\n");
		ret = -ENOMEM;
		goto err;
	}

	drvdata->desc.name = kstrdup(config->supply_name, GFP_KERNEL);
	if (drvdata->desc.name == NULL) {
		dev_err(&pdev->dev, "Failed to allocate supply name\n");
		ret = -ENOMEM;
		goto err;
	}
	drvdata->desc.type = REGULATOR_VOLTAGE;
	drvdata->desc.owner = THIS_MODULE;
	drvdata->desc.ops = &vbus_ops;

	drvdata->desc.enable_time = config->startup_delay;

	if (config->input_supply) {
		drvdata->desc.supply_name = kstrdup(config->input_supply,
							GFP_KERNEL);
		if (!drvdata->desc.supply_name) {
			dev_err(&pdev->dev,
				"Failed to allocate input supply\n");
			ret = -ENOMEM;
			goto err_name;
		}
	}

	if (config->microvolts)
		drvdata->desc.n_voltages = 1;

	drvdata->microvolts = config->microvolts;

	if (gpio_is_valid(config->gpio)) {
		cfg.ena_gpio_invert = !config->enable_high;

		if (config->enabled_at_boot) {
			if (config->enable_high)
				cfg.ena_gpio_flags |= GPIOF_OUT_INIT_HIGH;
			else
				cfg.ena_gpio_flags |= GPIOF_OUT_INIT_LOW;
		} else {
			if (config->enable_high)
				cfg.ena_gpio_flags |= GPIOF_OUT_INIT_LOW;
			else
				cfg.ena_gpio_flags |= GPIOF_OUT_INIT_HIGH;
		}

		ret = gpio_request_one(config->gpio,
				       GPIOF_DIR_OUT | cfg.ena_gpio_flags,
				       drvdata->desc.name);

		if (ret != 0) {
			dev_err(&pdev->dev, "Failed to request enable GPIO%d: %d\n",
				config->gpio, ret);
			goto err_name;
		}

		drvdata->gpio = config->gpio;
		drvdata->ena_gpio_invert = cfg.ena_gpio_invert;

		/* don't let regulator use this gpio */
		cfg.ena_gpio = 0;
	} else {
		dev_err(&pdev->dev, "gpio %d invalid\n", config->gpio);
		ret = -EINVAL;
		goto err_input;
	}

	cfg.dev = &pdev->dev;
	cfg.init_data = config->init_data;
	cfg.driver_data = drvdata;
	cfg.of_node = pdev->dev.of_node;

	drvdata->dev = regulator_register(&drvdata->desc, &cfg);
	if (IS_ERR(drvdata->dev)) {
		ret = PTR_ERR(drvdata->dev);
		dev_err(&pdev->dev, "Failed to register regulator: %d\n", ret);
		goto err_input;
	}

	platform_set_drvdata(pdev, drvdata);

	dev_dbg(&pdev->dev, "%s supplying %duV\n", drvdata->desc.name,
		drvdata->microvolts);

	return 0;

err_input:
	kfree(drvdata->desc.supply_name);
err_name:
	kfree(drvdata->desc.name);
err:
	return ret;
}

static int mv_vbus_reg_remove(struct platform_device *pdev)
{
	struct mv_vbus_data *drvdata = platform_get_drvdata(pdev);

	regulator_unregister(drvdata->dev);
	kfree(drvdata->desc.supply_name);
	kfree(drvdata->desc.name);

	return 0;
}

#if defined(CONFIG_OF)
static const struct of_device_id vbus_of_match[] = {
	{ .compatible = "mv,vbus-regulator", },
	{},
};
MODULE_DEVICE_TABLE(of, vbus_of_match);
#endif

static struct platform_driver regulator_vbus_voltage_driver = {
	.probe		= mv_vbus_reg_probe,
	.remove		= mv_vbus_reg_remove,
	.driver		= {
		.name		= "mv-vbus-regulator",
		.owner		= THIS_MODULE,
		.of_match_table = of_match_ptr(vbus_of_match),
	},
};

static int __init regulator_vbus_voltage_init(void)
{
	return platform_driver_register(&regulator_vbus_voltage_driver);
}
subsys_initcall(regulator_vbus_voltage_init);

static void __exit regulator_vbus_voltage_exit(void)
{
	platform_driver_unregister(&regulator_vbus_voltage_driver);
}
module_exit(regulator_vbus_voltage_exit);

MODULE_AUTHOR("Ofer Heifetz <oferh@marvell.com>");
MODULE_DESCRIPTION("Marvell vbus regulator");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:mv-vbus-regulator");
