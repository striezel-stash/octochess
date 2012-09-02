#include "assert.hpp"
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

void do_add_move( move_info*& moves, pieces::type const& pi,
				  uint64_t const& source, uint64_t const& target,
				  int flags, pieces::type captured )
{
	ASSERT( source < 64 );
	ASSERT( target < 64 );

	move_info& mi= *(moves++);

	mi.m.flags = flags;
	mi.m.piece = pi;
	mi.m.source = static_cast<unsigned char>(source);
	mi.m.target = static_cast<unsigned char>(target);
	mi.m.captured_piece = captured;
}

// Adds the move if it does not result in self getting into check
void add_if_legal( move_info*& moves, check_map const& check,
				  pieces::type const& pi,
				  uint64_t const& source, uint64_t const& target,
				  int flags, pieces::type captured )
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

	do_add_move( moves, pi, source, target, flags, captured );
}

// Adds the move if it does not result in self getting into check
void add_if_legal_pawn( move_info*& moves, check_map const& check,
				  uint64_t const& source, uint64_t const& target,
				  pieces::type captured )
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
		do_add_move( moves, pieces::pawn, source, target, move_flags::promotion_queen, captured );
		do_add_move( moves, pieces::pawn, source, target, move_flags::promotion_rook, captured );
		do_add_move( moves, pieces::pawn, source, target, move_flags::promotion_bishop, captured );
		do_add_move( moves, pieces::pawn, source, target, move_flags::promotion_knight, captured );
	}
	else {
		do_add_move( moves, pieces::pawn, source, target, move_flags::none, captured );
	}
}

void add_if_legal_king( position const& p,
						move_info*& moves, uint64_t const& source, uint64_t const& target,
						int flags, pieces::type captured )
{
	if( detect_check( p, p.self(), target, source ) ) {
		return;
	}

	do_add_move( moves, pieces::king, source, target, flags, captured );
}

void calc_moves_king( position const& p, move_info*& moves,
					  uint64_t const& source, uint64_t const& target )
{
	pieces::type captured = get_piece_on_square( p, p.other(), target );
	add_if_legal_king( p, moves, source, target, move_flags::none, captured );
}


void calc_moves_king( position const& p, move_info*& moves, check_map const& check )
{
	uint64_t king_moves = possible_king_moves[p.king_pos[p.self()]] & ~(p.bitboards[p.self()].b[bb_type::all_pieces] | possible_king_moves[p.king_pos[p.other()]]);
	while( king_moves ) {
		uint64_t king_move = bitscan_unset( king_moves );
		calc_moves_king( p, moves,
						 p.king_pos[p.self()], king_move );
	}

	if( check.check ) {
		return;
	}

	unsigned char row = p.white() ? 0 : 56;
	// Queenside castling
	if( p.castle[p.self()] & 0x2 ) {
		if( p.get_occupancy( 0xeull << row ) == 0 && !(possible_king_moves[2 + row] & p.bitboards[p.other()].b[bb_type::king]) ) {
			if( !detect_check( p, p.self(), 3 + row, 3 + row ) ) {
				add_if_legal_king( p, moves, 4 + row, 2 + row, move_flags::castle, pieces::none );
			}
		}
	}
	// Kingside castling
	if( p.castle[p.self()] & 0x1 ) {
		if( p.get_occupancy( 0x60ull << row ) == 0 && !(possible_king_moves[6 + row] & p.bitboards[p.other()].b[bb_type::king]) ) {
			if( !detect_check( p, p.self(), 5 + row, 5 + row ) ) {
				add_if_legal_king( p, moves, 4 + row, 6 + row, move_flags::castle, pieces::none );
			}
		}
	}
}


void calc_moves_queen( position const& p, move_info*& moves, check_map const& check, uint64_t queen )
{
	uint64_t const all_blockers = p.bitboards[p.self()].b[bb_type::all_pieces] | p.bitboards[p.other()].b[bb_type::all_pieces];

	uint64_t possible_moves = rook_magic( queen, all_blockers ) | bishop_magic( queen, all_blockers );
	possible_moves &= ~p.bitboards[p.self()].b[bb_type::all_pieces];

	while( possible_moves ) {
		uint64_t queen_move = bitscan_unset( possible_moves );

		pieces::type captured = get_piece_on_square( p, p.other(), queen_move );
		add_if_legal( moves, check, pieces::queen, queen, queen_move, move_flags::none, captured );
	}
}


void calc_moves_queens( position const& p, move_info*& moves, check_map const& check )
{
	uint64_t queens = p.bitboards[p.self()].b[bb_type::queens];
	while( queens ) {
		uint64_t queen = bitscan_unset( queens );
		calc_moves_queen( p, moves, check, queen );
	}
}


void calc_moves_bishop( position const& p, move_info*& moves, check_map const& check,
						uint64_t bishop )
{
	uint64_t const all_blockers = p.bitboards[p.self()].b[bb_type::all_pieces] | p.bitboards[p.other()].b[bb_type::all_pieces];

	uint64_t possible_moves = bishop_magic( bishop, all_blockers );
	possible_moves &= ~p.bitboards[p.self()].b[bb_type::all_pieces];

	while( possible_moves ) {
		uint64_t bishop_move = bitscan_unset( possible_moves );

		pieces::type captured = get_piece_on_square( p, p.other(), bishop_move );
		add_if_legal( moves, check, pieces::bishop, bishop, bishop_move, move_flags::none, captured );
	}
}


void calc_moves_bishops( position const& p, move_info*& moves, check_map const& check )
{
	uint64_t bishops = p.bitboards[p.self()].b[bb_type::bishops];
	while( bishops ) {
		uint64_t bishop = bitscan_unset( bishops );
		calc_moves_bishop( p, moves, check, bishop );
	}
}


void calc_moves_rook( position const& p, move_info*& moves, check_map const& check,
					  uint64_t rook )
{
	uint64_t const all_blockers = p.bitboards[p.self()].b[bb_type::all_pieces] | p.bitboards[p.other()].b[bb_type::all_pieces];

	uint64_t possible_moves = rook_magic( rook, all_blockers );
	possible_moves &= ~p.bitboards[p.self()].b[bb_type::all_pieces];

	while( possible_moves ) {
		uint64_t rook_move = bitscan_unset( possible_moves );

		pieces::type captured = get_piece_on_square( p, p.other(), rook_move );
		add_if_legal( moves, check, pieces::rook, rook, rook_move, move_flags::none, captured );
	}
}


void calc_moves_rooks( position const& p, move_info*& moves, check_map const& check )
{
	uint64_t rooks = p.bitboards[p.self()].b[bb_type::rooks];
	while( rooks ) {
		uint64_t rook = bitscan_unset( rooks );
		calc_moves_rook( p, moves, check, rook );
	}
}


void calc_moves_knight( position const& p, move_info*& moves, check_map const& check,
						uint64_t source, uint64_t target )
{
	pieces::type captured = get_piece_on_square( p, p.other(), target );
	add_if_legal( moves, check, pieces::knight, source, target, move_flags::none, captured );
}

void calc_moves_knight( position const& p, move_info*& moves, check_map const& check,
						uint64_t old_knight )
{
	uint64_t new_knights = possible_knight_moves[old_knight] & ~(p.bitboards[p.self()].b[bb_type::all_pieces]);
	while( new_knights ) {
		uint64_t new_knight = bitscan_unset( new_knights );
		calc_moves_knight( p, moves, check,
						   old_knight, new_knight );
	}
}


void calc_moves_knights( position const& p, move_info*& moves, check_map const& check )
{
	uint64_t knights = p.bitboards[p.self()].b[bb_type::knights];
	while( knights ) {
		uint64_t knight = bitscan_unset( knights );
		calc_moves_knight( p, moves, check, knight );
	}
}


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
			// Target position does capture checking piece nor blocks check
			return;
		}
	}
	else {
		if( cv_old && cv_old != cv_new ) {
			return;
		}
	}

	// Special case: black queen, black pawn, white pawn, white king from left to right on rank 5. Capturing opens up check!
	uint64_t kings = p.bitboards[p.self()].b[bb_type::king];
	uint64_t king = bitscan( kings );
	unsigned char king_col = static_cast<unsigned char>(king % 8);
	unsigned char king_row = static_cast<unsigned char>(king / 8);

	if( king_row == old_row ) {
		signed char cx = static_cast<signed char>(old_col) - king_col;
		if( cx > 0 ) {
			cx = 1;
		}
		else {
			cx = -1;
		}
		for( signed char col = old_col + cx; col < 8 && col >= 0; col += cx ) {
			if( col == new_col ) {
				continue;
			}

			if( p.bitboards[p.self()].b[bb_type::all_pieces] & (1ull << (col + old_row * 8 ) ) ) {
				// Own piece
				continue;
			}

			pieces::type t = get_piece_on_square( p, p.other(), col + old_row * 8 );

			if( t == pieces::none ) {
				continue;
			}

			if( t == pieces::queen || t == pieces::rook ) {
				// Not a legal move unfortunately
				return;
			}

			// Harmless piece
			break;
		}
	}

	do_add_move( moves, pieces::pawn, pawn, p.can_en_passant, move_flags::enpassant, pieces::pawn );
}


void calc_moves_pawn_captures( position const& p, move_info*& moves, check_map const& check, uint64_t pawn_captures, int shift )
{
	while( pawn_captures ) {
		uint64_t pawn_move = bitscan_unset( pawn_captures );

		pieces::type captured = get_piece_on_square( p, p.other(), pawn_move );

		add_if_legal_pawn( moves, check, pawn_move - shift, pawn_move, captured );
	}
}


template<int c>
void calc_moves_pawn_pushes( position const& p, move_info*& moves, check_map const& check )
{
	uint64_t free = ~(p.bitboards[p.self()].b[bb_type::all_pieces] | p.bitboards[p.other()].b[bb_type::all_pieces]);

	uint64_t pawn_pushes;
	uint64_t double_pushes;
	if( c == color::white ) {
		pawn_pushes = (p.bitboards[p.self()].b[bb_type::pawns] << 8) & free;
		double_pushes= ((pawn_pushes & pawn_double_move[p.self()]) << 8) & free;
	}
	else {
		pawn_pushes = (p.bitboards[p.self()].b[bb_type::pawns] >> 8) & free;
		double_pushes= ((pawn_pushes & pawn_double_move[p.self()]) >> 8) & free;
	}
	while( double_pushes ) {
		uint64_t pawn_move = bitscan_unset( double_pushes );
		add_if_legal( moves, check, pieces::pawn, pawn_move - (c ? -16 : 16), pawn_move, move_flags::pawn_double_move, pieces::none );
	}

	while( pawn_pushes ) {
		uint64_t pawn_move = bitscan_unset( pawn_pushes );

		add_if_legal_pawn( moves, check, pawn_move - (c ? -8 : 8), pawn_move, pieces::none );
	}
}


void calc_moves_pawns( position const& p, move_info*& moves, check_map const& check )
{

	if( p.white() ) {
		calc_moves_pawn_pushes<color::white>( p, moves, check );
		uint64_t pawns = p.bitboards[p.self()].b[bb_type::pawns];
		calc_moves_pawn_captures( p, moves, check, ((pawns & 0xfefefefefefefefeull) << 7) & p.bitboards[p.other()].b[bb_type::all_pieces], 7 );
		calc_moves_pawn_captures( p, moves, check, ((pawns & 0x7f7f7f7f7f7f7f7full) << 9) & p.bitboards[p.other()].b[bb_type::all_pieces], 9 );
	}
	else {
		calc_moves_pawn_pushes<color::black>( p, moves, check );
		uint64_t pawns = p.bitboards[p.self()].b[bb_type::pawns];
		calc_moves_pawn_captures( p, moves, check, ((pawns & 0xfefefefefefefefeull) >> 9) & p.bitboards[p.other()].b[bb_type::all_pieces], -9 );
		calc_moves_pawn_captures( p, moves, check, ((pawns & 0x7f7f7f7f7f7f7f7full) >> 7) & p.bitboards[p.other()].b[bb_type::all_pieces], -7 );
	}

	if( p.can_en_passant ) {
		uint64_t enpassants = pawn_control[p.other()][p.can_en_passant] & p.bitboards[p.self()].b[bb_type::pawns] & pawn_enpassant[p.self()];
		while( enpassants ) {
			uint64_t pawn = bitscan_unset( enpassants );
			calc_moves_pawn_en_passant( p, moves, check, pawn );
		}
	}
}
}

void calculate_moves( position const& p, move_info*& moves, check_map const& check )
{
	calc_moves_king( p, moves, check );

	if( !check.check || !check.multiple() )
	{
		calc_moves_pawns( p, moves, check );
		calc_moves_queens( p, moves, check );
		calc_moves_rooks( p, moves, check );
		calc_moves_bishops( p, moves, check );
		calc_moves_knights( p, moves, check );
	}
}
