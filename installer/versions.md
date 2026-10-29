# Supported host versions

One build of LookMatch covers every supported version. It installs into Adobe's shared MediaCore
folder, which every Premiere Pro and After Effects version since CC 2015 reads:

- macOS: `/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/`
- Windows: `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\`

| Host | Oldest supported | Notes |
|---|---|---|
| Premiere Pro | CC 2015 (9.0) | Compiles against the CC 2015 SDK headers. Newer hosts also use `PluginDataEntryFunction2`, older ones read the PiPL |
| After Effects | CC 2015 (13.5) | Same |
| macOS | 10.9 (Intel), 11 (Apple Silicon) | Universal binary, `CMAKE_OSX_DEPLOYMENT_TARGET` 10.9 |
| Windows | 7 SP1 / 10, 64-bit | |

Installers:
- macOS: `installer/mac/build_pkg.sh <AE SDK>/Examples` builds `LookMatch-Installer.pkg`. It lists the
  Premiere / After Effects versions it finds and logs them to `/Library/Logs/LookMatch-install.log`.
- Windows: `installer/windows/LookMatch.iss` (Inno Setup 6) lists the versions found in the registry and
  flags any older than CC 2015. To change that floor, edit `MIN_PREMIERE` / `MIN_AE`.

Only versions still installable from Creative Cloud can be tested before release. Older versions
are supported by API level, not by testing.
