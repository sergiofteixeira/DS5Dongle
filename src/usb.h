//
// Created by awalol on 2026/3/4.
//

#ifndef DS5_BRIDGE_USB_H
#define DS5_BRIDGE_USB_H

extern uint8_t mute[2]; // 0: SPEAKER(0x02) 1: MIC(0x05)
extern float volume[2]; // 0: SPEAKER(0x02) 1: MIC(0x05)

// Initialize PM state (call once from main after tusb_init()).
void usb_pm_init();

// Defer USB suspend / remote-wakeup actions to main loop.
void usb_pm_poll();

// Queue a remote-wakeup request (processed in usb_pm_poll).
void usb_remote_wakeup_request();

// Cached suspend state (updated by TinyUSB callbacks).
bool usb_is_suspended_cached();

// Request USB connect/disconnect from non-USB contexts.
void usb_request_connect();
void usb_request_disconnect();
void usb_request_reconnect(uint32_t delay_ms);

#endif //DS5_BRIDGE_USB_H
