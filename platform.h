#include <stdio.h>
#include "m1-pd-bmc.h"

static void platform_usleep(uint64_t us)
{
	sleep_ms(us / 1000);
	sleep_us(us % 1000);
}

