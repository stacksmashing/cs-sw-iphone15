#include <stdio.h>

#include "m1-pd-bmc.h"
#include "tcpm_driver.h"

/* I2C wrapper functions - get I2C port / slave addr from config struct. */
int16_t tcpc_write(int16_t port, int16_t reg, int16_t val)
{
	const struct hw_context *fusb = get_hw_from_port(port);
	uint8_t buf[] = {
		reg & 0xff,
		val & 0xff,
	};

	i2c_write_blocking(fusb->i2c, fusb->addr, buf, sizeof(buf), false);

	return 0;
}

int16_t tcpc_write16(int16_t port, int16_t reg, int16_t val)
{
	const struct hw_context *fusb = get_hw_from_port(port);
	uint8_t buf[] = {
		reg & 0xff,
		val & 0xff,
		(val >> 8) & 0xff,
	};

	i2c_write_blocking(fusb->i2c, fusb->addr, buf, sizeof(buf), false);

	return 0;
}

int16_t tcpc_read(int16_t port, int16_t reg, int16_t *val)
{
	const struct hw_context *fusb = get_hw_from_port(port);
	uint8_t buf[] = {
		reg & 0xff,
		0,
	};

	i2c_write_blocking(fusb->i2c, fusb->addr, &buf[0], 1, true);
	i2c_read_blocking(fusb->i2c, fusb->addr, &buf[1], 1, false);

	*val = buf[1];

	return 0;
}

int16_t tcpc_read16(int16_t port, int16_t reg, int16_t *val)
{
	const struct hw_context *fusb = get_hw_from_port(port);
	uint8_t buf[] = {
		reg & 0xff,
		0,
		0,
	};

	i2c_write_blocking(fusb->i2c, fusb->addr, &buf[0], 1, true);
	i2c_read_blocking(fusb->i2c, fusb->addr, &buf[1], 2, false);
	*val = buf[1];
	*val |= (buf[2] << 8);

	return 0;
}

int16_t tcpc_xfer(int16_t port,
	      const uint8_t * out, int16_t out_size,
	      uint8_t * in, int16_t in_size, int16_t flags)
{
	const struct hw_context *fusb = get_hw_from_port(port);

	if (out_size) {
		int err;
		err = i2c_write_blocking(fusb->i2c, fusb->addr, out, out_size,
					 !(flags & I2C_XFER_STOP));
	}

	if (in_size) {
		i2c_read_blocking(fusb->i2c, fusb->addr, in, in_size,
				  !(flags & I2C_XFER_STOP));
	}

	return 0;
}
