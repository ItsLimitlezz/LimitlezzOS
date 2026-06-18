# Local App Samples

These packages are SDK 0.1 foreground-shell samples that can be copied into a
T-Deck SD/appfs app root. They intentionally use only manifest metadata,
bounded display text, foreground actions, read-only tokens, and scoped counter
storage. They do not execute arbitrary script code.

Install into a simulator or card-style root:

```sh
python scripts/validate_local_app_samples.py --install-root .pio/local-app-samples --clean
```

The resulting layout is:

```text
.pio/local-app-samples/apps/<sample>/manifest.json
.pio/local-app-samples/apps/<sample>/main.lua
```

For hardware, copy the package folders under one of the firmware scan roots:

- `/sd/limitlezz/apps/`
- `/sd/apps/`
- `/appfs/apps/`

