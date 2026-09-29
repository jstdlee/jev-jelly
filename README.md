[![build](https://github.com/jstdlee/jev-jelly/actions/workflows/build.yml/badge.svg)](https://github.com/jstdlee/jev-jelly/actions/workflows/build.yml)

# jev-jelly

A little jelly friend that hops slowly and randomly around your X11 desktop.
It has no window frame or title bar, and clicks go through everywhere except its body.

- **Pinch & pull**: the spot you grab stretches out toward the cursor with a thinning neck. Past the stretch limit the
  body gets towed along. Let go and the lump snaps back and the whole body ripples. **Throw** it: it glides and splats
  off screen edges.
- **Put it somewhere** (drag and let go) and it stays there quietly for 5 minutes before wandering again. In a
  corner it naps for those 5 minutes (zzz).
- **Moods:** it gets *curious* when your cursor comes close (stops, stretches up, big eyes). Leave the cursor still
  and it tilts its head with a **?**. It *smiles* after gentle pets and *laughs* when tickled or poked 2–3 times. It
  sometimes *hurries* to a far spot in quick low hops with sweat drops and dust. On ordinary walks it sometimes
  stops to *look back* over its shoulder.
- **Click** to poke (poke 4× fast and it gets dizzy). **Wiggle the cursor** over it to pet it (hearts).
- **Right-click** opens the options panel (Dear ImGui via cimgui, GitHub-dark styling): Boy / Girl, face style
  (Tiny eyes / Classic), 7 flavors or any color from the wheel, stretch (Firm → Gooey), pull reach
  (a small pinch → more than half the body), and size. Changes apply live. "Take a nap" and "Quit jelly" are there too. Right-click
  again or press × to close. Settings are saved in `~/.config/jev-jelly/jelly.conf`.

## Download

Prebuilt Linux binaries (x86_64 and aarch64) are on the
[Releases page](https://github.com/jstdlee/jev-jelly/releases). Unpack one and run `./jelly &`. They're built on
Ubuntu 24.04, so they need glibc 2.39 or newer and the usual `libX11`, `libXext` and `libGL`.

## Build & run

    git clone --recursive https://github.com/jstdlee/jev-jelly
    cd jev-jelly
    sudo apt install libx11-dev libxext-dev libgl-dev g++   # or, without root: scripts/fetch-headers.sh
    make && ./jelly &

It needs X11 with a compositor (GNOME or KDE on Xorg are fine) and OpenGL 3.3. Dear ImGui 1.92 comes in through
the cimgui submodule. `JELLY_DEBUG=1 ./jelly` prints the physics state. Wayland is not supported, because clients
there can't position their own windows.

## How it works

- `src/jelly.c` is the jelly and `src/options.c` is the right-click panel (a second ARGB window with its own GL
  context and ImGui fed from X events). It uses an ARGB override-redirect window with GLX. The XShape input region is updated
  30×/s from the body silhouette.
- Feel model adapted from [cjxhaaa/slime](https://github.com/cjxhaaa/slime)'s `Blob`. The surface is a field of
  radial offsets. Each offset springs back to rest (stiffness ~220, damping ~7.5) and is coupled to its mesh
  neighbours (~380), so pokes, landings, wall hits and cursor brushes send a ripple across the body.
  Squash is its own volume-preserving spring. A held jelly is locked to the hand with no lag. It stretches into a
  droplet along a smoothed velocity trail and keeps that stretch when thrown. Throw velocity is fitted over the
  last 80 ms, 60% of it is transferred, and the jelly glides to a stop.
- Shader: Beer–Lambert-style depth tint, fresnel rim, fake-transmission env, analytic "room window" reflections
  for the glossy highlights, premultiplied alpha. A depth pre-pass makes only the front surface blend.
- Face, bow, hair, hearts and zzz are SDF quads pinned to surface anchor vertices, so they ride the deformation.
