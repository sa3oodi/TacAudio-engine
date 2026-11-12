# Code signing

TacAudio's releases are built on GitHub Actions (`.github/workflows/build.yml`). Signing uses
[SignPath](https://signpath.io), which signs open-source projects for free through the
[SignPath Foundation](https://signpath.org). Until the steps below are done, release builds are
simply unsigned, exactly as before - nothing breaks.

What gets signed, on every `v*` tag:

1. **The binaries** in `dist/stage` (`TacAudio.exe`, `tacaudio-cli.exe`, `tacrender.exe`,
   `tacapo-setup.exe`, `TacAudioApo.dll`), before they go into the installer.
2. **The installer** `TacAudio-Setup-<version>.exe`.

Pushes to `main` and pull requests are never signed.

## One-time setup (the maintainer does this)

1. **Apply** to the SignPath Foundation for free open-source signing: <https://signpath.org/apply>.
   - Project: `https://github.com/sa3oodi/TacAudio-engine`, licence MIT.
   - The README's *Code signing policy* and *Privacy* sections are what they ask for.
   - Wait for approval; they set up an organisation and a certificate for the project.
2. **In SignPath** (app.signpath.io), in the TacAudio project:
   - Project slug: `TacAudio-engine` (or set the repository variable `SIGNPATH_PROJECT_SLUG`).
   - Signing policy slug: `release-signing` (or set `SIGNPATH_POLICY_SLUG`). SignPath Foundation
     requires a manual approval for each release: you get an e-mail and click *Approve*.
   - Two artifact configurations, pasted from this repo:
     - slug `binaries` - [`.signpath/artifact-configurations/binaries.xml`](../.signpath/artifact-configurations/binaries.xml)
     - slug `installer` - [`.signpath/artifact-configurations/installer.xml`](../.signpath/artifact-configurations/installer.xml)
   - Link the GitHub repository as a *Trusted build system* (SignPath's GitHub connector), so
     only artifacts built by this workflow can be signed.
   - Create a CI user and copy its **API token**.
3. **On GitHub**, repository *Settings > Secrets and variables > Actions*:
   - Secret `SIGNPATH_API_TOKEN` = the CI user's API token.
   - Variable `SIGNPATH_ORGANIZATION_ID` = your SignPath organisation ID.

That's it. The next `v*` tag waits for your approval (twice: binaries, then installer, up to an
hour each), checks the installer's signature and publishes the signed release.

## Notes

- The Inno Setup uninstaller (`unins000.exe`) is generated on the user's PC and stays unsigned.
- A new certificate starts with little SmartScreen "reputation"; the warning fades as more
  people download signed releases.
- The signed installer is also what the Microsoft Store needs for an EXE submission.
