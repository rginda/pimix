#include "quirk_os_guessing.h"

static tusb_desc_type_t desc_req_buf[2];
static int desc_req_idx = 0;

void quirk_os_guessing_desc_device_cb(void) {
  desc_req_idx = 0;
}

void quirk_os_guessing_desc_configuration_cb(void) {
  if (desc_req_idx == 0 || (desc_req_idx == 1 && desc_req_buf[0] != TUSB_DESC_CONFIGURATION)) {
    desc_req_buf[desc_req_idx++] = TUSB_DESC_CONFIGURATION;
  }
}

void quirk_os_guessing_desc_bos_cb(void) {
  if (desc_req_idx == 0 || (desc_req_idx == 1 && desc_req_buf[0] != TUSB_DESC_BOS)) {
    desc_req_buf[desc_req_idx++] = TUSB_DESC_BOS;
  }
}

void quirk_os_guessing_desc_string_cb(void) {
  if (desc_req_idx == 0 || (desc_req_idx == 1 && desc_req_buf[0] != TUSB_DESC_STRING)) {
    desc_req_buf[desc_req_idx++] = TUSB_DESC_STRING;
  }
}

quirk_os_guessing_t quirk_os_guessing_get(void) {
  if (desc_req_idx < 2) {
    return QUIRK_OS_GUESSING_UNKNOWN;
  }

  if (desc_req_buf[0] == TUSB_DESC_BOS && desc_req_buf[1] == TUSB_DESC_CONFIGURATION) {
    return QUIRK_OS_GUESSING_LINUX;
  } else if (desc_req_buf[0] == TUSB_DESC_CONFIGURATION && desc_req_buf[1] == TUSB_DESC_BOS) {
    return QUIRK_OS_GUESSING_WINDOWS;
  } else if (desc_req_buf[0] == TUSB_DESC_STRING && (desc_req_buf[1] == TUSB_DESC_BOS || desc_req_buf[1] == TUSB_DESC_CONFIGURATION)) {
    return QUIRK_OS_GUESSING_OSX;
  }

  return QUIRK_OS_GUESSING_UNKNOWN;
}
