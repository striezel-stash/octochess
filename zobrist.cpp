#include "zobrist.hpp"
#include "chess.hpp"
#include "util.hpp"
#include "random.hpp"

namespace {
static uint64_t pawns[2][64];
static uint64_t knights[2][64];
static uint64_t bishops[2][64];
static uint64_t rooks[2][64];
static uint64_t queens[2][64];
static uint64_t kings[2][64];

uint64_t enpassant[64];

uint64_t castle[2][5];

uint64_t pawn_structure[2][64];

bool initialized = false;
}

extern unsigned char const queenside_rook_origin[2] = {
	0, 56
};
extern unsigned char const kingside_rook_origin[2] = {
	7, 63
};


void init_zobrist_tables()
{
	if( initialized ) {
		return;
	}

	push_rng_state();
	init_random( 0 );

	for( int c = 0; c < 2; ++c ) {
		for( int i = 0; i < 64; ++i ) {
			pawns[c][i] = get_random_unsigned_long_long();
			knights[c][i] = get_random_unsigned_long_long();
			bishops[c][i] = get_random_unsigned_long_long();
			rooks[c][i] = get_random_unsigned_long_long();
			queens[c][i] = get_random_unsigned_long_long();
			kings[c][i] = get_random_unsigned_long_long();
		}
	}
	
	for( unsigned int c = 0; c < 2; ++c ) {
		for( int i = 0; i < 5; ++i ) {
			castle[c][i] = get_random_unsigned_long_long();
		}
	}

	enpassant[0] = 0;
	for( unsigned int i = 1; i < 64; ++i ) {
		if( i / 8 == 2 || i / 8 == 5 ) {
			enpassant[i] = get_random_unsigned_long_long();
		}
		else {
			enpassant[i] = 0;
		}
	}

	for( unsigned int c = 0; c < 2; ++c ) {
		for( unsigned int pawn = 0; pawn < 64; ++pawn ) {
			pawn_structure[c][pawn] = get_random_unsigned_long_long();
		}
	}

	initialized = true;

	pop_rng_state();
}


uint64_t get_zobrist_hash( position const& p ) {
	uint64_t ret = 0;

	for( unsigned int c = 0; c < 2; ++c ) {
		uint64_t pieces = p.bitboards[c].b[bb_type::all_pieces];
		while( pieces ) {
			uint64_t piece = bitscan_unset( pieces );

			uint64_t bpiece = 1ull << piece;
			if( p.bitboards[c].b[bb_type::pawns] & bpiece ) {
				ret ^= pawns[c][piece];
			}
			else if( p.bitboards[c].b[bb_type::knights] & bpiece ) {
				ret ^= knights[c][piece];
			}
			else if( p.bitboards[c].b[bb_type::bishops] & bpiece ) {
				ret ^= bishops[c][piece];
			}
			else if( p.bitboards[c].b[bb_type::rooks] & bpiece ) {
				ret ^= rooks[c][piece];
			}
			else if( p.bitboards[c].b[bb_type::queens] & bpiece ) {
				ret ^= queens[c][piece];
			}
			else {//if( p.bitboards[c].b[bb_type::king] & bpiece ) {
				ret ^= kings[c][piece];
			}
		}

		ret ^= castle[c][p.castle[c]];
	}

	ret ^= enpassant[p.can_en_passant];

	return ret;
}

namespace {
static uint64_t get_piece_hash( pieces::type pi, color::type c, int pos )
{
	switch( pi ) {
		case pieces::pawn:
			return pawns[c][pos];
		case pieces::knight:
			return knights[c][pos];
		case pieces::bishop:
			return bishops[c][pos];
		case pieces::rook:
			return rooks[c][pos];
		case pieces::queen:
			return queens[c][pos];
		case pieces::king:
			return kings[c][pos];
		default:
			return 0;
	}
}
}

uint64_t update_zobrist_hash( position const& p, color::type c, uint64_t hash, move const& m )
{
	hash ^= enpassant[p.can_en_passant];

	if( m.flags & move_flags::enpassant ) {
		// Was en-passant
		hash ^= pawns[1-c][(m.target % 8) | (m.source & 0xf8)];
	}
	else if( m.captured_piece != pieces::none ) {
		hash ^= get_piece_hash( static_cast<pieces::type>(m.captured_piece), static_cast<color::type>(1-c), m.target );
		
		if( m.captured_piece == pieces::rook ) {
			if( m.target == queenside_rook_origin[1-c] && p.castle[1-c] & 0x2 ) {
				hash ^= castle[1-c][p.castle[1-c]];
				hash ^= castle[1-c][p.castle[1-c] & 0x5];
			}
			else if( m.target == kingside_rook_origin[1-c] && p.castle[1-c] & 0x1 ) {
				hash ^= castle[1-c][p.castle[1-c]];
				hash ^= castle[1-c][p.castle[1-c] & 0x6];
			}
		}
	}

	hash ^= get_piece_hash( m.piece, c, m.source );

	if( m.piece == pieces::pawn ) {
		unsigned char source_row = m.source / 8;
		unsigned char target_col = m.target % 8;
		unsigned char target_row = m.target / 8;
		if( m.flags & move_flags::pawn_double_move ) {
			// Becomes en-passantable
			hash ^= enpassant[target_col + (source_row + target_row) * 4];
		}
	}
	else if( m.piece == pieces::rook ) {
		if( m.source == queenside_rook_origin[c] && p.castle[c] & 0x2 ) {
			hash ^= castle[c][p.castle[c]];
			hash ^= castle[c][p.castle[c] & 0x5];
		}
		else if( m.source == kingside_rook_origin[c] && p.castle[c] & 0x1 ) {
			hash ^= castle[c][p.castle[c]];
			hash ^= castle[c][p.castle[c] & 0x6];
		}
	}
	else if( m.piece == pieces::king ) {
		if( m.flags & move_flags::castle ) {
			unsigned char target_col = m.target % 8;
			unsigned char target_row = m.target / 8;

			// Was castling
			if( target_col == 2 ) {
				hash ^= rooks[c][0 + target_row * 8];
				hash ^= rooks[c][3 + target_row * 8];
			}
			else {
				hash ^= rooks[c][7 + target_row * 8];
				hash ^= rooks[c][5 + target_row * 8];
			}
			hash ^= castle[c][p.castle[c]];
			hash ^= castle[c][0x4];
		}
		else {
			hash ^= castle[c][p.castle[c]];
			hash ^= castle[c][p.castle[c] & 0x4];
		}
	}

	int promotion = m.flags & move_flags::promotion_mask;
	if( !promotion ) {
		hash ^= get_piece_hash( m.piece, c, m.target );
	}
	else {
		switch( promotion ) {
			case move_flags::promotion_knight:
				hash ^= knights[c][m.target];
				break;
			case move_flags::promotion_bishop:
				hash ^= bishops[c][m.target];
				break;
			case move_flags::promotion_rook:
				hash ^= rooks[c][m.target];
				break;
			case move_flags::promotion_queen:
				hash ^= queens[c][m.target];
				break;
		}
	}

	return hash;
}

uint64_t get_pawn_structure_hash( color::type c, unsigned char pawn )
{
	return pawn_structure[c][pawn];
}
