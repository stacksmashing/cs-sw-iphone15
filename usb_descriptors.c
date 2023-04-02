#include "tusb.h"
#include "pico/unique_id.h"
#include "m1-pd-bmc.h"

/* Still a RPi Pico, but let's be creative anyway */
#define USBD_VID                (0x2e8a)
#define USBD_PID                (0x000a)

enum {
	USBD_STR_LANGUAGE,          // 0
	USBD_STR_MANUFACTURER,      // 1
	USBD_STR_PRODUCT,           // 2
	USBD_STR_SERIAL_NUMBER,     // 3
	USBD_STR_CDC_0_NAME,        // 4
	USBD_STR_CDC_1_NAME,        // 5
	USBD_STR_LAST,
};

static const tusb_desc_device_t usbd_desc_device = {
	.bLength                    = sizeof(tusb_desc_device_t),
	.bDescriptorType            = TUSB_DESC_DEVICE,
	.bcdUSB                     = 0x0200,
	.bDeviceClass               = TUSB_CLASS_MISC,
	.bDeviceSubClass            = MISC_SUBCLASS_COMMON,
	.bDeviceProtocol            = MISC_PROTOCOL_IAD,
	.bMaxPacketSize0            = CFG_TUD_ENDPOINT0_SIZE,
	.idVendor                   = USBD_VID,
	.idProduct                  = USBD_PID,
	.bcdDevice                  = 0x0100,
	.iManufacturer              = USBD_STR_MANUFACTURER,
	.iProduct                   = USBD_STR_PRODUCT,
	.iSerialNumber              = USBD_STR_SERIAL_NUMBER,
	.bNumConfigurations         = 1,
};

#define EPNUM_CDC_0_CMD         (0x81)
#define EPNUM_CDC_0_DATA        (0x82)

#define EPNUM_CDC_1_CMD         (0x83)
#define EPNUM_CDC_1_DATA        (0x84)

#define USBD_CDC_CMD_SIZE       (64)
#define USBD_CDC_DATA_SIZE      (64)

#define USBD_MAX_POWER_MA       (250)

#define USBD_DESC_LEN           (TUD_CONFIG_DESC_LEN + \
				 TUD_CDC_DESC_LEN * CFG_TUD_CDC)

enum {
	ITF_NUM_CDC_0,
	ITF_NUM_CDC_0_DATA,
	ITF_NUM_CDC_1,
	ITF_NUM_CDC_1_DATA,
	ITF_NUM_TOTAL,
};

static const uint8_t usbd_desc_cfg[USBD_DESC_LEN] = {

	TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL,
			      USBD_STR_LANGUAGE,
			      USBD_DESC_LEN,
			      TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,
			      USBD_MAX_POWER_MA),

	TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_0,
			   USBD_STR_CDC_0_NAME,
			   EPNUM_CDC_0_CMD,
			   USBD_CDC_CMD_SIZE,
			   EPNUM_CDC_0_DATA & 0x7F,
			   EPNUM_CDC_0_DATA,
			   USBD_CDC_DATA_SIZE),

	TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_1,
			   USBD_STR_CDC_1_NAME,
			   EPNUM_CDC_1_CMD,
			   USBD_CDC_CMD_SIZE,
			   EPNUM_CDC_1_DATA & 0x7F,
			   EPNUM_CDC_1_DATA,
			   USBD_CDC_DATA_SIZE),
};

const uint8_t *tud_descriptor_device_cb(void)
{
	return (const uint8_t *)&usbd_desc_device;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
	(void)index;
	return usbd_desc_cfg;
}

#define DESC_STR_MAX_LENGTH 20
#define USB_DESC_STRLEN(l)	((TUSB_DESC_STRING << 8) | ((l + 1) * 2))

static void str8_to_str16(const char *str, uint16_t *str16)
{
	int i;

	for (i = 0; i < DESC_STR_MAX_LENGTH - 1 && str[i]; i++)
		str16[i + 1] = str[i];

	str16[0] = USB_DESC_STRLEN(i);
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
	static uint16_t desc_str[DESC_STR_MAX_LENGTH];

	if (index >= USBD_STR_LAST)
		return NULL;

	switch (index) {
	case USBD_STR_LANGUAGE:
		desc_str[0] = USB_DESC_STRLEN(1);
		desc_str[1] = 0x0409; // English
		break;

	case USBD_STR_SERIAL_NUMBER: {
		char str[DESC_STR_MAX_LENGTH] = {};
		pico_unique_board_id_t id;

		pico_get_unique_board_id(&id);
		snprintf(str, DESC_STR_MAX_LENGTH,
			 "%02X%02X%02X%02X%02X%02X%02X%02X",
			 id.id[0], id.id[1], id.id[2], id.id[3],
			 id.id[4], id.id[5], id.id[6], id.id[7]);

		str8_to_str16(str, desc_str);
		break;
	}

	case USBD_STR_MANUFACTURER:
		str8_to_str16("AAAFNRAA", desc_str);
		break;

	case USBD_STR_PRODUCT:
		str8_to_str16("Central Scrutinizer", desc_str);
		break;

	case USBD_STR_CDC_0_NAME:
		str8_to_str16("Port-0", desc_str);
		break;

	case USBD_STR_CDC_1_NAME:
		str8_to_str16("Port-1", desc_str);
		break;
	}

	return desc_str;
}
