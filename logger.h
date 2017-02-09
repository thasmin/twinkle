#pragma once

#include <iostream>
#include <set>
#include <sstream>
#include <string>

class Logger {
private:
	std::ostringstream oss;
	Logger();

public:
	Logger(Logger const&)          = delete;
	void operator=(Logger const&)  = delete;

	static std::set<std::string> categories;
	static void addCategory(const std::string& category);

	static std::ostream& get(const std::string& category);
};
