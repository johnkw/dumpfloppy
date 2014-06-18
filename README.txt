aclocal --force && autoconf -f && automake --add-missing && ./configure

To dump disk with failed reads, first:
    imdcat -o ~/floppy.img ~/floppy.imd

Then open a separate terminal and look at:
    imdcat -x ~/floppy.imd | less
