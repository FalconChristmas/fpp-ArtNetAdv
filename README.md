# fpp-ArtNetAdv

Advanced ArtNet features for [Falcon Player (FPP)](https://github.com/FalconChristmas/fpp), beyond
the plain ArtNet UDP channel output that FPP has built in.

## Features

- **ArtNet Timecode** — send and receive `ArtTimeCode` packets, so FPP can drive or follow other
  show software over the network.
- **ArtNet Triggers** — react to incoming `ArtTrigger` packets by running an FPP Command, and send
  `ArtTrigger` packets out from a playlist or event.

## Installation

Install from **Content Setup → Plugins** in the FPP web UI, then restart FPPD.

## Configuration

**Input/Output Setup → ArtNet Advanced** in the FPP web UI.

**Trigger settings**

- *Enable ArtNet Trigger Processing* — listen for incoming `ArtTrigger` packets.
- *OEM Code (hex) for Trigger* — only triggers carrying this OEM code are acted on.

**Timecode settings**

- *Enable ArtNet Timecode* — generate `ArtTimeCode` from the running playlist.
- *ArtNet TimeCode Type* — the frame rate the timecode is expressed in.
- *Time Code Position Processing* — whether the hour field is elapsed time, a 15-minute block, or
  defined per playlist item.
- *Target IP Addresses* — where to send the timecode packets.
- *Playlist to sync the ArtNet TimeCode* — the playlist to follow when receiving timecode.

## Commands

- **Send ArtNet Trigger** — broadcast or unicast an `ArtTrigger` packet. Arguments are the target
  IP, OEM code (hex), key, subkey, and a free-form payload.

## License

GPLv2 — see [LICENSE](LICENSE).
