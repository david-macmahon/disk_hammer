# disk\_hammer.c

A program to repeatedly overwrite a file many many times as fast as possible.

# Purposes

The purposes of this program are:

  1. Determine how fast data can be written to a given
     file/filesystem/device.

  2. Determine the endurance of flash devices by writing to them until
     death.

# Theory of operation

The program works by:

  1. Allocating a buffer in memory (size specified on command line)
  2. Locking it in place
  3. Filling it with random data from `random()`
  4. Creating an array of `iovec` structures that point to the memory buffer
     such that the total sum of bytes reaches the target file size
     specified on command line.
  5. Looping N times (N specified on command line, defaults to infinite):
     5.1 Opens output file for writing using O\_DIRECT flag
     5.2 Writes data to output file using `writev`
     5.3 Closes output file

The size of the buffer is specified by the command line options -s/--size
and -c/--count.  The --count option specified the number of "chunks"
comprising the buffer. The --size option specifies the sise of each chunk.
The buffer will be allocated such that it satifies the alignment
recommendations from pathconf() for the given filename (or its containing
directory if it doesn't exist).  The chunk size must be an integer multiple
of this alignment size.  A typical alignment size is 4096 bytes and that is
the default alignment size used if none can be determined by pathconf().
Smaller chuck sizes may "work" in that they may not result in errors, but
they will likely result in poorer performance.  This program sometimes
refers to the chunks comprising the buffer as "unique chunks" because they
are unique within the buffer.

If the output file size given on the command line is not an integer multiple
of the chuck size, then the file size is rounded down to the nearest integer
multiple of chunk size less than the requested file size.  This determines
the total number of chunks that will be written to the file.  The same
limited set of unique chunks from the buffer will be used to fill the output
file, so chunks are not unique within a file unless the file contains only
COUNT chunks.

The array of iovec structures is actualy (COUNT-1) elements longer than
needed so that the desired file length can be obtained by starting with any
of the first COUNT elements of the array.  This is used to ensure that the
file contents are different with each iteration of the write loop.  The
first iteration starts the output file with the first chunk (pointed to by
the first iovec element).  The second iteration starts the output file with
the second chunk, and so on for the first COUNT iterations.  That pattern
repeats until the requested number of iterations have been completed.

The pseduo-random number generator is seeded with SEED, which is defined at
compile time.  The default value for SEED is 1 (the same as the random()
function on Linux, if not POSIX).  This means that the byte sequences
written should the same each time the program is invoked.  If a different
SEED value is desired, add "-DSEED=<seed>" to CFLAGS when compiling.  The
program can be compiled for non-constant seeding by setting defining SEED
to -1, in which case the output of `time(NULL)` is used as the seed.  For
example:

    # Use 2 as the PRNG seed
    make CFLAGS=-DSEED=2

    # Use `time(NULL)` as the PRNG seed
    make CFLAGS=-DSEED=-1

Passing the -v/--verbose option will result in more information being
displayed at startup.  If the program was compiled with zlib support, the
verbose output will include the CRC value for each unique chunk.  The CRC
value shown is the same value computed by the ubiquitous `cksum` utility.
This can be used to verify that the output file contains the expected data.

# Examples

Here are some examples:

  1. Verbosely create 8K test file containing 2 unique 4K chunks

    $ disk_hammer -v -s 4k testfile 8k
    using 2 unique chunks of 4096 bytes each
    writing 8192 bytes to testfile 1 times
    using alignment of 4096 bytes
    chunk 0 cksum 55cbd682 1439422082
    chunk 1 cksum f3221a34 4079098420
    2019-01-28 07:49:40 UTC wrote 8192 bytes in 272338 ns (0.241 Gbps)

  2. Show that cksum of first chunk matches 1439422082

    $ dd if=testfile bs=4096 count=1 2>/dev/null | cksum
    1439422082 4096

  3. Show that cksum of second chunk matches 4079098420

    $ dd if=testfile bs=4096 count=1 skip=1 2>/dev/null | cksum
    4079098420 4096
