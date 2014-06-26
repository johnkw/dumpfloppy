= About dumpfloppy

This is a suite of tools for reading floppy disks in arbitrary formats
supported by the PC floppy controller, and for working with the resulting
image files. For image files, it uses the IMD format defined by Dave
Dunfield's ImageDisk; you can capture an image with these tools and then write
it back with ImageDisk, or use the tools to examine or extract an ImageDisk
file.

I've used these tools successfully to read a large number of PC, BBC Micro, RM
380Z and Alphatronic PC floppies. The dumpfloppy tool currently requires the
Linux floppy device, although it would be relatively straightforward to port
to a different OS, provided there's a way of issuing raw commands to the PC
floppy controller; the other tools will work on any platform.
 --Adam Sampson

= Tools

* dumpfloppy reads a floppy disk, automatically identifying the format, and
  writes out an IMD file. It automatically retries several times when read
  errors occur, and displays status output indicating what it's read. The
  strategy it uses is based on How to identify an unknown disk.

* imdcat reads an IMD file, and can display information about it, display a hex
  dump of the sector data in it, or write the data to a flat file (e.g. for use
  with an emulator).

* floppyinfo reads a set of IMD files and displays a summary of information
  about their formats and contents, including directory listings when possible.
  (At present, it only understands a very limited range of formats!)

-------------------------------------------------------------------------

aclocal --force && autoconf -f && automake --add-missing && ./configure

To dump disk with failed reads, first:
    imdcat -o ~/floppy.img ~/floppy.imd

Then open a separate terminal and look at:
    imdcat -x ~/floppy.imd | less
