# Installer and releases

`AppSandbox.iss` is the installer, built with [Inno Setup 6](https://jrsoftware.org/isinfo.php). It was chosen over MSI (WiX) because App Sandbox installs no host drivers or services, which is where MSI earns its complexity, and because Inno gives one signed `.exe`, an uninstaller, and a script that can wait for the app's own mutex and reopen the app after an update.

## Building it

Build the solution first, then:

```
winget install --id JRSoftware.InnoSetup -e
.\tools\installer\build-installer.ps1 -SignThumbprint <thumbprint> -TestCerts
```

The result is `bin\Release\installer\AppSandbox-<version>-win-x64-setup.exe`, plus a `.sha256`. The version comes from `Directory.Build.props`. `-TestCerts` is for test-signed builds, whose guests need the drivers' `.cer` files.

## Releasing

Change the version in `Directory.Build.props`, build, then make a **draft**:

- **Test-signed** (the current setup, on the PC holding the test certificate):
  ```
  .\tools\installer\build-installer.ps1 -SignThumbprint 6546C03C0B5C55CD8688FCC203AD5598844C89C2 -TestCerts
  gh release create v0.1.7.0 bin\Release\installer\AppSandbox-0.1.7.0-win-x64-setup.exe --draft --prerelease --title "App Sandbox 0.1.7.0"
  ```
- **EV-signed:** run the *Signed release* workflow with a `release_tag`. It needs the self-hosted runner and YubiKey, and makes the draft for you.

Nothing reaches users until someone publishes the draft. The app finds a release through its asset, which must be named exactly as above, so a release without one is ignored. A pre-release is offered only to people who turned on **Include pre-releases**; publish it as a full release to offer it to everyone.

## Signing and the update check

The updater accepts only installers signed by a certificate pinned in `src/app_win/update_config.h`, by SHA-256. A valid signature from anyone else is refused, and an unsigned installer never updates anyone. To change certificate, first ship a release signed by the old one that adds the new pin. A fork must change the pin and the repository in that file.

## Testing it

- `tools\headless-api\tests\test_installer_contract.py`: the rules, from the sources.
- `tools\headless-api\tests\test_updater_mock.py`: the updater against a mock feed and tampered files.
- `test-install.ps1 -Setup <setup.exe>`: a real install, upgrade, refusal while the app runs, and uninstall, in a temporary folder. It needs an elevated prompt and refuses to run where App Sandbox is already installed. CI runs it on every pull request. `build-installer.ps1 -TestPerUser` with `test-install.ps1 -PerUser` covers most of it without elevation.

Not covered anywhere automatically: the app's own update run from a live window, which needs an interactive desktop and a release newer than the running build.
