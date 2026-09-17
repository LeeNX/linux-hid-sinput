# SInput Linux driver research

## 1. Protocol status

The Hand Held Legend SInput development document describes SInput as a targetable
HID setup intended to improve native SDL compatibility. It says `2E8A:10C6` may
be used for generic testing, while real devices should register their own PID.

The development document lists:

* up to 32 digital buttons
* 3-axis gyro
* 3-axis accelerometer
* 1 kHz polling
* stereo haptics
* player LEDs
* up to two touchpads

Important: the development document explicitly says aspects are subject to change.

## 2. SDL is currently the best executable protocol reference

SDL's `src/joystick/hidapi/SDL_hidapi_sinput.c` is a particularly useful reference
because it contains the actual packet offsets and capability handling used by a
large cross-platform project.

Current SDL source defines:

* input report ID `0x01`
* feature/command response input report ID `0x02`
* command output report ID `0x03`
* input report size 64 bytes
* command output size 48 bytes

Current state packet offsets include:

| Offset | Meaning |
|---:|---|
| 1 | plug/power status |
| 2 | charge level |
| 3-6 | four button bytes |
| 7-8 | left X |
| 9-10 | left Y |
| 11-12 | right X |
| 13-14 | right Y |
| 15-16 | left trigger |
| 17-18 | right trigger |
| 19-22 | IMU timestamp |
| 23-28 | accelerometer XYZ |
| 29-34 | gyro XYZ |
| 35-40 | touchpad 1 |
| 41-46 | touchpad 2 |

The values above are based on the current SDL implementation and should be
treated as protocol-reference data, not copied blindly into a future upstream
kernel driver.

## 3. Capability discovery

SDL's current implementation decodes feature information including:

* rumble
* player LEDs
* accelerometer
* gyroscope
* left/right sticks
* left/right analog triggers
* touchpad
* RGB LED
* handheld flag
* touchpad count/finger count
* polling rate
* subtype and mapping information

A kernel driver should use this capability information to decide which Linux
interfaces to register. Avoid creating a fixed six-axis device when the SInput
capability report says those axes do not exist.

## 4. Linux architecture

The Linux HID framework already supports transport-independent HID drivers. The
kernel documentation describes `hid_driver` callbacks such as `probe`, `remove`,
`raw_event`, and `input_configured`.

For a first DKMS experiment, this project intentionally uses:

    HID transport
        |
        v
    sinput HID driver
        |
        +--> evdev gamepad
        +--> future FF
        +--> future LEDs
        +--> future power_supply
        +--> future sensor input

A mature upstream driver should be compared closely with:

* `drivers/hid/hid-playstation.c`
* `drivers/hid/hid-nintendo.c`

Those drivers show how Linux-specific output features, force feedback, LEDs,
power and lifecycle handling can be integrated rather than exposing everything
through a generic userspace protocol.

## 5. Important design question: kernel driver vs SDL

SDL already has native SInput support. A kernel driver should not exist merely to
make SDL see the controller.

The useful Linux-native goals are:

* evdev compatibility
* FF through `/dev/input/event*`
* LED class integration
* `power_supply`
* sensor interfaces
* suspend/resume
* predictable behaviour for non-SDL applications

The driver should preserve the raw HID path where possible so that HIDAPI and
diagnostic tools remain useful.

## 6. Generic VID/PID strategy

For the development/test device:

    VID = 0x2E8A
    PID = 0x10C6

For production firmware, use the device's real VID/PID and add it to the driver
without making the generic test ID the permanent identity of unrelated products.

The initial module therefore matches the generic SInput ID only.

## 7. DKMS strategy

Keep this repository out-of-tree until the interface is stable.

DKMS should:

* build against the currently installed kernel headers;
* install one module under `/updates/dkms`;
* allow rapid testing across Ubuntu/Debian kernel versions;
* make it easy to remove the experiment.

Once the driver is mature, compare it against the kernel HID maintainer expectations
before considering an upstream submission.

## 8. Test plan

### Stage A - input

* USB connect/disconnect
* button press/release
* D-pad
* left/right stick
* analog triggers
* 1 kHz report stream
* no stuck keys after unplug

### Stage B - capabilities

* feature response retrieval
* no-IMU controller
* IMU controller
* one-stick controller
* two-stick controller
* digital trigger controller
* touchpad controller

### Stage C - output

* rumble
* player LED
* RGB LED
* command serialization
* timeout/error handling

### Stage D - power

* no battery
* charging
* charged
* discharging
* battery percentage changes

### Stage E - transport

* USB HID
* Bluetooth HID, if the SInput device supports it
* suspend/resume
* reconnect

### Stage F - HIL

Use the ESP32 SInput firmware as a deterministic test target. Capture known
reports and compare:

* raw report bytes
* kernel events
* SDL events
* timestamps
* output commands

## Sources

* SInput development specification:
  https://docs.handheldlegend.com/s/sinput/doc/sinput-hid-protocol-dev-SNSaEw36nc
* SDL SInput implementation:
  https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_sinput.c
* Linux HID introduction:
  https://www.kernel.org/doc/html/latest/hid/hidintro.html
* Linux HID configuration:
  https://github.com/torvalds/linux/blob/master/drivers/hid/Kconfig
* Linux PlayStation HID driver:
  https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c
* Linux Nintendo HID driver:
  https://github.com/torvalds/linux/blob/master/drivers/hid/hid-nintendo.c
