#include "assert.hpp"
#include "calc.hpp"
#include "config.hpp"
#include "eval.hpp"
#include "eval_values.hpp"
#include "fen.hpp"
#include "hash.hpp"
#include "moves.hpp"
#include "phased_move_generator.hpp"
#include "random.hpp"
#include "see.hpp"
#include "statistics.hpp"
#include "tables.hpp"
#include "util/logger.hpp"
#include "util/mutex.hpp"
#include "util/thread.hpp"
#include "util.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <math.h>
#include <map>
#include <sstream>
#include <string>
#include <vector>

int const check_extension = 6;
int const pawn_push_extension = 6;
int const recapture_extension = 6;
int const cutoff = DEPTH_FACTOR + MAX_QDEPTH + 1;

int const lmr_min_depth = cutoff + DEPTH_FACTOR * 2;

short const razor_pruning[] = { 220, 250, 290 };

#define USE_FUTILITY 1
short const futility_pruning[] = { 110, 130, 170, 210 };

#define NULL_MOVE_REDUCTION 3

int const NULLMOVE_VERIFICATION_DEPTH = cutoff + DEPTH_FACTOR * 5;

int const delta_pruning = 50;

null_new_best_move_callback null_new_best_move_cb;

void sort_moves( move_info* begin, move_info* end, position const& p )
{
	for( move_info* it = begin; it != end; ++it ) {
		it->sort = evaluate_move( p, it->m );
		pieces::type captured_piece = p.get_captured_piece( it->m );
		if( captured_piece != pieces::none ) {
			pieces::type piece = p.get_piece( it->m.source() );
			it->sort += eval_values::material_values[ captured_piece ].mg() * 1000000 - eval_values::material_values[ piece ].mg();
		}
	}
	std::stable_sort( begin, end, moveSort );
}


bool is_50move_draw( position const& p, check_map const& check, calc_state& state, int ply, short& ret )
{
	if( p.halfmoves_since_pawnmove_or_capture >= 100 ) {
		if( !check.check ) {
			ret = result::draw;
			return true;
		}
		else {
			move_info* m = state.move_ptr;
			calculate_moves<movegen_type::all>( p, m, check );
			if( m != state.move_ptr ) {
				ret = result::draw;
			}
			else {
				ret = result::loss + ply;
			}
			return true;
		}
	}

	return false;
}

/*
New multithreading concept
General idea: At each node, search first move on its own, then search rest of nodes in parallel
Main constraint: There are far far more nodes and far more moves at each node than there are threads.
Solution:
- Have a pool of worker threads
- If a thread has finished searching the first move at a node and there are idle threads, perform a split:
  1. push current state onto work list
  2. wake up threads and let them search moves in parallel
  3. wait[1] until either 
     a) a cutoff has been produced or
     b) all moves searched
  Otherwise: Don't split, do it single-threaded.

- [1] Idea to avoid idle, waiting threads: If a thread splits, let it participate in processing the work list by nesting the idle loops

Things to consider:
- Find a good minimum split depth. If it's too low, overhead becomes too large
- Synchronization overhead. Need to keep things fast. Start with a cheap idle check on a volatile bool and then
  only do the slow locking if things look promising
*/
class thread_pool;
namespace {
class work;
}
class worker_thread : public thread
{
public:
	worker_thread( thread_pool& pool, uint64_t thread_index );

	virtual void onRun();

	void process_work( scoped_lock& l );

	condition cond_;

	volatile bool quit_;
	volatile bool do_abort_;

	enum {
#if MAX_THREADS > 1
		max_calc_states = 10
#else
		max_calc_states = 1
#endif
	};

	calc_state calc_states_[max_calc_states];
	unsigned int state_it_;

	thread_pool& pool_;
	uint64_t thread_index_;

	work* current_work_;
};


struct move_data {
	move_data()
		: depth( 1 )
		, seldepth( 1 )
	{
	}

	move_info m;
	move pv[MAX_DEPTH];
	unsigned int depth;
	unsigned int seldepth;
};

typedef std::vector<move_data> sorted_moves;


class master_worker_thread : public worker_thread
{
public:
	master_worker_thread( thread_pool& pool, uint64_t thread_index )
		: worker_thread( pool, thread_index )
		, idle_(true)
		, max_depth_()
		, cb_()
		, multipv_(1)
	{
		state_it_ = 1;
	}

	sorted_moves get_moves() { return moves_; }

	bool idle() const { return idle_; }

	condition calc_cond_;

	void init( position const& p, sorted_moves const& moves, int clock, seen_positions const& seen, new_best_move_callback_base* cb, timestamp const& start );
	void process( scoped_lock& l, unsigned int max_depth );

	void set_multipv( std::size_t multipv ) {
		multipv_ = multipv;
	}

private:
	virtual void onRun();

	void process_root( scoped_lock& l );

	void print_best( unsigned int updated );

	bool idle_;

	position p_;
	sorted_moves moves_;
	unsigned int max_depth_;
	seen_positions seen_;

	new_best_move_callback_base* cb_;

	timestamp start_;

	std::size_t multipv_;
};

namespace {
class work
{
public:
	work( worker_thread* master, int depth, int ply, position const& p
		, check_map const& check, short alpha, short beta, short full_eval
		, unsigned char last_ply_was_capture, bool pv_node, short& best_value, move& best_move, phased_move_generator_base& gen
		, calc_state& master_state);

	int const depth_;
	int const ply_;
	position const& p_;
	check_map const& check_;
	short alpha_;
	short const beta_;
	short const full_eval_;
	unsigned char const last_ply_was_capture_;
	bool const pv_node_;
	short& best_value_;
	move& best_move_;
	phased_move_generator_base& gen_;
	unsigned int processed_moves_;
	calc_state& master_state_;

	worker_thread* master_;
	
	uint64_t active_workers_;
	unsigned char active_worker_calc_states_[64];

	// True if there has been a cutoff
	bool cutoff_;

	// True if all work processed, no more active children
	bool done_;

	work* parent_split_;
};

bool cutoff_in_tree( work* w )
{
	while( w && !w->cutoff_ ) {
		w = w->parent_split_;
	}
	return w != 0;
}

work::work( worker_thread* master, int depth, int ply, position const& p
		, check_map const& check, short alpha, short beta, short full_eval
		, unsigned char last_ply_was_capture, bool pv_node, short& best_value, move& best_move, phased_move_generator_base& gen
		, calc_state& master_state )
	: depth_(depth)
	, ply_(ply)
	, p_(p)
	, check_(check)
	, alpha_(alpha)
	, beta_(beta)
	, full_eval_(full_eval)
	, last_ply_was_capture_(last_ply_was_capture)
	, pv_node_(pv_node)
	, best_value_(best_value)
	, best_move_(best_move)
	, gen_(gen)
	, processed_moves_(1)
	, master_state_(master_state)
	, master_(master)
	, active_workers_()
	, cutoff_()
	, done_()
	, parent_split_(master->current_work_)
{
}
}


class thread_pool
{
public:
	thread_pool( context& ctx, mutex& m );
	~thread_pool();

	void update_threads();
	void reduce_histories();

	void abort( scoped_lock& l );
	void clear_abort();
	void wait_for_idle( scoped_lock& l );

	context& ctx_;
	
	volatile bool idle_;

	mutex& m_;

	work* work_;

	master_worker_thread* master() { return threads_.empty() ? 0 : reinterpret_cast<master_worker_thread*>(threads_[0]); }

	uint64_t idle_threads_;
	std::vector<worker_thread*> threads_;

#if USE_STATISTICS
	statistics stats_;
#endif
};


thread_pool::thread_pool( context& ctx, mutex& m )
	: ctx_(ctx)
	, idle_(true)
	, m_(m)
	, work_()
	, idle_threads_()
{
	scoped_lock l( m_ );

	for( unsigned int i = 0; i < std::min( 64u, ctx_.conf_.thread_count ); ++i ) {
		worker_thread* t;
		if( !i ) {
			t = new master_worker_thread( *this, i );
		}
		else {
			t = new worker_thread( *this, i );
		}
		idle_threads_ |= 1ull << i;
		threads_.push_back( t );
		t->spawn();
	}
}


thread_pool::~thread_pool()
{
	{
		scoped_lock l( m_ );
		for( auto thread : threads_ ) {
			thread->do_abort_ = true;
			thread->quit_ = true;
			thread->cond_.signal( l );
		}
		wait_for_idle( l );
	}

	for( auto thread : threads_ ) {
		thread->join();
		delete thread;
	}
}


void thread_pool::abort( scoped_lock& )
{
	if( work_ ) {
		work_->gen_.set_done();
		work_ = 0;
	}
	for( auto thread : threads_ ) {
		thread->do_abort_ = true;
		for( unsigned int i = 0; i < thread->max_calc_states; ++i ) {
			thread->calc_states_[i].do_abort_ = true;
		}
	}
}


void thread_pool::clear_abort()
{
	for( auto thread : threads_ ) {
		thread->do_abort_ = false;
	}
}


void thread_pool::wait_for_idle( scoped_lock& l )
{
	while( !master()->idle() ) {
		master()->calc_cond_.wait( l );
	}
}


void thread_pool::update_threads()
{
	unsigned int const thread_count = std::min( 64u, ctx_.conf_.thread_count );
	ASSERT( thread_count > 0 );
	ASSERT( idle_ );

	while( threads_.size() < thread_count ) {
		worker_thread* t = new worker_thread( *this, threads_.size() );
		idle_threads_ |= 1ull << threads_.size();
		threads_.push_back( t );
		t->spawn();
	}

	while( threads_.size() > thread_count ) {
		std::vector<worker_thread*>::iterator it = --threads_.end();
		(*it)->quit_ = true;

		{
			scoped_lock l( m_ );
			(*it)->cond_.signal( l );
		}

		(*it)->join();
		idle_threads_ &= ~(1ull << (*it)->thread_index_);
		delete *it;
		threads_.erase( it );
	}

	ASSERT( threads_.size() == popcount(idle_threads_) );
}

void thread_pool::reduce_histories()
{
	for( auto thread : threads_ ) {
		for( unsigned int i = 0; i < thread->max_calc_states; ++i ) {
			thread->calc_states_[i].history_.reduce();
		}
	}
}

worker_thread::worker_thread( thread_pool& pool, uint64_t thread_index )
	: quit_()
	, do_abort_()
	, state_it_()
	, pool_(pool)
	, thread_index_(thread_index)
	, current_work_()
{
	for( unsigned int i = 0; i < max_calc_states; ++i ) {
		calc_states_[i].thread_ = this;
		calc_states_[i].tt_ = &pool.ctx_.tt_;
		calc_states_[i].pawn_tt_ = &pool.ctx_.pawn_tt_;
	}
}

void worker_thread::onRun()
{
	scoped_lock l( pool_.m_ );
	while( !quit_ ) {

		pool_.idle_threads_ |= 1ull << thread_index_;
		pool_.idle_ = true;

		cond_.wait( l );
		
		pool_.idle_threads_ &= ~(1ull << thread_index_);
		if( !pool_.idle_threads_ ) {
			pool_.idle_ = false;
		}

		process_work( l );
	}
}

void worker_thread::process_work( scoped_lock& l )
{
	work* w;
	while( (w = pool_.work_) && !quit_ && !do_abort_ ) {

		ASSERT( !w->cutoff_ );
		if( cutoff_in_tree( w->parent_split_ ) ) {
			if( pool_.work_ == w ) {
				pool_.work_ = 0;
			}
			w->cutoff_ = true;
			w->gen_.set_done();
		}
		else {
			move const m = w->gen_.next();

			if( !m.empty() ) {
				ASSERT( state_it_ < max_calc_states );

				// Create calc_state from work
				calc_state& state = calc_states_[state_it_];
				state.do_abort_ = false;

				ASSERT( !(w->active_workers_ & (1ull << thread_index_)) );
				w->active_workers_ |= 1ull << thread_index_;
				w->active_worker_calc_states_[thread_index_] = state_it_++;
			
				// Extract non-const data
				unsigned int processed = w->processed_moves_++;
				short alpha = w->alpha_;
				phases::type phase = w->gen_.get_phase();
				short best_value = w->best_value_;

				work* old_work = current_work_;
				current_work_ = w;

				l.unlock();

				state.move_ptr = state.moves;
				state.seen.clone_from( w->master_state_.seen, w->ply_ );

				short value = state.inner_step( w->depth_, w->ply_, w->p_, w->check_, alpha, w->beta_, w->full_eval_
					, w->last_ply_was_capture_, w->pv_node_, m, processed, phase, best_value );

				l.lock();

				current_work_ = old_work;

				ASSERT( w->active_workers_ & (1ull << thread_index_) );
				w->active_workers_ &= ~(1ull << thread_index_);

				ASSERT( state_it_ > 0 );
				--state_it_;

				if( !do_abort_ && !state.do_abort_ ) {
					if( value > w->best_value_ ) {
						w->best_value_ = value;

						if( value > w->alpha_ ) {
							w->best_move_ = m;

							if( value >= w->beta_ ) {

								// Killer (and history handling)
								if( !w->p_.get_captured_piece(m) ) {
									state.killers[w->p_.self()].add_killer( m, w->ply_ );
									state.history_.record_cut( w->p_, m, processed );
									w->master_state_.killers[w->p_.self()].add_killer( m, w->ply_ );
									w->master_state_.history_.record_cut( w->p_, m, processed );
								}

								// We're done, with cutoff
								if( pool_.work_ == w ) {
									pool_.work_ = 0;
								}
								w->cutoff_ = true;

								// Abort other workers
								uint64_t active = w->active_workers_;
								while( active ) {
									uint64_t index = bitscan_unset( active );
									pool_.threads_[index]->calc_states_[w->active_worker_calc_states_[index]].do_abort_ = true;
								}
								w->gen_.set_done();
							}
							else {
								w->alpha_ = value;
							}
						}
					}
					if( value < w->beta_ ) {
						state.history_.record( w->p_, m );
						w->master_state_.history_.record( w->p_, m );
					}
				}
			}
			else {
				// Not much more to do with this node
				pool_.work_ = 0;
				ASSERT( w->gen_.get_phase() == phases::done );
			}
		}

		if( w->gen_.get_phase() == phases::done && !w->active_workers_ && !w->done_ ) {
			w->done_ = true;
			w->master_->cond_.signal( l );
		}
	}
}


void master_worker_thread::onRun()
{
	scoped_lock l( pool_.m_ );
	while( !quit_ ) {

		do {
			cond_.wait( l );
		}
		while( idle_ && !quit_ && !do_abort_ );
		
		if( moves_.empty() || do_abort_ ) {
			if( !idle_ ) {
				idle_ = true;
				calc_cond_.signal( l );
			}
		}
		else if( !idle_ ) {

			pool_.idle_threads_ &= ~(1ull << thread_index_);
			if( !pool_.idle_threads_ ) {
				pool_.idle_ = false;
			}

			process_root( l );

			idle_ = true;
			calc_cond_.signal( l );

			pool_.idle_threads_ |= 1ull << thread_index_;
			pool_.idle_ = true;
			ASSERT( popcount(pool_.idle_threads_) == pool_.threads_.size() );
		}
	}
}


void master_worker_thread::process_root( scoped_lock& l )
{
	calc_state& state = calc_states_[0];
	state.do_abort_ = false;

	l.unlock();

	short root_alpha = result::loss;
	short root_beta = result::win;

	std::size_t const multipv = std::min( moves_.size(), multipv_ );

	for( std::size_t i = 0; i < moves_.size() && !do_abort_; ++i ) {
		move_data& d = moves_[i];

		position new_pos = p_;
		apply_move( new_pos, d.m.m );

		state.seen = seen_;
		state.move_ptr = state.moves;

		short value;
		if( state.seen.is_two_fold( new_pos.hash_, 1 ) ) {
			value = result::draw;
		}
		else {
			state.seen.push_root( new_pos.hash_ );

			check_map check( new_pos );

			// Search using aspiration window:
			value = result::loss;
			if( root_alpha == result::loss && d.m.sort != result::loss && max_depth_ > 4 ) {

				int aspiration = 10;
				short alpha = std::max( static_cast<short>(result::loss), static_cast<short>(d.m.sort - aspiration) );
				short beta = std::min( static_cast<short>(result::win), static_cast<short>(d.m.sort + aspiration) );

				while( value == result::loss ) {
					short value = -state.step( max_depth_ * DEPTH_FACTOR + MAX_QDEPTH + 1, 1, new_pos, check, -beta, -alpha, false );
					if( value >= beta ) {
						if( result::win - aspiration > beta ) {
							beta += aspiration;
						}
						else {
							beta = result::win;
						}
					}
					else if( value <= alpha ) {
						if( result::loss + aspiration < alpha ) {
							alpha -= aspiration;
						}
						else {
							alpha = result::loss;
						}
					}
					else {
						break;
					}
					aspiration += aspiration / 2;
				}
			}

			if( root_alpha != result::loss && value == result::loss ) {
				short v = -state.step( max_depth_ * DEPTH_FACTOR + MAX_QDEPTH + 1, 1, new_pos, check, -root_alpha-1, -root_alpha, false );
				if( v <= root_alpha ) {
					value = v;
				}
			}
			
			if( value == result::loss ) {
				value = -state.step( max_depth_ * DEPTH_FACTOR + MAX_QDEPTH + 1, 1, new_pos, check, -root_beta, -root_alpha, false );
			}
		}

		if( value > root_alpha && !do_abort_ ) {
			// Bubble new best to front
			d.m.sort = value;
			d.depth = max_depth_;
#if USE_STATISTICS
			d.seldepth = pool_.stats_.highest_depth();
#endif
			get_pv_from_tt( *state.tt_, d.pv, p_, max_depth_ );
			std::size_t j;
			for( j = i; j > 0 && (j > multipv || moves_[j-1].m.sort < value); --j ) {
				std::swap( moves_[j], moves_[j-1] );
			}

			// Print new results
			print_best( static_cast<unsigned int>(j) );
	
			if( i + 1 >= multipv ) {
				// All PVs searched full with. Now we can use null windows.
				root_alpha = moves_[multipv-1].m.sort;
			}
		}
	}

	l.lock();
}


void master_worker_thread::print_best( unsigned int updated )
{
	std::size_t const multipv = std::min( moves_.size(), multipv_ );

#if USE_STATISTICS
	uint64_t nodes = pool_.stats_.nodes() + pool_.stats_.quiescence_nodes;
#else
	uint64_t nodes = 0;
#endif

	duration elapsed( start_, timestamp() );

	if( cb_->print_only_updated() ) {
		move_data& d = moves_[updated];
		cb_->on_new_best_move( updated + 1, p_, d.depth, d.seldepth, d.m.sort, nodes, elapsed, d.pv );
	}
	else {
		for( unsigned int pvi = 0; pvi < multipv; ++pvi ) {
			move_data& d = moves_[pvi];

			cb_->on_new_best_move( pvi + 1, p_, d.depth, d.seldepth, d.m.sort, nodes, elapsed, d.pv );
		}
	}
}

void master_worker_thread::init( position const& p, sorted_moves const& moves, int clock, seen_positions const& seen, new_best_move_callback_base* cb , timestamp const& start )
{
	ASSERT( idle_ );
	p_ = p;
	moves_ = moves;
	seen_ = seen;
	cb_ = cb;
	start_ = start;

	clock %= 256;
	for( auto thread : pool_.threads_ ) {
		for( unsigned int i = 0; i < max_calc_states; ++i ) {
			thread->calc_states_[i].clock = clock;
		}
	}
}


void master_worker_thread::process( scoped_lock& l, unsigned int max_depth )
{
	ASSERT( idle_ );
	
	max_depth_ = max_depth;

	idle_ = false;
	cond_.signal( l );
}


void calc_state::split( int depth, int ply, position const& p
		, check_map const& check, short alpha, short beta, short full_eval, unsigned char last_ply_was_capture, bool pv_node, short& best_value, move& best_move, phased_move_generator_base& gen )
{
	ASSERT( thread_ );
	if( !thread_->pool_.idle_ ) {
		return;
	}

	scoped_lock l( thread_->pool_.m_ );

	if( do_abort_ || thread_->do_abort_ ) {
		return;
	}

	// Recheck idle condition now that we got the lock
	if( thread_->pool_.work_ || !thread_->pool_.idle_threads_ ) {
		return;
	}

	{
		work* w = thread_->current_work_;
		if( cutoff_in_tree( w ) ) {
			do_abort_ = true;
			return;
		}
	}

	// Queue work
	work w( thread_, depth, ply, p, check, alpha, beta, full_eval, last_ply_was_capture, pv_node, best_value, best_move, gen, *this );
	thread_->pool_.work_ = &w;

	// Wake up idle workers
	uint64_t idle = thread_->pool_.idle_threads_;
	while( idle ) {
		uint64_t index = bitscan_unset( idle ); 
		thread_->pool_.threads_[index]->cond_.signal( l );
	}

	while( !w.done_ ) {
		if( thread_->state_it_ < thread_->max_calc_states ) {
			// Be a helpful master and participate in the search, if it either
			// is our own split point, or belongs to a slave.
			work* pw = thread_->pool_.work_;
			while( pw && pw != &w ) {
				pw = pw->parent_split_;
			}
			if( pw == &w ) {
				thread_->process_work( l );
				continue;
			}
		}

		thread_->cond_.wait( l );
	}
	ASSERT( !w.active_workers_ );
	ASSERT( thread_->pool_.work_ != &w );

	{
		if( cutoff_in_tree( thread_->current_work_ ) ) {
			do_abort_ = true;
		}
	}
}



short calc_state::quiescence_search( int ply, int depth, position const& p, check_map const& check, short alpha, short beta, short full_eval )
{
#if VERIFY_HASH
	if( p.init_hash() != p.hash_ ) {
		std::cerr << "FAIL HASH!" << std::endl;
		abort();
	}
#endif
#if VERIFY_POSITION
	p.verify_abort();
#endif

	if( do_abort_ ) {
		return result::loss;
	}

#if USE_STATISTICS
	add_relaxed( thread_->pool_.stats_.quiescence_nodes , 1 );
#endif

	short ret;
	if( is_50move_draw( p, check, *this, ply, ret ) ) {
		return ret;
	}
	if( !p.bitboards[color::white][bb_type::pawns] && !p.bitboards[color::black][bb_type::pawns] ) {
		if( p.material[color::white].eg() + p.material[color::black].eg() <= eval_values::insufficient_material_threshold ) {
			return result::draw;
		}
	}

	if( !depth ) {
		return result::draw; //beta;
	}

	bool pv_node = alpha + 1 != beta;

	bool do_checks = depth >= MAX_QDEPTH;

	int tt_depth = do_checks ? 2 : 1;

	short eval;
	move tt_move;
	score_type::type t = tt_->lookup( p.hash_, tt_depth, ply, alpha, beta, eval, tt_move, full_eval );
	ASSERT( full_eval == result::win || full_eval == evaluate_full( *pawn_tt_, p ) );

	if ( (!pv_node && t != score_type::none) || t == score_type::exact ) {
		return eval;
	}

#if 0
	full_eval = evaluate_full( p, p.self(), current_evaluation );
	short diff = std::abs( full_eval - current_evaluation );
	if( diff > LAZY_EVAL ) {
		std::cerr << "Bug in lazy evaluation, full evaluation differs too much from basic evaluation:" << std::endl;
		std::cerr << "Position:   " << position_to_fen_noclock( p, c ) << std::endl;
		std::cerr << "Current:    " << current_evaluation << std::endl;
		std::cerr << "Full:       " << full_eval << std::endl;
		std::cerr << "Difference: " << full_eval - current_evaluation << std::endl;
		abort();
	}
#endif

	short old_alpha = alpha;

	if( !check.check ) {
		if( full_eval == result::win ) {
			full_eval = evaluate_full( *pawn_tt_, p );
		}
		if( full_eval > alpha ) {
			if( full_eval >= beta ) {
				if( !do_abort_ && t == score_type::none && tt_move.empty() ) {
					tt_->store( p.hash_, tt_depth, ply, full_eval, alpha, beta, tt_move, clock, full_eval );
				}
				return full_eval;
			}
			alpha = full_eval;
		}
	}

	qsearch_move_generator gen( *this, p, check, pv_node, do_checks );

	if( check.check || do_checks || (!tt_move.empty() && p.get_captured_piece( tt_move ) != pieces::none ) ) {
		gen.hash_move = tt_move;
	}

	move best_move;
	short best_value = check.check ? static_cast<short>(result::loss) : full_eval;

	move m;
	while( !(m = gen.next()).empty() ) {

		pieces::type piece = p.get_piece( m.source() );
		pieces::type captured_piece = p.get_captured_piece( m );
		
		position new_pos(p);
		apply_move( new_pos, m );

		check_map new_check( new_pos );
		
		if( captured_piece == pieces::none && !check.check && !new_check.check ) {
			continue;
		}

		// Delta pruning
		if( !pv_node && !check.check && !new_check.check && (piece != pieces::pawn || (m.target() >= 16 && m.target() < 48 ) ) ) {
			short new_value = full_eval + eval_values::material_values[captured_piece].mg() + delta_pruning;
			if( new_value <= alpha ) {
				if( new_value > best_value ) {
					best_value = new_value;
				}
				continue;
			}
		}

		short value = -quiescence_search( ply + 1, depth - 1, new_pos, new_check, -beta, -alpha );

		if( value > best_value ) {
			best_value = value;

			if( value > alpha ) {
				best_move = m;
				
				if( value >= beta ) {
					break;
				}
				else {
					alpha = value;
				}
			}
		}
	}

	if( best_value == result::loss && check.check ) {
		return result::loss + ply;
	}

	if( !do_abort_ ) {
		tt_->store( p.hash_, tt_depth, ply, best_value, old_alpha, beta, best_move, clock, full_eval );
	}
	return best_value;
}


short calc_state::step( int depth, int ply, position& p, check_map const& check, short alpha, short beta, bool last_was_null, short full_eval, unsigned char last_ply_was_capture )
{
#if VERIFY_HASH
	if( p.init_hash() != p.hash_ ) {
		std::cerr << "FAIL HASH!" << std::endl;
		abort();
	}
#endif
#if VERIFY_POSITION
	p.verify_abort();
#endif

	if( depth < cutoff || ply >= MAX_DEPTH ) {
		return quiescence_search( ply, MAX_QDEPTH, p, check, alpha, beta );
	}

	if( do_abort_ ) {
		return result::loss;
	}
	else {
		work* w = thread_->current_work_;
		while( w && !w->cutoff_ ) {
			w = w->parent_split_;
		}
		if( w ) {
			do_abort_ = true;
			return result::loss;
		}
	}

#if USE_STATISTICS
	thread_->pool_.stats_.node( ply );
#endif

	short ret;
	if( is_50move_draw( p, check, *this, ply, ret ) ) {
		return ret;
	}
	if( !p.bitboards[color::white][bb_type::pawns] && !p.bitboards[color::black][bb_type::pawns] ) {
		if( p.material[color::white].eg() + p.material[color::black].eg() <= eval_values::insufficient_material_threshold ) {
			return result::draw;
		}
	}

	bool const pv_node = alpha + 1 != beta;
	short const old_alpha = alpha;

	// Mate distance pruning
	beta = std::min( static_cast<short>(result::win - ply - 1), beta );
	alpha = std::max( static_cast<short>(result::loss + ply), alpha );
	if( alpha >= beta ) {
		return alpha;
	}

	// Transposition table lookup and cutoff
	move tt_move;
	score_type::type t;
	{
		short eval;
		t = tt_->lookup( p.hash_, depth, ply, alpha, beta, eval, tt_move, full_eval );
		if( t != score_type::none ) {
			if( t == score_type::exact || !pv_node ) {
				return eval;
			}
		}
	}

	int plies_remaining = (depth - cutoff) / DEPTH_FACTOR;

	if( !pv_node && !check.check /*&& plies_remaining < 4*/ && full_eval == result::win ) {
		full_eval = evaluate_full( *pawn_tt_, p );
		if( tt_move.empty() && t == score_type::none ) {
			tt_->store( p.hash_, 0, ply, 0, 0, 0, tt_move, clock, full_eval );
		}
	}

	if( !pv_node && !check.check && plies_remaining < static_cast<int>(sizeof(razor_pruning)/sizeof(short)) && full_eval + razor_pruning[plies_remaining] < beta &&
		   tt_move.empty() && beta < result::win_threshold && beta > result::loss_threshold &&
		   !(p.bitboards[p.self()][bb_type::pawns] & (p.white() ? 0x00ff000000000000ull : 0x000000000000ff00ull)) )
	{
		short new_beta = beta - razor_pruning[plies_remaining];
		short value = quiescence_search( ply, MAX_QDEPTH, p, check, new_beta - 1, new_beta, full_eval );
		if( value < new_beta ) {
			return value;
		}
	}

#if NULL_MOVE_REDUCTION > 0
	if( !pv_node && !check.check &&
		full_eval >= beta && !last_was_null &&
		beta > result::loss_threshold && beta < result::win_threshold &&
		depth >= (cutoff + DEPTH_FACTOR) && p.material[p.self()].mg() )
	{
		short new_depth = depth - (NULL_MOVE_REDUCTION + 1) * DEPTH_FACTOR;
		short value;

		{
			unsigned char old_enpassant = p.do_null_move();

			null_move_block seen_block( seen, p.hash_, ply );

			check_map new_check( p );
			value = -step( new_depth, ply + 1, p, new_check, -beta, -beta + 1, true );
			p.do_null_move( old_enpassant );
		}

		if( value >= beta ) {
			if( value >= result::win_threshold ) {
				value = beta;
			}

			if( depth > NULLMOVE_VERIFICATION_DEPTH ) {
				// Verification search.
				// Helps against zugzwang and some other strange issues
				short research_value = step( new_depth, ply, p, check, alpha, beta, true, full_eval, last_ply_was_capture );
				if( research_value >= beta ) {
					return value;
				}
			}
			else {
				return value;
			}
		}
	}
#endif

	if( tt_move.empty() && depth > ( DEPTH_FACTOR * 4 + cutoff) ) {

		step( depth - 2 * DEPTH_FACTOR, ply, p, check, alpha, beta, true, full_eval, last_ply_was_capture );

		short eval;
		tt_->lookup( p.hash_, depth, ply, alpha, beta, eval, tt_move, full_eval );
	}

	short best_value = result::loss;
	move best_move;

	unsigned int processed_moves = 0;
	
	move_generator gen( *this, ply, killers[p.self()], p, check );
	gen.hash_move = tt_move;

	move m;
	while( !(m = gen.next()).empty() ) {

		short value = inner_step( depth, ply, p, check
			, alpha, beta, full_eval, last_ply_was_capture, pv_node
			, m, processed_moves, gen.get_phase(), best_value );
		++processed_moves;

		if( value > best_value ) {
			best_value = value;

			if( value > alpha ) {
				best_move = m;

				if( value >= beta ) {		
					if( !p.get_captured_piece(m) ) {
						killers[p.self()].add_killer( m, ply );
						history_.record_cut( p, m, processed_moves );
					}
#if USE_STATISTICS >= 2
					thread_->pool_.stats_.add_cutoff( processed_moves );
#endif
					break;
				}
				alpha = value;
			}
		}
		history_.record( p, m );

#if MAX_THREADS > 1
		if( processed_moves == 1 && plies_remaining > 4 ) {
			split( depth, ply, p, check, alpha, beta, full_eval, last_ply_was_capture, pv_node, best_value, best_move, gen );
			if( do_abort_ ) {
				return result::loss;
			}
		}
#endif
	}

	if( !processed_moves ) {
		if( check.check ) {
			return result::loss + ply;
		}
		else {
			return result::draw;
		}
	}

	if( best_value == result::loss ) {
		best_value = old_alpha;
	}

	if( !do_abort_ ) {
		tt_->store( p.hash_, depth, ply, best_value, old_alpha, beta, best_move, clock, full_eval );
	}

	return best_value;
}

short calc_state::inner_step( int const depth, int const ply, position const& p, check_map const& check, short const alpha, short const beta
						  , short const full_eval, unsigned char const last_ply_was_capture
						  , bool const pv_node, move const& m, unsigned int const processed_moves, phases::type const phase, short const best_value )
{
	short value;

	pieces::type piece = p.get_piece( m );
	pieces::type captured_piece = p.get_captured_piece( m );

	position new_pos(p);
	apply_move( new_pos, m );

	if( seen.is_two_fold( new_pos.hash_, ply ) ) {
		value = result::draw;
	}
	else {

		seen.set( new_pos.hash_, ply );

		check_map new_check( new_pos );

		bool extended = false;

		int new_depth = depth - DEPTH_FACTOR;

		// Check extension
		if( new_check.check ) {
			new_depth += check_extension;
			extended = true;
		}

		bool dangerous_pawn_move = false;

		// Pawn push extension
		if( piece == pieces::pawn ) {
			// Pawn promoting or moving to 7th rank
			if( m.target() < 16 || m.target() >= 48 ) {
				dangerous_pawn_move = true;
				if( !extended && pv_node ) {
					new_depth += pawn_push_extension;
					extended = true;
				}
			}
			// Pushing a passed pawn
			else if( !(passed_pawns[p.self()][m.target()] & p.bitboards[p.other()][bb_type::pawns] ) ) {
				dangerous_pawn_move = true;
				if( !extended && pv_node ) {
					new_depth += pawn_push_extension;
					extended = true;
				}
			}
		}

		// Recapture extension
		if( !extended && pv_node && captured_piece != pieces::none && m.target() == last_ply_was_capture && see(p, m) >= 0 ) {
			new_depth += recapture_extension;
			extended = true;
		}
		unsigned char new_capture = 64;
		if( captured_piece != pieces::none ) {
			new_capture = m.target();
		}

		// Open question: What's the exact reason for always searching exactly the first move full width?
		// Why not always use PVS, or at least in those cases where root alpha isn't result::loss?
		// Why not use full width unless alpha > old_alpha?
		if( processed_moves || !pv_node ) {

#if USE_FUTILITY
			// Futility pruning
			if( !extended && !pv_node && phase == phases::noncapture && !check.check &&
				!dangerous_pawn_move &&
				( best_value == result::loss || best_value > result::loss_threshold ) )
			{
				int plies_remaining = (depth - cutoff) / DEPTH_FACTOR;
				if( plies_remaining < static_cast<int>(sizeof(futility_pruning)/sizeof(short))) {
					value = full_eval + futility_pruning[plies_remaining];
					if( value <= alpha ) {
						return result::none;
					}
				}

				if( new_depth < cutoff + DEPTH_FACTOR && see( p, m ) < 0 ) {
					return result::none;
				}
			}

#endif

			bool search_full;
			if( !extended && processed_moves >= (pv_node ? 5u : 3u) && phase >= phases::noncapture &&
				!check.check && depth >= lmr_min_depth )
			{
				int red = pv_node ? (DEPTH_FACTOR) : (DEPTH_FACTOR * 2);

				red += (processed_moves - (pv_node?3:3)) / 5;
				int lmr_depth = new_depth - red;
				value = -step(lmr_depth, ply + 1, new_pos, new_check, -alpha-1, -alpha, false );

				search_full = value > alpha;
			}
			else {
				search_full = true;
			}

			if( search_full ) {
				value = -step( new_depth, ply + 1, new_pos, new_check, -alpha-1, -alpha, false, result::win, new_capture );
			}
		}

		if( pv_node && (!processed_moves || (value > alpha && value < beta) ) ) {
			value = -step( new_depth, ply + 1, new_pos, new_check, -beta, -alpha, false, result::win, new_capture );
		}
	}

	return value;
}


void def_new_best_move_callback::on_new_best_move( unsigned int, position const& p, int depth, int selective_depth , int evaluation, uint64_t nodes, duration const& /*elapsed*/, move const* pv )
{
	ss_.str( std::string() );
	ss_ << "Best: " << std::setw(2) << depth << " "
		<< std::setw(2) << selective_depth << " "
		<< std::setw(7) << evaluation << " "
		<< std::setw(10) << nodes << " "
		<< std::setw(0) << pv_to_string( conf_, pv, p ) << std::endl;
	std::cerr << ss_.str();
}

class calc_manager::impl
{
public:
	impl( context& ctx )
		: ctx_(ctx)
		, do_abort_()
		, pool_(ctx, mtx_)
	{
	}

	~impl() {
	}

	int get_max_depth( int depth ) const
	{
		if( depth <= 0 ) {
			return ctx_.conf_.max_search_depth();
		}
		else {
			return std::min( depth, ctx_.conf_.max_search_depth() );
		}
	}


	void abort( scoped_lock& l )
	{
		do_abort_ = true;
		pool_.abort( l );
	}
	
	context& ctx_;

	mutex mtx_;
	condition cond_;

	bool do_abort_;

	thread_pool pool_;
};


calc_manager::calc_manager( context& ctx )
	: impl_( new impl(ctx) )
{
}


calc_manager::~calc_manager()
{
	delete impl_;
}


calc_result calc_manager::calc( position const& p, int max_depth, timestamp const& start, duration const& move_time_limit, duration const& deadline, int clock, seen_positions const& seen
		  , new_best_move_callback_base& new_best_cb
		  , std::set<move> const& searchmoves )
{
	ASSERT( move_time_limit.is_infinity() || move_time_limit <= deadline );

	max_depth = impl_->get_max_depth( max_depth );

	impl_->pool_.update_threads();
	impl_->pool_.reduce_histories();

	calc_result result;

	bool ponder = false;
	if( !move_time_limit.is_infinity() || !deadline.is_infinity() ) {
		if( !deadline.is_infinity() ) {
			if( move_time_limit.is_infinity() ) {
				dlog() << "Time limit is unset with a deadline of " << deadline.milliseconds() << " ms" << std::endl;
			}
			else {
				dlog() << "Time limit is " << move_time_limit.milliseconds() << " ms with deadline of " << deadline.milliseconds() << " ms" << std::endl;
			}
		}
		else {
			dlog() << "Time limit is " << move_time_limit.milliseconds() << " ms without deadline" << std::endl;
		}
	}
	else {
		ponder = true;
	}

	check_map check( p );

	move_info moves[200];
	move_info* pm = moves;

	calculate_moves<movegen_type::all>( p, pm, check );
	sort_moves( moves, pm, p );

	duration time_limit = move_time_limit.is_infinity() ? deadline : move_time_limit;

	// Go through them sorted by previous evaluation. This way, if on time pressure,
	// we can abort processing at high depths early if needed.
	sorted_moves sorted;

	short ev = result::none;

	if( searchmoves.find( move() ) == searchmoves.end() ) {
		move tt_move;
		short hash_eval = 0;
		short tmp = 0;
		impl_->ctx_.tt_.lookup( p.hash_, 0, 0, 0, 0, hash_eval, tt_move, tmp );
		for( move_info* it = moves; it != pm; ++it ) {
			move_data md;
			md.m = *it;
			md.pv[0] = md.m.m;

			if( !searchmoves.empty() && searchmoves.find(it->m) == searchmoves.end() ) {
				continue;
			}

			if( it->m == tt_move ) {
				ev = hash_eval;
				sorted.insert( sorted.begin(), md );
			}
			else {
				sorted.push_back( md );
			}
		}
	}

	if( sorted.empty() ) {
		if( check.check ) {
			if( p.white() ) {
				dlog() << "BLACK WINS" << std::endl;
				result.forecast = result::loss;
			}
			else {
				dlog() << std::endl << "WHITE WINS" << std::endl;
				result.forecast = result::win;
			}
			return result;
		}
		else {
			if( !p.white() ) {
				dlog() << "\n";
			}
			dlog() << "DRAW" << std::endl;
			result.forecast = result::draw;
			return result;
		}
	}

	if( ev == result::none ) {
		ev = evaluate_full( impl_->ctx_.pawn_tt_, p );
	}
	result.forecast = ev;
	result.best_move = sorted.front().m.m;

	new_best_cb.on_new_best_move( 1, p, 1, 1, ev, 0, duration(), sorted.front().pv );

	if( moves + 1 >= pm && !ponder ) {
		result.forecast = ev;
		return result;
	}

	int min_depth = 2;
	if( max_depth < min_depth ) {
		min_depth = max_depth;
	}

	scoped_lock l( impl_->mtx_ );

	master_worker_thread* master = impl_->pool_.master();
	master->init( p, sorted, clock, seen, &new_best_cb, start );

	for( int depth = min_depth; depth <= max_depth && !impl_->do_abort_; ++depth ) {

		master->process( l, depth );

		while( !master->idle() ) {

			if( !ponder ) {
				timestamp now;
				if( time_limit > now - start ) {
					master->calc_cond_.wait( l, start + time_limit - now );
				}

				now = timestamp();
				if( !impl_->do_abort_ && (now - start) > time_limit && !master->idle() ) {
					dlog() << "Triggering search abort due to time limit at depth " << depth << std::endl;
					impl_->abort( l );
					impl_->pool_.wait_for_idle( l );
					break;
				}
			}
			else {
				master->calc_cond_.wait( l );
			}
		}

		sorted_moves new_sorted = master->get_moves();

		if( new_sorted.begin()->m.m != sorted.begin()->m.m ) {
			// PV changed
			if( !ponder && !move_time_limit.is_infinity() && move_time_limit.seconds() >= 1 && depth > 4 && (timestamp() - start) >= (move_time_limit / 10) ) {
				duration extra = move_time_limit / 3;
				if( time_limit + extra > deadline ) {
					extra = deadline - time_limit;
				}
				if( extra.milliseconds() > 0 ) {
					result.used_extra_time += extra;
					time_limit += extra;
					dlog() << "PV changed, adding " << extra.milliseconds() << " ms extra search time." << std::endl;
				}
			}
		}
		sorted = new_sorted;

		push_pv_to_tt( impl_->ctx_.tt_, sorted[0].pv, p, clock );

		duration elapsed = timestamp() - start;
		if( !ponder && !move_time_limit.is_infinity() && elapsed > (time_limit * 3) / 5 && depth < max_depth && !impl_->do_abort_ ) {
			dlog() << "Not increasing depth due to time limit. Elapsed: " << elapsed.milliseconds() << " ms" << std::endl;
			break;
		}
	}

	impl_->pool_.wait_for_idle( l );

	{
		move_data const& best = sorted.front();
		move const* pv = best.pv;

		result.best_move = best.m.m;
		result.forecast = best.m.sort;
		result.ponder_move = pv[1];

#if USE_STATISTICS
		timestamp stop;
		impl_->pool_.stats_.print( impl_->ctx_, stop - start );
		impl_->pool_.stats_.accumulate( stop - start );
		impl_->pool_.stats_.reset( false );
#endif
	}

	return result;
}


void calc_manager::clear_abort()
{
	scoped_lock l( impl_->mtx_ );

	impl_->do_abort_ = false;
	impl_->pool_.clear_abort();
}


void calc_manager::abort()
{
	scoped_lock l( impl_->mtx_ );

	impl_->abort( l );
}

bool calc_manager::should_abort() const
{
	scoped_lock l( impl_->mtx_ );
	return impl_->do_abort_;
}

#if USE_STATISTICS
statistics& calc_manager::stats()
{
	return impl_->pool_.stats_;
}
#endif

void calc_manager::set_multipv( unsigned int multipv )
{
	scoped_lock l( impl_->mtx_ );
	if( multipv > 99 ) {
		multipv = 99;
	}
	impl_->pool_.master()->set_multipv( static_cast<std::size_t>(multipv) );
}
