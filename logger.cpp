#include "logger.h"

Logger::Logger() { }

std::set<std::string> Logger::categories;
void Logger::addCategory(const std::string& category)
{
	Logger::categories.insert(category);
}

class null_buffer : public std::streambuf
{
public:
  int overflow(int c) { return c; }
};

null_buffer nullbuffer;
std::ostream nullout(&nullbuffer);

std::ostream& Logger::get(const std::string& category)
{
	if (categories.find("") != categories.end())
		return std::cerr;
	if (categories.find(category) != categories.end())
		return std::cerr;
	return nullout;
}
