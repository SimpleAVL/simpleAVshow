# Prepare a Windows PC for remote testing

Start with **RustDesk**, so the assistant can operate the Windows desktop from the Mac running this project. Add **Windows OpenSSH over Tailscale** after the first connection works, so builds, file transfers, and log collection can run from the Mac terminal.

The first goal is to test the packaged Windows build on the real PC. Its ZIP includes the runtime dependencies; installing a development environment is a later step if a failure needs a local rebuild.

## Your first steps

1. Start the Windows 11 x64 PC, connect it to the internet, and sign in to the desktop. Keep it plugged into power and stay nearby for the first connection.
2. Download the Windows x64 RustDesk client from the [official download page](https://rustdesk.com/). Run it and use **Install** if it offers an installation option. Approve the Windows administrator prompt. The installed client is preferred for setup involving elevated windows; RustDesk also documents a separate [portable elevation workflow](https://rustdesk.com/docs/en/client/windows/windows-portable-elevation/).
3. Open RustDesk and wait until it reports that it is ready. Send the assistant the **RustDesk ID**, and say that the Windows desktop is ready. Approve the incoming connection when it arrives. Depending on the client's security settings, connection approval may require enabling approval by click, or entering the displayed temporary RustDesk password on the Mac.

You can also install through Windows Terminal if `winget` is available:

```powershell
winget install --id RustDesk.RustDesk --exact --source winget
```

The graphical download is the fallback if `winget` is missing or the package cannot be found. Follow any installer prompts on screen.

**Stop here for now.** The assistant will prepare the Mac client and attempt the connection. You do not need to clone the repository, install Visual Studio, or configure SSH first. RustDesk's [client guide](https://rustdesk.com/docs/en/client/) describes the ID, temporary password, permissions, and service settings.

Account sign-ins, administrator credentials, and any setup of a permanent RustDesk password are tasks for you to complete locally when needed. An approval-based first session avoids having to set up a permanent password.

## First connection: prove access before installing more

The assistant will install/open the Mac RustDesk client if needed, enter the Windows ID, and check all of these:

- A fresh screenshot shows the Windows desktop.
- Mouse input opens Start or Notepad.
- Keyboard input types a short test string in Notepad.
- A harmless PowerShell command returns the Windows version and architecture.
- The connection can interact with the elevated windows needed for setup, or you can approve them locally.

Desktop control through a RustDesk window has not yet been verified in this session. These checks determine whether it works with the available computer-control tools. The session's Mac must stay powered on and accessible too. Mac permission prompts may need your help; RustDesk documents its [Mac installation and input permissions](https://rustdesk.com/docs/en/client/mac/).

If desktop screenshots or input fail, the assistant will diagnose the concrete failure. SSH can still support builds and automated checks, while interactive GUI checks remain pending until desktop access works.

## What the assistant handles after connection

| Work | Who handles it |
| --- | --- |
| Mac RustDesk client, connection, and access checks | Assistant; you handle any local permission prompts |
| Windows version, CPU, RAM, disk, and device inventory | Assistant |
| Tailscale installation on Windows | Assistant |
| Tailscale sign-in to the same network as the Mac | You; usually one browser sign-in |
| SSH installation, key authentication, and connection verification | Assistant |
| Download/extract the selected app package and record its version/hash | Assistant |
| App smoke checks, interactive checks, and evidence collection | Assistant |
| Development tools and local rebuilds if needed | Assistant |
| Plugging in audio/MIDI hardware, checking physical output, and credentials | You, when needed |

The Mac currently has a working Tailscale connection. Check that again when connecting; it is an observation from preparation, not a prerequisite baked into this guide.

## Reliable terminal access

Use **Microsoft OpenSSH Server on Windows**, with Tailscale providing the network connection. Tailscale's own SSH server currently supports Linux and certain macOS installations; it is not the Windows server used here. See [Tailscale SSH limitations](https://tailscale.com/docs/features/tailscale-ssh#limitations) and [Microsoft's installation guide](https://learn.microsoft.com/en-us/windows-server/administration/openssh/openssh_install_firstuse).

The assistant performs this setup through the desktop after the first connection:

1. Install [Tailscale for Windows](https://tailscale.com/docs/install/windows). You complete the sign-in to the same tailnet as the Mac. Verify that the two devices can communicate; a tailnet policy may need to allow this connection.
2. Generate a dedicated Ed25519 SSH key on the Mac, outside the repository. Keep the private key on the Mac. Transfer only its public key to Windows.
3. Use a local Windows account for SSH. Prefer a dedicated standard account for repeatable tests; use elevation through the desktop for machine-wide installations. Record the account name and working directory. Microsoft Entra-only accounts do not support this key-authentication method.
4. Install OpenSSH Server, restrict its firewall access to the Mac's Tailscale IPv4 address, and then start the service. Keep existing unrelated SSH configuration intact.
5. Install the public key with the appropriate Windows permissions. Standard users use their profile's `.ssh/authorized_keys`. The default configuration uses `C:\ProgramData\ssh\administrators_authorized_keys` for administrators, and keys there apply to administrator accounts collectively. Prefer the standard test account instead. Follow [Microsoft's key authentication guide](https://learn.microsoft.com/en-us/windows-server/administration/openssh/openssh_keymanagement).
6. Read the SSH host-key fingerprint on Windows and compare it with the Mac's first connection. Verify a command and an SFTP transfer before relying on SSH. Keep RustDesk working while configuring this.

For reference, these are the elevated Windows installation commands. **The assistant runs them; they are not additional preparation for you.** Replace the example address with the Mac's current `tailscale ip -4` result before running.

```powershell
$ErrorActionPreference = 'Stop'
$macTailscaleIPv4 = '100.64.0.1' # Replace with the Mac's actual Tailscale IPv4.

$capability = Get-WindowsCapability -Online -Name 'OpenSSH.Server~~~~0.0.1.0'
if ($capability.State -ne 'Installed') {
    Add-WindowsCapability -Online -Name 'OpenSSH.Server~~~~0.0.1.0'
}

# Installation can create a broadly scoped default inbound rule.
# Inspect existing SSH rules first on a machine already serving SSH.
Get-NetFirewallRule -Name 'OpenSSH-Server-In-TCP' -ErrorAction SilentlyContinue |
    Disable-NetFirewallRule

$ruleName = 'SimpleAVShow-SSH-from-Mac'
if (Get-NetFirewallRule -Name $ruleName -ErrorAction SilentlyContinue) {
    Set-NetFirewallRule -Name $ruleName -Enabled True -Profile Any -Action Allow
    Get-NetFirewallRule -Name $ruleName | Get-NetFirewallAddressFilter |
        Set-NetFirewallAddressFilter -RemoteAddress $macTailscaleIPv4
} else {
    New-NetFirewallRule -Name $ruleName -DisplayName 'SimpleAVShow SSH from Mac' `
        -Direction Inbound -Action Allow -Protocol TCP -LocalPort 22 `
        -RemoteAddress $macTailscaleIPv4 -Profile Any
}

Set-Service -Name sshd -StartupType Automatic
Start-Service sshd
Get-Service sshd
```

The commands above install and start the service; key installation and verification are separate steps. Other existing inbound rules allowing port 22 must also be inspected before calling access restricted. No router port forwarding is needed for this design. Only change password authentication after key access has been verified, accounting for any other existing SSH users.

The Mac then connects using the dedicated key:

```sh
ssh -i /path/to/dedicated-key savtest@WINDOWS_TAILSCALE_IP
```

SSH provides command execution and files. Launch the interactive app through RustDesk in the signed-in Windows desktop: a process started from an SSH session does not prove that the app is visible on that desktop.

For repeated sessions, the assistant can arrange service startup and [Tailscale's unattended mode](https://tailscale.com/docs/how-to/run-unattended). You can choose to set up a permanent RustDesk password locally. Without unattended desktop access, approve each new RustDesk connection. After a restart, you may still need to sign in to Windows; automatically logging into Windows is not part of this setup. Record any temporary power-setting changes and restore them afterward.

## Test the shipped build first

Use a work directory such as `%LOCALAPPDATA%\SimpleAVShow-test` in the desktop test account. Keep packages, test media, projects, and evidence there. Record the app package source, SHA-256, source revision when available, Windows version, and hardware/driver versions. GitHub account sign-in may be needed to obtain private repository artifacts; the assistant can transfer an artifact downloaded on the Mac without copying repository credentials to the PC.

Extract the entire Windows ZIP into a fresh `package` directory. The executable is `package\bin\simpleavshow.exe`. Keep all accompanying DLLs and plugins together. First run without adding a developer SDK to PATH, so missing packaged dependencies remain visible.

The assistant can reproduce the existing CI screenshot smoke check using Windows PowerShell:

```powershell
$ErrorActionPreference = 'Stop'
$root = Join-Path $env:LOCALAPPDATA 'SimpleAVShow-test'
$app = Join-Path $root 'package\bin\simpleavshow.exe'
$evidence = Join-Path $root 'evidence'
New-Item -ItemType Directory -Force -Path $evidence | Out-Null
$image = Join-Path $evidence 'editor.png'

# Preserve the desktop session's environment after the child exits.
$oldPath = $env:PATH
$oldPlatform = $env:QT_QPA_PLATFORM
$oldPluginPath = $env:QT_PLUGIN_PATH
try {
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    $env:QT_QPA_PLATFORM = 'offscreen'
    Remove-Item Env:QT_PLUGIN_PATH -ErrorAction SilentlyContinue
    if (Test-Path $image) { Remove-Item $image }
    $arguments = '--demo --preview-time 6 --screenshot "{0}"' -f $image
    $process = Start-Process -FilePath $app -ArgumentList $arguments -PassThru
    if (-not $process.WaitForExit(60000)) {
        Stop-Process -Id $process.Id
        throw 'Packaged screenshot check exceeded 60 seconds'
    }
    $process.Refresh()
    if ($process.ExitCode -ne 0 -or -not (Test-Path $image)) {
        throw 'Packaged screenshot check failed'
    }
} finally {
    $env:PATH = $oldPath
    $env:QT_QPA_PLATFORM = $oldPlatform
    $env:QT_PLUGIN_PATH = $oldPluginPath
}
```

Inspect the resulting image, then launch `simpleavshow.exe --demo` normally through the desktop and verify:

- Startup, window layout, menus, and the Outputs dialog.
- Play/pause, holds/resume, seeks, and Stop All.
- Audio/video/image import, project save/reopen, and moved media.
- Show-mode editing locks and keyboard controls.
- Program display selection and fullscreen with the intended monitors connected.
- Remembered devices and missing-device behavior.

If a local rebuild is needed, follow the [Windows build instructions](../README.md#windows-11) and the versions/commands in [.github/workflows/build.yml](../.github/workflows/build.yml). Prefer CI's prebuilt Qt/FFmpeg route to rebuilding both from source. Run `ctest --test-dir build -C Release --output-on-failure` with FFmpeg CLI available, and record skips as well as failures. The release ZIP alone does not provide the development test suite.

For ASIO, multichannel routing, MIDI, and a full rehearsal, follow [hardware validation](hardware-validation.md). Remote desktop audio and compressed screen frames do not establish physical audio/video quality or timing. Use the real interface and displays for those checks; you may need to listen, inspect outputs, or connect a capture/loopback device. Compare performance with desktop streaming paused or disconnected to identify its overhead.

The test report should distinguish passes, failures, and checks still pending on physical equipment. Include version/hash information, reproduction steps, screenshots, and relevant logs. Finish with a verified reconnect after restarting the app; verify reboot recovery separately if ongoing remote testing is wanted.

## After testing

Close the RustDesk connection when finished. For a temporary setup, remove the test SSH public key and its firewall rule, and stop/disable the SSH service if it was added exclusively for this task. Remove the Windows device from Tailscale or uninstall temporary access tools if you no longer want remote access. Preserve unrelated accounts, keys, and existing services. Restore any temporary sleep settings and keep the evidence directory until the results are reviewed.

Prepared on 2026-10-07. This guide and its command examples have been reviewed against the repository and vendor documentation; they have not yet been executed on the Windows PC.
