// FUSB302-based serial/reset/whatever controller for M1-based systems

#include <stddef.h>
#include <stdio.h>

#include "m1-pd-bmc.h"
#include "FUSB302.h"

static const struct gpio_pin_config m1_pd_bmc_pin_config0[] = {
	[LED_G] = {
		.pin	= 25,
		.mode	= GPIO_FUNC_SIO,
		.dir	= GPIO_OUT,
	},
	[I2C_SDA] = {		/* I2C0 */
		.pin	= 16,
		.mode	= GPIO_FUNC_I2C,
	},
	[I2C_SCL] = {		/* I2C0 */
		.pin	= 17,
		.mode	= GPIO_FUNC_I2C,
	},
	[FUSB_INT] = {
		.pin	= 18,
		.mode	= GPIO_FUNC_SIO,
		.dir	= GPIO_IN,
	},
	[FUSB_VBUS] = {
		.pin	= 26,
		.mode	= GPIO_FUNC_SIO,
		.dir	= GPIO_IN,
	},
	[UART_TX] = {		/* UART0 */
		.pin	= 12,
		.mode	= GPIO_FUNC_UART,
	},
	[UART_RX] = {		/* UART0 */
		.pin	= 13,
		.mode	= GPIO_FUNC_UART,
	},
};

static const struct gpio_pin_config m1_pd_bmc_pin_config1[] = {
	[LED_G] = {
		.pin	= 25,
		.mode	= GPIO_FUNC_SIO,
		.dir	= GPIO_OUT,
		.skip	= true,
	},
	[I2C_SDA] = {		/* I2C1 */
		.pin	= 22,
		.mode	= GPIO_FUNC_I2C,
	},
	[I2C_SCL] = {		/* I2C1 */
		.pin	= 27,
		.mode	= GPIO_FUNC_I2C,
	},
	[FUSB_INT] = {
		.pin	= 19,
		.mode	= GPIO_FUNC_SIO,
		.dir	= GPIO_IN,
	},
	[FUSB_VBUS] = {
		.pin	= 28,
		.mode	= GPIO_FUNC_SIO,
		.dir	= GPIO_IN,
	},
	[UART_TX] = {		/* UART1 */
		.pin	= 8,
		.mode	= GPIO_FUNC_UART,
	},
	[UART_RX] = {		/* UART1 */
		.pin	= 9,
		.mode	= GPIO_FUNC_UART,
	},
};

#define ARRAY_SIZE(arr)	(sizeof(arr) / sizeof((arr)[0]))

static void __not_in_flash_func(uart_irq_fn)(const struct hw_context *hw)
{
	while (uart_is_readable(hw->uart))
		putchar_raw(uart_getc(hw->uart));
}

static void uart0_irq_fn(void);
static void uart1_irq_fn(void);

static const struct hw_context hw0 = {
	.pins		= m1_pd_bmc_pin_config0,
	.nr_pins	= ARRAY_SIZE(m1_pd_bmc_pin_config0),
	.uart		= uart0,
	.uart_irq	= UART0_IRQ,
	.uart_handler	= uart0_irq_fn,
	.i2c		= i2c0,
	.addr		= fusb302_I2C_SLAVE_ADDR,
};

static const struct hw_context hw1 = {
	.pins		= m1_pd_bmc_pin_config1,
	.nr_pins	= ARRAY_SIZE(m1_pd_bmc_pin_config1),
	.uart		= uart1,
	.uart_irq	= UART1_IRQ,
	.uart_handler	= uart1_irq_fn,
	.i2c		= i2c1,
	.addr		= fusb302_I2C_SLAVE_ADDR,
};

static void __not_in_flash_func(uart0_irq_fn)(void)
{
	uart_irq_fn(&hw0);
}

static void __not_in_flash_func(uart1_irq_fn)(void)
{
	uart_irq_fn(&hw1);
}

static void init_system(const struct hw_context *hw)
{
	i2c_init(hw->i2c, 400 * 1000);

	uart_init(hw->uart, 115200);
	uart_set_hw_flow(hw->uart, false, false);
	uart_set_fifo_enabled(hw->uart, true);
	irq_set_exclusive_handler(hw->uart_irq, hw->uart_handler);
	irq_set_enabled(hw->uart_irq, true);
	uart_set_irq_enables(hw->uart, true, false);

	/* Interrupt when the RX FIFO is 3/4 full */
	hw_write_masked(&uart_get_hw(hw->uart)->ifls,
			3 << UART_UARTIFLS_RXIFLSEL_LSB,
                        UART_UARTIFLS_RXIFLSEL_BITS);
}

static void m1_pd_bmc_gpio_setup_one(const struct gpio_pin_config *pin)
{
	if (pin->skip)
		return;
	gpio_set_function(pin->pin, pin->mode);
	if (pin->mode == GPIO_FUNC_SIO) {
		gpio_init(pin->pin);
		gpio_set_dir(pin->pin, pin->dir);
	}
	if (pin->pu)
		gpio_pull_up(pin->pin);
}

static void m1_pd_bmc_system_init(const struct hw_context *hw)
{
	init_system(hw);

	for (unsigned int i = 0; i < hw->nr_pins; i++)
		m1_pd_bmc_gpio_setup_one(&hw->pins[i]);
}

int main(void)
{
	bool success;

	success = set_sys_clock_khz(250000, false);

	stdio_init_all();

	m1_pd_bmc_system_init(&hw0);
	m1_pd_bmc_system_init(&hw1);

	while (!stdio_usb_connected()) {
		static bool state = false;

		gpio_put(hw0.pins[LED_G].pin, state);
		sleep_ms(250);
		state = !state;
	}

	printf("This is the Central Scrutinizer\n"
		"Control character is ^_\n"
		"Press ^_ + ? for help\n");
	if (!success)
		printf("WARNING: Nominal frequency NOT reached\n");

	m1_pd_bmc_fusb_setup(0, &hw0);
	m1_pd_bmc_fusb_setup(1, &hw1);

	m1_pd_bmc_run();
}
