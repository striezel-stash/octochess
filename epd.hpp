#ifndef OCTOCHESS_EPD_HEADER
#define OCTOCHESS_EPD_HEADER

#if DEVELOPMENT
#include <map>
#include <string>
#include <vector>

class context;

struct epd {
	position p;
	move best_move;
	std::map<std::string, std::string> operations;
};

std::vector<epd> parse_epd( config const& conf, std::string const& fn );
std::vector<epd> parse_epd( config const& conf, std::istream& in );

// Runs the Strategic Test Suite from https://sites.google.com/site/strategictestsuite/
void run_sts( context& ctx );

#endif

#endif