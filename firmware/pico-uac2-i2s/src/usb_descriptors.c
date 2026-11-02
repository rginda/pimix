#include <stdio.h>
#include <string.h>
#include "bsp/board_api.h"
#include "pico/unique_id.h"
#include "tusb.h"
#include "usb_descriptors.h"
#include "quirk_os_guessing.h"

#define USB_VID   0xCafe
#define USB_PID   0x4010

tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0201,

    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void) {
    quirk_os_guessing_desc_device_cb();
    return (uint8_t const *)&desc_device;
}

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_AUDIO_SPEAKER_STEREO_FB_DESC_LEN)

#if defined(TUD_ENDPOINT_ONE_DIRECTION_ONLY)
  #define EPNUM_AUDIO_FB    0x01
  #define EPNUM_AUDIO_OUT   0x02
  #define EPNUM_AUDIO_INT   0x03
#else
  #define EPNUM_AUDIO_FB    0x01
  #define EPNUM_AUDIO_OUT   0x01
  #define EPNUM_AUDIO_INT   0x03
#endif

uint8_t const desc_configuration_default[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_AUDIO_SPEAKER_STEREO_FB_DESCRIPTOR(
        0, 4,
        CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX,
        CFG_TUD_AUDIO_FUNC_1_RESOLUTION_RX,
        EPNUM_AUDIO_OUT,
        CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX,
        EPNUM_AUDIO_FB | 0x80,
        4,
        EPNUM_AUDIO_INT | 0x80)
};

// macOS requires a 3-byte (10.14) feedback endpoint on Full-Speed USB
uint8_t const desc_configuration_osx_fs[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_AUDIO_SPEAKER_STEREO_FB_DESCRIPTOR(
        0, 4,
        CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX,
        CFG_TUD_AUDIO_FUNC_1_RESOLUTION_RX,
        EPNUM_AUDIO_OUT,
        CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX,
        EPNUM_AUDIO_FB | 0x80,
        3,
        EPNUM_AUDIO_INT | 0x80)
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    quirk_os_guessing_desc_configuration_cb();
    if (tud_speed_get() == TUSB_SPEED_FULL && quirk_os_guessing_get() == QUIRK_OS_GUESSING_OSX) {
        return desc_configuration_osx_fs;
    }
    return desc_configuration_default;
}

#define TUD_BOS_USB20_EXT_DESC_LEN 7
#define BOS_TOTAL_LEN              (TUD_BOS_DESC_LEN + TUD_BOS_USB20_EXT_DESC_LEN)

uint8_t const desc_bos[] = {
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),
    0x07, TUSB_DESC_DEVICE_CAPABILITY, DEVICE_CAPABILITY_USB20_EXTENSION, 0x00, 0x00, 0x00, 0x00
};

uint8_t const *tud_descriptor_bos_cb(void) {
    quirk_os_guessing_desc_bos_cb();
    return desc_bos;
}

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_AUDIO_ITF,
};

static uint16_t _desc_str[32 + 1];
static char product_name_buf[32];

static const char *get_pimix_product_name(void) {
    char uid_str[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    pico_get_unique_board_id_string(uid_str, sizeof(uid_str));

    // Match known Pico Flash IDs to deterministic channel numbers
    // E66164084335BC2E is wired to Pi 5 GPIO 20 (SDI0 / Channels 0-1)
    // E66164084354A62E is wired to Pi 5 GPIO 22 (SDI1 / Channels 2-3)
    if (strcmp(uid_str, "E66164084335BC2E") == 0) {
        return "PiMix USB 1";
    } else if (strcmp(uid_str, "E66164084354A62E") == 0) {
        return "PiMix USB 2";
    }

    size_t len = strlen(uid_str);
    const char *suffix = (len >= 4) ? (uid_str + len - 4) : uid_str;
    snprintf(product_name_buf, sizeof(product_name_buf), "PiMix USB (%s)", suffix);
    return product_name_buf;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    size_t chr_count = 0;

    quirk_os_guessing_desc_string_cb();

    switch (index) {
        case STRID_LANGID:
            _desc_str[1] = 0x0409; // English
            chr_count = 1;
            break;

        case STRID_SERIAL:
            chr_count = board_usb_get_serial(_desc_str + 1, 32);
            break;

        case STRID_MANUFACTURER: {
            const char *str = "PiMix Audio";
            chr_count = strlen(str);
            for (size_t i = 0; i < chr_count; i++) {
                _desc_str[1 + i] = str[i];
            }
            break;
        }

        case STRID_PRODUCT:
        case STRID_AUDIO_ITF: {
            const char *str = get_pimix_product_name();
            chr_count = strlen(str);
            size_t const max_count = sizeof(_desc_str) / sizeof(_desc_str[0]) - 1;
            if (chr_count > max_count) chr_count = max_count;
            for (size_t i = 0; i < chr_count; i++) {
                _desc_str[1 + i] = str[i];
            }
            break;
        }

        default:
            return NULL;
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}
