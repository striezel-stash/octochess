#ifndef __UTIL_H__
#define __UTIL_H__

#include "chess.hpp"
#include "detect_check.hpp"

#include <string>

struct move_info;

bool validate_move( position const& p, move const& m );

bool validate_move( move const& m, move_info const* begin, move_info const* end );

bool parse_move( position const& p, std::string const& line, move& m, std::string& error );

// E.g. O-O, Na3xf6, b2-b4
std::string move_to_string( position const& p, move const& m, bool padding = true );

std::string move_to_san( position const& p, move const& m );

// E.g. c4d6, e1g1, e7e8q
std::string move_to_long_algebraic( config const& conf, position const& p, move const& m );

void apply_move( position& p, move const& m );

// Checks if the given move is legal in the given position.
// Precondition: There must be some position where the move is legal, else the result is undefined.
//				 e.g. is_valid_move might return true on Na1b1
bool is_valid_move( position const& p, move const& m, check_map const& check );

std::string board_to_string( position const& p, color::type view );

template<typename T>
bool set_max( T& lhs, T const& rhs ) {
	if( lhs < rhs ) {
		lhs = rhs;
		return true;
	}
	return false;
}

template<typename T>
bool set_min( T& lhs, T const& rhs ) {
	if( rhs < lhs ) {
		lhs = rhs;
		return true;
	}
	return false;
}

#endif
