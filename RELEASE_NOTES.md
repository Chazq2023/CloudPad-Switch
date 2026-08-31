# CloudPad Switch release notes

## alpha 0.0.1

The first public alpha build of CloudPad for Nintendo Switch.

### Highlights

- Browse and launch PlayStation cloud games from PS3, PS4, and PS5 libraries.
- Stream PS5 games at 1080p and PS3/PS4 games at 720p, all at 60 FPS with configurable bitrate.
- Hardware-accelerated HEVC video decoding on Nintendo Switch.
- More efficient 60 FPS presentation that avoids duplicate 1080p texture uploads, reducing CPU/GPU overhead and the need for maximum CPU overclocking.
- Improved PS3/PS4 H.264 quality and performance with hardware deblocking, safer error handling, asynchronous GPU presentation, and reduced diagnostic logging overhead.
- Cloud bitrate choices now cap Sony's allocation bandwidth and game specification as well as the local decoder profile, preventing PS3/PS4 sessions from being provisioned far above the selected bitrate.
- Configurable sharpening, video pacing, haptics, and adaptive triggers.
- CloudPad branding and hbmenu metadata, including the CloudPad icon and `Chazq` author credit.
- The Nintendo Switch release artifact is named `cloudpad.nro`.
- Press ZL + ZR + Plus while streaming to close the stream and return to CloudPad.
- CloudPad's Select-modifier touchpad mappings are available on PS4/PS5 streams; PS3 uses Minus as Select, with Minus + Plus opening PS Home/XMB on every platform.

### Alpha notes

- A PlayStation account NPSSO token is required to sign in.
- Network quality and Sony's variable-bitrate encoder determine the bitrate achieved in practice.
- This is an early alpha release; stability and compatibility may vary between games and network environments.
