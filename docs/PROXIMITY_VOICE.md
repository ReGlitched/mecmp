# Proximity chat

Enable **Players → Proximity chat → Enable proximity chat** on each PC, join the
same MECMP session and enter free roam. Hold **N** to transmit. Change the key
(including mouse side buttons) or assign a gamepad combination in
**Controls → Voice chat**. The default hearing range is 35 metres.

The Players tab has microphone mute, optional open microphone, voice range,
receive volume, microphone gain, input level/status, retry, and a mute checkbox
for each player. Player mutes last for the current session/level. With your
microphone muted, incoming speech still plays. The input is Windows' default
communications microphone; select that device in Windows sound settings.
While playing, a corner indicator shows transmitting microphone level and nearby
players who are speaking. It follows the overlay hide setting.

The microphone starts only while transmission is requested, proximity chat is
enabled and multiplayer/free roam are active. Capture and transport can continue
while the local native playback source is initializing.
Menus, text chat and loss of game focus stop capture and discard partial speech.
Open microphone is off by default. Speech is never written to disk or monitored
back to the speaker. Steam, Steam authentication, Mumble and EA matchmaking are
not involved; MECMP's existing custom player names identify speakers.

Use the updated DLL on all clients. A dedicated server also needs the updated
`mecmp_server.exe`; old servers do not forward voice. Player-hosted sessions use
the same implementation directly in the updated DLL. Existing movement sounds,
effort vocals and combat audio remain separate from microphone speech.

Settings in `mecmp.ini`:

```ini
[voice]
enabled=0
microphone=1
open_mic=0
talk_key=N
talk_pad=
distance=35
volume=1
microphone_gain=1
```

## Implementation

`game/voice_chat.cpp` owns WASAPI shared-mode microphone capture on its own COM
thread. Windows converts the input to 16 kHz, mono, signed 16-bit PCM. Twenty
milliseconds (320 samples) form one independent mu-law block; `net/voice.h`
contains the bounded format and codec. There is no external codec/runtime DLL.

Optional protocol-7 packet type 16 contains `u32 stream | u32 sequence | 320 bytes`.
Player movement audio uses type 15; rendezvous hole punching uses type 14.
Including the ordinary 16-byte header, every packet is 344 bytes. Speech has its
own sequence space and is sent once, every 20 ms, without retransmission or UDP
fragmentation. It does not advance movement's sequence/loss counter. Hosts and
dedicated servers validate the exact size and nonzero stream ID, accept voice
only from admitted members under their bound session ID, and enforce a separate
55 packets/second bucket with a 12-packet burst. The ordinary movement/event
limit is unchanged. Application traffic is about 17.2 kB/s per active speaker.

Outgoing queues retain at most six blocks and discard anything over 120 ms old.
The shared receive queue is bounded to 128 packets; playback drops stale packets
and accepts at most 15 speakers. Each speaker has a fixed 16-block jitter window,
a 60 ms initial delay, sequence-based reordering/deduplication and silence for
missing blocks. Streams, partial microphone buffers and playback clear across
session/level changes and when voice is disabled. An idle source expires at 300 ms.

`game/native_voice.cpp` feeds decoded speech into a separate **native spatial
sound instance for each speaker**, before Catalyst's low-pass, panning, mixer sends
and room reverb. It no longer adds speech to the final DAC output. The DAC hook
only observes the current device sample rate and mixer heartbeat.

The native source uses a private copy of Faith's simple PipeRelease sound graph:
one sampler and one cone output. The output transform source changes from
**Listener** to **Sound**, so it follows the remote runner's mouth position.
Obstruction/occlusion high-frequency damping is enabled, along with the existing
native reverb send. The engine owns positional mixing, environmental queries,
filtering and reverb. The effect depends on the level's authored acoustic data.

The copied sampler uses an unconnected unity-amplitude input. Muting that input
would also mute injected speech downstream. Its native output voice stays scheduled
while speech is active; the isolated instance's `SndPlayer0` PCM callback replaces
the game sample with silence or decoded speech and supplies
microphone samples before the downstream spatial modules. Original native FIFO
bookkeeping is preserved. All other instances use the original callback unchanged.
There is no external playback device, synthetic echo, wall raycast filter or
replacement of an existing player's sound. Shared game assets stay untouched.

MECMP applies the voice-volume setting with a 0.8 receive-gain multiplier,
making speech 20% quieter at close range for existing and new settings. The
distance fade stays at full near gain within 2 metres, then follows
`clamp((range - distance) / (range - 2), 0, 1)^1.5` and reaches silence at
the selected hearing range. This falls off more strongly than the previous
linear fade. A 5 ms gain ramp smooths changes. The native graph provides
direction and environmental
attenuation. The camera's third-person offset does not change MECMP's hearing
range. PCM is resampled from 16 kHz to the game's rate before entering its mono
source buffer; the game handles final device channel layout and conversion.

Native sound creation, mouth-position updates and stop/release run from the client
replay updater. The audio callback uses fixed storage, makes no network calls or
allocations, and only tries a nonblocking lock. A missed lock produces silence.
Capture, decoding and network-state reads happen on the voice worker. Disabling
voice, leaving the level/session, muting, stale input or out-of-range players
clear playback and retire their native instances. Small copied graph descriptions
remain allocated for DLL lifetime because native audio commands are deferred.

An unsupported native descriptor or unavailable template leaves voice inactive
with status in the menu. There is no fallback to adding dry speech at the DAC.
The menu also reports sent/received voice packets, current/created native sources,
and processed PCM blocks, so transport and native playback failures are distinguishable.

## Supported PC native contract

Image VAs at base `0x140000000`:

| Purpose | Native address/layout |
|---|---|
| Native PCM source descriptor slot | `0x1420fddc8` → `0x142e17d10` (`SndPlayer0`) |
| Existing kernel dispatch | `0x142dcf300`, calls processing descriptor `+0x10` |
| Native sound instance / create | vtable `0x141acfad0`; system `+0x40` → `0x142fe2ee0` |
| Sound description / channel | instance `+0x28` / `+0x30` |
| Native sound / source kernel | channel `+0x20`; sound `+0x1b8` → VoiceRef, kernel `+8` |
| Native source identity | sound `+0x198` = description; description `+0x90` = private asset |
| Planar output buffer / frame count | mixer `+0xc490` / `+0xc4b8` |
| Source rate / channel count | mixer `+0xc4bc` / `+0xc4c4` |
| Output transform source | OutputNodeData `+0x54`: Sound=0, Listener=1 |
| Obstruction / occlusion damping | OutputNodeData `+0x3c` / `+0x40` |
| Reverb gain / native send | ConeOutputNodeData `+0xf0` / `+0xf8` |
| Device heartbeat descriptor | `0x1420fa368` → `0x142dfad40`; does not mix speech |

Read-only inspection confirmed native instances, descriptions, AudioCore VoiceRef
and module Kernel relationships in the running game. Factory, sampler, spatial
output and native FIFO/processing paths were traced in the executable. This is
structural verification; the new spatial voice path has not been listened to in game.

Validation: Release build only. No automated tests, gameplay tests, microphone
recording or voice playback tests were run.
