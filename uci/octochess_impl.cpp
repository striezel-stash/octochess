
#include "octochess_impl.hpp"
#include "info.hpp"
#include "time_calculation.hpp"

#include "../chess.hpp"
#include "../seen_positions.hpp"
#include "../book.hpp"
#include "../calc.hpp"
#include "../eval.hpp"
#include "../fen.hpp"
#include "../hash.hpp"
#include "../pawn_structure_hash_table.hpp"
#include "../pv_move_picker.hpp"
#include "../random.hpp"
#include "../util/logger.hpp"
#include "../util/mutex.hpp"
#include "../util/string.hpp"
#include "../util/thread.hpp"
#include "../util.hpp"

#include <sstream>
#include <set>

namespace octochess {
namespace uci {

class octochess_uci::impl : public new_best_move_callback_base, public thread {
public:
	impl( gui_interface_ptr const& p )
		: gui_interface_(p)
		, half_moves_played_()
		, started_from_root_()
		, book_( conf.self_dir )
		, depth_(-1)
		, wait_for_stop_(false)
		, ponder_()
	{
	}

	~impl() {
		calc_manager_.abort();
		join();
	}

	virtual void onRun();
	void on_new_best_move( unsigned int multipv, position const& p, int depth, int selective_depth, int evaluation, uint64_t nodes, duration const& elapsed, move const* pv ) override;

	void apply_move( move const& m );

	bool do_book_move();
	bool pick_pv_move();

public:
	gui_interface_ptr gui_interface_;

	position pos_;
	seen_positions seen_positions_;

	calc_manager calc_manager_;

	time_calculation times_;

	int half_moves_played_;
	bool started_from_root_;

	mutex mutex_;

	book book_;

	pv_move_picker pv_move_picker_;

	std::vector<move> move_history_;

	int depth_;
	bool wait_for_stop_;
	bool ponder_;

	std::set<move> searchmoves_;

	calc_result result_;

	timestamp start_;
};

octochess_uci::octochess_uci( gui_interface_ptr const& p ) 
	: impl_( new impl(p) )
{
	p->set_engine_interface(*this);

	pawn_hash_table.init( conf.pawn_hash_table_size() );
	new_game();
}

void octochess_uci::new_game() {
	impl_->calc_manager_.abort();
	impl_->join();
	impl_->pos_.reset();
	impl_->seen_positions_.reset_root( impl_->pos_.hash_);
	impl_->times_ = time_calculation();
	impl_->half_moves_played_ = 0;
	impl_->started_from_root_ = true;
	impl_->move_history_.clear();
}

void octochess_uci::set_position( std::string const& fen ) {
	impl_->calc_manager_.abort();
	impl_->join();
	impl_->pos_.reset();
	impl_->move_history_.clear();
	bool success = false;
	if( fen.empty() ) {
		impl_->started_from_root_ = true;
		success = true;
	}
	else {
		if( parse_fen( fen, impl_->pos_, 0 ) ) {
			impl_->started_from_root_ = false;
			success = true;
		}
		else {
			std::cerr << "failed to set position" << std::endl;
		}
	}

	if( success ) {
		impl_->seen_positions_.reset_root( impl_->pos_.hash_ );
		impl_->half_moves_played_ = 0;
	}
}

void octochess_uci::make_moves( std::string const& moves ) {
	make_moves( tokenize( moves ) );
}

void octochess_uci::make_moves( std::vector<std::string> const& moves )
{
	bool done = false;
	for( std::vector<std::string>::const_iterator it = moves.begin(); !done && it != moves.end(); ++it ) {
		move m;
		std::string error;
		if( parse_move( impl_->pos_, *it, m, error ) ) {
			impl_->apply_move( m );
		} else {
			std::cerr << error << ": " << *it << std::endl;
			done = true;
		}
	}
}

void octochess_uci::calculate( timestamp const& start, calculate_mode_type mode, position_time const& t, int depth, bool ponder, std::string const& searchmoves )
{
	transposition_table.init_if_needed( conf.memory );

	impl_->calc_manager_.abort();
	impl_->join();

	scoped_lock lock(impl_->mutex_);

	impl_->result_.best_move.clear();
	impl_->calc_manager_.clear_abort();
	impl_->start_ = start;

	if( mode == calculate_mode::ponderhit ) {
		impl_->ponder_ = false;
		impl_->spawn();
	}
	else {
		impl_->depth_ = depth;
		impl_->wait_for_stop_ = mode == calculate_mode::infinite;
		impl_->ponder_ = ponder;
		impl_->searchmoves_.clear();

		for( auto ms : tokenize(searchmoves) ) {
			std::string error;

			move m;
			if( parse_move( impl_->pos_, ms, m, error ) ) {
				impl_->searchmoves_.insert( m );
			}
		}

		impl_->times_.update( t, impl_->pos_.white(), impl_->half_moves_played_ );
		if( ponder || impl_->times_.time_for_this_move().is_infinity() || impl_->wait_for_stop_ || !impl_->searchmoves_.empty() ) {
			impl_->spawn();
		}
		else {
			if( !impl_->do_book_move() ) {
				if( !impl_->pick_pv_move() ) {
					impl_->spawn();
				}
			}
		}
	}
}

void octochess_uci::stop()
{
	impl_->calc_manager_.abort();
	impl_->join();

	scoped_lock lock(impl_->mutex_);

	if( !impl_->result_.best_move.empty() ) {
		impl_->gui_interface_->tell_best_move( move_to_long_algebraic( impl_->result_.best_move ), move_to_long_algebraic( impl_->result_.ponder_move ) );
		impl_->result_.best_move.clear();
	}
}

void octochess_uci::quit() {
	impl_->calc_manager_.abort();
}

void octochess_uci::impl::onRun() {
	//the function initiating all the calculating
	if( ponder_ ) {
		std::cerr << "Pondering..." << std::endl;
		calc_result result = calc_manager_.calc( pos_, -1, start_, duration::infinity(), duration::infinity()
			, half_moves_played_, seen_positions_, *this, searchmoves_ );

		scoped_lock lock(mutex_);

		if( !result.best_move.empty() ) {
			result_ = result;
		}
	}
	else {
		calc_result result = calc_manager_.calc( pos_, depth_, start_, times_.time_for_this_move(), std::max( duration(), times_.total_remaining() - times_.overhead() )
			, half_moves_played_, seen_positions_, *this, searchmoves_ );

		scoped_lock lock(mutex_);

		if( !result.best_move.empty() ) {

			if( !wait_for_stop_ ) {
				gui_interface_->tell_best_move( move_to_long_algebraic( result.best_move ), move_to_long_algebraic( result.ponder_move ) );
				result_.best_move.clear();
			}
			else {
				result_ = result;
			}

			timestamp stop;
			duration elapsed = stop - start_;

			std::cerr << "Elapsed: " << elapsed.milliseconds() << " ms" << std::endl;
			times_.after_move_update( elapsed, result.used_extra_time );
		}
	}
}

void octochess_uci::impl::on_new_best_move( unsigned int multipv, position const& p, int depth, int selective_depth, int evaluation, uint64_t nodes, duration const& elapsed, move const* pv ) {
	info i;
	i.depth( depth );
	i.selective_depth( selective_depth );
	i.node_count( nodes );
	i.multipv( multipv );

	int mate = 0;
	if( evaluation > result::win_threshold ) {
		mate = (result::win - evaluation + 1) / 2;
	} else if( evaluation < result::loss_threshold ) {
		mate = (result::loss - evaluation) / 2;
	}

	if( mate == 0 ) {
		i.score( evaluation );
	} else {
		i.mate_in_n_moves( mate );
	}

	i.time_spent( elapsed );
	if( !elapsed.empty() ) {
		i.nodes_per_second( elapsed.get_items_per_second(nodes) );
	}

	i.principal_variation( pv_to_string(pv, p, true) );

	gui_interface_->tell_info( i );

	pv_move_picker_.update_pv( p, pv );
}


void octochess_uci::impl::apply_move( move const& m )
{
	bool reset_seen = false;
	pieces::type piece = pos_.get_piece( m );
	pieces::type captured_piece = pos_.get_captured_piece( m );
	if( piece == pieces::pawn || captured_piece ) {
		reset_seen = true;
	}
	::apply_move( pos_, m );
	++half_moves_played_;

	if( !reset_seen ) {
		seen_positions_.push_root( pos_.hash_ );
	} else {
		seen_positions_.reset_root( pos_.hash_ );
	}

	move_history_.push_back( m );
}


bool octochess_uci::impl::do_book_move() {
	bool ret = false;

	if( conf.use_book && book_.is_open() && half_moves_played_ < 30 && started_from_root_ ) {
		std::vector<book_entry> moves = book_.get_entries( pos_ );
		if( !moves.empty() ) {
			ret = true;

			short best = moves.front().forecast;
			int count_best = 1;
			for( std::vector<book_entry>::const_iterator it = moves.begin() + 1; it != moves.end(); ++it ) {
				if( it->forecast > -30 && it->forecast + 15 >= best ) {
					++count_best;
				}
			}

			book_entry best_move = moves[get_random_unsigned_long_long() % count_best];
			gui_interface_->tell_best_move( move_to_long_algebraic( best_move.m ), "" );
		}
		else if( half_moves_played_ <= 20 ) {
			book_.mark_for_processing( move_history_ );
		}
	}

	return ret;
}


bool octochess_uci::impl::pick_pv_move()
{
	bool ret = false;
	std::pair<move,move> m = pv_move_picker_.can_use_move_from_pv( pos_ );
	if( !m.first.empty() ) {
		ret = true;
		gui_interface_->tell_best_move( move_to_long_algebraic( m.first ), move_to_long_algebraic( m.second ) );
	}

	return ret;
}


uint64_t octochess_uci::get_hash_size() const
{
	return conf.memory;
}


uint64_t octochess_uci::get_min_hash_size() const
{
	return 4ull;
}


void octochess_uci::set_hash_size( uint64_t mb )
{
	if( mb < 4 ) {
		mb = 4;
	}
	conf.memory = static_cast<unsigned int>(mb);
}


unsigned int octochess_uci::get_threads() const
{
	return conf.thread_count;
}


unsigned int octochess_uci::get_max_threads() const
{
	return get_cpu_count();
}


void octochess_uci::set_threads( unsigned int threads )
{
	if( !threads ) {
		threads = 1;
	}
	if( threads > get_cpu_count() ) {
		threads = get_cpu_count();
	}
	conf.thread_count = threads;
}


bool octochess_uci::use_book() const
{
	return impl_->book_.is_open();
}


void octochess_uci::use_book( bool use )
{
	if( use ) {
		impl_->book_.open( conf.self_dir );
	}
	else {
		impl_->book_.close();
	}
}


void octochess_uci::set_multipv( unsigned int multipv )
{
	impl_->calc_manager_.set_multipv( multipv );
}


bool octochess_uci::is_move( std::string const& ms )
{
	bool ret = false;
	std::string error;

	move m;
	if( parse_move( impl_->pos_, ms, m, error ) ) {
		ret = true;
	}

	return ret;
}


}
}

