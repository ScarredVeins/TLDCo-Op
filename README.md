# TLDCoop: co-op for The Long Dark

**Play The Long Dark's Survival mode together: 2 to 4 players in one world, over Steam, with join codes.**

> **Playtest build.** This is a prototype. It works, and the screenshots below are real in-game captures, but
> expect rough edges. Back up your saves before you play (see [Before you start](#before-you-start)).

![Your friend in your world, next to the wolf you brought down together](screenshots/05-friend-in-your-world.jpg)

- **No EXE.** The download is a zip with one DLL and some data files that you copy into the game folder yourself.
  The full source code is in this repo, so you can read exactly what it does or build it yourself.
- **One DLL, no mod loader needed, and fine with MelonLoader mods.** Tested in co-op with MelonLoader 0.7.2 and
  five popular mods loaded in both games (see [Playing with other mods](#playing-with-other-mods)).
- **Nothing goes online until you pick CO-OP.** Solo games stay exactly as they are.

---

## Contents

- [Features](#features)
- [Requirements](#requirements)
- [Install](#install)
- [How to play](#how-to-play)
- [Settings](#settings)
- [Playing with other mods](#playing-with-other-mods)
- [Uninstall or turn off](#uninstall-or-turn-off)
- [Is it safe?](#is-it-safe)
- [Troubleshooting](#troubleshooting)
- [Build it yourself](#build-it-yourself)
- [Credits](#credits)

---

## Features

All of these are in this release. Every screenshot is a real capture from the game with TLDCoop running: two copies
of the game on one PC, one hosting (TestHost) and one joining (TestFriend). More are in the
[screenshots](screenshots/) folder.

### Start a co-op game from the game's own menus
- **SOLO or CO-OP** appears when you pick NEW, RESUME or LOAD in Survival.
- **The host** picks HOST and gets a short **join code**. **Friends** pick JOIN and type that code. No IP
  addresses and no port forwarding: it goes through Steam.
- **A lobby** shows everyone and the character they picked. The host lets each friend in (LET IN / TURN AWAY,
  KICK), friends press READY, and the host presses START.
- **Up to 4 players** (the host and 3 friends). Friends can also ask to join a game that's already running: the
  host gets a message and presses **F9** to let them in or **F10** to turn them away.
- Co-op plays at the **host's** difficulty, in the host's world, time of day and weather.

| The host's lobby: two players asking to join | The friend is in and READY |
|---|---|
| ![Lobby, host](screenshots/01-lobby-host-asking.jpg) | ![Lobby, ready](screenshots/02-lobby-friend-ready.jpg) |

### See each other
- Friends appear as **one of 23 story characters** (Mackenzie, Astrid, Molly, Jane, Jeremiah, Methuselah and
  more). You pick yours on the co-op setup screen, and their idle, walk and run animations follow their real speed.
- **What a friend holds is in their hands:** a rifle, a revolver, a bow, a flare gun... A **lit lantern, torch,
  flare or flashlight really lights the world** around them.
- **Bodies act:** they eat with the food in hand, lie down in a bed to sleep, crouch, and hold an item out to you.
- **Finding each other:** a name over a friend's head when you look at them or when they're close, an arrow at the
  screen's edge when they're off screen, a **small radar map** with your friends on it, and your friends on the
  game's own map. Your map also **charts itself where you walk**. Each player can switch each of these on or off.

| A friend aiming a rifle | A friend's lantern lights up the night |
|---|---|
| ![Rifle](screenshots/06-friend-aims-rifle.jpg) | ![Lantern](screenshots/07-friend-lantern-at-night.jpg) |

| Torch at night | Red flare |
|---|---|
| ![Torch](screenshots/08-friend-torch-at-night.jpg) | ![Flare](screenshots/09-friend-red-flare.jpg) |

| Names over heads | Radar (top right): your friend is in a cabin | Friends on the game's map |
|---|---|---|
| ![Name](screenshots/12-name-over-head.jpg) | ![Radar](screenshots/13-radar-friend-in-cabin.jpg) | ![Map](screenshots/14-friend-on-the-map.jpg) |

### One shared world
- **Wildlife is shared.** The host's game runs the animals, and everyone sees the same wolf. Wolves go after
  friends too, and when one jumps your friend you see the fight. Your shots hit the same animal for everyone
  (rifle, revolver, bow, flare gun, stones).
- **Wolves fear your friend's torch or flare,** just as they fear yours.
- **Carcasses are shared:** harvest meat, hide and guts, or quarter it, and the others see it happen.
- **Items:** what one player picks up is gone for the others, and what one drops appears for them. If two players
  grab the same thing at the same moment, exactly one gets it.
- **Containers, lockers and safes:** open, search and take, and everyone sees the same contents. Locks and safes
  someone forced or cracked are open for everyone.
- **Fires:** light one, feed it, let it burn out or dismantle it, and it does the same for everyone. **Cooking**
  too: what goes into a pot or onto a fire, and what comes out cooked.
- **Things broken down and plants harvested** stay that way for everyone. **Decorations moved** around a room move
  for everyone, live.
- **Buildings:** a building's loot rolls the same for everyone, whichever door each player came in by.
- **Rabbits and snares** are shared.
- **One-of-a-kind items** (notes, keys, the technical backpack...) are each player's own, so everyone can find them.

| A wolf holds back from a friend's flare | A wolf sizes up a friend holding a torch |
|---|---|
| ![Wolf and flare](screenshots/10-wolf-held-off-by-flare.jpg) | ![Wolf and torch](screenshots/11-wolf-and-friends-torch.jpg) |

### Survive together
- **Give a friend any item:** look at them, interact, and the inventory's button says GIVE. For a stack, pick how
  many. Their body holds it out and yours reaches for it.
- **Sleeping and passing time together:** time skips only when everyone is in. Each player votes for how long on
  the sleep or pass-time screen; the most votes wins, and a tie goes to the shorter one. Sleeping alone rests you
  in real time (healing a little faster) without skipping time for the others. There's also **REST**: lie in bed in real time and heal
  1.5 times faster.
- **Actions don't fast-forward the clock for everyone.** Your body pays the time (calories, fatigue, thirst), but
  the world clock stays shared.
- **Death:** your body stays where you fell, with your pack on it. You come back as **a new survivor beside your
  friends** and can go back for your gear.
- **Joining, saving and reconnecting:** friends can join a game in progress. If the connection drops, everyone's
  changes are kept and merged when it comes back. The host's save keeps the shared world; each friend's save keeps
  their own character.

| Handing over a jar of peanut butter | Eating | Asleep in a bed |
|---|---|---|
| ![Give](screenshots/15-friend-hands-you-food.jpg) | ![Eat](screenshots/17-friend-eating.jpg) | ![Bed](screenshots/19-friend-asleep-in-bed.jpg) |

| Pass time together: everyone votes | Sleeping alone or REST: no time skip for the others |
|---|---|
| ![Vote](screenshots/20-pass-time-together-vote.jpg) | ![Rest](screenshots/22-rest.jpg) |

---

## Requirements

- **The Long Dark on Steam, for Windows, version 2.55.** Every player needs the same game version. The mod checks
  this when someone joins.
- **Steam running and logged in.** Co-op connects through Steam, and every player needs their own copy of the game.
- **Everyone on the same co-op protocol.** This release is **v0.13.1 (protocol v27)**. It's the mod from the
  `TLDCoop-Setup.exe` that went around on 2026-10-18, plus one fix that lets MelonLoader mods run beside it. The
  protocol is the same, so players who used that installer can play with players who use this zip. If you used the
  installer and also use MelonLoader mods, put this release's `winhttp.dll` in place of the installer's (see
  [Troubleshooting](#troubleshooting)).

## Before you start

Back up your saves. Copy this folder somewhere safe:

```
%LOCALAPPDATA%\Hinterland\TheLongDark
```

(Paste that into the File Explorer address bar.) TLDCoop is a playtest; a backup costs you nothing.

---

## Install

1. **Download** `TLDCoop-v0.13.1.zip` from the [Releases page](../../releases/latest).
2. **Close The Long Dark** if it's running.
3. **Open the game folder:** in Steam, right-click **The Long Dark** → **Manage** → **Browse local files**. It's the
   folder with `tld.exe` in it.
4. **Check for a `winhttp.dll` that's already there.** Normally there isn't one. If there is, right-click it →
   **Properties** → **Details**. If the product name is **MelonLoader.Bootstrap**, it's MelonLoader: **rename it to
   `winmm.dll`** and go on (MelonLoader works the same under that name; see
   [MelonLoader](#melonloader)). If it's something else, see
   [Playing with other mods](#playing-with-other-mods) before you go on.
5. **Copy everything inside the zip into the game folder,** so that `winhttp.dll` sits **right next to `tld.exe`**
   and there's a `TLDCoop` folder beside it. If Windows asks about replacing files in `TLDCoop`, say yes (that's an
   update).

When you're done, the game folder looks like this:

```
TheLongDark\
├── tld.exe                       (the game, already there)
├── winhttp.dll                   ← TLDCoop
└── TLDCoop\                      ← TLDCoop
    ├── tldcoop_bodies.bundle       the characters friends appear as
    ├── tldcoop_eating.bundle       the bodies' animations
    ├── tldcoop_handover.bundle
    └── tldcoop_sleeping.bundle
```

6. **Start the game from Steam** as usual. To check that it's in: in Survival, pick NEW or RESUME, and you'll get a
   **SOLO OR CO-OP?** screen. The mod also writes a log to `TLDCoop\coop.log`.

Every player installs the same zip.

## How to play

1. **Everyone** starts The Long Dark with Steam running.
2. **The host** picks **Survival → NEW** (or **RESUME** or **LOAD**), then **CO-OP**. On the setup screen: MODE
   **HOST**, pick a **CHARACTER**, then **CONFIRM**. The lobby shows **your code**: send it to your friends.
3. **Each friend** picks **Survival → NEW** (or RESUME or LOAD), then **CO-OP**, MODE **JOIN**, types the host's
   **code**, then **CONFIRM** and **READY**.
4. **The host** presses **LET IN** for each friend, then **START** once everyone is READY. Everyone loads into the
   host's world together.

Joining a game that's already going works too: the friend does step 3, and the host gets a message in the game:
**F9** lets them in, **F10** turns them away.

The name your friends see is your **Steam name**. The host's code stays the same from game to game, so your group
can reuse it.

## Settings

The co-op setup screen covers what most players need: host or join, your character, the join code, and the NAMES,
RADAR and AUTO MAP switches. There are a few more options in `TLDCoop\coop.ini` in the game folder, which is created
on first launch with a comment on every line. Restart the game after you edit it.

| Setting | What it does |
|---|---|
| `ask_to_join=1` | Each friend asks to join and the host lets them in. `0` lets anyone with the code straight in. |
| `body=` | Your character (the setup screen sets this). |
| `voice=male` / `female` | Your survivor's voice. |
| `[hud] names / radar / chart` | Name tags, the radar, and auto-charting your map (each `1` on, `0` off). |
| `[struggle] picker_scale=0.5` | How long the weapon picker stays up when an animal jumps you, in co-op (`1` = the game's own). |
| `transport=local` | For LAN play by IP instead of Steam codes (with `bind_ip`, `host_ip`, `port`). |

---

## Playing with other mods

TLDCoop is made to stay out of other mods' way:

- **It's its own loader.** The game loads `winhttp.dll` by itself; TLDCoop doesn't need MelonLoader and doesn't
  install, replace or change it. It doesn't touch `Mods\`, `Plugins\`, `UserData\`, `UserLibs\` or `MelonLoader\`.
- **It shares game functions with other mods safely.** TLDCoop hooks about 130 of the game's functions, and
  MelonLoader mods patch many of the same ones (`GameManager.Update` is a favourite). Both run, one after the other.
- **It changes no game files.** Everything happens in memory while the game runs. All of its own files are
  `winhttp.dll` and the `TLDCoop\` folder, and deleting them puts the game back exactly as it was.
- **Solo is untouched.** Until you pick CO-OP, nothing is sent anywhere, and your solo games play as before, with
  your other mods.

### MelonLoader

MelonLoader and TLDCoop each need the game to load one DLL, and they must have **different names**:

| MelonLoader's DLL in the game folder is called | What to do |
|---|---|
| `version.dll` | Nothing, if your mods load. If they don't (recent Windows 11 updates stop `version.dll` from loading for many players), rename it to `winmm.dll`. |
| `winhttp.dll` (the usual fix for that Windows update) | **Rename it to `winmm.dll`**, then install TLDCoop. TLDCoop needs the name `winhttp.dll`. |
| `winmm.dll` | Nothing. |

Not sure which `winhttp.dll` you have? Right-click it → **Properties** → **Details**: MelonLoader's says
**MelonLoader.Bootstrap** (about 10 MB). TLDCoop's has no product name (about 2 MB).

**Tested:** MelonLoader 0.7.2 as `winmm.dll` with ModSettings, DeveloperConsole, NoCameraSway,
TinyTweaks-PauseOnRadial and TinyTweaks-RunWithLantern, in both the host's and the friend's game. Both games load
all five, and the lobby, items, wolf fights, handing things over, containers, fires, sleeping together, and death and
respawn all work.

### What to keep in mind

- **Everyone in a group should run the same gameplay mods.** Co-op shares the host's world (items, containers,
  animals, fires). If the host has a mod that adds new items or changes loot and a friend doesn't, the friend's
  game has nothing to show for those items. Visual and interface mods (HUD, graphics, sounds) are fine to differ.
- **Another `winhttp.dll` that isn't MelonLoader's:** only one file can have that name. If some other tool already
  uses it, TLDCoop and that tool can't both be installed this way. Keep theirs, don't overwrite it, and open an
  issue here so we can look into it.
- **Mods that change the same game systems** (sleeping, time skipping, death and respawn, the Survival menu) may
  disagree with co-op's versions of them. If something odd happens, try without the other mod first, and tell us
  which one it was.

---

## Uninstall or turn off

- **Uninstall:** with the game closed, delete `winhttp.dll` and the `TLDCoop` folder from the game folder. (Keep
  `TLDCoop\coop.ini` if you want your settings next time.)
- **Turn it off without deleting:** create an empty file named `disabled` (no extension) inside the `TLDCoop` folder.
  The DLL then does nothing at all. Delete that file to turn co-op back on.

Your saves are the game's own, in `%LOCALAPPDATA%\Hinterland\TheLongDark`; uninstalling doesn't touch them.

## Is it safe?

The fair question for any DLL from the internet:

- **The source is right here.** Everything that goes into `winhttp.dll` is in [`src/`](src/), and you can
  [build it yourself](#build-it-yourself) instead of using the download.
- **Check your download.** The release page lists the SHA-256 of each file. In PowerShell,
  `Get-FileHash winhttp.dll` should print the same value.
- **Why `winhttp.dll`?** That's how the mod gets loaded with no installer and no mod loader: the game looks in its
  own folder first for this standard Windows library. TLDCoop passes every real Windows call through to the real
  `winhttp.dll` in `C:\Windows\System32`, and starts co-op alongside. Many game mods and tools load this way.
- **Antivirus:** because the mod hooks into the game's code, some antivirus programs may flag it with a "generic"
  or "suspicious" detection. That's a false positive; the source is here to check.
- **What it reads and writes:** only its own `TLDCoop\` folder (settings, log) and the game's normal saves. It talks
  to Steam only to connect your group, and only after you pick CO-OP.

## Troubleshooting

- **No SOLO OR CO-OP? screen:** check that `winhttp.dll` is right next to `tld.exe` (not in a subfolder) and that
  there's no `TLDCoop\disabled` file. Then look at `TLDCoop\coop.log`.
- **"mod protocol version differs":** someone has a different TLDCoop version. Everyone installs the same release.
- **"game version differs":** someone's game has a different update. Let Steam update everyone.
- **The game closes at the main menu with MelonLoader mods installed:** you have the `winhttp.dll` from the
  2026-10-18 `TLDCoop-Setup.exe`. That build crashes when a MelonLoader mod patches one of the game functions
  TLDCoop also hooks. Put this release's `winhttp.dll` in its place.
- **My MelonLoader mods stopped loading after I installed TLDCoop:** MelonLoader was probably named `winhttp.dll`.
  The `TLDCoop-Setup.exe` installer moves another mod's `winhttp.dll` aside as `winhttp.dll.tldcoop-backup`. Rename
  that file to `winmm.dll`. If you copied TLDCoop over MelonLoader's `winhttp.dll` by hand, reinstall MelonLoader
  and rename its DLL to `winmm.dll` (see [MelonLoader](#melonloader)).
- **Friends show as coloured capsules:** `TLDCoop\tldcoop_bodies.bundle` is missing on the PC of whoever sees
  capsules. Copy it in from the zip.
- **Reporting a bug:** open an [issue](../../issues) and attach `TLDCoop\coop.log` from **every** player in the
  game. The log is rewritten each time the game starts, so copy it before you launch again.

---

## Build it yourself

You need Windows, [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/downloads/) with "Desktop
development with C++", [CMake](https://cmake.org/) and [Ninja](https://ninja-build.org/).

```
git clone --recursive https://github.com/ScarredVeins/TLDCo-Op.git
cd TLDCo-Op
build.cmd
```

That makes `build\winhttp.dll` (release, static runtime). Copy it next to `tld.exe`, and the `.bundle` files from the
release zip into `TLDCoop\`. The animation bundles are also in [`bodypack/Actions`](bodypack/Actions).
`build\*_test.exe` are offline unit tests (no game needed). `build\tldbot.exe` is a fake second player for testing
alone (`transport=local`).

| Folder | What's in it |
|---|---|
| `src/` | The mod. `dllmain.cpp` loads it; `src/net/` is the protocol and connection code (no game code, unit-tested). |
| `tests/` | Offline unit tests. |
| `bodypack/` | How the character bodies are built (Unity 6000.0.60f1, from the game's Episode 5 characters), and the action animations. |
| `third_party/minhook` | [MinHook](https://github.com/TsudaKageyu/minhook), for hooking. Its trampoline builder is replaced by `src/minhook_trampoline.c`, so that other mods can hook the same functions. |
| `tools/` | `gen_proxy_exports.py` (the winhttp exports) and the body-pack build helpers. |

## Credits

- Made by **ScarredVeins**, built with AI coding agents (Claude Code).
- [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu (BSD 2-Clause).
- **The Long Dark** and its characters belong to **Hinterland Studio**. TLDCoop is an unofficial fan mod. It isn't
  made or endorsed by Hinterland, and you need to own the game to play it. The character bodies in the release are
  built from the game's own Episode 5 characters, for players who own The Long Dark.
