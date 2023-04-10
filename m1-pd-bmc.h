// FUSB302-based serial/reset/whatever controller for M1-based systems

#include <stdint.h>
#include <stdbool.h>

#include "pico/stdlib.h"

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "tusb.h"

struct gpio_pin_config {
	uint16_t		pin;
	enum gpio_function	mode;
	int			dir;
	bool			pu;
	bool			skip;
};

enum m1_pd_bmc_pins {
	LED_G,
	I2C_SDA,
	I2C_SCL,
	FUSB_INT,
	FUSB_VBUS,
	UART_TX,
	UART_RX,
};

struct hw_context {
	const struct gpio_pin_config 	*pins;
	uart_inst_t 			*const uart;
	i2c_inst_t			*const i2c;
	void				(*uart_handler)(void);
	uint8_t		 		addr;
	uint8_t				nr_pins;
	uint8_t				uart_irq;
};

const struct hw_context *get_hw_from_port(int port);
void m1_pd_bmc_fusb_setup(unsigned int port,
			  const struct hw_context *hw);
void m1_pd_bmc_run(void);

void usb_tx_bytes(int32_t port, const char *ptr, int len);
void usb_tx_str(int32_t port, char *ptr);
int32_t usb_rx_byte(int32_t port);

#define PRINTF_SIZE	512

#define __printf(__p, __f, ...)	do {					\
		char __str[PRINTF_SIZE];				\
		snprintf(__str, PRINTF_SIZE, __f, ##__VA_ARGS__);	\
		usb_tx_str(__p, __str);					\
		tud_task();						\
	} while(0)

#define ARRAY_SIZE(arr)	(sizeof(arr) / sizeof((arr)[0]))
