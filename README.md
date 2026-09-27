<img width="488" height="104" alt="chihiro-logo" src="https://github.com/user-attachments/assets/6fd9a78f-b636-4eb9-91f9-4f27a075e9bf" />

# xemu Chihiro fork by Tovarichtch

A fork of [xemu](https://xemu.app) that emulates the **Sega Chihiro**, the arcade
board Sega built on Xbox hardware. It boots the cabinet's own firmware, speaks the
arcade's own protocols, and plays games with the controls they were built for.

# Supported games

| Game | Status | Note |
|---|---|---|
| Crazy Taxi High Roller | ✅ |
| Ghost Squad | ✅ |
| Gundam Battle Operating Simulator | ✅ | network firmware required |
| OutRun 2 | ✅ |
| OutRun 2 SP | ✅ | network firmware required |
| Ollie King | ✅ |
| The House of the Dead III | ✅ |
| Virtua Cop 3 | ✅ |
| Wangan Midnight: Maximum Tune | ✅ |
| Wangan Midnight: Maximum Tune 2 | ✅ |
| Satellite Terminal games | ❌ | ALL.net; touchpad not emulated |

# Main features

### Type-1 and Type-3 boards

Both types are emulated with their own full protocol from boot to launch. The arcade I/O protocol is spoken by the emulated AN2131 with its 8051 core, its serial lines and its endpoints, not faked above them. The media board and its ROMs are emulated, and their state is real state. All regions are supported (unless something specific is required). **Only .bin netboot disc format are accepted** Refer to my tools [Chihiro-netboot](https://github.com/Tovarichtch/chihiro-netboot) to make a playable .bin netboot game disc.

## Light guns

- One pointer per player, each read on its own device (evdev on Linux, Raw Input
  on Windows). Listed support devices up to 32, perfect for big arcade cabinet setups.
- A crosshair per player, with its own image and size of your choice.
- Sinden border with its style and thickness, for guns that need it with ratio and thickness slider. (Shout out to Sinden light gun discord community!)
- Toggle F3 (default) to lock the pointer in and hide xemu notifications and menu.

## Steering wheel and force feedback

- Namco V257 STR PCB (Maximum Tune series) and Sega 838-13683 (OutRun 2 series) drive boards emulated at the protocol level (SUD) with the real haptics on the wheel and translated rumble on the gamepad.
- Settings for rotation, strength and weight to your taste; invert switch for wheels whose drivers push the wrong way.
- Auto-centering for Crazy Taxi, while the game is not driving the wheel, because no auto-centering feels wrong on a driving game.

## Card reader

- Tamura (SAXA) HW210 (Ghost Squad, Gundam) and Sanwa CRP-1231LR-10NAB (Maximum Tune series) card readers fully emulated.
- Create, share, swap cards on the fly, one per player. Insert the card with a single key.
- Cards are read, written and kept: a card carries its player's progress from one session to the next.
- For Ghost Squad players, grab [Dogehiro](https://github.com/Tovarichtch/dogehiro) to view and edit your cards.

## Snapshots

Native VM snapshots, kept in a qcow2 beside the settings. Snapshots are saved per game, quicksaving and quickloading on the fly.

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

This heavy lifting was done with an enormous amount of debugging with Ghidra-MCP and Claude Code sessions. The aim is to test and bring incremental changes to upstream for native integration in xemu.

## Reporting a problem

Tick **Debug mode** in the experimental settings. It writes a log to the `logs` folder saying
where each frame went: the frame rate, the stutters, the effects being compiled, the
waits on the graphics card and on the processor. Attach it to the report and the
problem can be read rather than guessed. It costs nothing while it is off.

**DO NOT REPORT ISSUES AND FEATURE REQUESTS ON MAIN XEMU GITHUB**

## Important notes

- To know how to setup and what features are baked in xemu Chihiro, consult the wiki (WIP)
- Make sure you selected your controller that is used to play in `Input` tab on port 1. If you are playing with a steering wheel, select it in `Port 2`.
- Xbox retail games run, but only a few have been tested, expect glitches and bugs.

## Doge mode (Easter egg)

In your `xemu.toml` file, at the very bottom, add:

```
[doge]
doge_mode = 'wow'
```
## About

Special thanks to all testers and SUPA Peter for extensive testing over and over again.

You can follow me on [Arcade Community discord](https://discord.gg/a2rHM7BVy) for my next projects.
