
#ifndef OCTOCHESS_UCI_OCTOCHESS_IMPL_HEADER
#define OCTOCHESS_UCI_OCTOCHESS_IMPL_HEADER

#include "interface.hpp"

#include "../config.hpp"

#include <vector>

class context;

namespace octochess {
namespace uci {

class octochess_uci : engine_interface {
public:
	octochess_uci( context& ctx, gui_interface_ptr const& );
	//callbacks
	virtual void new_game() override;
	virtual void set_position( std::string const& fen ) override;
	virtual void make_moves( std::string const& list_of_moves ) override;
	virtual void calculate( timestamp const& start, calculate_mode_type, position_time const&, int depth, bool ponder, std::string const& searchmoves ) override;
	virtual void stop() override;
	virtual void quit() override;
	virtual bool is_move( std::string const& ms ) override;

	//generic info
	virtual std::string name() const;
	virtual std::string author() const { return "Tim Kosse"; }

	// options
	virtual uint64_t get_hash_size() const; // In MiB
	virtual uint64_t get_min_hash_size() const; // In MiB
	virtual void set_hash_size( uint64_t mb );
	virtual unsigned int get_threads() const;
	virtual unsigned int get_max_threads() const;
	virtual void set_threads( unsigned int threads );
	virtual bool use_book() const;
	virtual void use_book( bool use );
	virtual void set_multipv( unsigned int multipv );
	virtual void fischer_random( bool frc );

private:
	virtual void make_moves( std::vector<std::string> const& list_of_moves );
private:
	class impl;
	std::shared_ptr<impl> impl_;
};

}
}

#endif
