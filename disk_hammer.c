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
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/uio.h>

#define KiB (1024UL)
#define MiB (KiB*KiB)
#define GiB (MiB*KiB)
#define TiB (GiB*KiB)
#define PiB (TiB*KiB)

#define ELAPSED_NS(start,stop) \
  (((int64_t)stop.tv_sec-start.tv_sec)*1000*1000*1000+(stop.tv_nsec-start.tv_nsec))

// Allow compile-time customization of seed value
#ifndef SEED
#define SEED 1
#elif SEED == -1
#undef SEED
#define SEED time(NULL)
#endif

// Allow compile-time customization of default chunk size.  Chunk size must be
// compatible with alignment requirements of target filesystems when using
// O_DIRECT, which is always used if the target filesystem supports it.
#ifndef DEFAULT_CHUNK_SIZE
#define DEFAULT_CHUNK_SIZE 4096
#endif

// Allow compile-time customization of chunk count.
#ifndef DEFAULT_CHUNK_COUNT
#define DEFAULT_CHUNK_COUNT 2
#endif

// Show help message
void usage(const char *argv0) {
    printf(
      "Usage: %s [options] OUTFILE [LENGTH [ITERS]]\n"
      "\n"
      "Options:\n"
      "  -h,      --help      .Show this message\n"
      "  -s SIZE, --size=SIZE  Specifies chunk size in bytes [%lu]\n"
      "  -c NUM,  --count=NUM  Number of unique chunks [%u]\n"
      "  -n,      --dry-run    Dry run, no data written\n"
      "  -v,      --verbose    Display more info\n"
//    "  -V,   --version        Show version\n"
      "\n"
      "Defaults:\n"
      "  LENGTH  512 MiB\n"
      "  ITERS     1 iteration\n"
      "  SIZE   4096 bytes\n"
      "  NUM       2 unique chunks\n"
      "\n"
      "LEGNTH and SIZE can have suffix of k/m/g/t/p for KiB/MiB/GiB/TiB/PiB\n"
      "Passing 0 for ITERS means loop forever\n"
      ,argv0, (size_t)DEFAULT_CHUNK_SIZE, DEFAULT_CHUNK_COUNT
    );
}

size_t strtosize(const char *s)
{
  char * suffix;
  size_t size = strtoul(s, &suffix, 0);
  switch(*suffix) {
    case 'k':
    case 'K':
      size *= KiB;
      break;
    case 'm':
    case 'M':
      size *= MiB;
      break;
    case 'g':
    case 'G':
      size *= GiB;
      break;
    case 't':
    case 'T':
      size *= TiB;
      break;
    case 'p':
    case 'P':
      size *= PiB;
      break;
  }

  return size;
}

// Enum for command line parsing status
enum cmdline_status {
  cmdline_ok,
  cmdline_help,
  cmdline_error
};

// Structure to hold parameters from command line options
struct dh_opts {
  size_t chunk_size;
  uint32_t chunk_count;
  int dry_run;
  int verbose;
};

// Returns index of first non-option argv element (i.e. filename) or
// 0 if help was requested or -1 if an error is encountered.  For return values
// less than 1, the dh_opts structure pointed to by opts is not changed.
int parse_command_line(int argc, char * argv[], struct dh_opts * opts)
{
  int opt;
  enum cmdline_status cmdline_status = cmdline_ok;

  // Working values that will update opts just before successful return
  struct dh_opts tmp_opts = {
    .chunk_size = DEFAULT_CHUNK_SIZE,
    .chunk_count = DEFAULT_CHUNK_COUNT,
    .dry_run = 0
  };

  static struct option long_opts[] = {
    {"help",     0, NULL, 'h'},
    {"size",     0, NULL, 's'},
    {"count",    0, NULL, 'c'},
    {"dry-run",  0, NULL, 'n'},
    {"verbose",  0, NULL, 'v'},
//  {"version",  0, NULL, 'V'},
    {0,0,0,0}
  };

  while((opt=getopt_long(argc,argv,"hc:ns:v",long_opts,NULL))!=-1) {
    switch (opt) {
      case 'h':
        usage(argv[0]);
        cmdline_status = cmdline_help;
        break;

      case 'c':
        tmp_opts.chunk_count = strtoul(optarg, NULL, 0);
        if(tmp_opts.chunk_count == 0) {
          fprintf(stderr, "chunk count cannot be zero\n");
          cmdline_status = cmdline_error;
        }
        break;

      case 'n':
        tmp_opts.dry_run = 1;
        break;

      case 's':
        tmp_opts.chunk_size = strtosize(optarg);
        if(tmp_opts.chunk_count == 0) {
          fprintf(stderr, "chunk size cannot be zero\n");
          cmdline_status = cmdline_error;
        }
        break;

      case 'v':
        tmp_opts.verbose = 1;
        break;

      case '?': // Command line parsing error
      default:
        cmdline_status = cmdline_error;
        break;
    }

    if(cmdline_status != cmdline_ok) {
      break;
    }
  }

  if(cmdline_status == cmdline_error) {
    return -1;
  } else if(cmdline_status == cmdline_help) {
    return 0;
  }

  // If a filename was given, optind will be less than argc, otherwise it is an
  // error.
  if(optind >= argc) {
    usage(argv[0]);
    return -1;
  }

  // Success, update opts
  *opts = tmp_opts;

  return optind;
}

int main(int argc, char *argv[])
{
  int i;
  int fd;
  int argi;
  int oflags;
  int niters;
  size_t file_size;
  size_t buffer_size;
  uint64_t file_chunks;
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
  struct dh_opts opts;

  argi = parse_command_line(argc, argv, &opts);

  if(argi < 1) {
    return -argi;
  }

  filename = argv[argi];

  file_size = 512 * MiB; // Default filesize 512 MiB
  if(argc > argi+1) {
    file_size = strtosize(argv[argi+1]);
  }

  niters = 1;
  if(argc > argi+2) {
    niters = strtol(argv[argi+2], NULL, 0);
  }

  // Number of chunks per file
  file_chunks = file_size / opts.chunk_size;
  // Adjust file_size (in case file_size was non-multiple of chunk size)
  file_size = file_chunks * opts.chunk_size;

  if(file_size == 0) {
    printf("error: requested file size is smaller than chunk size\n");
    return 1;
  } else if(file_chunks < opts.chunk_count) {
    printf("warning: requested file size smaller than all unique chunks\n");
  }

  if(opts.verbose) {
    printf("using %u unique chunks of %lu bytes each\n",
        opts.chunk_count, opts.chunk_size);
  }

  if(niters == 0) {
    printf("writing %ld bytes to %s infinite times\n",
        file_size, filename);
  } else {
    printf("writing %ld bytes to %s %d times\n",
        file_size, filename, niters);
  }

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
  } else if(opts.verbose) {
    printf("using alignment of %d bytes\n", alignment);
  }

  // Validate alignment (must be less than or equal to chunk size)
  if(alignment > opts.chunk_size) {
    printf("error: alignment requirement is greater than chunk size\n");
    return 1;
  }

  // Allocate buffer with suitable alignment
  buffer_size = opts.chunk_size + (opts.chunk_count-1)*alignment;
  if((errno=posix_memalign((void **)&buffer, alignment, buffer_size))) {
    perror("posix_memalign");
    return 1;
  }

  // Lock buffer in place
  if(mlock(buffer, buffer_size)) {
    perror("mlock");
    return 1;
  }

  // Fill buffer with random data
  srandom(SEED);
  for(i=0; i < buffer_size; i++) {
    buffer[i] = random() % 0xff;
  }

  if(opts.dry_run) {
    if(opts.verbose) {
      printf("dry run requested, no data written\n");
    }
    return 0;
  }

  // Allocate iovec array
  iovs = malloc((file_chunks+(opts.chunk_count-1)) * sizeof(*iovs));
  if(!iovs) {
    perror("malloc[iovs]");
    return 1;
  }
  // Populate iovec array
  for(i=0; i<file_chunks+(opts.chunk_count-1); i++) {
    iovs[i].iov_base = buffer + (i % opts.chunk_count) * alignment;
    iovs[i].iov_len = opts.chunk_size;
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
    piov = &iovs[i % opts.chunk_count];
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
