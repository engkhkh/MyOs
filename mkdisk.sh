#!/bin/bash
# Creates disk.img: a 64 MB hard disk with one FAT32 partition and some sample files.
set -e
rm -f disk.img
truncate -s 64M disk.img
echo 'start=2048, type=c' | sfdisk -q disk.img
mkfs.fat -F 32 -n MYOS --offset 2048 disk.img >/dev/null
IMG="disk.img@@1M"
T=$(mktemp -d)
printf 'Welcome to MyOS!\n\nThis is drive C:, a FAT32 disk.\nTry these in the Terminal:\n  dir\n  type welcome.txt\n  write hello.txt Hi there\n  mkdir Projects\n  cd Docs\n\nOr open File Explorer and double-click files.\n\nPrograms live in C:\\Apps. Type a name to run one:\n  calc  solitaire  freecell  spider  web  paint  music  viewer  snake\n  mines  widgets  hello  bounce  guess  spin  crash\n\nMusic is in C:\\Music, pictures in C:\\Pictures.\n\nTip: select text in Notepad with the mouse or Shift+arrows,\nthen Ctrl+C to copy and Ctrl+V to paste - in Notepad, the\nTerminal or Calculator. Ctrl+Z undoes. Ctrl+F finds,\nCtrl+H replaces.\n' > "$T/Welcome.txt"
printf 'MyOS README\n===========\nA Windows-style operating system written from scratch.\n' > "$T/README.TXT"
printf 'Ideas for MyOS\n- Paint app\n- Games\n- Networking\n- Run on a watch\n' > "$T/Ideas for MyOS.txt"
printf 'Shopping list\n- milk\n- bread\n- coffee\n' > "$T/shopping list.txt"
mcopy -i "$IMG" "$T/Welcome.txt" "$T/README.TXT" ::/
mmd -i "$IMG" ::/Docs ::/Pictures ::/Desktop
printf 'My notes\n\nAnything you put in C:\\Desktop shows up on the desktop.\nDrag files here from File Explorer, or right-click the\ndesktop for New folder / New text file.\n' > "$T/My notes.txt"
mcopy -i "$IMG" "$T/My notes.txt" ::/Desktop/
mcopy -i "$IMG" "$T/Ideas for MyOS.txt" "$T/shopping list.txt" ::/Docs/
python3 mkmedia.py "$T/media"
mmd -i "$IMG" ::/Music
mcopy -i "$IMG" "$T/media/Music/"*.wav ::/Music/
mcopy -i "$IMG" "$T/media/Pictures/"*.bmp "$T/media/Pictures/"*.png media/*.jpg media/*.gif ::/Pictures/
mmd -i "$IMG" ::/Apps
for f in apps/*.elf; do mcopy -i "$IMG" "$f" ::/Apps/; done
rm -rf "$T"
echo "disk.img ready"
