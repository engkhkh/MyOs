@echo off
rem Boots the MyOS CD image through the GRUB bootloader, like a real PC would.
set QEMU="C:\Program Files\qemu\qemu-system-i386.exe"
if not exist %QEMU% (
  echo QEMU not found at %QEMU%. Install it first.
  pause
  exit /b 1
)
%QEMU% -cdrom "%~dp0myos.iso" -boot d -m 256 -rtc base=localtime -audiodev dsound,id=snd0 -device sb16,audiodev=snd0 -machine pcspk-audiodev=snd0 -netdev user,id=n0 -device e1000,netdev=n0 -drive file="%~dp0disk.img",format=raw,index=0,media=disk
