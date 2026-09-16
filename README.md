<img width="488" height="104" alt="chihiro-logo" src="https://github.com/user-attachments/assets/6fd9a78f-b636-4eb9-91f9-4f27a075e9bf" />

# xemu Chihiro fork by Tovarichtch

A fork of [xemu](https://xemu.app) that emulates the **Sega Chihiro**, the arcade
board Sega built on Xbox hardware. It boots the cabinet's own firmware, speaks the
arcade's own protocols, and plays six games (all type-1 compatible) with the controls they were built for:
light guns, a steering wheel with force feedback, and a magnetic card reader.

Current build: **Playtest #1**.

## Supported games (Type-1 compatible)

| Game | Cabinet |
|---|---|
| Crazy Taxi High Roller | Steering wheel and pedals |
| Ghost Squad | Light gun, magnetic card reader |
| The House of the Dead III | Light gun with pump reloading |
| OutRun 2 | Steering wheel, pedals, sequential shifter, force feedback |
| Ollie King | Very cool skateboard |
| Virtua Cop 3 | Light gun, pedal (ES MODE) |

Every game carries its own bindings, per player: *Settings, Chihiro, Game*. The
keys the whole cabinet shares, coin and start and test and service, live under
*System*. For now, only type-1 compatible games are supported. Once the type-3's V850 media board is emulated, more games will follow.

## The SEGA Chihiro board

SEGABOOT runs, the media board answers, the game is netbooted
the way the cabinet does it. Load a `.bin` netboot image through *Machine, Load Disc*.

The arcade I/O protocol is spoken by the emulated
AN2131 with its 8051 core, its serial lines and its endpoints, not faked above them.
The media board and its ROMs are emulated, and their state is real state.

Everything the board needs goes in one place: *Settings, Chihiro, Files* — BIOS, media board flash,
and the three EEPROMs (QC, baseboard, SC).

## Light guns

- **One pointer per player, each read on its own device** (evdev on Linux, Raw Input
  on Windows) so two people aim with two guns instead of fighting over one cursor.
- A crosshair per player, with its own image and size of your choice.
- Exclusive grab of the aiming devices while playing (Linux), so the desktop does not
  follow the shots.
- **Sinden border** with its style and thickness, for guns that need it.
- Every gun feature stays off in the games that have no gun.
- Toggle F3 to lock the pointer in and hide xemu notifications and menu.

## Wheel and force feedback

- The **OutRun 2 drive board** is emulated at the protocol level (SUD), so the game
  drives the wheel itself: haptics on a wheel, rumble on a pad.
- Strength, and an invert switch for wheels whose drivers push the wrong way.
- **Rotation**: 270 degrees reaches full lock, like the cabinet; or the whole wheel,
  one to one.
- **Auto-centering**: for Crazy Taxi, while the game is not driving the wheel, because no auto-centering feels wrong on a driving game.

## Card reader

- The **Sanwa CRP-1231LR-10NAB** reader, driven through the SC 8051 UART like the real cabinet.
- **One card file per player**, chosen in *Settings, Chihiro*.
- Ghost Squad only, which is the only cabinet that has readers.
- Cards are read, written and kept: a card carries its player's progress from one
  session to the next. [Dogehiro](https://github.com/Tovarichtch/dogehiro) edits them.

## Snapshots

Native VM snapshots, kept in a qcow2 beside the settings. **The media board and the
drive board travel with them**, so a snapshot taken mid-race comes back mid-race
rather than half alive.

## Cabinet settings

Free play, region, and the operator keys. The test menu works, and so do the
settings it writes. Hi-scores are saved.

## Performance optimization (experimental)

The fork carries its own work on the NV2A and the guest CPU, switchable:

| Setting | What it does |
|---|---|
| `GPU Boost` | Smoother, faster rendering. Turn it off and you get stock xemu |
| `CPU Boost` | Games run faster. Turn it off only if one misbehaves; needs a restart. |
| `Smooth first play` | Prepares the effects while the game boots, so the first minutes do not stutter as it compiles them |
| `Real hardware speed` | Holds the machine to the cabinet's own pace. Off, it runs as fast as your computer allows. |

This heavy lifting was done with an enormous amount of debugging with Ghidra-MCP and Claude Code sessions.

## Reporting a problem

Tick **Debug mode**, in the same place. It writes a log to the `logs` folder saying
where each frame went: the frame rate, the stutters, the effects being compiled, the
waits on the graphics card and on the processor. Attach it to the report and the
problem can be read rather than guessed. It costs nothing while it is off.

## Important notes

- Make sure you selected your controller that is used to play in `Input` tab on port 1. If you are playing with a steering wheel, select it in `Port 2`.
- Xbox retail games run, but only a few have been tested 
- Silent Scope games are playable on both Vulkan and OpenGL.
- Upscaling needs a good GPU.

## Doge mode (Easter egg)

In your `xemu.toml` file, at the very bottom, add:

```
[doge]
doge_mode = 'wow'
```
## Open source when?

xemu devs can request access to code. I have already granted a few of them. I will submit the Chihiro code (without optimization) after a few playtests to make sure everything is working properly for most users. The code won't be shared with sellers and vampires.

(Thanks SUPA Peter for the help, and every tester out there! Helped me a lot. See you in next projects!)
