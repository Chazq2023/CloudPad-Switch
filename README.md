# CloudPad for Nintendo Switch

CloudPad is a Nintendo Switch homebrew application for PlayStation cloud gaming. It lets you browse and stream supported games from your PS3, PS4, and PS5 cloud libraries at 60 FPS.

Resolution is fixed per cloud library: PS5 streams at 1080p, while PS4 and PS3 stream at 720p.

CloudPad is an unofficial community project and is not affiliated with or endorsed by Sony Interactive Entertainment.

## Performance requirement

A Nintendo Switch CPU overclock to **1785 MHz** is currently required for a smooth streaming experience. Performance at lower CPU clock speeds is still being improved.

## Default controller mappings

CloudPad for Switch follows the default controller mappings used by [CloudPad for Android](https://github.com/Chazq2023/CloudPad-Android):

| Nintendo Switch input | PlayStation input |
| --- | --- |
| A | Circle |
| B | Cross |
| X | Triangle |
| Y | Square |
| L | L1 |
| R | R1 |
| ZL | L2 |
| ZR | R2 |
| Left Stick button | L3 |
| Right Stick button | R3 |
| Plus | Options / Start |
| Minus (PS3) | Select |
| Tap Minus (PS4/PS5) | Touchpad click |
| Hold Minus (PS4/PS5) | Touchpad click and hold |
| Minus + L (PS4/PS5) | Touchpad left click |
| Minus + R (PS4/PS5) | Touchpad right click |
| Minus + X (PS4/PS5) | Touchpad swipe up |
| Minus + Y (PS4/PS5) | Touchpad swipe left |
| Minus + A (PS4/PS5) | Touchpad swipe right |
| Minus + B (PS4/PS5) | Touchpad swipe down |
| Minus + Plus | PS Home |
| D-Pad | D-Pad |
| Left Stick | Left Stick |
| Right Stick | Right Stick |
| Switch touchscreen | PlayStation touchpad |
| ZL + ZR + Plus | Close the stream and return to CloudPad |

The Switch touchscreen maps its position to the PlayStation touchpad and supports touchpad clicks and gestures.

## Install

Download `cloudpad.nro` from the latest release and place it in:

```text
/switch/cloudpad/cloudpad.nro
```

Launch CloudPad from the Homebrew Menu.

## Build

Docker is required. Run:

```sh
./scripts/switch/run-docker-build-chiaki.sh
```

The finished application is written to `build_switch/switch/cloudpad.nro`.
