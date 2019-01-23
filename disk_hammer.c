// disk_hammer.c - A program to repeated overwrite a file many many times as
// fast as possible.  The purposes of this program are:
//
//   1. Determine how fast data can be written to a given
//      file/filesystem/device.
//
//   2. Determine the endurance of flash devices by writing to them until
//      death.
//
// The program works by:
//
//   1. Allocating a buffer in memory (size specified on command line)
//   2. Locking it in place
//   3. Filling it with random data from `random()`
//   4. Creating an array of `iovec` structures that point to the memory buffer
//      such that the total sum of bytes reaches the target file size
//      specified on command line.
//   5. Looping N times (N specified on command line, defaults to infinite):
//      5.1 Opens output file for writing using O_DIRECT flag
//      5.2 Writes data to output file using `writev`
//      5.3 Closes output file
//
// The array of iovec structures is actualy one element longer than needed so
// that the desired file length can be obtained by starting with for first or
// second element of the array.  This can be used to ensure that the file
// contents are different with each write cycle.
//
// The pseduo-random number generator is seeded with SEED, which is defined at
// compile time.  The default value for SEED is 1 (the same as the random()
// function on Linux, if not POSIX).  This means that the byte sequences
// written should the same each time the program is invoked.  If a different
// SEED value is desired, add "-DSEED=<seed>" to CFLAGS when compiling.  The
// program can be compiled for non-constant seeding by setting defining SEED
// to -1, in which case the output of `time(NULL)` is used as the seed.  For
// example:
//
//     # Use 2 as the PRNG seed
//     make CFLAGS=-DSEED=2
//
//     # Use `time(NULL)` as the PRNG seed
//     make CFLAGS=-DSEED=-1

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/uio.h>

#define KiB (1024)
#define MiB (KiB*KiB)
#define GiB (MiB*KiB)
#define TiB (GiB*KiB)
#define PiB (TiB*KiB)

#define ELAPSED_NS(start,stop) \
  (((int64_t)stop.tv_sec-start.tv_sec)*1000*1000*1000+(stop.tv_nsec-start.tv_nsec))

// Allow user to specify custom seed value
#ifndef SEED
#define SEED 1
#elif SEED == -1
#undef SEED
#define SEED time(NULL)
#endif

int main(int argc, char *argv[])
{
  int i;
  int fd;
  int oflags;
  int niters;
  size_t file_size;
  size_t chunk_size = 4096;
  int file_chunks;
  int alignment;
  int iovs_idx;
  int iovs_remaining;
  int iovs_to_write;
  char * buffer;
  struct iovec * iovs;
  struct iovec * piov;
  const char * filename;
  struct timespec start, stop;
  int64_t elapsed_ns;

  if(argc < 2) {
    printf("usage: %s FILENAME [GiB [ITERS]]\n", argv[0]);
    return 1;
  }
  filename = argv[1];

  file_size = 512;
  if(argc > 2) {
    file_size = strtol(argv[2], NULL, 0);
  }
  file_size *= GiB;

  niters = 1;
  if(argc > 3) {
    niters = strtol(argv[3], NULL, 0);
  }

  if(niters == 0) {
    printf("writing %ld bytes to '%s' infinite times\n",
        file_size, filename);
  } else {
    printf("writing %ld bytes to '%s' %d times\n",
        file_size, filename, niters);
  }

  // Number of chunks per file
  file_chunks = file_size / chunk_size;
  // Adjust file_size (in case file_size was non-multiple of chunk_size)
  file_size = file_chunks * chunk_size;

  // Use pathconf to find required/recommended I/O alignment, if any
  errno = 0;
  alignment = pathconf(filename, _PC_REC_XFER_ALIGN);
  if(alignment == -1 && !errno) {
    perror("pathconf");
    return 1;
  } else if(alignment == -1) {
    // No requirement, default to 512
    alignment = 512;
    printf("using default alignment of 512 bytes\n");
  } else {
    printf("using alignment of %d bytes\n", alignment);
  }

  // Allocate buffer with suitable alignment
  if((errno=posix_memalign((void **)&buffer, alignment, 2*chunk_size))) {
    perror("posix_memalign");
    return 1;
  }

  // Lock buffer in place
  if(mlock(buffer, 2*chunk_size)) {
    perror("mlock");
    return 1;
  }

  // Fill buffer with random data
  srandom(SEED);
  for(i=0; i < 2*chunk_size; i++) {
    buffer[i] = random() % 0xff;
  }

  // Allocate iovec array
  iovs = malloc((file_chunks+1) * sizeof(*iovs));
  if(!iovs) {
    perror("malloc[iovs]");
    return 1;
  }
  // Populate iovec array
  for(i=0; i<=file_chunks; i++) {
    iovs[i].iov_base = buffer + (i%2)*chunk_size;
    iovs[i].iov_len = chunk_size;
  }

  oflags = O_WRONLY | O_CREAT | O_DIRECT;

  // Main loop
  for(i=0; i < niters || niters == 0; i++) {
    // Get start time
    clock_gettime(CLOCK_MONOTONIC, &start);

    // Open file
    fd = open(filename, oflags, 0666);
    if(fd == -1) {
      // Maybe O_DIRECT is not supported?
      if(errno == EINVAL && i == 0 && (oflags & O_DIRECT)) {
        oflags &= ~O_DIRECT;
        // Open file
        fd = open(filename, oflags, 0666);
        if(fd == -1) {
          perror(filename);
          return 1;
        } else {
          printf("warning: O_DIRECT not supported for %s\n", filename);
        }
      } else {
        perror(filename);
        return 1;
      }
    }

    // Write file.  The number of iovecs that can be written in one call to
    // writev is limited to IOV_MAX, so we have to loop through iovs in case
    // file_chunks is greater than IOV_MAX.
    piov = &iovs[i%2];
    iovs_remaining = file_chunks;
    while(iovs_remaining > 0) {
      iovs_to_write = (iovs_remaining > IOV_MAX) ? IOV_MAX : iovs_remaining;
      // TODO Handle incomplete writes (which are allowed)
      if(writev(fd, piov, iovs_to_write) == -1) {
        perror("writev");
        fprintf(stderr, "iter %d iovs_remaining %d iovs_to_write %d\n",
            i, iovs_remaining, iovs_to_write);
        fprintf(stderr, "piov %p iov_base %p iov_len %lu\n",
            piov, piov->iov_base, piov->iov_len);
        return 1;
      }
      piov += iovs_to_write;
      iovs_remaining -= iovs_to_write;
    }

    // Close file
    if(close(fd) == -1) {
      perror("close");
      return 1;
    }

    // Get stop time
    clock_gettime(CLOCK_MONOTONIC, &stop);
    elapsed_ns = ELAPSED_NS(start, stop);

    // Output timing stats
    // TODO Limit/aggregate stats reports if elapsed time is short?
    printf("wrote %lu bytes in %lu ns (%.3f Gbps)\n",
        file_size, elapsed_ns, (8.0 * file_size)/elapsed_ns);
  }

  return 0;
}
