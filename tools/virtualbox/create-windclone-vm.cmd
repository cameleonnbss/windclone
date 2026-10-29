@echo off
rem WindClone - create an Oracle VirtualBox VM that boots the current ISO.
rem
rem Usage:   create-windclone-vm.cmd [path\to\windclone.iso]
rem Default ISO: the windclone-v0.1.34.iso next to this script, or
rem                ..\..\windclone.iso (a local build).
rem
rem What this configures, and why each line is what it is:
rem   * no hard disk - WindClone has no block-device driver yet, its only
rem     writable store is tmpfs, so a disk would be dead weight and a lie
rem   * DVD drive holds the hybrid ISO; the image boots from it through
rem     Limine's MBR (BIOS) - the EFI/OVMF entry is the optional switch below
rem   * NIC type 82540EM is the e1000 chip the kernel actually drives
rem     (src/kernel/e1000.c); VirtualBox's Intel PRO/1000 MT Desktop is that
rem     device, so the guest's ARP/DHCP/TCP path is talking to a NIC it knows
rem   * UART0 is COM1 at 0x3F8, the 16550 the kernel logs to - file-enabled so
rem     the serial log lands on the host and can be compared with QEMU's
setlocal enabledelayedexpansion

set VBOX=C:\Program Files\Oracle\VirtualBox\VBoxManage.exe
set NAME=WindClone
if not exist "%VBOX%" (
    echo ERROR: VBoxManage.exe not found at "%VBOX%"
    echo Install VirtualBox, or edit the VBOX line in this script.
    exit /b 1
)

set ISO=%~1
if "%ISO%"=="" set ISO=%~dp0windclone-v0.1.34.iso
if not exist "%ISO%" set ISO=%~dp0..\..\windclone.iso
if not exist "%ISO%" (
    echo ERROR: no ISO found. Pass one as the first argument:
    echo     create-windclone-vm.cmd C:\path\windclone.iso
    exit /b 1
)
for %%I in ("%ISO%") do set ISO=%%~fI
echo Using ISO: !ISO!

"%VBOX%" list vms | findstr /C:"\"%NAME%\"" >nul
if not errorlevel 1 (
    echo VM "%NAME%" already exists - deleting it to rebuild cleanly.
    "%VBOX%" controlvm "%NAME%" poweroff >nul 2>&1
    "%VBOX%" unregistervm "%NAME%" --delete >nul 2>&1
)

echo Creating VM...
"%VBOX%" createvm --name "%NAME%" --ostype Other --ostype64 --register || exit /b 1
"%VBOX%" modifyvm "%NAME%" --memorysize 512 --cpus 2 --vram 128 ^
    --nic1 nat --nictype1 82540EM --cable-connected1 on ^
    --audio none --usb off --mouse ps2 --keyboard ps2 ^
    --serial0 1-4F8 --uartmode0 file --uartfilename "%TEMP%\windclone-serial.log" ^
    --boot1 dvd --boot2 floppy --boot3 none --boot4 none || exit /b 1
"%VBOX%" storagectl "%NAME%" --name "PIIX4" --add ide --controller PIIX4 || exit /b 1
"%VBOX%" storageattach "%NAME%" --storagectl "PIIX4" --port 1 --device 0 ^
    --type dvddrive --medium "%ISO%" || exit /b 1

echo.
echo Done. Boot it with:
echo     "%VBOX%" startvm "%NAME%"
echo or headless (serial log is written to:):
echo     "%VBOX%" startvm "%NAME%" --type headless
echo     %TEMP%\windclone-serial.log
echo.
echo Optional - UEFI instead of BIOS (Limine has an EFI entry, and CI boots
echo the same image through OVMF):
echo     "%VBOX%" modifyvm "%NAME%" --firmware efi --boot1 dvd
echo.
echo Honest note: WindClone boots, shows its desktop, runs programs and answers
echo over its e1000 NIC, but it has no disk driver - nothing the guest writes
echo survives a power off, and there is no way to install it. See
echo docs/WINDOWS-VM-PLAN.md and docs/ROADMAP.md ("A block device driver").
endlocal
