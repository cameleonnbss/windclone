# WindClone in Oracle VirtualBox

One script, one VM, configured for what this OS actually is.

```bat
create-windclone-vm.cmd C:\path\to\windclone-v0.1.34.iso
VBoxManage startvm WindClone
```

`create-windclone-vm.cmd` looks for `windclone-v0.1.34.iso` next to itself, then
for a local build at `../../windclone.iso`; pass a path as the first argument to
pick the ISO explicitly. Re-running it deletes and rebuilds the VM, so it is
safe to use after every release.

## Why each setting is that value

| Setting | Value | The reason |
|---|---|---|
| Storage | **no hard disk**, DVD holds the ISO | WindClone has no block-device driver. Its only writable store is tmpfs, so a virtual disk would be a device the guest cannot open. Everything boots from the ISO and nothing survives a power off. |
| Boot order | DVD first | The image is a hybrid ISO with Limine's MBR, so a BIOS boot reads the catalog directly. |
| NIC | **Intel PRO/1000 MT Desktop (82540EM)**, NAT | That is the e1000 the kernel drives (`src/kernel/e1000.c`). Any other emulated chip is a device WindClone will never find on the PCI bus. |
| Serial | UART0, COM1 `0x3F8`, output to a host file | The 16550 the kernel logs to. A VirtualBox boot then produces the same serial transcript the QEMU suite greps, which is how you compare the two. |
| Input | PS/2 mouse and keyboard | The only controllers the kernel decodes (`src/kernel/keyboard.c`, `src/kernel/mouse.c`). |
| Memory / CPU / VRAM | 512 MB, 2 vCPU, 128 MB | Matches what the QEMU tests give the image, and the kernel's heap reservation is sized against it. |
| Audio, USB | off | No driver exists for either; enabling them only adds devices the kernel has to ignore. |

## UEFI instead of BIOS

Limine has an EFI entry and CI already boots this same image through OVMF:

```bat
VBoxManage modifyvm WindClone --firmware efi --boot1 dvd
```

## What to expect on screen

The Windows-style desktop: wallpaper, taskbar, start menu with one row per
program in `/bin`, windows you can drag and close, a terminal that runs real
ring-3 processes, colour in program windows, and a bitmap decoder
(`/bin/imgview /src/logo.bmp`) that paints pixels rather than glyphs.

## What will not work — said before you discover it

* **Nothing installs or persists.** No disk driver, so tmpfs is the whole
  filesystem and a reboot erases every file a program wrote.
* **No DNS and no TLS.** The stack does DHCP, ARP, ICMP, UDP, one TCP
  connection and a plain HTTP `GET`. `fetch /second.html` works against a local
  server; the public web does not resolve or handshake.
* **Networking depends on the emulation.** VirtualBox's NAT gives a 10.0.2.x
  guest the same way QEMU's slirp does, but the tests' server at
  `10.0.2.2:8080` exists only because QEMU was started with it. In VirtualBox
  you must forward a port to something running on the host:
  `VBoxManage modifyvm WindClone --natpf1 "http,tcp,,8080,,8080"`.
* **The display is untested here.** This host's VirtualBox has its `VBoxSDS`
  service disabled, and re-enabling it requires an administrator account — a
  script cannot supply that, and this README will not pretend the VM booted when
  it was never started. The authoritative test of what boots is the headless QEMU
  suite (`tools/local-test.sh`), which runs on every commit and every tag.

If a VirtualBox boot does not reach the desktop, the first thing worth reading
is the serial file (`%TEMP%\windclone-serial.log`), because it is the same
evidence the tests use.
