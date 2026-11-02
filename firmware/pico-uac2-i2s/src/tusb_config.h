#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "usb_descriptors.h"

#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT      0
#endif

#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED   OPT_MODE_DEFAULT_SPEED
#endif

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS           OPT_OS_NONE
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG        0
#endif

#define CFG_TUD_ENABLED       1
#define CFG_TUD_MAX_SPEED     BOARD_TUD_MAX_SPEED

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN    __attribute__ ((aligned(4)))
#endif

// Enable host OS guessing so macOS (3-byte 10.14 feedback) and Windows/Linux (4-byte 16.16 feedback) both work seamlessly
#define CFG_QUIRK_OS_GUESSING 1

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE 64
#endif

//------------- CLASS -------------//
#define CFG_TUD_AUDIO         1
#define CFG_TUD_HID           0
#define CFG_TUD_CDC           0
#define CFG_TUD_MSC           0
#define CFG_TUD_MIDI          0
#define CFG_TUD_VENDOR        0

//--------------------------------------------------------------------
// AUDIO CLASS DRIVER CONFIGURATION (48 kHz, 24-bit Stereo in 3-byte slots)
//--------------------------------------------------------------------

#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN                        TUD_AUDIO_SPEAKER_STEREO_FB_DESC_LEN
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_FORMAT_CORRECTION      1

#define CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE                 48000
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX                   2

// 24-bit signed PCM packed in 3 bytes per sample (S24_3LE)
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX           3
#define CFG_TUD_AUDIO_FUNC_1_RESOLUTION_RX                   24

#define CFG_TUD_AUDIO_ENABLE_EP_OUT                          1

#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX    TUD_AUDIO_EP_SIZE(CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE, CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX, CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX)
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ (8 * CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX)

#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP                     1
#define CFG_TUD_AUDIO_ENABLE_INTERRUPT_EP                    1
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT                        1
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ                     64

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
