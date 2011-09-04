#ifndef __PAWN_STRUCTURE_HASH_TABLE_H__
#define __PAWN_STRUCTURE_HASH_TABLE_H__

#include <stdint.h>

/*
 * Hash table to hold the pawn structure evaluation.
 * The general idea is the following:
 * Naive hash structure using always-replace.
 * Entry size is a power of two, thus somewhat cache-friendly.
 * Using Hyatt's lockless transposition table algorithm
 *
 * Key 64bit zobrist over pawns, white's point of view.
 *
 * Data is 16bit evaluation, 48 bit spare.
 *
 * If there are type-1 collisions, they are not handled the slightest.
 * Rationale being pawn evaluation only having a slight impact on overall
 * strength. Effort in handling collisions would exceed potential gain in
 * strengh. Also, pawn table collisions are more rare than transposition table
 * type-1 collisions due to lower number of pawn structures compared to full
 * board positions.
 */

class pawn_structure_hash_table
{
	struct entry;
public:
	class stats
	{
	public:
		stats()
			: hits()
			, misses()
		{
		}

		uint64_t hits;
		uint64_t misses;
	};

	pawn_structure_hash_table();
	~pawn_structure_hash_table();

	bool init( uint64_t size_in_mib );

	bool lookup( uint64_t key, short& eval ) const;

	void store( uint64_t key, short eval );

	stats get_stats( bool reset );

private:
	mutable stats stats_;

	entry* data_;
	unsigned long long size_;
};


extern pawn_structure_hash_table pawn_hash_table;

#endif