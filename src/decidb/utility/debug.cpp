#include "duckdb/decidb/utility/debug.hpp"

#include <cassert>
#include "fmt/printf.h"

namespace decidb{
const char* RED = "\033[31m";
const char* GREEN = "\033[32m";
const char* RESET = "\033[0m";

Profiler::Profiler(){
}

void Profiler::clock(const std::string& label){
    timePoints[label] = std::chrono::high_resolution_clock::now();
}

void Profiler::stop(const std::string& label){
    if (timePoints.count(label)){
        auto now = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(now - timePoints[label]);
        if (!clocks.count(label)) clocks[label] = {0.0, 0};
        clocks[label] = clocks[label] + std::make_pair(duration.count(), 1);
    }
}

void Profiler::add(const Profiler& pro){
    for (const auto& cl : pro.clocks){
        auto label = cl.first;
        if (!clocks.count(label)) clocks[label] = {0.0, 0};
        clocks[label] = clocks[label] + cl.second;
    }
}

void Profiler::print() const{
    // Profiler output disabled for production
    // Uncomment the following lines to enable debug profiling
    /*
    for (const auto& cl : clocks){
        auto label = cl.first;
        if (!label.size()) label = "Ø";
        duckdb_fmt::printf("{}[count={} avg={}ms]\n", label, cl.second.second, cl.second.first/cl.second.second);
    }
    */
}

}
