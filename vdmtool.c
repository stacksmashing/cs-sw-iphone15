#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tcpm_driver.h"
#include "FUSB302.h"
#include "m1-pd-bmc.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"
#include "pico/bootrom.h"

enum state {
	STATE_INVALID = -1,
	STATE_DISCONNECTED = 0,
	STATE_CONNECTED,
	STATE_READY,
	STATE_DFP_VBUS_ON,
	STATE_DFP_CONNECTED,
	STATE_DFP_ACCEPT,
	STATE_IDLE,
};

struct vdm_context {
	const struct hw_context		*hw;
	enum state			state;
	int16_t				std_flag;
	int16_t				source_cap_timer;
	int16_t				cc_debounce;
	volatile bool			pending;
	bool 				verbose;
	bool				vdm_escape;
};

static struct vdm_context vdm_contexts[CONFIG_USB_PD_PORT_COUNT];

#define PIN(cxt, idx)	(cxt)->hw->pins[(idx)].pin
#define PORT(cxt)	((cxt) - vdm_contexts)
#define UART(cxt)	(cxt)->hw->uart

#define HIGH true
#define LOW false

#define dprintf(cxt, ...)	do {					\
		if (cxt->verbose)					\
			printf(__VA_ARGS__);				\
		} while(0)

#define cprintf(cxt, str, ...)	do {					\
		printf("P%ld: " str, PORT(cxt), ##__VA_ARGS__);		\
	} while(0)

#define STATE(cxt, x)	do {						\
		cxt->state = STATE_##x;					\
		cprintf(cxt, "S: " #x "\n");				\
	} while(0)

static void vbus_off(struct vdm_context *cxt)
{
	gpio_put(PIN(cxt, FUSB_VBUS), LOW);
	sleep_ms(800);
	gpio_set_dir(PIN(cxt, FUSB_VBUS), GPIO_IN);
	cprintf(cxt, "VBUS OFF\n");
}

static void vbus_on(struct vdm_context *cxt)
{
	cprintf(cxt, "VBUS ON\n");
	gpio_set_dir(PIN(cxt, FUSB_VBUS), GPIO_OUT);
	gpio_put(PIN(cxt, FUSB_VBUS), HIGH);
}

void debug_poke(struct vdm_context *cxt)
{
	int16_t hdr = PD_HEADER(PD_DATA_VENDOR_DEF, 1, 1, 0, 1, PD_REV20, 0);
	const uint32_t x = 0;

	cprintf(cxt, "Empty debug message\n");
	fusb302_tcpm_transmit(PORT(cxt), TCPC_TX_SOP_DEBUG_PRIME_PRIME, hdr, &x);
}

static void evt_dfpconnect(struct vdm_context *cxt)
{
	int16_t cc1 = -1, cc2 = -1;
	fusb302_tcpm_get_cc(PORT(cxt), &cc1, &cc2);
	cprintf(cxt, "Connected: cc1=%d cc2=%d\n", cc1, cc2);
	if (cc1 < 2 && cc2 < 2) {
		cprintf(cxt, "Nope.\n");
		return;
	}
	fusb302_pd_reset(PORT(cxt));
	fusb302_tcpm_set_msg_header(PORT(cxt), 1, 1);	// Source
	if (cc1 > cc2) {
		fusb302_tcpm_set_polarity(PORT(cxt), 0);
		cprintf(cxt, "Polarity: CC1 (normal)\n");
	} else {
		fusb302_tcpm_set_polarity(PORT(cxt), 1);
		cprintf(cxt, "Polarity: CC2 (flipped)\n");
	}
	fusb302_tcpm_set_rx_enable(PORT(cxt), 1);
	vbus_on(cxt);
	STATE(cxt, DFP_VBUS_ON);

	debug_poke(cxt);
}

static void evt_connect(struct vdm_context *cxt)
{
	int16_t cc1 = -1, cc2 = -1;
	fusb302_tcpm_get_cc(PORT(cxt), &cc1, &cc2);
	cprintf(cxt, "Connected: cc1=%d cc2=%d\n", cc1, cc2);
	if (cc1 < 2 && cc2 < 2) {
		cprintf(cxt, "Nope.\n");
		return;
	}
	fusb302_pd_reset(PORT(cxt));
	fusb302_tcpm_set_msg_header(PORT(cxt), 0, 0);	// Sink
	if (cc1 > cc2) {
		fusb302_tcpm_set_polarity(PORT(cxt), 0);
		cprintf(cxt, "Polarity: CC1 (normal)\n");
	} else {
		fusb302_tcpm_set_polarity(PORT(cxt), 1);
		cprintf(cxt, "Polarity: CC2 (flipped)\n");
	}
	fusb302_tcpm_set_rx_enable(PORT(cxt), 1);
	STATE(cxt, CONNECTED);
}

static void evt_disconnect(struct vdm_context *cxt)
{
	vbus_off(cxt);
	cprintf(cxt, "Disconnected\n");
	fusb302_pd_reset(PORT(cxt));
	fusb302_tcpm_set_rx_enable(PORT(cxt), 0);
	fusb302_tcpm_select_rp_value(PORT(cxt), TYPEC_RP_USB);
	fusb302_tcpm_set_cc(PORT(cxt), TYPEC_CC_RP);	// DFP mode
	STATE(cxt, DISCONNECTED);
}

static void send_power_request(struct vdm_context *cxt, uint32_t cap)
{
	int16_t hdr = PD_HEADER(PD_DATA_REQUEST, 0, 0, 0, 1, PD_REV20, 0);
	uint32_t req =
		(1L << 28) | // Object position (fixed 5V)
		(1L << 25) | // USB communications capable
		(0L << 10) | // 0mA operating
		(0L << 0);   // 0mA max

	fusb302_tcpm_transmit(PORT(cxt), TCPC_TX_SOP, hdr, &req);
	cprintf(cxt, ">REQUEST\n");
	(void)cap;
}

static void send_sink_cap(struct vdm_context *cxt)
{
	int16_t hdr = PD_HEADER(PD_DATA_SINK_CAP, 1, 1, 0, 1, PD_REV20, 0);
	uint32_t cap =
		(1L << 26) | // USB communications capable
		(0L << 10) | // 0mA operating
		(0L << 0);   // 0mA max

	fusb302_tcpm_transmit(PORT(cxt), TCPC_TX_SOP, hdr, &cap);
	cprintf(cxt, ">SINK_CAP\n");
	STATE(cxt, READY);
}

static void send_source_cap(struct vdm_context *cxt)
{
	int16_t hdr = PD_HEADER(PD_DATA_SOURCE_CAP, 1, 1, 0, 1, PD_REV20, 0);
	uint32_t cap = 0x37019096;

	fusb302_tcpm_transmit(PORT(cxt), TCPC_TX_SOP, hdr, &cap);
	cprintf(cxt, ">SOURCE_CAP\n");
	cxt->source_cap_timer = 0;
}

static void dump_msg(enum fusb302_rxfifo_tokens sop, int16_t hdr, uint32_t * msg)
{
	int16_t len = PD_HEADER_CNT(hdr);
	switch (sop) {
	case fusb302_TKN_SOP:
		printf("RX SOP (");
		break;
	case fusb302_TKN_SOP1:
		printf("RX SOP' (");
		break;
	case fusb302_TKN_SOP2:
		printf("RX SOP\" (");
		break;
	case fusb302_TKN_SOP1DB:
		printf("RX SOP'DEBUG (");
		break;
	case fusb302_TKN_SOP2DB:
		printf("RX SOP\"DEBUG (");
		break;
	default:
		printf("RX ? (");
		break;
	}

	printf("%d) [%x]", len, hdr);
	for (int16_t i = 0; i < PD_HEADER_CNT(hdr); i++) {
		printf(" ");
		printf("%x", msg[i]);
	}
	printf("\n");
}

static void handle_discover_identity(struct vdm_context *cxt)
{
	int16_t hdr = PD_HEADER(PD_DATA_VENDOR_DEF, 0, 0, 0, 4, PD_REV20, 0);
	uint32_t vdm[4] = {
		0xff008001L | (1L << 6),	// ACK

		(1L << 30) |	// USB Device
		(0L << 27) |	// UFP Product Type = Undefined
		(0L << 26) |	// No modal operation
		(0L << 23) |	// DFP Product Type = Undefined
		0x5acL,	// USB VID = Apple

		0L,		// XID

		(0x0001L << 16) |	// USB PID,
		0x100L	// bcdDevice
	};

	fusb302_tcpm_transmit(PORT(cxt), TCPC_TX_SOP, hdr, vdm);
	cprintf(cxt, ">VDM DISCOVER_IDENTITY\n");
}

static void handle_power_request(struct vdm_context *cxt, uint32_t req)
{
	int16_t hdr = PD_HEADER(PD_CTRL_ACCEPT, 1, 1, 0, 0, PD_REV20, 0);

	fusb302_tcpm_transmit(PORT(cxt), TCPC_TX_SOP, hdr, NULL);
	cprintf(cxt, ">ACCEPT\n");
	STATE(cxt, DFP_ACCEPT);
}

static void send_ps_rdy(struct vdm_context *cxt)
{
	int16_t hdr = PD_HEADER(PD_CTRL_PS_RDY, 1, 1, 0, 0, PD_REV20, 0);
	fusb302_tcpm_transmit(PORT(cxt), TCPC_TX_SOP, hdr, NULL);
	cprintf(cxt, ">PS_RDY\n");

	STATE(cxt, IDLE);
}

static void send_reject(struct vdm_context *cxt)
{
	int16_t hdr = PD_HEADER(PD_CTRL_REJECT, 1, 1, 0, 0, PD_REV20, 0);

	fusb302_tcpm_transmit(PORT(cxt), TCPC_TX_SOP, hdr, NULL);
	cprintf(cxt, ">REJECT\n");

	STATE(cxt, IDLE);
}

static void handle_vdm(struct vdm_context *cxt, enum fusb302_rxfifo_tokens sop,
		       int16_t hdr, uint32_t *msg)
{
	switch (*msg) {
	case 0xff008001:	// Structured VDM: DISCOVER IDENTITY
		cprintf(cxt, "<VDM DISCOVER_IDENTITY\n");
		handle_discover_identity(cxt);
		STATE(cxt, READY);
		break;
	default:
		cprintf(cxt, "<VDM ");
		dump_msg(sop, hdr, msg);
		break;
	}
}

static void handle_msg(struct vdm_context *cxt, enum fusb302_rxfifo_tokens sop,
		       int16_t hdr, uint32_t *msg)
{
	int16_t len = PD_HEADER_CNT(hdr);
	int16_t type = PD_HEADER_TYPE(hdr);

	if (len != 0) {
		switch (type) {
		case PD_DATA_SOURCE_CAP:
			cprintf(cxt, "<SOURCE_CAP: %x\n", msg[0]);
			send_power_request(cxt, msg[0]);
			break;
		case PD_DATA_REQUEST:
			cprintf(cxt, "<REQUEST: %x\n", msg[0]);
			handle_power_request(cxt, msg[0]);
			break;
		case PD_DATA_VENDOR_DEF:
			handle_vdm(cxt, sop, hdr, msg);
			break;
		default:
			cprintf(cxt, "<UNK DATA ");
			dump_msg(sop, hdr, msg);
			break;
		}
	} else {
		switch (type) {
		case PD_CTRL_ACCEPT:
			cprintf(cxt, "<ACCEPT\n");
			break;
		case PD_CTRL_REJECT:
			cprintf(cxt, "<REJECT\n");
			break;
		case PD_CTRL_PS_RDY:
			cprintf(cxt, "<PS_RDY\n");
			break;
		case PD_CTRL_PR_SWAP:
			cprintf(cxt, "<PR_SWAP\n");
			send_reject(cxt);
			break;
		case PD_CTRL_DR_SWAP:
			cprintf(cxt, "<DR_SWAP\n");
			send_reject(cxt);
			break;
		case PD_CTRL_GET_SINK_CAP:
			cprintf(cxt, "<GET_SINK_CAP\n");
			send_sink_cap(cxt);
			break;
		default:
			cprintf(cxt, "<UNK CTL ");
			dump_msg(sop, hdr, msg);
			break;
		}
	}
}

static void evt_packet(struct vdm_context *cxt)
{
	int16_t hdr, len, ret;
	enum fusb302_rxfifo_tokens sop;
	uint32_t msg[16];

	ret = fusb302_tcpm_get_message(PORT(cxt), msg, &hdr, &sop);
	if (ret) {
		// No packet or GoodCRC
		return;
	}

	handle_msg(cxt, sop, hdr, msg);
}

static void vdm_fun(struct vdm_context *cxt);

static void evt_sent(struct vdm_context *cxt)
{
	switch (cxt->state) {
	case STATE_DFP_VBUS_ON:
		STATE(cxt, DFP_CONNECTED);
		vdm_fun(cxt);
		break;
	case STATE_DFP_ACCEPT:
		send_ps_rdy(cxt);
		break;
	default:
		break;
	}
}

static void handle_irq(struct vdm_context *cxt)
{
	int16_t irq, irqa, irqb;
	fusb302_get_irq(PORT(cxt), &irq, &irqa, &irqb);

	cprintf(cxt, "IRQ=%x %x %x\n", irq, irqa, irqb);
	if (irq & TCPC_REG_INTERRUPT_VBUSOK) {
		cprintf(cxt, "IRQ: VBUSOK (VBUS=");
		if (fusb302_tcpm_get_vbus_level(PORT(cxt))) {
			cprintf(cxt, "ON)\n");
			send_source_cap(cxt);
			debug_poke(cxt);
		} else {
			cprintf(cxt, "OFF)\n");
			evt_disconnect(cxt);
		}
	}
	if (irqa & TCPC_REG_INTERRUPTA_HARDRESET) {
		cprintf(cxt, "IRQ: HARDRESET\n");
		evt_disconnect(cxt);
	}
	if (irqa & TCPC_REG_INTERRUPTA_TX_SUCCESS) {
		//cprintf(cxt, "IRQ: TXSUCCESS\n");
		evt_sent(cxt);
	}
	if (irqb & TCPC_REG_INTERRUPTB_GCRCSENT) {
		//cprintf(cxt, "IRQ: GCRCSENT\n");
		while (!fusb302_rx_fifo_is_empty(PORT(cxt)))
			evt_packet(cxt);
	}
}

static void vdm_fun(struct vdm_context *cxt)
{

	//uint32_t vdm[] = { 0x5ac8010 }; // Get Action List
	//uint32_t vdm[] = { 0x5ac8012, 0x0105, 0x8002<<16 }; // PMU Reset + DFU Hold
	//uint32_t vdm[] = { 0x5ac8011, 0x0809  }; // Get Action List
	//uint32_t vdm[] = { 0x5ac8012, 0x0105, 0x8000<<16 };

	// VDM to mux debug UART over SBU1/2
	uint32_t vdm[] = { 0x5AC8012, 0x01840306 };

	int16_t hdr = PD_HEADER(PD_DATA_VENDOR_DEF, 1, 1, 0, sizeof(vdm) / 4, PD_REV20, 0);
	fusb302_tcpm_transmit(PORT(cxt), TCPC_TX_SOP_DEBUG_PRIME_PRIME, hdr, vdm);
	cprintf(cxt, ">VDM serial -> SBU1/2\n");

}

void vdm_send_reboot(struct vdm_context *cxt)
{
	uint32_t vdm[] = { 0x5ac8012, 0x0105, 0x8000UL<<16 };
	int hdr = PD_HEADER(PD_DATA_VENDOR_DEF, 1, 1, 0, sizeof(vdm) / 4, PD_REV20, 0);
	fusb302_tcpm_transmit(PORT(cxt), TCPC_TX_SOP_DEBUG_PRIME_PRIME, hdr, vdm);
	cprintf(cxt, ">VDM SET ACTION reboot\n\r");
}

static void serial_out(struct vdm_context *cxt, char c)
{
	uart_putc_raw(UART(cxt), c);
}

static void help(void)
{
	printf("^_    Escape character\n"
	       "^_ ^_ Raw ^_\n"
	       "^_ ^@ Send break\n"
	       "^_ !  DUT reset\n"
	       "^_ ^R Central Scrutinizer reset\n"
	       "^_ ^^ Central Scrutinizer reset to programming mode\n"
	       "^_ ^D Toggle debug\n"
	       "^_ ^M Send empty debug VDM\n"
	       "^_ ?  This message\n");
	for (int i = 0; i < CONFIG_USB_PD_PORT_COUNT; i++)
		cprintf(&vdm_contexts[i], "%s\n",
			vdm_contexts[i].hw ? "present" : "absent");
}

static bool serial_handler(struct vdm_context *cxt)
{
	bool uart_active = false;
	int c;

	while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
		uart_active = true;

		if ((!cxt->vdm_escape && c != 0x1f)) {
			serial_out(cxt, c);
			continue;
		}

		switch (c) {
		case '!':			/* ! */
			vdm_send_reboot(cxt);
			break;
		case 0x12:			/* ^R */
			watchdog_enable(1, 1);
			break;
		case 0x1E:			/* ^^ */
			reset_usb_boot(1 << PICO_DEFAULT_LED_PIN,0);
			break;
		case 0x1F:			/* ^_ */
			if (!cxt->vdm_escape) {
				cxt->vdm_escape = true;
				continue;
			}

			serial_out(cxt, c);
			break;
		case 4:				/* ^D */
			cxt->verbose = !cxt->verbose;
			break;
		case 0:				/* ^@ */
			uart_set_break(UART(cxt), true);
			sleep_ms(1);
			uart_set_break(UART(cxt), false);
			break;
		case '\r':			/* Enter */
			debug_poke(cxt);
			break;
		case '?':
			help();
			break;
		}

		cxt->vdm_escape = false;
	}

	return uart_active;
}

static void state_machine(struct vdm_context *cxt)
{
	switch (cxt->state) {
	case STATE_DISCONNECTED:{
		int16_t cc1 = -1, cc2 = -1;
		fusb302_tcpm_get_cc(PORT(cxt), &cc1, &cc2);
		dprintf(cxt, "Poll: cc1=%d  cc2=%d\n", (int)cc1, (int)cc2);
		sleep_ms(200);
		if (cc1 >= 2 || cc2 >= 2)
			evt_dfpconnect(cxt);
		break;
	}
	case STATE_CONNECTED:{
		break;
	}
	case STATE_DFP_VBUS_ON:{
		if (++cxt->source_cap_timer > 37) {
			send_source_cap(cxt);
			debug_poke(cxt);
		}
		break;
	}
	case STATE_DFP_CONNECTED:{
		break;
	}
	case STATE_DFP_ACCEPT:{
		break;
	}
	case STATE_READY:{
		STATE(cxt, IDLE);
		break;
	}
	case STATE_IDLE:{
		break;
	}
	default:{
		cprintf(cxt, "Invalid state %d", cxt->state);
		cprintf(cxt, "\n");
	}
	}
	if (cxt->state != STATE_DISCONNECTED) {
		int16_t cc1 = -1, cc2 = -1;
		fusb302_tcpm_get_cc(PORT(cxt), &cc1, &cc2);
		if (cc1 < 2 && cc2 < 2) {
			if (cxt->cc_debounce++ > 5) {
				cprintf(cxt, "Disconnect: cc1=%d cc2=%d\n",
					cc1, cc2);
				evt_disconnect(cxt);
				cxt->cc_debounce = 0;
			}
		} else {
			cxt->cc_debounce = 0;
		}
	}
}

const struct hw_context *get_hw_from_port(int port)
{
	return vdm_contexts[port].hw;
}

static void fusb_int_handler(uint gpio, uint32_t event_mask)
{
	for (int i = 0; i < CONFIG_USB_PD_PORT_COUNT; i++) {
		struct vdm_context *cxt = &vdm_contexts[i];

		if (!cxt->hw)
			continue;

		if (gpio == PIN(cxt, FUSB_INT) &&
		    (event_mask & GPIO_IRQ_LEVEL_LOW)) {
			cxt->pending = true;
			gpio_set_irq_enabled(PIN(cxt, FUSB_INT), GPIO_IRQ_LEVEL_LOW, false);
		}
	}
}

void m1_pd_bmc_fusb_setup(unsigned int port,
			  const struct hw_context *hw)
{
	struct vdm_context *cxt;
	int16_t reg;

	if (port >= CONFIG_USB_PD_PORT_COUNT) {
		cprintf(cxt, "Bad port number %d\n", port);
		return;
	}

	cxt = vdm_contexts + port;
	*cxt = (struct vdm_context) {
		.hw			= hw,
		.state 			= STATE_DISCONNECTED,
		.source_cap_timer	= 0,
		.cc_debounce		= 0,
		.verbose		= false,
		.vdm_escape		= false,
	};

	/*
	 * If we can't infer pull-ups on the I2C, it is likely that
	 * nothing is connected and we'd better skip this port.
	 */
	if (!gpio_get(PIN(cxt, I2C_SCL)) || !gpio_get(PIN(cxt, I2C_SDA))) {
		cprintf(cxt, "I2C pins low while idling, skipping port\n");
		cxt->hw = NULL;
		return;
	}

	gpio_put(PIN(cxt, LED_G), HIGH);
	vbus_off(cxt);

	tcpc_read(PORT(cxt), TCPC_REG_DEVICE_ID, &reg);
	cprintf(cxt, "Device ID: 0x%x\n", reg);
	if (!(reg & 0x80)) {
		cprintf(cxt, "Invalid device ID. Is the FUSB302 alive?\n");
		cxt->hw = NULL;
		return;
	}

	cprintf(cxt, "Init\n");
	fusb302_tcpm_init(PORT(cxt));

	fusb302_pd_reset(PORT(cxt));
	fusb302_tcpm_set_rx_enable(PORT(cxt), 0);
	fusb302_tcpm_set_cc(PORT(cxt), TYPEC_CC_OPEN);
	sleep_ms(500);

	tcpc_read(PORT(cxt), TCPC_REG_STATUS0, &reg);
	cprintf(cxt, "STATUS0: 0x%x\n", reg);

	gpio_set_irq_enabled_with_callback(PIN(cxt, FUSB_INT), GPIO_IRQ_LEVEL_LOW, true,
					   fusb_int_handler);

	evt_disconnect(cxt);
	debug_poke(cxt);
}

static bool m1_pd_bmc_run_one(struct vdm_context *cxt)
{
	if (cxt->pending) {
		handle_irq(cxt);
		state_machine(cxt);
		cxt->pending = false;
		gpio_set_irq_enabled(PIN(cxt, FUSB_INT), GPIO_IRQ_LEVEL_LOW, true);
	}
		
	return serial_handler(cxt) || cxt->pending;
}

void m1_pd_bmc_run(void)
{
	int i;

	while (1) {
		bool busy = false;

		for (i = 0; i < CONFIG_USB_PD_PORT_COUNT; i++) {
			if (vdm_contexts[i].hw)
				busy |= m1_pd_bmc_run_one(&vdm_contexts[i]);
		}

		if (!busy)
			__wfi();
	}
}
