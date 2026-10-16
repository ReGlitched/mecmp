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
enabled and free roam is active with a multiplayer session.
Capture and transport can continue while the local
native playback source is initializing.
Menus, text chat and loss of game focus stop capture and discard partial speech.
Open microphone is off by default. Speech is never written to disk or monitored
back to the speaker. Steam,
Steam authentication, Mumble and EA matchmaking are not involved; MECMP's existing custom player names identify speakers.

Use the updated DLL on all clients. A dedicated server also needs the updated
`mecmp_server.exe` and a restart. This build uses 16 kHz mono mu-law voice on
packet type 16. Type 17 (48 kHz/16-bit PCM) is incompatible and ignored. Player-hosted sessions use
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
milliseconds (320 samples) form one independent block, companded to 320 bytes
with the built-in mu-law encoder in `net/voice.h`. There is no external codec or
runtime DLL. Capture gain is applied in float and peak-limited before companding,
avoiding software hard clipping when microphone gain is raised. The lower speech
bandwidth retains the corrected native spatial and reverb-only send routing.

Optional protocol-7 packet type 16 contains `u32 stream | u32 sequence | 320 bytes`.
Type 17 PCM voice is retired. Player movement audio uses type 15;
rendezvous hole punching uses type 14. Including the ordinary 16-byte header,
every voice packet is 344 bytes, within the existing 1024-byte transport limit.
Speech has its own sequence space and is sent once, every 20 ms, without
retransmission or UDP fragmentation. It does not advance movement's sequence/loss
counter. Hosts and dedicated servers validate the exact size and nonzero stream ID,
accept voice only from admitted members under their bound session ID, and enforce
a separate 55 packets/second bucket with a 6-packet burst (120 ms). The ordinary
movement/event limit is unchanged. Companded audio is 128 kb/s; application traffic
is about 17.2 kB/s per active speaker before UDP/IP headers, down from 98.4 kB/s
for the previous PCM format.

Outgoing queues retain at most 6 blocks and discard anything over 120 ms old.
The shared receive queue is bounded to 128 packets; playback drops stale packets
and accepts at most 15 remote speakers.
Each speaker has a fixed 16-block jitter window (320 ms), a 60 ms initial delay,
sequence-based reordering/deduplication and silence for missing blocks. Streams,
partial microphone buffers and playback clear across session/level changes and
when voice is disabled. An idle source expires at 300 ms.

`game/native_voice.cpp` feeds decoded speech into a separate **native spatial
sound instance for each speaker**, before Catalyst's low-pass, panning, mixer sends
and room reverb. It no longer adds speech to the final DAC output. The DAC hook
only observes the current device sample rate and mixer heartbeat.

The native source uses a private copy of Faith's simple PipeRelease sound graph:
one sampler and one cone output. The output transform source changes from
**Listener** to **Sound**, so it follows the remote runner's mouth position.
The private cone also disables the template's near-listener bleed: its native
panner radius is `(max(distance, min) - min) / (max - min)`, and a radius of at
least one selects fully positional panning. `BleedMinDistance=-1` and
`BleedMaxDistance=0` keep that radius at least one even at zero distance, without
dividing by zero. The template previously used `[-1, 0.1]`, which allowed a
listener-centered contribution at very close range. `PanSize=0` selects a point
source; center and LFE levels are explicitly disconnected zero constants. This
applies to remote speech. These settings affect only the private voice graph,
not Faith's original sounds.
Obstruction/occlusion high-frequency damping stays enabled. The environmental
send now targets the master's two-channel **Reverb** bus, which feeds the native
room impulse-response processor. The pipe template instead targeted **Reverb &
EarlyReflections**, which splits unpanned speech between that processor and a
separate reflection branch. That extra branch could leave faint, clear speech
in the opposite ear even when the direct mouth-position panner gave that side
zero gain. Near-listener bleed changes could not remove this far-field route.

Both the private cone's `ReverbSend` pointer and its compiled `Send0` connection
context are retargeted; changing the pointer alone does not change the compiled
send's bus binding. The eight-channel **GameMixMain** positional send stays intact.
The master asset, bus nodes and all other game sounds remain untouched. Room
reverb can still reach both ears as environmental reflections; there is no added
local microphone monitor. The effect depends on the level's authored acoustic data.

The copied sampler uses an unconnected unity-amplitude input. Muting that input
would also mute injected speech downstream. Its native output voice stays scheduled
while speech is active; the isolated instance's `SndPlayer0` PCM callback at
`0x142e17410` (dispatch slot `0x1420fd9f8`) replaces the game sample with
silence or decoded speech and supplies
microphone samples before the downstream spatial modules. Replacement only runs
after the decoder reports a produced block, overwrites exactly its reported
frame count and resamples speech to that block's native sample rate. Native
buffer swaps, frame counts, channel counts, sample rates, readiness flags and
no-output return values remain under the decoder's control. This preserves
native FIFO bookkeeping and downstream block-size expectations. Source ownership uses Sound +0x1b0, a 40-byte
VoiceRef array sized by SoundGraphInfo::Voices. Each row's +8 member is the
Kernel pointer itself and matches module +0x20 directly. Sound +0x1b8 is a
separate table. Ownership also matches Sound +0x188 against the private graph;
Sound +0x198 is an internal description and differs from instance +0x28.
The Sub0 callback at `0x1420fddc8` is not the sampler PCM output and is left
untouched. Unsupported voice layouts prevent template creation, and a
failed output-buffer replacement for an owned source drops the block instead
of returning the pipe sample. All other instances use the original callback unchanged.
There is no external playback device, wall raycast filter or
replacement of an existing player's sound. Shared game assets stay untouched.

MECMP applies the voice-volume setting with a 1.6 receive-gain multiplier,
doubling the previous receive gain at the default volume for existing and
new settings. Final receive gain is capped at 2, the native playback limit.
The distance fade stays at full near gain within 2 metres, then follows
`clamp((range - distance) / (range - 2), 0, 1)^1.5` and reaches silence at
the selected hearing range. This falls off more strongly than the previous
linear fade. A 5 ms gain ramp smooths changes. A block peak limiter uses
immediate attack, an 80 ms release and a 0.98 ceiling after receive gain,
preserving the waveform instead of hard-clipping loud speech. The native
graph provides direction and environmental attenuation. The camera's
third-person offset does not change MECMP's hearing range. PCM is resampled
from 16 kHz to the native sampler's reported rate for its mono source buffer;
the game handles final device channel layout and conversion.

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
| Native PCM source descriptor slot | `0x1420fd9f8` → `0x142e17410` (`SndPlayer0`) |
| Sampler class / processing descriptor | class `0x1420fdaa0` +0x18 → dispatch `0x1420fd9e8` |
| Existing kernel dispatch | `0x142dcf300`, calls processing descriptor `+0x10` |
| Native sound instance / create | vtable `0x141acfad0`; system `+0x40` → `0x142fe2ee0` |
| Sound description / channel | instance `+0x28` / `+0x30` |
| Native sound / voices | channel `+0x20`; sound `+0x1b0` → 40-byte VoiceRef array |
| Voice count | asset `+0x38` → graph; graph `+0x38` → Voices array; count at array `-4` |
| Kernel identity | VoiceRef `+8` and processing module `+0x20` contain the same Kernel pointer |
| Native source identity | instance description `+0x90` = private asset; Sound `+0x188` = its private graph |
| Planar output buffer / frame count | mixer `+0xc490` / `+0xc4b8` |
| Source rate / channel count | mixer `+0xc4bc` / `+0xc4c4` |
| Output transform source | OutputNodeData `+0x54`: Sound=0, Listener=1 |
| Point-source panning | ConeOutputNodeData `+0xc4` = 0; `+0xc8` = -1; `+0xcc` = 0 |
| Extra center / LFE levels | ConeOutputNodeData `+0xd0` / `+0xd8`: disconnected zero AudioGraphNodePort values |
| Obstruction / occlusion damping | OutputNodeData `+0x3c` / `+0x40`; factors set to 1 |
| Reverb gain / native send | ConeOutputNodeData `+0xf8` / `+0x100`; attenuation curve at `+0xe8` stays intact |
| Reverb-only bus discovery | ClientSoundSystem `+0x50` → AudioSystemAsset; `+0x18` → SoundMasterPatchAsset; `+0x20` → Busses; named `Reverb`, 2 channels |
| Actual native send binding | Private SoundGraphData `+0x48` connections; retarget the old reverb bus context as well as ConeOutputNodeData `+0x100` |
| Device heartbeat descriptor | `0x1420fa368` → `0x142dfad40`; does not mix speech |

Read-only inspection confirmed native instances, descriptions, AudioCore VoiceRef
and module Kernel relationships in the running game. Factory, sampler, spatial
output and native FIFO/processing paths were traced in the executable. This is
structural verification; the new spatial voice path has not been listened to in game.

Validation: Release build only. No automated tests, gameplay tests, microphone
recording or voice playback tests were run.
