
#include "minimalistic_uci_protocol.hpp"
#include "info.hpp"
#include "uci_time_calculation.hpp"

#include "../util/logger.hpp"
#include "../util/string.hpp"

#include <cassert>
#include <iostream>
#include <sstream>

namespace octochess {
namespace uci {

minimalistic_uci_protocol::minimalistic_uci_protocol()
	: callbacks_()
	, connected_(false)
{}

void minimalistic_uci_protocol::thread_entry_point( bool already_got_init ) {
	if( already_got_init ) {
		init();
	}

	std::string line;
	bool quit = false;
	while( !quit && std::getline(std::cin, line) ) {
		logger::log_input(line);
		if( connected_ ) {
			quit = parse_command( line );
		} else {
			parse_init( line );
		}
	}
}

void minimalistic_uci_protocol::parse_init( std::string const& line ) {
	if( line == "uci" ) {
		init();
	} else {
		std::cerr << "unknown command when not connected: " << line << std::endl;
	}
}

void minimalistic_uci_protocol::init()
{
	assert( callbacks_ && "callbacks not set" );
	identify( callbacks_->name(), callbacks_->author() );
	send_options();
	std::cout << "uciok" << std::endl;
	connected_ = true;
}

void minimalistic_uci_protocol::send_options()
{
	std::cout << "option name Hash type spin default " << callbacks_->get_hash_size() << " min " << callbacks_->get_min_hash_size() << " max 1048576" << "\n";
	std::cout << "option name Threads type spin default " << callbacks_->get_threads() << " min 1 max " << callbacks_->get_max_threads() << "\n";
	std::cout << "option name OwnBook type check default " << (callbacks_->use_book() ? "true" : "false") << "\n";
	std::cout << "option name Ponder type check default true\n";
	std::cout << "option name MultiPV type spin default 1 min 1 max 99\n";
	std::cout << "option name UCI_Chess960 type check default false\n";
	std::cout.flush();
}


void minimalistic_uci_protocol::handle_option( std::string const& args )
{
	std::stringstream ss;
	ss.flags(std::stringstream::skipws);
	ss.str( args );

	std::string tokName, name, tokValue, value;

	ss >> tokName >> name >> tokValue >> value;

	if( !ss || tokName != "name" || tokValue != "value" ) {
		std::cerr << "malformed setoption: " << args << std::endl;
	}

	if( name == "Hash" ) {
		uint64_t memory;
		if( !to_int<uint64_t>( value, memory, callbacks_->get_min_hash_size(), 1048576 ) ) {
			std::cerr << "malformed setoption: " << args << std::endl;
		}
		else {
			callbacks_->set_hash_size( memory );
		}
	}
	else if( name == "Threads" ) {
		int threads;
		if( !to_int<int>( value, threads, 1, callbacks_->get_max_threads() ) ) {
			std::cerr << "malformed setoption: " << args << std::endl;
		}
		else {
			callbacks_->set_threads( threads );
		}
	}
	else if( name == "OwnBook" ) {
		bool use_book;
		if( !to_bool( value, use_book ) ) {
			std::cerr << "malformed setoption: " << args << std::endl;
		}
		else {
			callbacks_->use_book( use_book );
		}
	}
	else if( name == "MultiPV" ) {
		unsigned int v;
		if( !to_int<unsigned int>( value, v, 1, 99 ) ) {
			std::cerr << "malformed setoption: " << args << std::endl;
		}
		else {
			callbacks_->set_multipv( v );
		}
	}
	else if( name == "Ponder" ) {
		bool use_book;
		if( !to_bool( value, use_book ) ) {
			std::cerr << "malformed setoption: " << args << std::endl;
		}
		else {
			// NOP, the GUI will send "go ponder" anyhow
		}
	}
	else if( name == "UCI_Chess960" ) {
		bool frc;
		if( !to_bool(value, frc) ) {
			std::cerr << "malformed setoption: " << args << std::endl;
		}
		else {
			callbacks_->fischer_random(frc);
		}
	}
	else {
		std::cerr << "Unknown option: " << args << std::endl;
	}
}


bool minimalistic_uci_protocol::parse_command( std::string const& line ) {

	bool quit = false;

	std::string args;
	std::string cmd = split( line, args );

	if( cmd == "isready" ) {
		std::cout << "readyok" << std::endl;
	}
	else if( cmd == "quit" ) {
		callbacks_->quit();
		connected_ = false;
		quit = true;
	}
	else if( cmd == "stop" ) {
		callbacks_->stop();
	}
	else if( cmd == "ucinewgame" ) {
		callbacks_->new_game();
	}
	else if( cmd == "position" ) {
		handle_position( args );
	}
	else if( cmd == "go" ) {
		handle_go( args );
	}
	else if( cmd == "ponderhit" ) {
		handle_ponderhit();
	}
	else if( cmd == "setoption" ) {
		handle_option( args );
	}
	else {
		std::cerr << "unknown command when connected: " << line << std::endl;
	}

	return quit;
}

void minimalistic_uci_protocol::handle_position( std::string const& params ) {
	std::string::size_type pos = params.find("moves "); //there should be always this string

	if( params.substr(0,3) == "fen" ) {
		callbacks_->set_position( params.substr(4, pos) );
	} else if( params.substr(0,8) == "startpos" ) {
		callbacks_->set_position( std::string() );
	} else {
		std::cerr << "unknown parameter with position command: " << params << std::endl;
	}

	if( pos != std::string::npos ) {
		callbacks_->make_moves( params.substr( pos+6 ) );
	}
}

namespace {
	template<typename T>
	T extract( std::istream& in ) {
		T v = T();
		in >> v;
		return v;
	}
}

void minimalistic_uci_protocol::handle_go( std::string const& params )
{
	// Create at early as possible
	timestamp start;

	calculate_mode::type mode = calculate_mode::forced;

	position_time t;

	int depth = -1;
	bool ponder = false;

	std::istringstream in( params );
	std::string cmd;
	std::string searchmoves;

	while( in >> cmd ) {
loop:
		if( cmd == "wtime" ) {
			t.set_time( 0, duration::milliseconds(extract<uint>(in)) );
		} else if( cmd == "btime" ) {
			t.set_time( 1, duration::milliseconds(extract<uint>(in)) );
		} else if( cmd == "winc" ) {
			t.set_increment( 0, duration::milliseconds(extract<uint>(in)) );
		} else if( cmd == "binc" ) {
			t.set_increment( 1, duration::milliseconds(extract<uint>(in)) );
		} else if( cmd == "movetime" ) {
			t.set_movetime( duration::milliseconds(extract<uint>(in)) );
		} else if( cmd == "movestogo" ) {
			t.set_moves_to_go( extract<uint>(in) );
		} else if( cmd == "depth" ) {
			depth = extract<int>(in);
		} else if( cmd == "ponder" ) {
			ponder = true;
		} else if( cmd == "infinite" ) {
			mode = calculate_mode::infinite;
		}
		else if( cmd == "searchmoves" ) {
			while( in >> cmd ) {
				if( !callbacks_->is_move( cmd ) ) {
					break;
				}
				searchmoves += cmd + " ";
			}

			if( in ) {
				goto loop;
			}
		}
	}

	callbacks_->calculate( start, mode, t, depth, ponder, searchmoves );
}


void minimalistic_uci_protocol::handle_ponderhit()
{
	callbacks_->calculate( timestamp(), calculate_mode::ponderhit, position_time(), -1, false, std::string() );
}


void minimalistic_uci_protocol::set_engine_interface( engine_interface& p ) {
	callbacks_ = &p;
}

void minimalistic_uci_protocol::identify( std::string const& name, std::string const& author ) {
	std::cout << "id name " << name << '\n' << "id author " << author << std::endl;
}

void minimalistic_uci_protocol::tell_best_move( std::string const& move, std::string const& ponder ) {
	std::cout << "bestmove " << move;
	if( !ponder.empty() ) {
		std::cout << " ponder " << ponder;
	}
	std::cout << std::endl;
}

void minimalistic_uci_protocol::tell_info( info const& i ) {
	if( !i.empty() ) {
		std::cout << "info";
		for( info::const_iterator it = i.begin(); it != i.end(); ++it ) {
			std::cout << ' ' << it->first << ' ' << it->second;
		}
		std::cout << std::endl;
	}
}

}
}

