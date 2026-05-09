# Power Management / Remote Wake

This firmware supports waking the host PC via USB remote-wakeup when the DualSense reconnects.

## USB Descriptor

The USB configuration descriptor sets `bmAttributes` bit 5 (REMOTE_WAKEUP).

## Behavior

- When the host suspends USB, the dongle disconnects the controller over Bluetooth.
- While the host remains suspended, the dongle keeps scanning/accepting controller connections.
- When the controller reconnects, the dongle requests a USB remote-wakeup (`tud_remote_wakeup()`).

## Notes (Linux)

On Linux you may need to enable wakeup for the USB device:

- `echo enabled | sudo tee /sys/bus/usb/devices/<bus-port>/power/wakeup`

For persistence, install the provided udev rule:

- `udev/99-ds5dongle-wakeup.rules`
