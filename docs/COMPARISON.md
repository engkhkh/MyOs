# MyOS compared with Windows 11, macOS and Ubuntu Linux

Written for MyOS 2.7 and kept up to date (2.8 dark mode, 2.9 virtual desktops, 3.0 Arabic added). "Yes" means the feature exists and works in
MyOS today; "Partly" means a simpler version exists; "No" means it is missing.
The big three are mature systems built by large teams over decades; MyOS is a
from-scratch learning OS, so the point of this table is to show what is
there, what is simpler, and what to build next.

## At a glance

| Area | Windows 11 | macOS | Ubuntu (GNOME) | MyOS 2.7 |
|---|---|---|---|---|
| CPU mode | 64-bit, many cores | 64-bit, many cores | 64-bit, many cores | 32-bit, one core |
| Multitasking | Preemptive, threads | Preemptive, threads | Preemptive, threads | Preemptive processes, no threads |
| Memory protection | Per process, ASLR, DEP | Per process, ASLR | Per process, ASLR | Per process (ring 3 + own page tables); no ASLR |
| File system | NTFS (journaled, permissions) | APFS (snapshots, encryption) | ext4 (journaled, permissions) | FAT32 (no journal, no permissions) |
| Users and sign-in | Accounts, passwords, Windows Hello | Accounts, Touch ID | Accounts, sudo | **One user, optional lock-screen password (new)** |
| Lock screen | Win+L | Ctrl+Cmd+Q | Super+L | **Win+L, start-up lock (new)** |
| Start / launcher search | Start search (apps, files, settings) | Spotlight | Activities search | **Start search: apps and files (new)** |
| Notifications | Toasts + notification centre | Notification Centre | Message tray | **Pop-ups + notification centre (new)** |
| Calendar reminders | Yes | Yes | Yes | **Yes, even with Calendar closed (new)** |
| Screenshots | PrtScn, Snipping Tool | Cmd+Shift+3/4 | PrtScn tool | **PrtScn saves a PNG (new)**; no area selection |
| Window management | Snap layouts, virtual desktops, Task View | Spaces, Mission Control | Workspaces, overview | Move, resize, maximise, snap halves, Alt+Tab, Win+D, **virtual desktops + Task View (2.9)** |
| File manager | Explorer: tabs, search, previews | Finder | Files (Nautilus) | Explorer: icons/list, previews, multi-select, drag and drop, Recycle Bin, right-click menus, Properties |
| Desktop icons | Yes | Yes | Extension | Yes, files in C:\Desktop, movable |
| Dark mode / themes | Yes | Yes | Yes | **Dark mode (2.8)**, accent colours, wallpapers |
| Accessibility | Narrator, magnifier, high contrast | VoiceOver, zoom | Orca, zoom | **No** |
| Languages / right-to-left | Many, incl. Arabic | Many, incl. Arabic | Many, incl. Arabic | English and **Arabic (3.0)**: keyboard, joined letters, right to left, Arabic file names; no other scripts yet |
| Fonts | Scalable (TrueType), many | Scalable | Scalable | One bitmap font (8x16), 2x/6x scaled |
| Graphics | GPU drivers, DirectX | Metal | Mesa / Vulkan | Framebuffer only (no GPU acceleration) |
| Sound | Mixer, many devices | Core Audio | PipeWire | PC speaker + Sound Blaster 16, one volume |
| Networking | Wi-Fi, Ethernet, IPv6, TLS | Same | Same | Ethernet (e1000), IPv4, DHCP, DNS, TCP; **no Wi-Fi, no HTTPS** |
| Web browser | Edge (full web) | Safari | Firefox | Simple HTML + pictures, http only |
| Hardware | USB, NVMe, Bluetooth, ACPI power | Same | Same | PS/2 keyboard + mouse, IDE disk; **no USB, no NVMe, no Bluetooth** |
| Updates / app store | Windows Update, Store | App Store | apt, Snap | **No** (programs are copied to C:\Apps) |
| Apps included | Many | Many | Many | Explorer, Notepad, Terminal, Paint, Calculator, Music, Pictures, Web, Notes, Contacts, Calendar, Clock, Task Manager, Settings, 5 games |
| Runs on | PCs, tablets | Macs | PCs, servers, Raspberry Pi | QEMU (and PCs with BIOS + IDE + PS/2) |

## What MyOS 2.7 took from the others

The gaps that matter most in everyday use, and that fit a single step, were
built into MyOS 2.7:

1. **Search from Start** (like Windows Start search, macOS Spotlight, GNOME
   Activities): open Start and just type. Apps come first, then files and
   folders anywhere on C: (names, not contents). Up/Down and Enter, or click.
2. **Notifications** (like the Windows notification centre / macOS
   Notification Centre): a pop-up at the bottom right for 6 seconds, and a bell
   on the taskbar that keeps the last 20, with a red count of unread ones and
   "Clear all". Clicking one opens what it is about. Programs can send them
   too (new system call 43, `notify(title, text)`), and the Terminal has
   `notify <text>`.
3. **Calendar reminders**: 10 minutes before an event and when it starts,
   even when Calendar is closed, each one only once.
4. **Screenshots** (like PrtScn on Windows / Cmd+Shift+3 on macOS): Print Screen
   saves the whole screen as a PNG in C:\Pictures\Screenshots and the pop-up
   opens it. MyOS writes the PNG itself (png_write.c).
5. **Lock screen** (like Win+L / Ctrl+Cmd+Q / Super+L): Win+L, the Lock button in
   Start, or the `lock` command. A big clock and the date over the wallpaper.
   With a password (`password <new>` in the Terminal; `password off` removes it)
   the screen asks for it, and MyOS starts locked. Only a hash of the password is
   kept in settings.ini - enough to keep a curious person at the keyboard out,
   not strong security (FAT32 has no permissions, so anyone with the disk can
   read every file).
6. **Windows-key shortcuts**: Win alone opens Start (when let go, like Windows),
   Win+E File Explorer, Win+D show the desktop (again: bring the windows back),
   Win+L lock.
7. Start menu redesign: a search box on top and Lock / Restart / Shut down
   buttons at the bottom, like Windows 11.

## What is still missing (the next steps, roughly in order)

- A scalable (vector) font, other scripts (Hebrew, Chinese...) and full Unicode bidi rules
- Accessibility: magnifier, high contrast, keyboard-only navigation everywhere
- HTTPS (TLS) so the browser can open real sites
- User accounts with file permissions (needs a file system with permissions)
- 64-bit, several CPU cores, USB, NVMe, ACPI power management, Wi-Fi
