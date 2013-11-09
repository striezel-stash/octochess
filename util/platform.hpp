#ifndef __PLATFORM_H__
#define __PLATFORM_H__

#ifndef WINDOWS
  #if _WIN32 || _WIN64 || WIN32 || WIN64 || _MSC_VER
    #define WINDOWS 1
  #endif
#endif

#if WINDOWS
  #include "windows.hpp"
#else
  #define UNIX 1
  #include "unix.hpp"
#endif

#ifndef MAX_THREADS
#define MAX_THREADS 64
#else
static_assert( MAX_THREADS >= 1 && MAX_THREADS <= 64, "MAX_THREADS needs to be between 1 and 64 inclusive." );
#endif

/*
 * Allocates a block of memory of size bytes with a start adress being an
 * integer multiple of the system's memory page size.
 * Needs to be freed using aligned_free.
 */
void* page_aligned_malloc( uint64_t size );

void aligned_free( void* p );

// Returns the system's memory page size.
uint64_t get_page_size();

// Forward bitscan, returns zero-based index of lowest set bit and nulls said bit.
// Precondition: mask != 0
inline uint64_t bitscan_unset( uint64_t& mask ) {
	uint64_t index = bitscan( mask );
	mask &= mask - 1;
	return index;
}

inline uint64_t generic_popcount( uint64_t w )
{
      w = w - ((w >> 1) & 0x5555555555555555ull);
      w = (w & 0x3333333333333333ull) + ((w >> 2) & 0x3333333333333333ull);
      w = (w + (w >> 4)) & 0x0f0f0f0f0f0f0f0full;
      return (w * 0x0101010101010101ull) >> 56;
}

bool uses_native_popcnt();
bool cpu_has_popcnt();

void millisleep( int ms );

#endif
