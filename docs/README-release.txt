BigScale
========

Use
  Start the game, open Settings, and select the Upscaling category.
  NVIDIA RTX hardware is required for the DLSS backends.

Additional providers and runtimes (optional, not included)
  BigScale ships its standard DLSS, FidelityFX and XeSS runtimes. See _ADDITIONAL_PROVIDERS.txt in
  this folder for the ANR download, the official NVIDIA DLSS source, the DLSS neural-rendering file,
  the exact destinations and the switching rules. BigScale never redistributes ANR or
  nvngx_dlssnr.dll. If an optional runtime is missing, only that feature is unavailable and the
  status line reports why.

Upgrading from BigWalkDLSS
  This is the first public BigScale 1.0.0 release, not an in-place upgrade. Delete
  BepInEx\plugins\BigWalkDLSS before installing. BigScale uses its own plugin directory and
  configuration file and never reads or migrates the old one.

Not affiliated
  This is an unofficial mod. NVIDIA, AMD, Intel, Unity Technologies and the developers and
  publishers of Big Walk do not endorse or support it.

Troubleshooting
  BepInEx\LogOutput.log and BepInEx\BigScale\native.log (created only when [Diagnostics] NativeLog
  is enabled).

Licence
  See LICENSE and THIRD-PARTY-NOTICES.txt.
