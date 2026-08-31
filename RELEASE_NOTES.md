# CloudPad Switch release notes

## alpha 0.0.1

The first public alpha build of CloudPad for Nintendo Switch.

### Highlights

- Browse and launch PlayStation cloud games from PS3, PS4, and PS5 libraries.
- Stream at 1080p with 30 or 60 FPS options and configurable bitrate.
- Hardware-accelerated HEVC video decoding on Nintendo Switch.
- More efficient 60 FPS presentation that avoids duplicate 1080p texture uploads, reducing CPU/GPU overhead and the need for maximum CPU overclocking.
- Configurable sharpening, video pacing, haptics, and adaptive triggers.
- CloudPad branding and hbmenu metadata, including the CloudPad icon and `Chazq` author credit.
- The Nintendo Switch release artifact is named `cloudpad.nro`.
- Press ZL + ZR + Plus while streaming to close the stream and return to CloudPad.

### Alpha notes

- A PlayStation account NPSSO token is required to sign in.
- Network quality and Sony's variable-bitrate encoder determine the bitrate achieved in practice.
- This is an early alpha release; stability and compatibility may vary between games and network environments.
