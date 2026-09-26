# Google TV Remote for Flipper Zero

A Flipper Zero remote for **Chromecast with Google TV** (voice remote G9N9N) and similar Google TV devices, with **Epson projector power** over infrared.

Google TV is controlled over Bluetooth HID, so no line of sight is needed. The projector's On/Off uses the Flipper's own IR transmitter, the same way the Google remote's IR blaster does.

Runs on the official Flipper Zero firmware. Built against firmware **1.4.3 / API 87.1**.

Independent project, not affiliated with Google, Epson or Flipper Devices.

## Install and pair

1. Build it (see below) or download `google_tv_remote.fap` from the GitHub Actions artifacts.
2. Copy it to `SD Card/apps/Bluetooth/` with qFlipper.
3. On the Flipper, open **Apps > Bluetooth > Google TV**.
4. On Google TV, open **Settings > Remotes & Accessories > Pair remote or accessory** and choose `Control <your Flipper name>`.
5. Confirm the pairing code if asked.

**Bluetooth must be on** in the Flipper's Settings > Bluetooth, or the Flipper will not advertise. On the Chromecast pairing screen, ignore the "hold Back and Home" hint, which is for Google's own remote. The Flipper appears in the list of found devices on that same screen.

The app shares the official Bluetooth Remote app's pairing store, so an existing pairing is reused.

## Controls

| Flipper button | Action |
| --- | --- |
| Arrows | D-pad; hold to repeat |
| OK | Select |
| Back | Back |
| Hold OK | Actions menu |
| Hold Back | Exit app |

Actions menu:

| Item | Sent as |
| --- | --- |
| Home | BLE HID consumer AC Home |
| Volume + / - | BLE HID consumer volume; Left/Right in the menu also adjusts volume |
| Mute | BLE HID consumer mute |
| Play / Pause | BLE HID consumer play/pause |
| Search / Assistant | BLE HID consumer AC Search. Opens search; there is no microphone |
| Projector power (IR) | Epson NECext `83 55` / `90 6F`. Press twice to turn off, as with the Epson remote |
| Google TV power | BLE HID consumer power |
| Back to remote | Closes the menu |

The projector action works even when Bluetooth is not connected.

## Mapping to the Google remote

| Google remote button | Flipper app |
| --- | --- |
| D-pad, select | Arrows, OK |
| Back, Home | Back, menu Home |
| Assistant | menu Search / Assistant |
| Mute, side volume | menu Mute, Volume, or Left/Right in the menu |
| Power (projector) | menu Projector power (IR) |
| YouTube, Netflix, Input | **Not supported**, see below |

## Limitations

**YouTube, Netflix and Input buttons are not implemented.** The Google remote sends consumer codes `0x0C0077`, `0x0C0078` and `0x0C01BB` for these. Android only maps them in its key layout for Google's own remote (`Vendor_0957_Product_0001.kl`). The Flipper SDK does not let an app change its Bluetooth vendor and product ID, so Google TV would ignore these codes from the Flipper.

**Other Epson models** may use a different power code. The firmware's projector library also lists NECext `81 03` / `F0 0F` for some Epson projectors. Change `EPSON_IR_ADDRESS` and `EPSON_IR_COMMAND` in `google_tv_remote.c` if yours does not respond.

**Volume** goes to Google TV, which forwards it to the projector or sound system only if HDMI-CEC or the remote's IR setup is configured there.

## Build

```sh
pipx install ufbt
ufbt update --branch=1.4.3
ufbt            # builds dist/google_tv_remote.fap
ufbt launch     # installs and runs it on a USB-connected Flipper (close qFlipper first)
```

## Credits and license

The Bluetooth setup and app structure are adapted from [flipper-apple-tv-remote](https://github.com/KronenbergBN/flipper-apple-tv-remote) by Hanns Kronenberg, which is GPL-3.0-only. This project is therefore also GPL-3.0-only. See [LICENSE](LICENSE).

Epson IR codes come from the official firmware's `projector.ir` library.
