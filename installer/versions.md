# Supported host versions

| Host | Oldest supported | Why |
|---|---|---|
| Premiere Pro | 13.0 (CC 2019) | Effect controls, pixel-format suite and button params all present; still installable from Creative Cloud for testing |
| After Effects | 16.0 (CC 2019) | Same API level |
| macOS | 10.15 Catalina | Oldest target the current Xcode can build for (check `LOOKMATCH_MACOS_MIN`) |
| Windows | 10 (64-bit) | Premiere 2019+ is 64-bit only |

The plug-in goes into Adobe's shared MediaCore folder, which every Premiere and After Effects
version since CC reads, so one install covers every version on the machine:

- macOS: `/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/`
- Windows: `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\`

Both installers detect the installed Premiere Pro and After Effects versions. They list what they
found, and warn (without blocking) when a version is older than the floor above. To change the floor,
edit `MIN_PREMIERE` / `MIN_AE` in `macos/distribution.xml` and `windows/LookMatch.iss`.
