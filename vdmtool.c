#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
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
	bool				cc_line;
	uint8_t				serial_pin_set;
};

static struct vdm_context vdm_contexts[CONFIG_USB_PD_PORT_COUNT];

#define PIN(cxt, idx)	(cxt)->hw->pins[(idx)].pin
#define PORT(cxt)	((cxt) - vdm_contexts)
#define UART(cxt)	(cxt)->hw->uart

#define HIGH true
#define LOW false

#define cprintf_cont(cxt, str, ...)	do {				\
		__printf(PORT(cxt), str, ##__VA_ARGS__);		\
	} while(0)

#define cprintf(cxt, str, ...)	do {					\
		cprintf_cont(cxt,					\
			     "P%ld: " str, PORT(cxt), ##__VA_ARGS__);	\
	} while(0)

#define dprintf(cxt, ...)	do {					\
		if (cxt->verbose)					\
			cprintf(cxt, ##__VA_ARGS__);			\
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

	fusb302_tcpm_set_vconn(PORT(cxt), 0);

	fusb302_pd_reset(PORT(cxt));
	fusb302_tcpm_set_msg_header(PORT(cxt), 1, 1);	// Source
	cxt->cc_line = !(cc1 > cc2);
	fusb302_tcpm_set_polarity(PORT(cxt), cxt->cc_line);
	cprintf(cxt, "Polarity: CC%d (%s)\n",
		(int)cxt->cc_line + 1, cxt->cc_line ? "flipped" : "normal");

	/* If none of the CCs are disconnected, enable VCONN */
	if (cc1 && cc2) {
		fusb302_tcpm_set_vconn(PORT(cxt), 1);
		cprintf(cxt, "VCONN on CC%d\n", (int)cxt->cc_line + 1);
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
	fusb302_tcpm_set_vconn(PORT(cxt), 0);
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
	uint32_t cap = 1UL << 31; /* Variable non-battery PS, 0V, 0mA */

	fusb302_tcpm_transmit(PORT(cxt), TCPC_TX_SOP, hdr, &cap);
	cprintf(cxt, ">SOURCE_CAP\n");
	cxt->source_cap_timer = 0;
}

static void dump_msg(struct vdm_context *cxt,
		     enum fusb302_rxfifo_tokens sop, int16_t hdr, uint32_t * msg)
{
	int16_t len = PD_HEADER_CNT(hdr);
	switch (sop) {
	case fusb302_TKN_SOP:
		cprintf_cont(cxt, "RX SOP (");
		break;
	case fusb302_TKN_SOP1:
		cprintf_cont(cxt, "RX SOP' (");
		break;
	case fusb302_TKN_SOP2:
		cprintf_cont(cxt, "RX SOP\" (");
		break;
	case fusb302_TKN_SOP1DB:
		cprintf_cont(cxt, "RX SOP'DEBUG (");
		break;
	case fusb302_TKN_SOP2DB:
		cprintf_cont(cxt, "RX SOP\"DEBUG (");
		break;
	default:
		cprintf_cont(cxt, "RX ? (");
		break;
	}

	cprintf_cont(cxt, "%d) [%x]", len, hdr);
	for (int16_t i = 0; i < PD_HEADER_CNT(hdr); i++)
		cprintf_cont(cxt, " %x", msg[i]);

	cprintf_cont(cxt, "\n");
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
		dump_msg(cxt, sop, hdr, msg);
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
			dump_msg(cxt, sop, hdr, msg);
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
			dump_msg(cxt, sop, hdr, msg);
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

static void vdm_claim_serial(struct vdm_context *cxt);

static void evt_sent(struct vdm_context *cxt)
{
	switch (cxt->state) {
	case STATE_DFP_VBUS_ON:
		STATE(cxt, DFP_CONNECTED);
		vdm_claim_serial(cxt);
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
			cprintf_cont(cxt, "ON)\n");
			send_source_cap(cxt);
			debug_poke(cxt);
		} else {
			cprintf_cont(cxt, "OFF)\n");
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

static void vdm_send_msg(struct vdm_context *cxt, const uint32_t *vdm, int nr_u32)
{
	int16_t hdr = PD_HEADER(PD_DATA_VENDOR_DEF, 1, 1, 0, nr_u32, PD_REV20, 0);
	fusb302_tcpm_transmit(PORT(cxt), TCPC_TX_SOP_DEBUG_PRIME_PRIME, hdr, vdm);
}

static void vdm_pd_reset(struct vdm_context *cxt)
{
	uint32_t vdm[] = { 0x5ac8012, 0x0103, 0x8000<<16 };
	vdm_send_msg(cxt, vdm, ARRAY_SIZE(vdm));
	cprintf(cxt, ">VDM SET ACTION PD reset\n");
}

static void vdm_claim_serial(struct vdm_context *cxt)
{
	static const char *pinsets[] = {
		"AltUSB", "PrimUSB", "SBU1/2",
	};
	bool usb_serial, sbu_swap;

	//uint32_t vdm[] = { 0x5ac8010 }; // Get Action List
	//uint32_t vdm[] = { 0x5ac8012, 0x0105, 0x8002<<16 }; // PMU Reset + DFU Hold
	//uint32_t vdm[] = { 0x5ac8011, 0x0809  }; // Get Action List
	//uint32_t vdm[] = { 0x5ac8012, 0x0105, 0x8000<<16 };

	// VDM to mux debug UART over some set of pin...
	uint32_t vdm[] = { 0x5AC8012, 0x01800306 };

	vdm[1] |= 1 << (cxt->serial_pin_set + 16);

	vdm_send_msg(cxt, vdm, ARRAY_SIZE(vdm));
	cprintf(cxt, ">VDM serial -> %s\n", pinsets[cxt->serial_pin_set]);

	/* If using the SBU pins, swap the pins if using CC2. */
	sbu_swap = (cxt->serial_pin_set == 2) ? cxt->cc_line : LOW;
	usb_serial = (cxt->serial_pin_set == 1);

	gpio_put(PIN(cxt, SBU_SWAP), sbu_swap);
	gpio_put(PIN(cxt, SEL_USB), usb_serial);
	dprintf(cxt, "SBU_SWAP = %d, SEL_USB = %d\n", sbu_swap, usb_serial);
}

void vdm_send_reboot(struct vdm_context *cxt)
{
	uint32_t vdm[] = { 0x5ac8012, 0x0105, 0x8000UL<<16 };
	vdm_send_msg(cxt, vdm, ARRAY_SIZE(vdm));
	cprintf(cxt, ">VDM SET ACTION reboot\n");
}

static void serial_out(struct vdm_context *cxt, char c)
{
	uart_putc_raw(UART(cxt), c);
}

static void help(struct vdm_context *cxt)
{
	cprintf(cxt, "Current port\n"
		"^_    Escape character\n"
		"^_ ^_ Raw ^_\n"
		"^_ !  DUT reset\n"
		"^_ ^R Central Scrutinizer reset\n"
		"^_ ^^ Central Scrutinizer reset to programming mode\n"
		"^_ ^X Force disconnect\n"
		"^_ ^D Toggle debug\n"
		"^_ ^M Send empty debug VDM\n"
		"^_ 1  Serial on Primary USB pins\n"
		"^_ 2  Serial on SBU pins\n"
		"^_ ?  This message\n");
	for (int i = 0; i < CONFIG_USB_PD_PORT_COUNT; i++)
		cprintf(cxt, "Port %d: %s\n",
			PORT(&vdm_contexts[i]),
			vdm_contexts[i].hw ? "present" : "absent");
}

/* Break is handled as sideband data via the CDC layer */
void tud_cdc_send_break_cb(uint8_t itf, uint16_t duration_ms)
{
	struct vdm_context *cxt;

	if (itf > CONFIG_USB_PD_PORT_COUNT)
		return;

	cxt = &vdm_contexts[itf];

	if (!cxt->hw)
		return;

	/* Section 6.2.15 of the spec has the recipe */
	uart_set_break(UART(cxt), !!duration_ms);
	if (duration_ms && duration_ms != (uint16_t)~0) {
		sleep_ms(duration_ms);
		uart_set_break(UART(cxt), 0);
	}
}

static bool serial_handler(struct vdm_context *cxt)
{
	bool uart_active = false;
	int32_t c;

	while ((c = usb_rx_byte(PORT(cxt))) != -1) {
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
		case '\r':			/* Enter */
			debug_poke(cxt);
			break;
		case '1' ... '2':
			cxt->serial_pin_set = c - '0';
			vdm_pd_reset(cxt);
			break;
		case 0x18:			/* ^X */
			cxt->pending = true;
			evt_disconnect(cxt);
			break;
		case '?':
			help(cxt);
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
		cprintf(cxt, "Invalid state %d\n", cxt->state);
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

	if (port >= CONFIG_USB_PD_PORT_COUNT)
		return;

	cxt = vdm_contexts + port;
	*cxt = (struct vdm_context) {
		.hw			= hw,
		.state 			= STATE_DISCONNECTED,
		.source_cap_timer	= 0,
		.cc_debounce		= 0,
		.verbose		= false,
		.vdm_escape		= false,
		.serial_pin_set		= 2, /* SBU1/2 */
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
	/* No swapping */
	gpio_put(PIN(cxt, SBU_SWAP), LOW);
	/* USB2.0 pins routed to USB */
	gpio_put(PIN(cxt, SEL_USB), LOW);
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

#define for_each_cxt(___c)						\
	for (struct vdm_context *___c = &vdm_contexts[0];		\
	     (___c - vdm_contexts) < CONFIG_USB_PD_PORT_COUNT;		\
	     ___c++)							\
		if (___c->hw)

void m1_pd_bmc_run(void)
{
	int i;

	while (1) {
		bool busy = false;

		for_each_cxt(cxt) {
			gpio_put(PIN(cxt, LED_G), HIGH);
			busy |= m1_pd_bmc_run_one(cxt);
			irq_set_enabled(cxt->hw->uart_irq, false);
		}

		tud_task();

		for_each_cxt(cxt)
			irq_set_enabled(cxt->hw->uart_irq, true);

		if (busy)
			continue;

		for_each_cxt(cxt)
			gpio_put(PIN(cxt, LED_G), LOW);

		__wfe();
	}
}
