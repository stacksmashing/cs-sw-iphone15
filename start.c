// FUSB302-based serial/reset/whatever controller for M1-based systems

#include <stddef.h>

#include "bsp/board.h"
#include "tusb.h"
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
	[SBU_SWAP] = {
		.pin	= 20,
		.mode	= GPIO_FUNC_SIO,
		.dir	= GPIO_OUT,
	},
	[SEL_USB] = {
		.pin	= 7,
		.mode	= GPIO_FUNC_SIO,
		.dir	= GPIO_OUT,
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
	[SBU_SWAP] = {
		.pin	= 21,
		.mode	= GPIO_FUNC_SIO,
		.dir	= GPIO_OUT,
	},
	[SEL_USB] = {
		.pin	= 6,
		.mode	= GPIO_FUNC_SIO,
		.dir	= GPIO_OUT,
	},
};

static struct {
	/*
	 * we rely on prod/cons rollover behaviour, and buf must cover
	 * the full range of prod/cons.
	 */
	uint8_t		prod;
	uint8_t		cons;
	char		buf[256];
} uart1_buf;

static void __not_in_flash_func(uart_irq_fn)(int port,
					     const struct hw_context *hw)
{
	while (uart_is_readable(hw->uart)) {
		char c = uart_getc(hw->uart);
		upstream_ops->tx_bytes(port, &c, 1);
	}
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
	uart_irq_fn(0, &hw0);
}

static void __not_in_flash_func(uart1_irq_fn)(void)
{
	if (!upstream_is_serial()) {
		uart_irq_fn(1, &hw1);
		return;
	}

	while (uart_is_readable(uart1)) {
		uart1_buf.buf[uart1_buf.prod++] = uart_getc(uart1);

		/* Oops, we're losing data... */
		if (uart1_buf.prod == uart1_buf.cons)
			uart1_buf.cons++;
	}
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
		if (pin->dir == GPIO_OUT)
			gpio_put(pin->pin, pin->level);
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

static void usb_tx_bytes(int32_t port, const char *ptr, int len)
{
        if (!tud_cdc_n_connected(port))
		return;

	while (len > 0) {
		size_t available = tud_cdc_n_write_available(port);

		if (!available) {
			tud_task();
		} else {
			size_t send = MIN(len, available);
			size_t sent = tud_cdc_n_write(port, ptr, send);

			ptr += sent;
			len -= sent;
		}

		tud_cdc_n_write_flush(port);
        }
}

static int32_t usb_rx_byte(int32_t port)
{
	uint8_t c;

	if (!tud_cdc_n_connected(port) || !tud_cdc_n_available(port))
		return -1;

	tud_cdc_n_read(port, &c, 1);
	return c;
}

static const struct upstream_ops usb_upstream_ops = {
	.tx_bytes	= usb_tx_bytes,
	.rx_byte	= usb_rx_byte,
	.flush		= tud_task,
};

static void serial1_tx_bytes(int32_t port, const char *ptr, int len)
{
	uart_write_blocking(uart1, ptr, len);
}

static int32_t serial1_rx_byte(int32_t port)
{
	uint32_t status;
	int32_t val;

	tud_task();
	val = usb_rx_byte(port);
	if (val != -1)
		return val;

	status = save_and_disable_interrupts();

	if (uart1_buf.prod == uart1_buf.cons)
		val = -1;
	else
		val = uart1_buf.buf[uart1_buf.cons++];

	restore_interrupts(status);

	return val;
}

static void serial1_flush(void) {}

static const struct upstream_ops serial1_upstream_ops = {
	.tx_bytes	= serial1_tx_bytes,
	.rx_byte	= serial1_rx_byte,
	.flush		= serial1_flush,
};

const struct upstream_ops *upstream_ops;

void upstream_tx_str(int32_t port, const char *str)
{
	do {
		const char *cursor = str;

		while (*cursor && *cursor != '\n')
			cursor++;

		upstream_ops->tx_bytes(port, str, cursor - str);

		if (!*cursor)
			return;

		upstream_ops->tx_bytes(port, "\n\r", 2);

		str = cursor + 1;
	} while (*str);
}

void set_upstream_ops(bool serial)
{
	if (serial) {
		upstream_ops = &serial1_upstream_ops;
	} else {
		upstream_ops = &usb_upstream_ops;
	}

	__dsb();
}

bool upstream_is_serial(void)
{
	return upstream_ops == &serial1_upstream_ops;
}

int main(void)
{
	bool success;
	int port;

	set_upstream_ops(false);

	success = set_sys_clock_khz(133000, false);

	board_init();
	tusb_init();

	m1_pd_bmc_system_init(&hw0);
	m1_pd_bmc_system_init(&hw1);

	do {
		static bool state = false;
		static int cnt = 0;

		tud_task();
		gpio_put(hw0.pins[LED_G].pin, state);
		sleep_ms(1);
		cnt++;
		cnt %= 256;
		if (!cnt)
			state = !state;
	} while (!tud_cdc_n_connected(0) && !tud_cdc_n_connected(1));

	port = !tud_cdc_n_connected(0);

	__printf(port, "This is the Central Scrutinizer\n");
	__printf(port, "Control character is ^_\n");
	__printf(port, "Press ^_ + ? for help\n");

	if (!success)
		__printf(port, "WARNING: Nominal frequency NOT reached\n");

	m1_pd_bmc_fusb_setup(0, &hw0);
	m1_pd_bmc_fusb_setup(1, &hw1);

	m1_pd_bmc_run();
}
