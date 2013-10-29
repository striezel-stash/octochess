#ifndef __BOOK_H__
#define __BOOK_H__

#include "calc.hpp"
#include "chess.hpp"

#include <list>
#include <map>
#include <string>
#include <vector>

extern int const eval_version;

struct book_entry
{
	book_entry();

	move m;
	short forecast;
	unsigned char search_depth;
	unsigned char eval_version;

	bool operator<( book_entry const& rhs ) const {
		if( forecast > rhs.forecast ) {
			return true;
		}
		else if( forecast < rhs.forecast ) {
			return false;
		}
		return search_depth < rhs.search_depth;
	}

	bool is_folded() const { return eval_version == 0; }
};

struct work {
	std::vector<move> move_history;
	seen_positions seen;
	position p;
};


struct book_entry_with_position
{
	work w;
	std::vector<book_entry> entries;
};


struct stat_entry {
	stat_entry() : processed(), queued() {}

	uint64_t processed;
	uint64_t queued;
};

struct book_stats {
	book_stats() : total_processed(1), total_queued() {}

	std::map<uint64_t, stat_entry> data;

	uint64_t total_processed;
	uint64_t total_queued;
};


class book
{
public:
	class impl;
	explicit book( std::string const& book_dir );
	virtual ~book();

	book( book const& ) = delete;
	book& operator=( book const& ) = delete;

	bool open( std::string const& book_dir );
	void close();

	bool is_open() const;
	bool is_writable() const;

	// Returned entries are sorted by folded forecast, highest first.
	std::vector<book_entry> get_entries( position const& p, std::vector<move> const& history, bool allow_transpositions = false );

	// As above but does not regard move history and always goes by hash.
	// Beware: Suspectible to 3-fold repetition
	std::vector<book_entry> get_entries( position const& p );

	// Entries do not have to be sorted
	bool add_entries( std::vector<move> const& history, std::vector<book_entry> entries );

	bool mark_for_processing( std::vector<move> const& history );

	uint64_t size();

	book_stats stats();

	std::list<work> get_unprocessed_positions();

	// move_limit limits the number of returned moves per position, sorted descendingly by forecast.
	// max_evaluation_version ensures that only entries with an eval_version lower or equal than the given version are returned.
	std::list<book_entry_with_position> get_all_entries();

	bool update_entry( std::vector<move> const& history, book_entry const& entry );

	bool fold( bool verify = false );

	// If set to a non-empty string, the resulting SQL inserts from calls to add_entries
	// are logged into the file.
	bool set_insert_logfile( std::string const& log_file );

	bool redo_hashes();

	bool export_book( std::string const& fn );

private:
	impl *impl_;
};


std::string entries_to_string( position const& p, std::vector<book_entry> const& entries );


#endif
