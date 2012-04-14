
#include "minimalistic_uci_protocol.hpp"
#include "info.hpp"
#include "time_calculation.hpp"

#include "../logger.hpp"
#include "../string.hpp"

#include <cassert>
#include <iostream>
#include <sstream>

namespace octochess {
namespace uci {

minimalistic_uci_protocol::minimalistic_uci_protocol()
	: connected_(false)
{}

void minimalistic_uci_protocol::thread_entry_point( bool already_got_init ) {
	if( already_got_init ) {
		init();
	}

	std::string line;
	while( std::getline(std::cin, line) ) {
		logger::log_input(line);
		if( connected_ ) {
			parse_command( line );
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
	std::cout << "option name Hash type spin default " << callbacks_->get_hash_size() << " min " << callbacks_->get_min_hash_size() << " max 1048576" << std::endl;
	std::cout << "option name Threads type spin default " << callbacks_->get_threads() << " min 1 max " << callbacks_->get_max_threads() << std::endl;
}


void minimalistic_uci_protocol::handle_option( std::string const& args )
{
	std::stringstream ss;
	ss.flags(std::stringstream::skipws);
	ss.str( args );

	std::string tokName, name, tokValue;
	uint64_t value;

	ss >> tokName >> name >> tokValue >> value;

	if( !ss || tokName != "name" || tokValue != "value" ) {
		std::cerr << "malformed setoption: " << args << std::endl;
	}

	if( name == "Hash" ) {
		callbacks_->set_hash_size( value );
	}
	if( name == "Threads" ) {
		callbacks_->set_threads( value );
	}
	else {
		std::cerr << "Unknown option: " << args << std::endl;
	}
}


void minimalistic_uci_protocol::parse_command( std::string const& line ) {
	std::string args;
	std::string cmd = split( line, args );

	if( cmd == "isready" ) {
		std::cout << "readyok" << std::endl;
	} else if( cmd == "quit" ) {
		callbacks_->quit();
		connected_ = false;
	} else if( cmd == "stop" ) {
		callbacks_->stop();
	} else if( cmd == "ucinewgame" ) {
		callbacks_->new_game();
	} else if( cmd == "position" ) {
		handle_position( args );
	} else if( cmd == "go" ) {
		handle_go( args );
	} else if( cmd == "go" ) {
		handle_go( args );
	} else if( cmd == "setoption" ) {
		handle_option( args );
	} else {
		std::cerr << "unknown command when connected: " << line << std::endl;
	}
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

void minimalistic_uci_protocol::handle_go( std::string const& params ) {
	calculate_mode_type mode = calculate_mode::forced;

	if( params.find("infinite") != std::string::npos ) {
		mode = calculate_mode::infinite;
	}
	position_time t;

	std::istringstream in( params );
	std::string cmd;
	while( in >> cmd ) {
		if( cmd == "wtime" ) {
			t.set_white_time( duration::milliseconds(extract<uint>(in)) );
		} else if( cmd == "btime" ) {
			t.set_black_time( duration::milliseconds(extract<uint>(in)) );
		} else if( cmd == "winc" ) {
			t.set_white_increment( duration::milliseconds(extract<uint>(in)) );
		} else if( cmd == "winc" ) {
			t.set_black_increment( duration::milliseconds(extract<uint>(in)) );
		}
	}

	callbacks_->calculate( mode, t );
}

void minimalistic_uci_protocol::set_engine_interface( engine_interface& p ) {
	callbacks_ = &p;
}

void minimalistic_uci_protocol::identify( std::string const& name, std::string const& author ) {
	std::cout << "id name " << name << '\n' << "id author " << author << std::endl;
}

void minimalistic_uci_protocol::tell_best_move( std::string const& move ) {
	std::cout << "bestmove " << move << std::endl;
}

void minimalistic_uci_protocol::tell_info( info const& i ) {
	std::cout << "info ";
	for( info::const_iterator it = i.begin(); it != i.end(); ++it ) {
		std::cout << it->first << ' ' << it->second << ' ';
	}
	std::cout << std::endl;
}

}
}

