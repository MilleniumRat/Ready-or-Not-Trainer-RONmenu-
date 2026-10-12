# RonMenu

A trainer for Ready or Not. It has an in-game overlay menu and a set of game features.
(for the DX11 version of the game)
## Languages

- **C++**: the in-game menu (`menu/src/`), the DirectX overlay code, and the injector (`menu/injector/injector.cpp`).
- **Lua**: the UE4SS mod (`lua/RonTrainer/`), which runs the game-side features.

## What is in the repo

- `menu/`: the C++ menu DLL source, the bundled ImGui library, the injector, and `build.bat`.
- `lua/RonTrainer/`: the Lua mod and its live feature scripts.

## Platform

Windows only. The game is Ready or Not, and the mod runs on UE4SS.

## Features

**Weapon**
- Infinite ammo, infinite ammo that keeps magazines, custom magazine size, vest slot limit
- No recoil, rapid fire, full auto on every gun, no sway
- Automatic reload (when empty, also while aiming)
- Explosive bullets (radius, cooldown, optional blast effect)
- Friendly-fire protection

**Aim**
- Aimbot, with visible-only, stay-on-target, circle and aim line options
- Triggerbot, with suspects-only and ADS-only options

**Health and player**
- God mode, god mode through limb health
- Move speed, fly (with noclip), spinbot (body only, visible in third person)

**AI**
- Freeze AI, suspect accuracy, no fake surrenders, break morale, slow AI reactions
- Fast restrain, squad god mode, squad auto-clear
- Buttons to surrender, arrest, report, eliminate or move suspects and civilians

**Mission and doors**
- Game speed, freeze mission timer, bombs (freeze and instant defuse)
- Reveal traps and locks, kicks always open, fast lockpick and multitool
- Buttons to open, close, unlock, lock, kick or blow doors, and to collect evidence and report to TOC

**ESP**
- Who is shown (suspects, civilians, AI SWAT, other players, surrendered, cuffed, down)
- Visible-only mode, colours, highlights, boxes, skeletons, health bars, labels, tracers and off-screen arrows
- Objects: evidence, dropped weapons, bodies, traps, bombs with countdown, grenades, objectives
- Radar, hit markers, gunshot markers, grenade arc, HUD counters for score, bomb and suspects or civilians

**View and camera**
- Field of view, stretched resolution, third person

**Unlocks**
- Earned cosmetics, all missions, unlock all maps, unlock all cosmetic items
- DLC missions available

**Teleports**
- Blink forward, glide to crosshair, teleport to next suspect, civilian, evidence or objective

**Fun and players**
- Launch everyone, explosion at crosshair, party light, spawning effects and sounds
- Player tools: teleport, bring, heal, ragdoll, give and take items, giant or tiny, big head, world look
