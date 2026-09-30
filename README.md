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
- **Calendar reminders:** add your calendar's secret iCal link (right-click → Calendar reminders → *Paste link*).
  The jelly checks the next 48 hours every 10 minutes. When you're at the desk (mouse or keyboard used in the last
  minute), an upcoming event appears beside it as solid words, each word in its own slowly shifting color, and
  pixel-dissolves in and out. Click the words to dismiss them; otherwise they go out after
  10 minutes. Japanese, Chinese and Korean titles are supported (Noto Sans CJK). Each event is shown at most
  three times:
  - a heads-up, anywhere from 70 minutes to 48 hours ahead, at most one every 20–40 minutes
  - one when it's 15–70 minutes away
  - one when it's about to start

  Recurring events, exceptions, cancellations, time zones and all-day events are handled.
  *Google Calendar:* Settings → your calendar → Integrate calendar → **Secret address in iCal format**. This needs
  no Google sign-in or app registration. Outlook, iCloud and Fastmail share links work too. Links are stored in
  `~/.config/jev-jelly/calendars` (mode 600), since anyone with a link can read that calendar. Fetching uses `curl`.
- **Chat:** triple-click the jelly. A translucent, borderless line to type in fades in beside it with the keyboard
  ready (IME input for Japanese, Chinese and Korean works, and so does Ctrl+V). Press Enter and the question goes
  to your model in the background. The jelly bounces, thinks with little dots, and cheers when the reply comes
  back, game-dialogue style: your question on top, the answer below. Enter to reply, Esc to close. The box stays open
  after you send, so you can keep asking: waiting questions are listed above the input (the current one "thinking",
  the rest "queued"), they're answered in order, and each answer comes back under its own question (Enter steps to
  the next). Esc hides the box while it keeps answering; one click on the thinking jelly shows it again. It works with any OpenAI-compatible
  endpoint and defaults to a local TensorFold / Qwen server on :8888. The default personality is kawaii, cheerful,
  caring and curious, and it has safety guidance built in.
  **Routing:** each message is handled by one of three routes, each with a prompt made for it: **Quick** (greetings
  and easy questions: a deliberately short prompt, one to three sentences back), **Think** (the model reasons first
  and answers conclusion-first) and **Research** (live information, answered from fresh results with sources). Clear
  cues decide first ("price", "weather", 比特币, 天気 → Research; "prove", "code", 为什么 → Think). For the rest, a jev
  model through the SystemOne API (a local Julia-1 on :8011) scores whether the web is needed; long messages go to
  Think. The question sent to jev was picked on a labelled test set, and it's editable in settings. Research uses
  **web search** (Brave, Exa or Tavily, with your API key); without it, the question goes to the **oh-my-pi**
  agent (`omp`, which searches on its own). The agent is told to use the chat model: the provider in
  `~/.omp/agent/models.yml` that serves the same endpoint (or a model you set). Only if neither works does the
  model answer from memory, and it says so.
  The **LLM** tab holds it all, in sub-tabs: **Model** (endpoint / model / key with Test, and sampling, in
  collapsible groups), **Prompts** (one tab per route: prompt, reply length, thinking, *Improve* with Undo and
  Default), **jev** (its API, score threshold, a "try a message" box, and its questions) and **Search** (web search
  API and the agent). The **Chat** tab keeps chat's own options: saving history, viewing it, deleting it. Settings
  are in `~/.config/jev-jelly/llm.conf` and history in `chat-history.jsonl`, both mode 600. API keys are passed to
  curl on stdin, never on the command line.
- **Right-click** opens the options panel on top of other windows (Dear ImGui via cimgui, styled like gpu-hud).
  Drag it by its header to move it. The **check icon** beside × in the header (green when everything turned on works, yellow when something needs attention) runs **Test all**: it checks everything the settings depend on (chat model,
  jev, web search, the oh-my-pi agent, curl, CJK fonts, calendars) and lists them with a light each: green works,
  yellow needs attention, gray is off or not installed. It has an app theme (White / Dark / Tokyo Night), Boy / Girl, face style (Tiny eyes / Classic), 7 flavors or any color from the wheel, opacity,
  stretch (Firm → Gooey), pull reach (a small pinch → more than half the body), and size. It also lists your
  calendars: add as many links as you like (up to 10), each shown by its own name with an event count and an
  on/off switch. **Events** opens a list of the next 3 months grouped by date and weekday. Click an event to read
  its description. Changes apply live. "Take a nap" and "Quit jelly" are there too. Right-click
  again or press × to close. Settings are saved in `~/.config/jev-jelly/jelly.conf`.

## Download

Prebuilt Linux binaries (x86_64 and aarch64) are on the
[Releases page](https://github.com/jstdlee/jev-jelly/releases). Unpack one and run `./jelly &`, or run `scripts/install-desktop.sh` once for a **Jelly** icon on the desktop and in the app menu (only one jelly runs at a time; `--remove` takes it away). They're built on
Ubuntu 24.04, so they need glibc 2.39 or newer and the usual `libX11`, `libXext` and `libGL`.

## Build & run

    git clone --recursive https://github.com/jstdlee/jev-jelly
    cd jev-jelly
    sudo apt install libx11-dev libxext-dev libgl-dev g++   # or, without root: scripts/fetch-headers.sh
    make && ./jelly &

The jelly runs under a small watchdog. If the NVIDIA driver can't give it a window at startup (this happens
on a DGX Spark while a large model holds most of the memory), it restarts on Mesa software rendering
(`LP_NUM_THREADS=1`, 30 fps, about 15% CPU). A later crash just restarts it. `JELLY_NO_WATCHDOG=1` runs it directly.

It needs X11 with a compositor (GNOME or KDE on Xorg are fine) and OpenGL 3.3. Dear ImGui 1.92 comes in through
the cimgui submodule. `JELLY_DEBUG=1 ./jelly` prints the physics state. Wayland is not supported, because clients
there can't position their own windows.

## How it works

- `src/chat.c` + `src/llm.c` are the chat box and the model client, `src/kbd.c` is keyboard / IME input for the
  ImGui windows.
- `src/jelly.c` is the jelly, `src/calendar.c` fetches and expands the calendars, `src/bubble.c` draws the
  LED reminder, and `src/options.c` is the right-click panel (a second ARGB window with its own GL
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
