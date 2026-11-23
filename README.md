# MyOS — a Windows-style OS built from scratch

Version 2.5 (stage 25: Notes and Contacts). A 32-bit x86 operating system with a graphical
desktop (1024x768, mouse, draggable windows, taskbar, Start menu), a FAT32 drive C:,
and real programs: they are loaded from disk, run in user mode (ring 3) in their own
protected memory, and share the CPU through preemptive multitasking.
Built-in apps: File Explorer, Terminal, Notepad, Task Manager, Clock, This PC, About.
Programs in C:\Apps: **Solitaire**, **FreeCell**, **Calculator**, **Web** (browser), **Paint**, **Music**, **Pictures** (viewer), **Snake**, **Minesweeper**,
hello, bounce, guess, spin, crash. Sound: PC speaker + Sound Blaster 16.

## Run it on Windows
1. Download the ready-to-run zip (**MyOS-x.y.zip**) from the repository's **Releases** page
   and unzip it. The built files (myos.bin, myos.iso, disk.img) are only in the release
   zip; the repository itself holds the source code.
2. Install QEMU: https://www.qemu.org/download/#windows
3. Double-click **run.bat** (quick start), or **run-iso.bat** (boots the CD image via GRUB)

**disk.img** is drive C: — everything you save goes into that file, so it is kept
between runs. Copy it to keep a backup; delete it and copy a fresh one to reset.
Windows can't open it directly, but tools like 7-Zip can look inside.

Also works in VirtualBox: new VM (Other / 32-bit), attach **myos.iso** as the CD and
disk.img as an IDE hard disk (convert first: `VBoxManage convertfromraw disk.img disk.vdi`).

## Using the desktop
- Double-click desktop icons, or use **Start** (or F1 / Windows key)
- Drag windows by the title bar; _ minimises, X closes; taskbar buttons switch windows
- **Windows**: drag by the title bar, minimise, close. **Resize** Notepad, File Explorer,
  Web and Pictures by dragging any edge or corner (the pointer changes shape); the
  **maximise** button or a double-click on the title bar fills the screen, and again
  restores it. **Snap**: drag a window against the top of the screen to maximise it, or
  against the left / right edge to fill that half (a see-through preview shows where);
  pulling it away gives it back its old size. **Alt+F4** closes the window in
  front (and ends its program); **Alt+Tab** brings the window at the back to the front -
  press it again to go through all windows. The Windows key opens the Start menu
- **File Explorer**: two views - **Icons** (big tiles; every picture shows a preview,
  made in the background a few at a time) and **List** (name, size, date). Switch with
  the "View" button or Ctrl+E; the choice is remembered. Type the first letters of a name
  to jump to it. Double-click folders to enter, double-click files to open them
  (Notepad, Music, Pictures, or run a program). Buttons: Up, New folder, New file, Rename,
  Copy, Cut, Paste, Delete. Keys: arrows, Enter, Backspace (up a folder), F2 rename,
  Ctrl+C / Ctrl+X / Ctrl+V copy / cut / paste (whole folders too). A pasted
  name that already exists becomes "name - Copy".
  **Delete** (button or Del) moves the item to the **Recycle Bin**; **Shift+Del** deletes
  it forever (asks first). **Drag and drop**: drag a file or folder onto a folder to move
  it there (hold **Ctrl** to copy), onto the **Up** button to move it one folder up, onto
  the **Recycle Bin** icon on the desktop to delete it, or onto Notepad to open it there. A
  label next to the pointer says what will happen
- **Movable desktop icons**: drag any desktop icon (This PC, Recycle Bin, files...) to a
  new spot; it snaps to the grid, swaps with an icon already there, and the others stay
  where they are. The places are kept in C:\$Desktop.ini (they survive a restart).
  Right-click the desktop > **Auto arrange icons** puts everything back in order
- **Files on the desktop**: everything in **C:\Desktop** is shown on the desktop, in
  columns from the right-hand edge (folders first). Double-click to open; drag a file or
  folder from File Explorer onto the desktop to move it there (Ctrl = copy), drag desktop
  items into an Explorer window, onto a desktop folder or onto the Recycle Bin.
  Right-click an item: Open, Cut, Copy, Paste into folder, Delete, Rename, Properties.
  Right-click empty desktop: New folder, New text file, Paste. After clicking the desktop,
  Del, F2, Enter, Ctrl+C / Ctrl+X / Ctrl+V and Up/Down work on its icons
- **Right-click menus** everywhere:
  - File Explorer, on an item: Open, Open with Notepad, Cut, Copy, Paste into folder,
    Delete, Rename, Properties. On empty space: View, New folder, New text file, Paste,
    Properties of the folder. In the Recycle Bin: Restore, Delete forever, Restore all,
    Empty Recycle Bin
  - **Properties** (also **Alt+Enter**): type, location, size, what a folder contains
    (files, folders, total size), date; for several items the totals
  - Title bars and taskbar buttons: Restore, Minimise, Maximise, Close
  - The desktop: File Explorer, Terminal, Task Manager, Change wallpaper, About; desktop
    icons: Open (the Recycle Bin also has Empty Recycle Bin)
  - Notepad: Undo, Cut, Copy, Paste, Select all, Find. Terminal: Paste
  - Keyboard: the **Menu** key opens the menu for the selected file; Up/Down, Enter, Esc
- **Selecting many items** in File Explorer: **Ctrl-click** adds or removes one item,
  **Shift-click** selects everything from the last clicked item, **Shift+arrows** grow the
  selection, **Ctrl+A** selects all, **Esc** goes back to one item. Or press on an empty
  spot and **drag a box** over the items (hold Ctrl to add to what is selected). The status
  bar says how many are selected. Delete, Shift+Del, Copy, Cut, Paste, drag and drop and
  the bin's Restore all work on the whole selection
- **Recycle Bin** (desktop icon - it shows paper when something is inside): lists deleted
  items with where they came from. **Restore** puts the selected item back (its folder is
  made again if needed), **Restore all**, **Empty Recycle Bin** (asks first), **Delete**
  removes one item for good. The bin is the hidden folder C:\$Recycle and survives restarts
- **Calculator** (Start > Calculator, or `calc`): + - x / , %, 1/x, x^2, square root,
  +/-, CE / C / backspace; press = again to repeat the last step. Works with the mouse
  or keyboard (digits, + - * /, Enter, Backspace, Esc = C, Del = CE). Uses real
  floating-point maths (15 digits) - the kernel now saves each program's FPU state
- **Notepad**: New / Open / Save buttons and a file-name box; Ctrl+S saves, Ctrl+N new.
  Click anywhere to put the cursor there; arrows, Home/End, PgUp/PgDn move it. Select with
  the mouse (drag; double-click = word, triple-click = line) or Shift+arrows; Ctrl+A selects
  all. Ctrl+C / Ctrl+X / Ctrl+V copy, cut, paste; Ctrl+Z undo (6 steps). The status bar
  shows the line and column. Files up to 32 KB.
  **Find** (Ctrl+F): a bar opens under the toolbar; matches are marked in yellow and the
  status bar says "Match 3 of 12". Enter / F3 = next, Shift+Enter / Shift+F3 = previous,
  "Match case" check box, Esc closes. **Replace** (Ctrl+H): Replace (one at a time) or
  Replace all (one Ctrl+Z undoes it)
- **Clipboard**: one shared clipboard for text. Copy in Notepad, paste in the Terminal
  (Ctrl+V pastes one line), in Calculator (Ctrl+C copies the result, Ctrl+V pastes a
  number) or in your own programs (`clipset` / `clipget` system calls)
- Terminal: type `help`. Up/Down arrows recall earlier commands
- **Programs**: type a name (`hello`, `bounce`, `guess`, `spin`, `crash`), double-click
  a `.elf` file in File Explorer, or Start > Programs. Each opens its own window;
  closing the window ends the program
- **Task Manager**: live CPU graph, every process with CPU % and memory, End task
- **Paint**: left button paints, right button erases; colours, 4 brush sizes, Fill bucket,
  New, Save / Ctrl+S -> C:\Pictures\painting.bmp (a normal BMP; reopened next time)
- **Music**: plays .wav files from C:\Music (8/16-bit, mono/stereo, 4-48 kHz) with a
  level meter; Space play/stop, arrows choose, +/- volume. Double-click a .wav in Explorer
- **Pictures**: shows **.jpg / .jpeg**, **.png**, **.gif** and .bmp files up to 6 megapixels:
  JPEG photos (normal and progressive, any subsampling, turned upright using the
  camera's EXIF orientation - phone photos come out the right way up) and every kind of
  PNG (see-through logos on a checkerboard, palette pictures, 16-bit, interlaced).
  Big pictures are shrunk smoothly to fit; **100%** (or F) shows the real size - drag to
  look around. Prev / Next or Left / Right flip through the folder. **Set as wallpaper**
  (or W) puts the picture on the desktop at once. Double-click a picture in Explorer
- **Sound**: a start-up chime and `beep` / `tune` use the PC speaker; music uses the
  Sound Blaster 16. run.bat starts QEMU with both (Windows audio via dsound)
- **Settings** (Start > Settings, or `settings`): wallpaper (4 designs or any .jpg / .png /
  .bmp from C:\Pictures, scaled smoothly to fill the screen), accent colour (6), time zone, volume,
  start-up chime on/off, mouse speed. Saved at once to C:\settings.ini (plain text,
  editable in Notepad) and restored at every start
- **Network**: an Intel e1000 network card driver and a home-made TCP/IP stack
  (Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS, TCP). run.bat connects MyOS to the
  internet through QEMU, so it gets an address automatically
- **Web**: type an address and press Enter (Ctrl+L focuses the bar). Shows the text and
  clickable links of http:// pages, then loads the page's **pictures** (PNG, JPEG, GIF,
  BMP): small ones sit in the text, big ones get their own space, pictures inside links
  can be clicked, and a picture's own address shows the picture. Esc stops loading;
  scrolling works while pictures load. Back, reload, redirects. Try http://info.cern.ch -
  the first website. https:// sites are not supported yet (they need encryption)
- **Solitaire** (Start > Solitaire, or `solitaire`): classic Klondike. Drag cards with the
  mouse, click the deck to turn cards, double-click a card to send it up to the Aces.
  Game menu: New game (F2), Undo (Ctrl+Z, many steps), Finish, Draw 1 / Draw 3. Score,
  moves and time at the bottom; when every card is face up it finishes by itself - and
  winning ends with the bouncing cards
- **Notes** (Start > Notes, or `notes`): quick notes that save themselves a moment after
  you stop typing. Each note is a text file in **C:\Notes** named after its first line
  (so it also opens in Notepad and File Explorer). The list on the left shows each title
  and the start of its text; the search box filters by any word. The editor wraps long
  lines and has mouse selection, double-click to select a word, Ctrl+A / C / X / V,
  arrows, Home/End, PgUp/PgDn. Ctrl+N new note, Ctrl+F search, Ctrl+S save now,
  Delete (asks first)
- **Contacts** (Start > Contacts, or `contacts`): an address book - name, phone, email,
  address, birthday, notes - with coloured initials, an A-Z list and a search box that
  looks in every field. Tab moves between fields, Enter or Ctrl+S saves (the name is
  required, the email and phone are checked), Ctrl+N adds a person, Delete asks first.
  Everything is kept in one readable text file, **C:\Contacts\Contacts.txt**
- **Toolkit**: new `ui_textarea()` multi-line text widget (word wrap, selection,
  clipboard, scrolling); new system calls `deletefile()` (41) and `makedir()` (42)
- **Spider Solitaire** (Start > Spider, or `spider`, `spider 1`, `spider 2`, `spider 4`):
  two decks in 10 columns. Build down from King to Ace in any suit, but only a run of one
  suit moves together; a full King-to-Ace run of one suit leaves the table - clear all 8.
  Click the stock (bottom right) or press D to deal a card onto every column (not while a
  column is empty). Game menu: Easy 1 suit / Medium 2 suits / Hard 4 suits, Restart, Undo
  (Ctrl+Z), Hint (H - the cards lift and their target is outlined). Double-click a card to
  send its run to the best place. Score starts at 500: -1 per move, +100 per finished run
- **FreeCell** (Start > FreeCell, or `freecell`): all cards face up in 8 columns, 4 free
  cells, 4 foundations. Drag cards or runs (as many as your free cells and empty columns
  allow); double-click sends a card to a foundation or a free cell; safe cards fly to the
  foundations by themselves. The deals are numbered like Windows FreeCell - Game #1 is
  the same game - Game > Select game (F3), or `freecell 617`. Undo with Ctrl+Z
- **Widget Gallery** (`widgets`): every control of the programming toolkit in one window
- **Snake**: arrow keys, P = pause, Space = play again; best score kept in C:\Apps\snake.hi
- **Minesweeper**: 16x16, 40 mines; left click opens, right click flags, click the face for a new game

## Shell commands
Files: dir, cd, type, write <file> <text>, append, copy <a> <b>, ren <a> <name>,
mkdir, del, disk — names with spaces go in quotes: `type "shopping list.txt"`
Apps: notepad [file], explorer [folder], open <app>, taskmgr
Programs: <name> or run <name>, ps, kill <pid>
Network: ipconfig, ping <host>, nslookup <host> (Ctrl+C stops ping), web [address]
Sound: beep, tune, volume [0-100]   (and the music / viewer programs, e.g. `music`)
Settings: settings (window); tz <hours> and volume <0-100> are saved too
System: about, mem, memtest, uptime, time, tz <hours>, clear, color, echo,
beep, gfxbench, bsod / pagefault (crash the kernel on purpose), reboot, shutdown

## Files
| File | What it does |
|---|---|
| boot.asm | Multiboot entry, stack (asks GRUB for graphics in the -grub build) |
| interrupts.asm | GDT, exception and IRQ entry stubs |
| idt.c | Interrupt table, PIC, timer, keyboard + mouse IRQs |
| mm.c | Physical page allocator, paging, kmalloc/kfree heap |
| gfx.c | Framebuffer driver (GRUB or Bochs/QEMU adapter), drawing, text, cursor |
| font.h | 8x16 bitmap font generated from DejaVu Sans Mono |
| mouse.c | PS/2 mouse driver |
| shell.c | Command shell on a character grid |
| wm.c | Window manager: desktop, taskbar, Start menu, windows, apps |
| ata.c | IDE/ATA hard-disk driver (reads and writes 512-byte sectors) |
| fat32.c | FAT32 file system: folders, long file names, read/write/delete/rename |
| proc.c | Processes: scheduler, ELF program loader, system calls |
| settings.c | Loads/saves C:\settings.ini and applies time zone and volume |
| sound.c | PC speaker tone queue + Sound Blaster 16 driver (16-bit DMA, IRQ 5) |
| e1000.c | Intel e1000 network card driver (DMA rings, polled) |
| net.c | Network stack: ARP, IPv4, ICMP ping, UDP, DHCP, DNS, TCP |
| image.c / image.h | Picture decoding shared by the kernel (wallpaper) and programs: PNG (own inflate), GIF, BMP, smooth resizing |
| jpeg.c | JPEG decoder: baseline + progressive, subsampling, restart markers, EXIF orientation |
| mkmedia.py | Generates the sample songs and pictures (all original) |
| make_photos.py / media/ | Renders the sample JPEG photos (needs numpy + Pillow); the results are kept in media/ |
| syscalls.h | System call numbers (shared with programs) |
| kernel.c | Boot sequence, keyboard decoding, clock, crash screen, main loop |
| apps/ | User programs + their library (myos.h, malloc.c, crt0.asm, user.ld) |
| mkdisk.sh | Builds disk.img (64 MB, FAT32 partition, sample files) |
| grub.cfg | Boot menu for the ISO |
| myos.bin / myos-grub.bin / myos.iso | Ready-built kernel / GRUB kernel / bootable CD image |
| disk.img | Drive C: (64 MB FAT32 hard-disk image) |

## Build (Linux or WSL)
    sudo apt install gcc nasm qemu-system-x86 grub-pc-bin grub-efi-amd64-bin xorriso mtools dosfstools fdisk python3
    make          # myos.bin + myos-grub.bin
    make iso      # myos.iso (BIOS + UEFI bootable)
    make disk.img # sample drive C:
    make run      # or: make run-iso

## Real hardware (experimental)
myos.iso can be written to a USB stick (e.g. with Rufus in DD mode) and booted.
GRUB sets up the screen on BIOS and UEFI PCs. It needs a PS/2-compatible keyboard
and mouse (many laptops' built-in keyboards qualify; USB mice and touchpads
usually need "legacy USB" in the BIOS and are not supported directly yet).
Networking needs an Intel e1000-family card (common on older PCs; most newer ones use
other chips). Disk access needs an IDE disk or a SATA controller set to "IDE/legacy" mode in the
BIOS; modern AHCI/NVMe drives are not supported yet, so on most new laptops MyOS
runs without drive C:. It never writes to a disk that isn't FAT32.

## Writing your own program
Programs can use **apps/ui.h**, the MyOS widget toolkit: menus, buttons, check boxes,
radio buttons, text boxes (with clipboard), lists, progress bars and message boxes.
Each frame is drawn in the program's own memory and shown in one step, so nothing
flickers. Start from apps/widgets.c; add your program to `UI_APPS` in the Makefile.
To let the user resize or maximise your window, call `ui_resizable(min_w, min_h)`; when
`ui.resized` is set, lay out again for the new `ui.w` x `ui.h` (see apps/viewer.c).

Copy `apps/hello.c`, add its name to `APPS` in the Makefile, then `make disk.img`.
Programs use the functions in `apps/myos.h`: print, getkey, pollkey, getevent /
pollevent (keyboard + mouse), sleep, fill, text, clear, blit (draw a whole image),
resize (window size up to 1000x640), malloc / free, readfile, writefile, gettime,
ticks, readline, rand, tone / play / stopsound / soundpos / volume, getarg (the file
the program was opened with), listdir, resolve / tcpopen / tcpsend / tcprecv /
tcpclose (see apps/web.c for a full HTTP client)... Each program starts with a 480x320 window and 64 KB of
stack, its heap grows on demand, and it can't touch the kernel or other programs.

## Roadmap
1. ✅ Boot + text desktop + shell
2. ✅ Interrupts, timer, keyboard IRQ, crash screen
3. ✅ Memory: page allocator, paging, kernel heap
4. ✅ Graphics desktop, mouse, windows, apps, bootable ISO
5. ✅ Disk driver + FAT32 file system, File Explorer, Notepad save/open
6. ✅ Preemptive multitasking, user mode, system calls, ELF programs, Task Manager
7. ✅ Mouse + keyboard events for programs, malloc, resizable windows, Paint, Snake, Minesweeper
8. ✅ Sound (PC speaker + Sound Blaster 16), Music player, Picture viewer, open-with in Explorer
9. ✅ Networking: e1000 driver, TCP/IP stack, ipconfig/ping/nslookup, Web browser
10. ✅ Settings: wallpaper (designs + pictures), accent colour, time zone, volume, chime, mouse speed
11. ✅ Calculator (floating point, per-program FPU state), Explorer rename / copy / cut / paste / delete folders
12. ✅ System clipboard, Notepad editing (cursor, selection, copy/cut/paste, undo), Terminal and Calculator paste
13. ✅ Widget toolkit for programs (apps/ui.c: menus, buttons, text boxes, lists, dialogs), Solitaire, Widget Gallery
14. ✅ Notepad Find / Replace, FreeCell (Windows game numbers), shared card code (apps/cards.c)
15. ✅ PNG pictures (image.c: inflate + all PNG types, shared by kernel and programs), new Pictures app, "Set as wallpaper"
16. ✅ JPEG photos (jpeg.c: baseline + progressive, EXIF orientation; matches libjpeg-turbo pixel for pixel)
17. ✅ Pictures in the Web browser; GIF pictures (first frame of animations, transparency, interlacing)
18. ✅ Explorer Icons view with picture previews (fast 1/8-size JPEG decoding), type-to-select,
    Alt+F4 / Alt+Tab, Notepad opens new file names, mouse moves never lost; a full regression test
19. ✅ Resizable windows: edges and corners, maximise / restore, double-click title, snap to
    top / left / right; programs opt in (`resizable()`, EV_RESIZE; ui.h: `ui_resizable`, `ui.resized`)
20. ✅ Recycle Bin (delete / restore / empty, desktop icon) and drag and drop in File Explorer
21. ✅ Select many files at once (Ctrl/Shift-click, Shift+arrows, Ctrl+A, selection box); copy, move, delete, drag them together
22. ✅ Right-click menus (Explorer, Recycle Bin, desktop, title bars, taskbar, Notepad, Terminal) and Properties
23. ✅ Files on the desktop (C:\Desktop): drag to and from File Explorer, right-click New / Paste
24. ✅ Spider Solitaire (1, 2 or 4 suits, hints, undo) and movable desktop icons
25. ✅ Notes and Contacts apps; multi-line text widget; delete-file / make-folder system calls
26. More apps (Calendar, a text-based Mail), HTTPS (TLS)
27. 64-bit, USB keyboard/mouse/touchpad, AHCI/NVMe disks; ARM port for watches
