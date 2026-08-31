# CloudPad-Switch

A Nintendo Switch homebrew port of [CloudPad](https://github.com/Chazq2023/CloudPad-Android), a PS4/PS5 Remote
Play client descended from [Chiaki](https://git.sr.ht/~thestr4ng3r/chiaki) / [chiaki-ng](https://github.com/streetpea/chiaki-ng).
It streams from your PlayStation console to your Switch over LAN (or the internet, with Remote Play over
Internet enabled on the console) using [Borealis](https://github.com/xfangfang/borealis) for the on-console UI.

This repo vendors the parts of CloudPad-Android needed for the Switch build — `switch/` (the Borealis app),
`lib/` (the portable Chiaki protocol/streaming core), and the `third-party/` pieces the core library depends
on (nanopb, jerasure, gf-complete) — so it can be built standalone without the Android/Qt/iOS/macOS/Steam Deck
parts of the upstream tree.

## Building

The build runs inside a pinned devkitPro container (`docker.io/xlanor/chiaki-ng-switch-builder:latest`), the
same one chiaki-ng's own Switch port uses, so no local devkitPro install is required — just Docker.

```
bash scripts/switch/run-docker-build-chiaki.sh
```

This produces `build_switch/switch/cloudpad.nro`.

## Installing / testing on a real Switch

1. On the Switch, open the Homebrew Menu and press **Y** to start Homebrew Netloader.
2. From this repo, push the build over the network:
   ```
   bash scripts/switch/push-docker-build-chiaki.sh -a <switch-ip>
   ```
3. Launch the app from the Homebrew Menu once it's received.

On first run it needs to be paired with your PS4/PS5 (via the console's Remote Play registration flow) before
it can stream — see `switch/README.md` for the `chiaki.conf` fields (console name, IP, PSN online ID / account
ID) if you need to set them up manually.

## Status

Work in progress. The Switch/Borealis frontend and core library build cleanly, but this hasn't yet been
verified streaming against a real PS4/PS5 on real hardware.
