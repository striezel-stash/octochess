#include "hash.hpp"
#include "statistics.hpp"

#include <string.h>

hash transposition_table;

struct entry {
	uint64_t v;
	uint64_t key;
} __attribute__((__packed__));

uint64_t const bucket_entries = 4;
unsigned int const bucket_size = sizeof(entry) * bucket_entries;

hash::hash()
	: size_()
	, bucket_count_()
	, lock_block_size_()
	, data_()
{
}


hash::~hash()
{
	delete [] data_;
}


bool hash::init( unsigned int max_size )
{
	size_ = static_cast<hash_key>(max_size) * 1024 * 1024;

	// Make sure size is a multiple of block size
	bucket_count_ = size_ / bucket_size;
	size_ = bucket_count_ * bucket_size;

	lock_block_size_ = ((bucket_count_ * bucket_entries) + ((bucket_count_ * bucket_entries) % RWLOCKS) ) / RWLOCKS;

	delete [] data_;
	data_ = 0;
	data_ = new entry[ bucket_count_ * bucket_entries ];
	memset( data_, 0, size_ );

	for( int i = 0; i < RWLOCKS + 1; ++i ) {
		init_rw_lock( rwl[i] );
	}

	return true;
}


namespace field_shifts {
enum type {
	age = 0,
	depth = 8,
	move = 16,
	node_type = 32,
	score = 48
};
}

namespace field_masks {
enum type {
	age = 0xff,
	depth = 0xff,
	move = 0xfff,
	node_type = 0x3,
	score = 0xffff
};
}

void hash::store( hash_key key, unsigned char remaining_depth, short eval, short alpha, short beta, move const& best_move, unsigned char clock )
{
	unsigned long long bucket_offset = (key % bucket_count_) * bucket_entries;
	entry* bucket = data_ + bucket_offset;

	scoped_exclusive_lock l(rwl[ bucket_offset / lock_block_size_ ]);

	uint64_t v = static_cast<unsigned long long>(clock) << field_shifts::age;
	v |= static_cast<unsigned long long>(remaining_depth) << field_shifts::depth;
	v |= (
				(static_cast<unsigned long long>(best_move.source_col)) |
				(static_cast<unsigned long long>(best_move.source_row) << 3) |
				(static_cast<unsigned long long>(best_move.target_col) << 6) |
				(static_cast<unsigned long long>(best_move.target_row) << 9) ) << field_shifts::move;

	if( eval >= beta ) {
		v |= static_cast<unsigned long long>(score_type::lower_bound) << field_shifts::node_type;
	}
	else if( eval <= alpha ) {
		v |= static_cast<unsigned long long>(score_type::upper_bound) << field_shifts::node_type;
	}
	else {
		v |= static_cast<unsigned long long>(score_type::exact) << field_shifts::node_type;
	}
	v |= static_cast<unsigned long long>(eval) << field_shifts::score;

	for( unsigned int i = 0; i < bucket_entries; ++i ) {
		if( (bucket + i)->key == key ) {
			(bucket + i)->v = v;
			return;
		}
	}

	unsigned char lowest_depth = 255;
	entry* pos = 0;
	for( unsigned int i = 0; i < bucket_entries; ++i ) {
		unsigned char old_age = ((bucket + i)->v >> field_shifts::age) & field_masks::age;
		unsigned char old_depth = ((bucket + i)->v >> field_shifts::depth) & field_masks::depth;
		if( old_age != clock && old_depth < lowest_depth ) {
			lowest_depth = old_depth;
			pos = bucket + i;
		}
	}

	if( pos ) {
		pos->v = v;
#if USE_STATISTICS
		if( !pos->key ) {
			++stats_.entries;
		}
#endif
		pos->key = key;

		return;
	}

	lowest_depth = 255;
	for( unsigned int i = 0; i < bucket_entries; ++i ) {
		unsigned char old_depth = ((bucket + i)->v >> field_shifts::depth) & field_masks::depth;
		if( old_depth < lowest_depth ) {
			lowest_depth = old_depth;
			pos = bucket + i;
		}
	}

	pos->v = v;
#if USE_STATISTICS
	if( !pos->key ) {
		++stats_.entries;
	}
#endif
	pos->key = key;
}


bool hash::lookup( hash_key key, unsigned char remaining_depth, short alpha, short beta, short& eval, move& best_move )
{
	unsigned long long bucket_offset = (key % bucket_count_) * bucket_entries;
	entry const* bucket = data_ + bucket_offset;

	scoped_shared_lock l(rwl[ bucket_offset / lock_block_size_ ]);

	for( unsigned int i = 0; i < bucket_entries; ++i, ++bucket ) {
		if( bucket->key != key ) {
			continue;
		}

		unsigned char depth = (bucket->v >> field_shifts::depth) & field_masks::depth;
		if( depth >= remaining_depth ) {
			unsigned char type = (bucket->v >> field_shifts::node_type) & field_masks::node_type;
			eval = (bucket->v >> field_shifts::score) & field_masks::score;
			if( ( type == score_type::exact ) ||
				( type == score_type::lower_bound && beta <= eval ) ||
				( type == score_type::upper_bound && alpha >= eval ) )
			{
#if USE_STATISTICS
				++stats_.hits;
#endif
				return true;
			}
		}

#if USE_STATISTICS
		++stats_.best_move;
#endif

		best_move.other = 1;
		best_move.source_col = (bucket->v >> field_shifts::move) & 0x07;
		best_move.source_row = (bucket->v >> (field_shifts::move + 3)) & 0x07;
		best_move.target_col = (bucket->v >> (field_shifts::move + 6)) & 0x07;
		best_move.target_row = (bucket->v >> (field_shifts::move + 9)) & 0x07;

		return false;
	}

#if USE_STATISTICS
	++stats_.misses;
#endif

	best_move.other = 0;
	return false;
}


hash::stats hash::get_stats(bool reset)
{
	stats ret = stats_;

	if( reset ) {
		stats_.hits = 0;
		stats_.misses = 0;
		stats_.best_move = 0;
	}
	return ret;
}

uint64_t hash::max_hash_entry_count() const
{
	return bucket_count_ * bucket_entries;
}
