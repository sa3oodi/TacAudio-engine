# TacAudio patch notes

<!-- The app shows this file (built in) on its "What's new" page, and each release's notes on
     GitHub come from its section. Format: "## <version> - <date>", then "### New", "### Improved"
     or "### Fixed", then "- " lines. Keep it short and in plain words for players. -->

## 0.7.8 - 2026-11-16

### Fixed
- After Update now, the "update the engine" notice could stay up even though the engine was already updated, until you closed and reopened TacAudio. The app now reopens only after the engine is updated, and the notice goes away by itself.

## 0.7.7 - 2026-11-16

### Fixed
- Discord, browsers, music and other apps on your headphones could sound robotic or hollow. TacAudio now only processes surround sound (7.1 / 5.1), which is normally your game. Other apps play clean, with just your headphone EQ. Your game sounds exactly the same as before.

### New
- Settings > Other apps > Process stereo apps: turn it on if your game only plays in stereo and you still want TacAudio on it.
- Direct mode: after updating, click Update engine when the app asks (Update now does it for you).

## 0.7.6 - 2026-11-14

### New
- What's new is back: patch notes in the app (About > What's new), a notice after each update, and the new version's notes before you update.

## 0.7.5 - 2026-11-14

### New
- Update now: TacAudio downloads the new version, checks that the download is complete and genuine, installs it and reopens by itself.

### Improved
- Clean updates: the old app files and the old engine in Windows audio are removed and the new version is installed fresh (one Windows prompt if the engine is installed). Your settings, game profiles and headphone EQ are kept.
- Installing or updating the engine works even while Windows audio is using it, and with non-English device or user names.
- Coming from 0.7.4 or older: install this version from the download below once. From now on, updates are one click (Update now).

## 0.7.4 - 2026-11-14

### Improved
- TacAudio is back to version 0.7.0: the sound engine and the app are exactly as they were in 0.7.0, including Smart footsteps (on by default).
- Removed again: the patch notes page in the app and Headphone EQ setting Volume and Bass (defaults are back to Volume -6 dB, Bass 0 dB).
- Direct mode: after updating, click Update engine (or reinstall the engine on the Setup page).

## 0.7.3 - 2026-11-14

### Fixed
- "Update engine" could fail while Windows audio still had the old engine open, so the update notice kept coming back and reinstalling showed "Something went wrong". The engine is now replaced even while it is in use.
- Installing the engine failed when the headphone name or the Windows user name had non-English characters.

### Improved
- If installing or updating the engine fails, the notice says so, with Details and Try again buttons.

## 0.7.2 - 2026-11-14

### Improved
- Footstep clarity is back to the classic boost from before 0.7.0. The Smart option is removed: the classic boost sounded better in games.
- Direct mode: after updating, click Update engine (or reinstall the engine on the Setup page) to get the change.

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
