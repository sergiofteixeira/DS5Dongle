//
// Created by awalol on 2026/3/4.
//

#ifndef DS5_BRIDGE_USB_H
#define DS5_BRIDGE_USB_H

extern uint8_t mute[2]; // 0: SPEAKER(0x02) 1: MIC(0x05)
extern float volume[2]; // 0: SPEAKER(0x02) 1: MIC(0x05)

// Defer USB suspend / remote-wakeup actions to main loop.
void usb_pm_poll();

// Queue a remote-wakeup request (processed in usb_pm_poll).
void usb_remote_wakeup_request();

#endif //DS5_BRIDGE_USB_H
