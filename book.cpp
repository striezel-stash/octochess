#include "assert.hpp"
#include "book.hpp"
#include "util.hpp"
#include "util/mutex.hpp"
#include "simple_book.hpp"

#include "sqlite/sqlite3.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

int const db_version = 2;
int const eval_version = 9;

namespace {
void append_move_to_history( std::vector<unsigned char>& h, move const& m ) {
	h.push_back( static_cast<unsigned char>(m.d % 256) );
	h.push_back( static_cast<unsigned char>(m.d / 256) );
}

/*
 * This can only be used to uniquely sort book moves for a given position.
 * Sorting the complete move structure independend of position is not possible
 * With this class. E.g. it doesn't distinguish between capture and non-capture.
 */
class BookMoveSort
{
public:
	BookMoveSort() {}

	bool operator()( move const& lhs, move const& rhs ) const {
		if( lhs.source() < rhs.source() ) {
			return true;
		}
		else if( lhs.source() > rhs.source() ) {
			return false;
		}
		if( lhs.target() < rhs.target() ) {
			return true;
		}
		else if( lhs.target() > rhs.target() ) {
			return false;
		}

		return lhs.promotion_piece() < rhs.promotion_piece();
	}


	bool operator()( move_info const& lhs, move_info const& rhs ) const {
		return operator()( lhs.m, rhs.m );
	}


	bool operator()( book_entry const& lhs, book_entry const& rhs ) const {
		return operator()( lhs.m, rhs.m );
	}
};

BookMoveSort const book_move_sort;
}


book_entry::book_entry()
	: forecast()
	, search_depth()
	, eval_version()
{
}


class book::impl : public database
{
public:
	impl( std::string const& book_dir )
		: database( book_dir + "opening_book.db" )
	{
	}

	std::vector<unsigned char> serialize_history( std::vector<move>::const_iterator const& begin, std::vector<move>::const_iterator const& end );
	std::vector<unsigned char> serialize_history( std::vector<move> const& history );

	mutex mtx;

	std::ofstream logfile;
};


book::book( std::string const& book_dir )
	: impl_( new impl( book_dir ) )
{
	if( is_open() ) {
		if( impl_->user_version() != db_version ) {
			impl_->close();
		}
	}
}


std::vector<unsigned char> book::impl::serialize_history( std::vector<move>::const_iterator const& begin, std::vector<move>::const_iterator const& end )
{
	std::vector<unsigned char> ret;
	ret.reserve( (end - begin) * 2);
	for( std::vector<move>::const_iterator it = begin; it != end; ++it ) {
		append_move_to_history( ret, *it );
	}

	return ret;
}


std::vector<unsigned char> book::impl::serialize_history( std::vector<move> const& history )
{
	return serialize_history( history.begin(), history.end() );
}


book::~book()
{
	delete impl_;
}


bool book::is_open() const
{
	scoped_lock l(impl_->mtx);
	return impl_->is_open();
}


void book::close()
{
	scoped_lock l(impl_->mtx);
	impl_->close();
}


bool book::open( std::string const& book_dir )
{
	scoped_lock l(impl_->mtx);
	bool open = impl_->open( book_dir + "opening_book.db" );
	if( open ) {
		if( impl_->user_version() != db_version ) {
			impl_->close();
			open = false;
		}
	}

	return open;
}

namespace {
struct cb_data {
	cb_data()
		: entries()
		, pm()
		, valid_()
		, by_hash_()
	{
	}

	std::vector<book_entry>* entries;
	position p;

	move_info moves[200];
	move_info* pm;

	bool valid_;
	bool by_hash_;
};


bool decode_entries( unsigned char const* data, uint64_t bytes, std::vector<book_entry>& entries )
{
	if( bytes % 4 ) {
		return false;
	}

	uint64_t size = bytes / 4;

	for( unsigned int i = 0; i < size; ++i ) {
		book_entry e;
		unsigned short forecast = *(data++);
		forecast += static_cast<unsigned short>(*(data++)) * 256;
		e.forecast = static_cast<short>(forecast);

		e.search_depth = *(data++);
		e.eval_version = *(data++);

		entries.push_back(e);
	}

	return true;
}


bool encode_entries( std::vector<book_entry>& entries, std::vector<unsigned char>& data, bool set_current_eval_version = false )
{
	uint64_t bytes = entries.size() * 4;
	data.resize(bytes);

	unsigned char* p = &data[0];
	for( std::size_t i = 0; i < entries.size(); ++i ) {
		*(p++) = static_cast<unsigned short>(entries[i].forecast) % 256;
		*(p++) = static_cast<unsigned short>(entries[i].forecast) / 256;
		*(p++) = entries[i].search_depth;
		if( set_current_eval_version ) {
			*(p++) = eval_version;
		}
		else {
			*(p++) = static_cast<unsigned char>(entries[i].eval_version);
		}
	}

	return true;
}

int get_cb( void* p, statement& s )
{
	cb_data* d = reinterpret_cast<cb_data*>(p);
	if( d->valid_ ) {
		return 0;
	}

	if( s.column_count() != 1 ) {
		std::cerr << "Wrong column count" << std::endl;
		return 1;
	}
	if( s.is_null( 0 ) ) {
		return 0;
	}

	std::vector<unsigned char> data = s.get_blob( 0 );
	if( data.size() % 4 ) {
		std::cerr << "Wrong data size not multiple of 4: " << data.size() << std::endl;
		return 1;
	}
	uint64_t size = data.size() / 4;

	if( !d->pm ) {
		check_map check( d->p );
		d->pm = d->moves;
		calculate_moves<movegen_type::all>( d->p, d->pm, check );

		std::sort( d->moves, d->pm, book_move_sort );
	}

	if( size != static_cast<uint64_t>(d->pm - d->moves) ) {
		if( d->by_hash_ ) {
			std::cerr << "Possible hash collision" << std::endl;
			return 0;
		}
		else {
			std::cerr << "Corrupt book entry, expected " << (d->pm - d->moves) << " moves, got " << size << " moves." << std::endl;
			return 1;
		}
	}

	if( !decode_entries( &data[0], data.size(), *d->entries ) ) {
		std::cerr << "Could not decode entries" << std::endl;
		return 1;
	}

	for( unsigned int i = 0; i < size; ++i ) {
		(*d->entries)[i].m = d->moves[i].m;
	}

	std::sort( d->entries->begin(), d->entries->end() );

	d->valid_ = true;

	return 0;
}
}


std::vector<book_entry> book::get_entries( position const& p, std::vector<move> const& history, bool allow_transpositions )
{
	std::vector<book_entry> ret;

	{
		scoped_lock l(impl_->mtx);

		if( !impl_->is_open() ) {
			return ret;
		}

		cb_data data;
		data.p = p;
		data.entries = &ret;

		std::vector<unsigned char> hs = impl_->serialize_history( history );

		statement s( *impl_, "SELECT data FROM position WHERE pos = :1" );

		s.bind( 1, hs );
		bool success = s.exec( get_cb, reinterpret_cast<void*>(&data) );
		if( !success || !data.valid_ ) {
			ret.clear();
		}
	}

	if( ret.empty() && allow_transpositions ) {
		ret = get_entries( p );
	}

	return ret;
}


std::vector<book_entry> book::get_entries( position const& p )
{
	std::vector<book_entry> ret;

	scoped_lock l(impl_->mtx);

	if( !impl_->is_open() ) {
		return ret;
	}

	cb_data data;
	data.p = p;
	data.entries = &ret;
	data.by_hash_ = true;

	statement s( *impl_, "SELECT data FROM position WHERE hash = :1" );
	s.bind( 1, p.hash_ );


	bool success = s.exec( get_cb, reinterpret_cast<void*>(&data) );
	if( !success || !data.valid_ ) {
		ret.clear();
	}
	else {
		std::cerr << "Found entry by transposition" << std::endl;
	}

	return ret;
}


namespace {
int get_data_cb( void* p, statement& s )
{
	std::vector<unsigned char> *data = reinterpret_cast<std::vector<unsigned char>*>(p);

	if( s.column_count() != 1 ) {
		return 1;
	}
	if( s.is_null( 0 ) ) {
		return 0;
	}

	*data = s.get_blob( 0 );
	if( data->size() % 4 ) {
		data->clear();
		return 1;
	}

	return 0;
}


bool get_position( std::vector<move> const& history, position& p )
{
	p.reset();

	for( auto m : history ) {
		apply_move( p, m );
	}

	return true;
}


bool get_position( std::vector<unsigned char> const& history, position& p )
{
	p.reset();

	for( std::size_t i = 0; i < history.size(); i += 2 ) {
		move m;
		m.d = history[i] + static_cast<unsigned short>(history[i+1]) * 256;
		check_map check( p );
		if( !is_valid_move( p, m, check ) ) {
			return false;
		}
		apply_move( p, m );
	}

	return true;
}


int do_fold_position( void* q, statement& s, bool verify )
{
	book::impl* impl_ = reinterpret_cast<book::impl*>(q);

	if( s.column_count() != 2 ) {
		std::cerr << "Wrong column count" << std::endl;
		return 1;
	}

	if( s.is_null( 0 ) ) {
		std::cerr << "No move history returned" << std::endl;
		return 1;
	}
	std::vector<unsigned char> const history = s.get_blob(0);
	if( history.size() % 2 ) {
		std::cerr << "Move history malformed" << std::endl;
		return 1;
	}
	if( !history.size() ) {
		return 0;
	}

	if( s.is_null( 1 ) ) {
		std::cerr << "NULL data in position to fold." << std::endl;
		return 1;
	}

	std::vector<unsigned char> data = s.get_blob( 1 );
	if( data.size() % 4 ) {
		std::cerr << "Data has wrong size" << std::endl;
		return 1;
	}

	position p;
	if( !get_position( history, p ) ) {
		std::cerr << "Could not get position from move history" << std::endl;
		return 1;
	}

	check_map const check( p );

	short forecast = result::loss;
	unsigned char depth = 0;
	if( !data.size() ) {
		// Mate or draw.
		if( !check.check ) {
			forecast = 0;
		}
	}
	else {
		std::vector<book_entry> entries;
		if( !decode_entries( &data[0], data.size(), entries ) ) {
			std::cerr << "Could not decode entries of position" << std::endl;
			return 1;
		}

		if( verify ) {
			move_info moves[200];
			move_info* pm;

			pm = moves;
			calculate_moves<movegen_type::all>( p, pm, check );

			std::sort( moves, pm, book_move_sort );

			if( entries.size() != static_cast<uint64_t>(pm - moves) ) {
				std::cerr << "Corrupt book entry, expected " << (pm - moves) << " moves, got " << entries.size() << " moves." << std::endl;
				return 1;
			}

			for( unsigned int i = 0; i < entries.size(); ++i ) {
				entries[i].m = moves[i].m;
			}
		}

		for( std::size_t i = 0; i < entries.size(); ++i ) {
			if( entries[i].forecast > forecast ) {
				forecast = entries[i].forecast;
				depth = entries[i].search_depth;
			}
			else if( entries[i].forecast == forecast && entries[i].search_depth < depth ) {
				depth = entries[i].search_depth;
			}
			if( verify && entries[i].eval_version != 0 ) {
				std::vector<unsigned char> chs = history;
				append_move_to_history( chs, entries[i].m );
				position cp;
				if( !get_position( chs, cp ) ) {
					std::cerr << "Corrupt book, child node not found." << std::endl;
					return 1;
				}
			}
		}
	}
	forecast = -forecast;
	++depth;

	// We now got the best forecast of the current position and its depth
	// Get parent position:
	std::vector<unsigned char> parent( history.begin(), history.end() - 2 );

	move ms;
	ms.d = history[history.size() - 2] + static_cast<unsigned short>(history[history.size() - 1]) * 256;

	statement ps( *impl_, "SELECT data FROM position WHERE pos = :1" );
	ps.bind( 1, parent );
	std::vector<unsigned char> parent_data;
	if( !ps.exec( get_data_cb, reinterpret_cast<void*>(&parent_data) ) ) {
		return 1;
	}

	if( parent_data.empty() ) {
		if( verify ) {
			std::cerr << "Error in book, position has no parent" << std::endl;
			return 1;
		}
		else {
			// Can't fold yet. This situation can happen if processing a tree with multiple threads.
			return 0;
		}
	}

	position pp;
	if( !get_position( parent, pp ) ) {
		return 1;
	}

	move_info moves[200];
	move_info* it = moves;
	calculate_moves<movegen_type::all>( pp, it, check_map( pp ) );
	if( static_cast<uint64_t>(it - moves) != parent_data.size() / 4 ) {
		std::cerr << "Wrong move count in parent position's data: " << (it - moves) << " " << parent_data.size() / 4 << std::endl;
		return 1;
	}
	std::sort( moves, it, book_move_sort );

	int i = 0;
	for( ; i < it - moves; ++i ) {
		if( moves[i].m == ms ) {
			break;
		}
	}
	if( i == it - moves ) {
		std::cerr << "Target move not found in all valid parent moves" << std::endl;
		return 1;
	}

	std::vector<unsigned char> new_parent_data = parent_data;
	new_parent_data[i*4] = static_cast<unsigned short>(forecast) % 256;
	new_parent_data[i*4 + 1] = static_cast<unsigned short>(forecast) / 256;
	new_parent_data[i*4 + 2] = depth;
	new_parent_data[i*4 + 3] = 0;

	if( new_parent_data != parent_data ) {
		statement ps( *impl_, "UPDATE position SET data = :1 WHERE pos = :2" );
		ps.bind( 1, new_parent_data );
		ps.bind( 2, parent );
		if( !ps.exec() ) {
			return 1;
		}
	}

	return 0;
}

int fold_position( void* q, statement& s )
{
	return do_fold_position( q, s, false );
}

int verify_position( void* q, statement& s )
{
	return do_fold_position( q, s, true );
}
}


bool book::add_entries( std::vector<move> const& history, std::vector<book_entry> entries )
{
	if( !is_writable() ) {
		std::cerr << "Cannot add entries to read-only book" << std::endl;
		return false;
	}

	std::sort( entries.begin(), entries.end(), book_move_sort );

	std::vector<unsigned char> hs = impl_->serialize_history( history );

	position p;
	get_position( history, p );

	scoped_lock l(impl_->mtx);

	transaction t( *impl_ );
	if( !t.init() ) {
		return false;
	}

	statement s( *impl_, "INSERT OR REPLACE INTO position (pos, hash, data) VALUES (:1, :2, :3)" );
	s.bind( 1, hs );
	s.bind( 2, p.hash_ );

	std::vector<unsigned char> data;
	encode_entries( entries, data, true );

	s.bind( 3, data );

	if( !s.exec() ) {
		return false;
	}

	statement s2( *impl_, "SELECT pos, data FROM position WHERE pos= :1 AND data IS NOT NULL;" );

	for( std::vector<book_entry>::const_iterator it = entries.begin(); it != entries.end(); ++it ) {

		std::vector<unsigned char> hs2 = hs;
		append_move_to_history( hs2, it->m );
		s2.bind( 1, hs2 );
		if( !s2.exec( fold_position, impl_ ) ) {
			return false;
		}
	}

	while( hs.size() >= 2 ) {
		s2.bind( 1, hs );
		if( !s2.exec( fold_position, impl_ ) ) {
			return false;
		}
		hs.resize( hs.size() - 2 );
	}

	return t.commit();
}


namespace {
int count_cb( void* p, statement& s ) {
	uint64_t* count = reinterpret_cast<uint64_t*>(p);
	*count = s.get_int( 0 );

	return 0;
}
}


uint64_t book::size()
{
	scoped_lock l(impl_->mtx);

	if( !impl_->is_open() ) {
		return 0;
	}

	statement s( *impl_, "SELECT COUNT(pos) FROM position" );

	uint64_t count = 0;
	s.exec( count_cb, reinterpret_cast<void*>(&count) );

	return count;
}


bool book::mark_for_processing( std::vector<move> const& history )
{
	scoped_lock l(impl_->mtx);

	if( !impl_->is_open() || !impl_->is_writable() ) {
		return false;
	}

	transaction t( *impl_ );
	if( !t.init() ) {
		return false;
	}

	position p;

	statement s( *impl_, "INSERT OR IGNORE INTO position (pos, hash) VALUES (:1, :2)");
	for( std::vector<move>::const_iterator it = history.begin(); it != history.end(); ++it ) {
		apply_move( p, *it );

		std::vector<unsigned char> hs = impl_->serialize_history( history.begin(), it + 1 );
		s.bind( 1, hs );
		s.bind( 2, p.hash_ );
		if( !s.exec() ) {
			return false;
		}
	}

	return t.commit();
}


namespace {
int work_cb( void* p, statement& s )
{
	std::list<work> *wl = reinterpret_cast<std::list<work>*>(p);

	if( s.column_count() != 1 || s.is_null( 0 ) ) {
		return 1;
	}

	std::vector<unsigned char> pos = s.get_blob( 0 );

	if( pos.size() % 2 ) {
		std::cerr << "Deleting position with incorrect length" << std::endl;
		statement s2( s.db(), "DELETE FROM POSITION WHERE pos = :1" );
		s2.bind( 1, pos );
		if( !s2.exec() ) {
			return 1;
		}
		return 0;
	}

	work w;
	w.seen.reset_root( w.p.hash_ );

	std::size_t i = 0;
	while( i < pos.size() ) {
		move m;
		m.d = pos[i] + static_cast<unsigned short>(pos[i+1]) * 256;
		i += 2;
		check_map check( w.p );
		if( !is_valid_move( w.p, m, check ) ) {
			std::cerr << "Deleting entry with invalid move" << std::endl;
			statement s2( s.db(), "DELETE FROM POSITION WHERE pos = :1" );
			s2.bind( 1, pos );
			if( !s2.exec() ) {
				return 1;
			}
			return 0;
		}

		apply_move( w.p, m );
		w.move_history.push_back( m );
		w.seen.push_root( w.p.hash_ );
	}

	wl->push_back( w );

	return 0;
}
}


std::list<work> book::get_unprocessed_positions()
{
	std::list<work> wl;

	scoped_lock l(impl_->mtx);

	statement s( *impl_, "SELECT pos FROM position WHERE data IS NULL ORDER BY LENGTH(pos) ASC;" );
	s.exec( work_cb, reinterpret_cast<void*>(&wl) );

	return wl;
}


bool book::redo_hashes()
{
	if( !impl_->is_writable() ) {
		std::cerr << "Error: Cannot redo hashes on read-only opening book\n" << std::endl;
		return false;
	}

	scoped_lock l(impl_->mtx);

	transaction t( *impl_ );
	if( !t.init() ) {
		return false;
	}

	std::list<work> wl;
	{
		statement s( *impl_, "SELECT pos FROM position ORDER BY LENGTH(pos) DESC" );
		if( !s.ok() || !s.exec( work_cb, reinterpret_cast<void*>(&wl) ) ) {
			return false;
		}
	}

	statement s( *impl_, "UPDATE position SET hash = :1 WHERE pos = :2" );
	if( !s.ok() ) {
		return false;
	}

	for( std::list<work>::const_iterator it = wl.begin(); it != wl.end(); ++it ) {
		std::vector<unsigned char> hs = impl_->serialize_history( it->move_history );
		s.bind( 1, it->p.hash_ );
		s.bind( 2, hs );
		s.exec();
	}

	return t.commit();
}


std::list<book_entry_with_position> book::get_all_entries()
{
	std::list<book_entry_with_position> ret;

	transaction t( *impl_ );
	if( !t.init() ) {
		return ret;
	}

	std::list<work> wl;
	{
		statement s( *impl_, "SELECT pos FROM position ORDER BY LENGTH(pos) DESC" );
		s.exec( &work_cb, reinterpret_cast<void*>(&wl) );
	}

	for( std::list<work>::const_iterator it = wl.begin(); it != wl.end(); ++it ) {
		book_entry_with_position entry;
		entry.w = *it;
		entry.entries = get_entries( it->p, it->move_history );

		if( !entry.entries.empty() ) {
			ret.push_back( entry );
		}
	}

	return ret;
}


bool book::update_entry( std::vector<move> const& history, book_entry const& entry )
{
	scoped_lock l(impl_->mtx);

	transaction t( *impl_ );
	if( !t.init() ) {
		return false;
	}

	std::vector<unsigned char> hs = impl_->serialize_history( history );

	std::vector<unsigned char> data;
	statement s( *impl_, "SELECT data FROM position WHERE pos = :1" );
	if( !s.ok() ) {
		return false;
	}
	s.bind( 1, hs );
	if( !s.exec( get_data_cb, reinterpret_cast<void*>(&data) ) ) {
		return false;
	}
	if( data.empty() ) {
		std::cerr << "Cannot update empty entry" << std::endl;
		return false;
	}

	position p;
	if( !get_position( history, p ) ) {
		return false;
	}

	move_info moves[200];
	move_info* it = moves;
	calculate_moves<movegen_type::all>( p, it, check_map( p ) );
	if( static_cast<uint64_t>(it - moves) != data.size() / 4 ) {
		std::cerr << "Wrong move count in position's data: " << (it - moves) << " " << data.size() / 4 << std::endl;
		return false;
	}
	std::sort( moves, it, book_move_sort );

	int i = 0;
	for( ; i < it - moves; ++i ) {
		if( moves[i].m == entry.m ) {
			break;
		}
	}
	if( i == it - moves ) {
		std::cerr << "Target move not found in all valid moves" << std::endl;
		return false;
	}

	if( !data[i*4+3] ) {
		std::cerr << "Cannot update folded entry" << std::endl;
		return false;
	}

	std::vector<unsigned char> new_data = data;
	new_data[i*4] = static_cast<unsigned short>(entry.forecast) % 256;
	new_data[i*4 + 1] = static_cast<unsigned short>(entry.forecast) / 256;
	new_data[i*4 + 2] = entry.search_depth;
	new_data[i*4 + 3] = eval_version;

	if( data != new_data ) {
		statement s( *impl_, "UPDATE position SET data = :1 WHERE pos = :2" );
		s.bind( 1, new_data );
		s.bind( 2, hs );
		if( !s.exec() ) {
			return false;
		}

		append_move_to_history( hs, entry.m );

		statement s2( *impl_, "SELECT pos, data FROM position WHERE pos= :1 AND data IS NOT NULL;" );

		while( hs.size() >= 2 ) {
			s2.bind( 1, hs );
			if( !s2.exec( fold_position, impl_ ) ) {
				return false;
			}
			hs.resize( hs.size() - 2 );
		}
	}

	return t.commit();
}

struct export_entry
{
	export_entry()
		: mi_()
		, forecast()
		, depth_()
	{}

	unsigned char mi_;
	short forecast;
	unsigned char depth_;
};

struct export_data
{
	book::impl* impl_;
	std::map<uint64_t, std::vector<export_entry>> output;
};

int export_book( void* q, statement& s )
{
	export_data& ed = *reinterpret_cast<export_data*>(q);

	if( s.column_count() != 3 ) {
		std::cerr << "Wrong column count" << std::endl;
		return 1;
	}

	if( s.is_null( 0 ) ) {
		std::cerr << "No move history returned" << std::endl;
		return 1;
	}
	std::vector<unsigned char> const history = s.get_blob(0);
	if( history.size() % 2 ) {
		std::cerr << "Move history malformed" << std::endl;
		return 1;
	}

	if( s.is_null( 1 ) ) {
		std::cerr << "NULL hash in position to fold." << std::endl;
		return 1;
	}
	uint64_t hash = s.get_int( 1 );

	if( s.is_null( 1 ) ) {
		std::cerr << "NULL data in position to fold." << std::endl;
		return 0;
	}

	std::vector<unsigned char> data = s.get_blob( 2 );
	if( data.size() % 4 ) {
		std::cerr << "Data has wrong size" << std::endl;
		return 1;
	}
	if( data.size() < 8 ) {
		return 0;
	}

	position p;
	if( !get_position( history, p ) ) {
		std::cerr << "Could not get position from move history" << std::endl;
		return 1;
	}
	if( hash != p.hash_ ) {
		std::cerr << "Hash mismatch" << std::endl;
		return 1;
	}

	std::vector<book_entry> entries;
	if( !decode_entries( &data[0], data.size(), entries ) ) {
		std::cerr << "Could not decode entries of position" << std::endl;
		return 1;
	}

	auto moves = calculate_moves<movegen_type::all>( p );

	if( moves.size() != entries.size() ) {
		std::cerr << "Move count mismatch" << std::endl;
		return 1;
	}
	ASSERT( moves.size() < 255 );

	auto book_sorted_moves = moves;
	std::sort( book_sorted_moves.begin(), book_sorted_moves.end(), book_move_sort );
	for( unsigned int i = 0; i < entries.size(); ++i ) {
		entries[i].m = book_sorted_moves[i];
	}

	std::sort( entries.begin(), entries.end() );
	std::sort( moves.begin(), moves.end() );

	std::vector<export_entry> output;
	auto existing = ed.output.find( hash );
	if( existing != ed.output.end() ) {
		output = existing->second;
	}

	for( std::size_t i = 0; i < 5 && i < entries.size(); ++i ) {
		export_entry e;
		e.mi_ = static_cast<unsigned char>(std::find( moves.begin(), moves.end(), entries[i].m ) - moves.begin() + 1);
		e.forecast = entries[i].forecast;
		e.depth_ = entries[i].search_depth;

		// Merge with existing item
		for( auto it = output.begin(); it != output.end(); ++it ) {
			if( it->mi_ == e.mi_ ) {
				if( it->depth_ >= e.depth_ ) {
					e = *it;
				}
				output.erase( it );
				break;
			}
		}

		output.push_back( e );
	}
	if( !output.empty() ) {
		ed.output[ hash ] = output;
	}

	return 0;
}


bool book::export_book( std::string const& fn )
{
	if( !impl_->is_writable() ) {
		std::cerr << "Error: Cannot fold read-only opening book\n" << std::endl;
		return false;
	}

	scoped_lock l(impl_->mtx);

	transaction t( *impl_ );
	if( !t.init() ) {
		return false;
	}

	statement s( *impl_, "SELECT pos, hash, data FROM position WHERE data IS NOT NULL ORDER BY LENGTH(pos) ASC" );
	if( !s.ok() ) {
		return false;
	}

	export_data ed;
	if( !s.exec( ::export_book, &ed ) ) {
		return false;
	}

	char null_entry[3] = {0};

	std::ofstream out( fn, std::ofstream::trunc | std::ofstream::binary );

	char header[5] = {0};
	// Header length
	header[0] = 5;
	header[1] = 0;

	// Version
	header[2] = simple_book_version % 256;
	header[3] = simple_book_version / 256;

	// Moves per entry
	header[4] = 5;

	out.write( header, 5 );

	unsigned int written_positions = 0;
	std::size_t written_moves = 0;

	// Data
	for( auto it : ed.output ) {
		auto entries = it.second;

		// Sort entries
		std::stable_sort( entries.begin(), entries.end(),
				[]( export_entry const& lhs, export_entry const& rhs ) {
					if( lhs.forecast > rhs.forecast ) {
						return true;
					}

					if( lhs.forecast == rhs.forecast ) {
						return lhs.depth_ > rhs.depth_;
					}

					return false;
				}
			);

		std::size_t i;
		for( i = 0; i < entries.size() && i < 5; ++i ) {
			auto const& e = entries[i];

			if( e.forecast > 250 || e.forecast < -250 ) {
				break;
			}
			if( i && e.forecast + 25 < entries[0].forecast ) {
				break;
			}
		}

		if( i ) {
			uint64_t hash = it.first;
			for( unsigned int k = 0; k < 8; ++k ) {
				out << static_cast<unsigned char>(hash % 256);
				hash >>= 8;
			}

			for( std::size_t j = 0; j < i; ++j ) {
				auto const& e = entries[j];
				out << e.mi_;

				unsigned short f = static_cast<unsigned short>( e.forecast );
				out << static_cast<unsigned char>( f % 256 );
				out << static_cast<unsigned char>( f / 256 );
			}

			++written_positions;
			written_moves += i;

			for( ; i < 5; ++i ) {
				out.write( null_entry, 3 );
			}
		}
	}

	std::cerr << " done" << std::endl;

	std::cout << "Exported book contains " << written_positions << " positions with " << written_moves << " moves." << std::endl;

	return true;
}

bool book::fold( bool verify )
{
	if( !impl_->is_writable() ) {
		std::cerr << "Error: Cannot fold read-only opening book\n" << std::endl;
		return false;
	}

	scoped_lock l(impl_->mtx);

	transaction t( *impl_ );
	if( !t.init() ) {
		return false;
	}

	statement sc( *impl_, "SELECT LENGTH(pos) FROM position ORDER BY LENGTH(pos) DESC LIMIT 1" );

	uint64_t max_length = 0;
	if( !sc.exec( count_cb, &max_length ) ) {
		return false;
	}

	std::cerr << "Folding";

	statement s( *impl_, "SELECT pos, data FROM position WHERE length(pos) = :1 AND data IS NOT NULL;" );
	if( !s.ok() ) {
		return false;
	}

	for( uint64_t i = max_length; i > 0; i -= 2 ) {
		std::cerr << ".";
		s.bind( 1, i );
		if( !s.exec( verify ? verify_position : fold_position, impl_ ) ) {
			return false;
		}
	}
	std::cerr << " done" << std::endl;

	std::cerr << "Comitting...";

	if( !t.commit() ) {
		return false;
	}
	std::cerr << " done" << std::endl;

	return true;
}


namespace {
int stats_processed_cb( void* p, statement& s ) {
	book_stats* stats = reinterpret_cast<book_stats*>(p);

	int64_t depth = s.get_int(0) / 2;
	int64_t processed = s.get_int(1);

	if( depth > 0 && processed > 0 ) {
		stats->data[depth].processed = static_cast<uint64_t>(processed);
		stats->total_processed += static_cast<uint64_t>(processed);
	}

	return 0;
}

int stats_queued_cb( void* p, statement& s ) {
	book_stats* stats = reinterpret_cast<book_stats*>(p);

	int64_t depth = s.get_int(0) / 2;
	int64_t queued = s.get_int(1);

	if( depth > 0 && queued > 0 ) {
		stats->data[depth].queued = static_cast<uint64_t>(queued);
		stats->total_queued += static_cast<uint64_t>(queued);
	}

	return 0;
}
}

book_stats book::stats()
{
	book_stats ret;

	scoped_lock l(impl_->mtx);

	statement s( *impl_, "SELECT length(pos), count(pos), SUM(LENGTH(data)/4) FROM position WHERE data is NOT NULL GROUP BY LENGTH(pos) ORDER BY LENGTH(pos)");
	s.exec( stats_processed_cb, reinterpret_cast<void*>(&ret) );

	statement s2( *impl_, "SELECT length(pos), count(pos), SUM(LENGTH(data)/4) FROM position WHERE data is NULL GROUP BY LENGTH(pos) ORDER BY LENGTH(pos)");
	s2.exec( stats_queued_cb, reinterpret_cast<void*>(&ret) );

	return ret;
}


bool book::set_insert_logfile( std::string const& log_file )
{
	scoped_lock l(impl_->mtx);

	impl_->logfile.close();
	if( !log_file.empty() ) {
		impl_->logfile.open( log_file.c_str(), std::ofstream::out|std::ofstream::app );
	}

	return impl_->logfile.is_open();
}


std::string entries_to_string( position const& p, std::vector<book_entry> const& entries )
{
	std::ostringstream out;
	out << "  Move     Forecast   In book" << std::endl;
	for( std::vector<book_entry>::const_iterator it = entries.begin(); it != entries.end(); ++it ) {
		out << std::setw(6) << move_to_san( p, it->m )
			<< std::setw(7) << it->forecast << " @ " << std::setw(2) << static_cast<int>(it->search_depth) << std::setw(6) << (it->is_folded() ? "yes" : "")
			<< std::endl;
	}

	return out.str();
}

bool book::is_writable() const
{
	return impl_->is_writable();
}
