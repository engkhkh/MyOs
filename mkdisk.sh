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
mmd -i "$IMG" ::/Docs ::/Pictures ::/Desktop ::/Notes ::/Contacts ::/Calendar
# a few sample events around the day the disk is made
{ echo "$(date -d '+2 days' +%Y-%m-%d) 19:30 Dinner with Sara"; echo "$(date -d '+6 days' +%Y-%m-%d) 09:00 Team meeting";
  echo "$(date -d '+6 days' +%Y-%m-%d) --:-- Pay the rent"; echo "$(date -d '+20 days' +%Y-%m-%d) 16:00 Dentist"; } > "$T/Events.txt"
mcopy -i "$IMG" "$T/Events.txt" ::/Calendar/
printf 'Weekend plans\n\n- Finish the Spider game (2 suits!)\n- Call grandma\n- Clean the desk\n' > "$T/Weekend plans.txt"
printf 'Book ideas\n\nA story about an operating system that learns to dream.\n' > "$T/Book ideas.txt"
mcopy -i "$IMG" "$T/Weekend plans.txt" "$T/Book ideas.txt" ::/Notes/
printf 'Name: Sara Ahmed\nPhone: +966 50 123 4567\nEmail: sara.ahmed@example.com\nAddress: Riyadh\nBirthday: 14 March\n\nName: Omar Hassan\nPhone: +20 100 555 0199\nEmail: omar@example.com\nAddress: Cairo\n\nName: Lina Khaled\nPhone: 050 222 3344\nEmail: lina.k@example.com\nNotes: Met at the conference\n\nName: David Miller\nPhone: +44 20 7946 0000\nEmail: david@example.co.uk\nAddress: London\n\n' > "$T/Contacts.txt"
mcopy -i "$IMG" "$T/Contacts.txt" ::/Contacts/
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
