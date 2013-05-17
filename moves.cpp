#include "assert.hpp"
#include "eval_values.hpp"
#include "moves.hpp"
#include "eval.hpp"
#include "magic.hpp"
#include "util.hpp"
#include "calc.hpp"
#include "tables.hpp"

#include <algorithm>
#include <iostream>
#include <string>

MoveSort moveSort;

extern uint64_t const pawn_enpassant[2] = {
	0x000000ff00000000ull,
	0x00000000ff000000ull
};

extern uint64_t const pawn_double_move[2] = {
	0x0000000000ff0000ull,
	0x0000ff0000000000ull
};

namespace {

template<movegen_type type>
void do_add_move( position const& p, move_info*& moves, uint64_t const& source, uint64_t const& target,
				  int flags, pieces::type piece )
{
	ASSERT( source < 64 );
	ASSERT( target < 64 );

	move_info& mi= *(moves++);

	mi.m = move( static_cast<unsigned short>(source), static_cast<unsigned short>(target), flags );

	if( type == movegen_type::capture ) {
		pieces::type captured = p.get_captured_piece( mi.m );
		mi.sort = eval_values::material_values[ captured ].mg() * 32 - eval_values::material_values[ piece ].mg();
	}
}

// Adds the move if it does not result in self getting into check
template<movegen_type type>
void add_if_legal( position const& p, move_info*& moves, check_map const& check,
				  uint64_t const& source, uint64_t const& target,
				  int flags, pieces::type piece )
{
	unsigned char const& cv_old = check.board[source];
	unsigned char const& cv_new = check.board[target];
	if( check.check ) {
		if( cv_old ) {
			// Can't come to rescue, this piece is already blocking yet another check.
			return;
		}
		if( cv_new != check.check ) {
			// Target position does capture checking piece nor blocks check
			return;
		}
	}
	else {
		if( cv_old && cv_old != cv_new ) {
			return;
		}
	}

	do_add_move<type>( p, moves, source, target, flags, piece );
}

// Adds the move if it does not result in self getting into check
template<movegen_type type>
void add_if_legal_pawn( position const& p, move_info*& moves, check_map const& check,
				  uint64_t const& source, uint64_t const& target )
{
	unsigned char const& cv_old = check.board[source];
	unsigned char const& cv_new = check.board[target];
	if( check.check ) {
		if( cv_old ) {
			// Can't come to rescue, this piece is already blocking yet another check.
			return;
		}
		if( cv_new != check.check ) {
			// Target position does capture checking piece nor blocks check
			return;
		}
	}
	else {
		if( cv_old && cv_old != cv_new ) {
			return;
		}
	}

	if( target >= 56 || target < 8 ) {
		do_add_move<type>( p, moves, source, target, move_flags::promotion_queen, pieces::pawn );
		do_add_move<type>( p, moves, source, target, move_flags::promotion_rook, pieces::pawn );
		do_add_move<type>( p, moves, source, target, move_flags::promotion_bishop, pieces::pawn );
		do_add_move<type>( p, moves, source, target, move_flags::promotion_knight, pieces::pawn );
	}
	else {
		do_add_move<type>( p, moves, source, target, move_flags::none, pieces::pawn );
	}
}

template<movegen_type type>
void add_if_legal_king( position const& p,
				move_info*& moves, uint64_t const& source, uint64_t const& target,
						int flags )
{
	if( detect_check( p, p.self(), target, source ) ) {
		return;
	}

	do_add_move<type>( p, moves, source, target, flags, pieces::king );
}


template<movegen_type type>
void calc_moves_castles( position const& p, move_info*& moves, check_map const& check )
{
	if( check.check ) {
		return;
	}

	unsigned char row = p.white() ? 0 : 56;
	// Queenside castling
	if( p.castle[p.self()] & 0x2 ) {
		if( p.get_occupancy( 0xeull << row ) == 0 && !(possible_king_moves[2 + row] & p.bitboards[p.other()][bb_type::king]) ) {
			if( type != movegen_type::pseudocheck || (p.king_pos[p.other()] % 8) == 3 ) {
				if( !detect_check( p, p.self(), 3 + row, 3 + row ) ) {
					add_if_legal_king<type>( p, moves, 4 + row, 2 + row, move_flags::castle );
				}
			}
		}
	}
	// Kingside castling
	if( p.castle[p.self()] & 0x1 ) {
		if( p.get_occupancy( 0x60ull << row ) == 0 && !(possible_king_moves[6 + row] & p.bitboards[p.other()][bb_type::king]) ) {
			if( type != movegen_type::pseudocheck || (p.king_pos[p.other()] % 8) == 5 ) {
				if( !detect_check( p, p.self(), 5 + row, 5 + row ) ) {
					add_if_legal_king<type>( p, moves, 4 + row, 6 + row, move_flags::castle );
				}
			}
		}
	}
}


template<movegen_type type>
void calc_moves_king( position const& p, move_info*& moves, check_map const& check )
{
	if( type != movegen_type::capture ) {
		calc_moves_castles<type>( p, moves, check );
	}

	uint64_t king_moves;
	if( type == movegen_type::capture ) {
		king_moves = possible_king_moves[p.king_pos[p.self()]] & ~possible_king_moves[p.king_pos[p.other()]] & p.bitboards[p.other()][bb_type::all_pieces];
	}
	else if( type == movegen_type::all ) {
		king_moves = possible_king_moves[p.king_pos[p.self()]] & ~(p.bitboards[p.self()][bb_type::all_pieces] | possible_king_moves[p.king_pos[p.other()]]);
	}
	else {
		king_moves = possible_king_moves[p.king_pos[p.self()]] & ~(p.bitboards[p.self()][bb_type::all_pieces] | possible_king_moves[p.king_pos[p.other()]] | p.bitboards[p.other()][bb_type::all_pieces]);

		if( type == movegen_type::pseudocheck ) {
			uint64_t occ = p.bitboards[p.self()][bb_type::all_pieces] | p.bitboards[p.other()][bb_type::all_pieces];
			occ ^= p.bitboards[p.self()][bb_type::king];

			uint64_t ba = bishop_magic( p.king_pos[p.other()], occ ) & (p.bitboards[p.self()][bb_type::bishops] | p.bitboards[p.self()][bb_type::queens]);
			uint64_t ra = rook_magic( p.king_pos[p.other()], occ ) & (p.bitboards[p.self()][bb_type::rooks] | p.bitboards[p.self()][bb_type::queens]);
			if( !(ba | ra) ) {
				return;
			}
		}
	}
	
	while( king_moves ) {
		uint64_t king_move = bitscan_unset( king_moves );
		add_if_legal_king<type>( p, moves, p.king_pos[p.self()], king_move, move_flags::none );
	}
}


template<movegen_type type>
void calc_moves_queen( position const& p, move_info*& moves, check_map const& check, uint64_t queen )
{
	uint64_t const all_blockers = p.bitboards[p.self()][bb_type::all_pieces] | p.bitboards[p.other()][bb_type::all_pieces];

	uint64_t possible_moves = rook_magic( queen, all_blockers ) | bishop_magic( queen, all_blockers );
	if( type == movegen_type::capture ) {
		possible_moves &= p.bitboards[p.other()][bb_type::all_pieces];
	}
	else if( type == movegen_type::all ) {
		possible_moves &= ~p.bitboards[p.self()][bb_type::all_pieces];
	}
	else {
		possible_moves &= ~(p.bitboards[p.self()][bb_type::all_pieces] | p.bitboards[p.other()][bb_type::all_pieces]);

		if( type == movegen_type::pseudocheck ) {
			uint64_t occ = all_blockers ^ (1ull << queen);
			uint64_t ba = bishop_magic( p.king_pos[p.other()], occ ) & (p.bitboards[p.self()][bb_type::bishops] | p.bitboards[p.self()][bb_type::queens]);
			uint64_t ra = rook_magic( p.king_pos[p.other()], occ ) & (p.bitboards[p.self()][bb_type::rooks] | p.bitboards[p.self()][bb_type::queens]);
			if( !(ba | ra ) ) {
				possible_moves &= bishop_magic( p.king_pos[p.other()], all_blockers ) | rook_magic( p.king_pos[p.other()], all_blockers );
			}
		}
	}
	
	while( possible_moves ) {
		uint64_t queen_move = bitscan_unset( possible_moves );
		add_if_legal<type>( p, moves, check, queen, queen_move, move_flags::none, pieces::queen );
	}
}


template<movegen_type type>
void calc_moves_queens( position const& p, move_info*& moves, check_map const& check )
{
	uint64_t queens = p.bitboards[p.self()][bb_type::queens];
	while( queens ) {
		uint64_t queen = bitscan_unset( queens );
		calc_moves_queen<type>( p, moves, check, queen );
	}
}


template<movegen_type type>
void calc_moves_bishop( position const& p, move_info*& moves, check_map const& check,
						uint64_t bishop )
{
	uint64_t const all_blockers = p.bitboards[p.self()][bb_type::all_pieces] | p.bitboards[p.other()][bb_type::all_pieces];

	uint64_t possible_moves = bishop_magic( bishop, all_blockers );
	if( type == movegen_type::capture ) {
		possible_moves &= p.bitboards[p.other()][bb_type::all_pieces];
	}
	else if( type == movegen_type::all ) {
		possible_moves &= ~p.bitboards[p.self()][bb_type::all_pieces];
	}
	else {
		possible_moves &= ~(p.bitboards[p.self()][bb_type::all_pieces] | p.bitboards[p.other()][bb_type::all_pieces]);

		if( type == movegen_type::pseudocheck ) {
			uint64_t occ = all_blockers ^ (1ull << bishop);
			uint64_t ba = bishop_magic( p.king_pos[p.other()], occ ) & (p.bitboards[p.self()][bb_type::bishops] | p.bitboards[p.self()][bb_type::queens]);
			uint64_t ra = rook_magic( p.king_pos[p.other()], occ ) & (p.bitboards[p.self()][bb_type::rooks] | p.bitboards[p.self()][bb_type::queens]);
			if( !(ba | ra ) ) {
				possible_moves &= bishop_magic( p.king_pos[p.other()], all_blockers );
			}
		}
	}

	while( possible_moves ) {
		uint64_t bishop_move = bitscan_unset( possible_moves );
		add_if_legal<type>( p, moves, check, bishop, bishop_move, move_flags::none, pieces::bishop );
	}
}


template<movegen_type type>
void calc_moves_bishops( position const& p, move_info*& moves, check_map const& check )
{
	uint64_t bishops = p.bitboards[p.self()][bb_type::bishops];
	while( bishops ) {
		uint64_t bishop = bitscan_unset( bishops );
		calc_moves_bishop<type>( p, moves, check, bishop );
	}
}


template<movegen_type type>
void calc_moves_rook( position const& p, move_info*& moves, check_map const& check,
					  uint64_t rook )
{
	uint64_t const all_blockers = p.bitboards[p.self()][bb_type::all_pieces] | p.bitboards[p.other()][bb_type::all_pieces];

	uint64_t possible_moves = rook_magic( rook, all_blockers );
	if( type == movegen_type::capture ) {
		possible_moves &= p.bitboards[p.other()][bb_type::all_pieces];
	}
	else if( type == movegen_type::all ) {
		possible_moves &= ~p.bitboards[p.self()][bb_type::all_pieces];
	}
	else {
		possible_moves &= ~(p.bitboards[p.self()][bb_type::all_pieces] | p.bitboards[p.other()][bb_type::all_pieces]);

		if( type == movegen_type::pseudocheck ) {
			uint64_t occ = all_blockers ^ (1ull << rook);
			uint64_t ba = bishop_magic( p.king_pos[p.other()], occ ) & (p.bitboards[p.self()][bb_type::bishops] | p.bitboards[p.self()][bb_type::queens]);
			uint64_t ra = rook_magic( p.king_pos[p.other()], occ ) & (p.bitboards[p.self()][bb_type::rooks] | p.bitboards[p.self()][bb_type::queens]);
			if( !(ba | ra ) ) {
				possible_moves &= rook_magic( p.king_pos[p.other()], all_blockers );
			}
		}
	}

	while( possible_moves ) {
		uint64_t rook_move = bitscan_unset( possible_moves );
		add_if_legal<type>( p, moves, check, rook, rook_move, move_flags::none, pieces::rook );
	}
}


template<movegen_type type>
void calc_moves_rooks( position const& p, move_info*& moves, check_map const& check )
{
	uint64_t rooks = p.bitboards[p.self()][bb_type::rooks];
	while( rooks ) {
		uint64_t rook = bitscan_unset( rooks );
		calc_moves_rook<type>( p, moves, check, rook );
	}
}


template<movegen_type type>
void calc_moves_knight( position const& p, move_info*& moves, check_map const& check,
						uint64_t old_knight )
{
	uint64_t new_knights = possible_knight_moves[old_knight] & ~(p.bitboards[p.self()][bb_type::all_pieces]);
	if( type == movegen_type::capture ) {
		new_knights &= p.bitboards[p.other()][bb_type::all_pieces];
	}
	else if( type != movegen_type::all ) {
		new_knights &= ~p.bitboards[p.other()][bb_type::all_pieces];

		if( type == movegen_type::pseudocheck ) {
			uint64_t const all_blockers = p.bitboards[p.self()][bb_type::all_pieces] | p.bitboards[p.other()][bb_type::all_pieces];
			uint64_t occ = all_blockers ^ (1ull << old_knight);
			uint64_t ba = bishop_magic( p.king_pos[p.other()], occ ) & (p.bitboards[p.self()][bb_type::bishops] | p.bitboards[p.self()][bb_type::queens]);
			uint64_t ra = rook_magic( p.king_pos[p.other()], occ ) & (p.bitboards[p.self()][bb_type::rooks] | p.bitboards[p.self()][bb_type::queens]);
			if( !(ba | ra ) ) {
				new_knights &= possible_knight_moves[p.king_pos[p.other()]];
			}
		}
	}
	while( new_knights ) {
		uint64_t new_knight = bitscan_unset( new_knights );
		add_if_legal<type>( p, moves, check, old_knight, new_knight, move_flags::none, pieces::knight );
	}
}


template<movegen_type type>
void calc_moves_knights( position const& p, move_info*& moves, check_map const& check )
{
	uint64_t knights = p.bitboards[p.self()][bb_type::knights];
	while( knights ) {
		uint64_t knight = bitscan_unset( knights );
		calc_moves_knight<type>( p, moves, check, knight );
	}
}


template<movegen_type type>
void calc_moves_pawn_en_passant( position const& p, move_info*& moves, check_map const& check,
								 uint64_t pawn )
{
	unsigned char new_col = p.can_en_passant % 8;

	unsigned char old_col = static_cast<unsigned char>(pawn % 8);
	unsigned char old_row = static_cast<unsigned char>(pawn / 8);

	// Special case: Cannot use normal check from add_if_legal as target square is not piece square and if captured pawn gives check, bad things happen.
	unsigned char const& cv_old = check.board[pawn];
	unsigned char const& cv_new = check.board[p.can_en_passant];
	if( check.check ) {
		if( cv_old ) {
			// Can't come to rescue, this piece is already blocking yet another check.
			return;
		}
		if( cv_new != check.check && check.check != (0x80 + new_col + old_row * 8) ) {
			// Target position does neither capture checking piece nor blocks check
			return;
		}
	}
	else {
		if( cv_old && cv_old != cv_new ) {
			return;
		}
	}

	// Special case: Captured pawn uncovers bishop/queen check
	// While this cannot occur in a regular game, board might have been setup like this.
	uint64_t occ = (p.bitboards[p.self()][bb_type::all_pieces] | p.bitboards[p.other()][bb_type::all_pieces]);
	occ &= ~(1ull << (new_col + old_row * 8));
	if( bishop_magic( p.king_pos[p.self()], occ) & (p.bitboards[p.other()][bb_type::bishops] | p.bitboards[p.other()][bb_type::queens] ) ) {
		return;
	}

	// Special case: Enpassant capture uncovers a check against own king, e.g. in this position:
	// black queen, black pawn, white pawn, white king from left to right on rank 5
	unsigned char king_col = static_cast<unsigned char>(p.king_pos[p.self()] % 8);
	unsigned char king_row = static_cast<unsigned char>(p.king_pos[p.self()] / 8);

	if( king_row == old_row ) {
		uint64_t mask;
		if( king_col < old_col ) {
			mask = 0xff & ~((2 << king_col) - 1);
		}
		else {
			mask = (1 << king_col) - 1;
		}
		mask &= ~(1ull << old_col);
		mask &= ~(1ull << new_col);
		mask <<= 8 * king_row;

		uint64_t occ = (p.bitboards[p.self()][bb_type::all_pieces] | p.bitboards[p.other()][bb_type::all_pieces]) & mask;
		if( occ ) {
			uint64_t sq;
			if( king_col < old_col ) {
				sq = bitscan( occ );
			}
			else {
				sq = bitscan_reverse( occ );
			}

			pieces_with_color::type pwc = p.get_piece_with_color( sq );
			if( get_color(pwc) == p.other() ) {
				pieces::type pi = get_piece(pwc);
				if( pi == pieces::rook || pi == pieces::queen ) {
					return;
				}
			}
		}
	}

	do_add_move<type>( p, moves, pawn, p.can_en_passant, move_flags::enpassant, pieces::pawn );
}


template<movegen_type type>
void calc_moves_pawn_captures( position const& p, move_info*& moves, check_map const& check, uint64_t pawn_captures, int shift )
{
	while( pawn_captures ) {
		uint64_t pawn_move = bitscan_unset( pawn_captures );
		add_if_legal_pawn<type>( p, moves, check, pawn_move - shift, pawn_move );
	}
}


template<movegen_type type, int c>
void calc_moves_pawn_pushes( position const& p, move_info*& moves, check_map const& check )
{
	uint64_t blockers = p.bitboards[p.self()][bb_type::all_pieces] | p.bitboards[p.other()][bb_type::all_pieces];
	uint64_t free = ~blockers;

	uint64_t pawn_pushes;
	uint64_t double_pushes;
	if( c == color::white ) {
		pawn_pushes = (p.bitboards[p.self()][bb_type::pawns] << 8) & free;
		double_pushes = ((pawn_pushes & pawn_double_move[p.self()]) << 8) & free;

		if( type == movegen_type::pseudocheck ) {
			uint64_t checks = rook_magic( p.king_pos[p.other()], blockers ) | bishop_magic( p.king_pos[p.other()], blockers );
			pawn_pushes &= (checks << 8) | pawn_control[p.other()][p.king_pos[p.other()]] | 0xff000000000000ffull;
			double_pushes &= (checks << 16) | pawn_control[p.other()][p.king_pos[p.other()]] | 0xff000000000000ffull;
		}
	}
	else {
		pawn_pushes = (p.bitboards[p.self()][bb_type::pawns] >> 8) & free;
		double_pushes = ((pawn_pushes & pawn_double_move[p.self()]) >> 8) & free;

		if( type == movegen_type::pseudocheck ) {
			uint64_t checks = rook_magic( p.king_pos[p.other()], blockers ) | bishop_magic( p.king_pos[p.other()], blockers );
			pawn_pushes &= (checks >> 8) | pawn_control[p.other()][p.king_pos[p.other()]] | 0xff000000000000ffull;
			double_pushes &= (checks >> 16) | pawn_control[p.other()][p.king_pos[p.other()]] | 0xff000000000000ffull;
		}
	}
	while( double_pushes ) {
		uint64_t pawn_move = bitscan_unset( double_pushes );
		add_if_legal<type>( p, moves, check, pawn_move - (c ? -16 : 16), pawn_move, move_flags::none, pieces::pawn );
	}

	while( pawn_pushes ) {
		uint64_t pawn_move = bitscan_unset( pawn_pushes );
		add_if_legal_pawn<type>( p, moves, check, pawn_move - (c ? -8 : 8), pawn_move );
	}
}


template<movegen_type type>
void calc_moves_pawns( position const& p, move_info*& moves, check_map const& check )
{

	if( p.white() ) {
		if( type != movegen_type::capture ) {
			calc_moves_pawn_pushes<type, color::white>( p, moves, check );
		}
		if( type == movegen_type::all || type == movegen_type::capture ) {
			uint64_t pawns = p.bitboards[p.self()][bb_type::pawns];
			calc_moves_pawn_captures<type>( p, moves, check, ((pawns & 0xfefefefefefefefeull) << 7) & p.bitboards[p.other()][bb_type::all_pieces], 7 );
			calc_moves_pawn_captures<type>( p, moves, check, ((pawns & 0x7f7f7f7f7f7f7f7full) << 9) & p.bitboards[p.other()][bb_type::all_pieces], 9 );
		}
	}
	else {
		if( type != movegen_type::capture ) {
			calc_moves_pawn_pushes<type, color::black>( p, moves, check );
		}
		if( type == movegen_type::all || type == movegen_type::capture ) {
			uint64_t pawns = p.bitboards[p.self()][bb_type::pawns];
			calc_moves_pawn_captures<type>( p, moves, check, ((pawns & 0xfefefefefefefefeull) >> 9) & p.bitboards[p.other()][bb_type::all_pieces], -9 );
			calc_moves_pawn_captures<type>( p, moves, check, ((pawns & 0x7f7f7f7f7f7f7f7full) >> 7) & p.bitboards[p.other()][bb_type::all_pieces], -7 );
		}
	}

	if( (type == movegen_type::all || type == movegen_type::capture) && p.can_en_passant ) {
		uint64_t enpassants = pawn_control[p.other()][p.can_en_passant] & p.bitboards[p.self()][bb_type::pawns] & pawn_enpassant[p.self()];
		while( enpassants ) {
			uint64_t pawn = bitscan_unset( enpassants );
			calc_moves_pawn_en_passant<type>( p, moves, check, pawn );
		}
	}
}
}


template<movegen_type type>
void calculate_moves( position const& p, move_info*& moves, check_map const& check )
{
	if( !check.check || !check.multiple() )
	{
		calc_moves_pawns<type>( p, moves, check );
		calc_moves_queens<type>( p, moves, check );
		calc_moves_rooks<type>( p, moves, check );
		calc_moves_bishops<type>( p, moves, check );
		calc_moves_knights<type>( p, moves, check );
	}

	calc_moves_king<type>( p, moves, check );
}


template<movegen_type type>
std::vector<move> calculate_moves( position const& p, check_map const& check )
{
	std::vector<move> ret;

	move_info moves[200];
	move_info* pm = moves;
	calculate_moves<type>( p, pm, check );

	ret.reserve( pm - moves );

	for( move_info* it = &moves[0]; it != pm; ++it ) {
		ret.push_back( it->m );
	}

	return ret;
}


void calculate_moves_by_piece( position const& p, move_info*& moves, check_map const& check, pieces::type pi )
{
	switch( pi ) {
	case pieces::pawn:
		calc_moves_pawns<movegen_type::all>( p, moves, check );
		break;
	case pieces::knight:
		calc_moves_knights<movegen_type::all>( p, moves, check );
		break;
	case pieces::bishop:
		calc_moves_bishops<movegen_type::all>( p, moves, check );
		break;
	case pieces::rook:
		calc_moves_rooks<movegen_type::all>( p, moves, check );
		break;
	case pieces::queen:
		calc_moves_queens<movegen_type::all>( p, moves, check );
		break;
	case pieces::king:
		calc_moves_king<movegen_type::all>( p, moves, check );
		break;
	default:
		calculate_moves<movegen_type::all>( p, moves, check );
		break;
	}
}


// Explicit instanciations
template void calculate_moves<movegen_type::all>( position const& p, move_info*& moves, check_map const& check );
template void calculate_moves<movegen_type::capture>( position const& p, move_info*& moves, check_map const& check );
template void calculate_moves<movegen_type::noncapture>( position const& p, move_info*& moves, check_map const& check );
template void calculate_moves<movegen_type::pseudocheck>( position const& p, move_info*& moves, check_map const& check );

template std::vector<move> calculate_moves<movegen_type::all>( position const& p, check_map const& check );
template std::vector<move> calculate_moves<movegen_type::capture>( position const& p, check_map const& check );
template std::vector<move> calculate_moves<movegen_type::noncapture>( position const& p, check_map const& check );
template std::vector<move> calculate_moves<movegen_type::pseudocheck>( position const& p, check_map const& check );
