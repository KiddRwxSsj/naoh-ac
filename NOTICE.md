# Third-Party Notices

The original source in this repository (`main.c`, `Makefile`, build scripts) is licensed under the zlib license — see `LICENSE`. That license is permissive, including for commercial use, but it only covers code written for this project. The compiled `.vpk` you build or distribute statically links two third-party libretro cores whose licenses are more restrictive than zlib and which override the permissiveness of zlib for the resulting binary. Read this in full before publishing a release.

## 1. Emulator cores (non-commercial — governs the whole binary)

**FB Alpha 2012 CPS2 (`cores/fbalpha2012_cps2`)**
Source: https://github.com/libretro/fbalpha2012_cps2 
License: Non-Commercial (FinalBurn/FB Alpha license).
Copyright (c) 2001 Dave (formerly of finalburn.com), all rights reserved. You can use, modify and redistribute this code freely as long as you don't do so commercially. This copyright notice must remain with the code. If your program uses this code, you must either distribute or link to the source code. You may not distribute FB Alpha with ROM images unless you have the legal right to distribute them. You may not ask for donations to support your work on any project that uses the FB Alpha source code.

**MAME 2000 (`cores/mame2000`)**
Source: https://github.com/libretro/mame2000-libretro 
License: Non-Commercial (original MAME license, pre-GPL relicensing) — freeware, non-commercial use only, source must remain available.

**What this means for you:**
* You may not sell this project, put it behind a paywall, or bundle it with anything sold.
* You may not solicit donations/tips tied to this project (no "buy me a coffee" button on the repo/release, no Patreon perks for it).
* You must keep this repository (or an equivalent source drop) publicly available alongside any binary release, since the cores require the source to remain distributable/linkable.
* These terms apply regardless of what license you put on your own `main.c` — zlib's "including commercial applications" grant does not extend to the linked cores.

## 2. Font — Saira-Regular.ttf (`assets/`)

Copyright The Saira Project Authors (Omnibus-Type), with Reserved Font Name "Saira". Licensed under the SIL Open Font License, Version 1.1 (https://scripts.sil.org/OFL). Permits bundling/embedding in software, including commercial software, at no cost. You may not sell the font by itself, and a modified copy may not keep the reserved name "Saira". Include a copy of the OFL 1.1 text alongside the font file (e.g. `assets/OFL.txt`) to stay compliant.

## 3. Sound effects (`assets/audio/*.ogg`)

Source: Arcade Sound Effects Pack by Chequered Ink (https://ci.itch.io/arcade-sound-effects-pack). 
License: free for any use including commercial, credit not required. You may not resell the unaltered pack as your own asset pack (not applicable here — you're using them inside a larger project).

## 4. deps/stb_vorbis.c

Public domain (Sean Barrett / nothings). No restrictions.

## 5. ROMs — not included

This repository and every release do not contain, and will never contain, arcade ROM files. `naoh_arcade_collection` only scans `ux0:data/NaohAC/roms/` on the user's own memory card at runtime and displays entries for ROMs it recognizes (Marvel Super Heroes, Marvel Super Heroes vs. Street Fighter, Marvel vs. Capcom, X-Men: Children of the Atom, X-Men vs. Street Fighter, The Punisher). Users must supply ROM files they legally own. The listed titles are trademarks/copyrights of Capcom and their respective owners; nothing in this repo grants rights to them.

## 6. Cover art

Not bundled in this repository or any release. If you personally add box-art images to `ux0:data/NaohAC/covers/` at runtime, they remain the property of their respective publishers and are used solely for game identification.
