#ifndef __CHECK_H__
#define __CHECK_H__

#include <string.h>

struct config
{
	config();
	int thread_count;
	unsigned int memory;
	unsigned int max_moves; // only for auto play
};
extern config conf;

namespace pieces {
enum type {
	pawn1,
	pawn2,
	pawn3,
	pawn4,
	pawn5,
	pawn6,
	pawn7,
	pawn8,
	king,
	queen,
	rook1,
	rook2,
	bishop1,
	bishop2,
	knight1,
	knight2,
	nil = 255
};
}

namespace promotions {
enum type {
	queen,
	rook,
	knight,
	bishop
};
}

struct piece
{
	unsigned char alive : 1;
	unsigned char column : 3;
	unsigned char row : 3;

	// For knights, indicates whether they can castle.
	// For king, indicates whether it has castled
	// For pawns:
	//   Alive set: promoted
	unsigned char special : 1;
};

struct position_base
{
	// [color][piece]
	piece pieces[2][16];

	// 2 bit for every pawn.
	unsigned short promotions[2];

	unsigned char can_en_passant; // Piece of last-moved player that can be en-passanted
} __attribute__((__packed__));


struct position : public position_base
{
	// board[column][row] as piece indexes in lower 4 bits, color in 5th bit.
	// nil if square is empty.
	unsigned char board[8][8];
} __attribute__((__packed__));


struct move
{
	unsigned char source_col: 3;
	unsigned char source_row: 3;
	unsigned char target_col : 3;
	unsigned char target_row : 3;
	unsigned char other : 2;

	bool operator!=( move const& rhs ) const {
		return source_col != rhs.source_col || source_row != rhs.source_row || target_col != rhs.target_col || target_row != rhs.target_row;
	}
	bool operator==( move const& rhs ) const {
		return source_col == rhs.source_col && source_row == rhs.source_row && target_col == rhs.target_col && target_row == rhs.target_row;
	}
};


namespace result {
enum type {
	win = 30000,
	win_threshold = win - 100,
	draw = 0,
	loss = -30000,
	loss_threshold = loss + 100
};
}

namespace color {
enum type {
	white = 0,
	black = 1
};
}

#define USE_QUIESCENCE 1

#endif
