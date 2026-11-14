# TacAudio patch notes

<!-- The app shows this file (built in) on its "What's new" page, and each release's notes on
     GitHub come from its section. Format: "## <version> - <date>", then "### New", "### Improved"
     or "### Fixed", then "- " lines. Keep it short and in plain words for players. -->

## 0.7.1 - 2026-11-14

### New
- Patch notes in the app: see what's new after an update, and what a new version brings before you download it.
- Headphone EQ now sets Volume and Bass to suit your headphones, and the Sound page's reset buttons go back to those values instead of the app's.

### Improved
- TacAudio is now open source under the MIT licence.
- Groundwork for code-signed installers, so Windows warns less when you install.
- `tacaudio-cli record` / `channels`: record what a device receives and see which channels carry sound.

## 0.7.0 - 2026-11-12

### New
- Game profiles: a game library with sound settings for each game, switched automatically when the game is in front.
- Find your head: a 2-minute listening test that picks the 3D head that suits your ears best.
- Smart footsteps: boosts short impacts like footsteps only, not rain, wind or engines.
- Headphone EQ: import a correction made for your headphone model (AutoEq / Equalizer APO).
- Footstep ring (experimental): an accessibility overlay in a screen corner that glows towards enemy footsteps.
- Update check: a notice when a newer TacAudio is out.
- Record 15 s of game sound (Direct mode) to help tune footstep detection.

### Improved
- A prompt to update the engine in Windows audio after the app is updated.

## 0.6.0 - 2026-11-10

### New
- Direct mode: TacAudio's engine runs inside Windows audio itself. No virtual cable and almost no delay.

### Improved
- VB-CABLE is now optional, for setups where Direct mode can't be used.

## 0.5.0 - 2026-11-10

### New
- Setup assistant: a step-by-step check of your devices and the game's 7.1 output.

## 0.4.1 - 2026-11-10

### New
- First public release: 3D headphone audio for 7.1 games, combat dynamics, footstep clarity, bass control, tray icon, start with Windows and hotkeys.
