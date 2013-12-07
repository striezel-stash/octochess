#include "see.hpp"
#include "eval.hpp"
#include "eval_values.hpp"
#include "magic.hpp"
#include "util/platform.hpp"
#include "tables.hpp"

#include <algorithm>
#include <iostream>

static uint64_t least_valuable_attacker( position const& p, color::type c, uint64_t attackers, pieces::type& attacker_piece_out )
{
	// Exploit that our bitboards are sorted by piece value and that we do have an attacker.
	uint64_t match;

	int piece = pieces::pawn;
	while( !(match = p.bitboards[c][piece] & attackers) ) {
		++piece;
	}

	attacker_piece_out = static_cast<pieces::type>(piece);
	uint64_t ret = bitscan( match );
	return 1ull << ret;
}


int see( position const& p, move const& m )
{
	// Iterative SEE algorithm as described by Fritz Reul, adapted to use bitboards.
	unsigned char target = m.target();

	pieces::type attacker_piece = p.get_piece(m);

	int score[32];

	uint64_t const all_rooks = p.bitboards[color::white][bb_type::rooks] | p.bitboards[color::white][bb_type::queens] | p.bitboards[color::black][bb_type::rooks] | p.bitboards[color::black][bb_type::queens];
	uint64_t const all_bishops = p.bitboards[color::white][bb_type::bishops] | p.bitboards[color::white][bb_type::queens] | p.bitboards[color::black][bb_type::bishops] | p.bitboards[color::black][bb_type::queens];

	uint64_t all_pieces = p.bitboards[color::white][bb_type::all_pieces] | p.bitboards[color::black][bb_type::all_pieces];
	all_pieces ^= 1ull << m.source();

	if( m.enpassant() ) {
		all_pieces ^= 1ull << ((m.source() & 0x38) + m.target() % 8);
	}

	uint64_t attackers =
			(rook_magic( target, all_pieces ) & all_rooks) |
			(bishop_magic( target, all_pieces ) & all_bishops) |
			(possible_knight_moves[target] & (p.bitboards[color::white][bb_type::knights] | p.bitboards[color::black][bb_type::knights])) |
			(possible_king_moves[target] & (p.bitboards[color::white][bb_type::king] | p.bitboards[color::black][bb_type::king])) |
			(pawn_control[color::black][target] & p.bitboards[color::white][bb_type::pawns]) |
			(pawn_control[color::white][target] & p.bitboards[color::black][bb_type::pawns]);
	// Don't have to remove source piece here, done implicitly in the loop.

	pieces::type captured_piece = p.get_captured_piece( m );
	score[0] = eval_values::material_values[ captured_piece ].mg();

	// Get new attacker
	if( !(attackers & p.bitboards[p.other()][bb_type::all_pieces]) ) {
		return score[0];
	}

	int depth = 1;
	color::type c = p.other();

	// Can "do", as we always have at least one.
	do {
		score[depth] = eval_values::material_values[ attacker_piece ].mg() - score[depth - 1];

		if( score[depth] < 0 && score[depth - 1] < 0 ) {
			break; // Bad capture
		}

		// We have verified that there's already at least one attacker, so this works.
		uint64_t attacker_mask = least_valuable_attacker( p, c, attackers, attacker_piece );

		++depth;
		c = other(c);

		// Remove old attacker
		all_pieces ^= attacker_mask;

		// Update attacker list due to uncovered attacks
		attackers |= ((rook_magic( target, all_pieces )) & all_rooks) |
						((bishop_magic( target, all_pieces )) & all_bishops);
		attackers &= all_pieces;

		if( !(attackers & p.bitboards[c][bb_type::all_pieces]) ) {
			break;
		}

		if( attacker_piece == pieces::king ) {
			// This is needed in case both kings can attack the target square.
			// If only one piece can attack the target square, see() still returns correct
			// result even without this condition.
			score[depth++] = eval_values::material_values[pieces::king].mg();
			break;
		}
	} while( true );

	// Propagate scores back
	while( --depth ) {
		score[depth - 1] = -std::max(score[depth], -score[depth - 1]);
	}

	return score[0];
}
