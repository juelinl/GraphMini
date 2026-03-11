//
// Created by ubuntu on 1/2/23.
//

#include "common.h"
#include "preprocess/graph_converter.h"
#include <cxxopts.hpp>
#include <iostream>
#include <sstream>

namespace {
std::string build_prep_help(const cxxopts::Options &) {
    std::ostringstream out;
    out << "Preprocess a SNAP graph into GraphMini's binary format.\n\n";
    out << "Usage:\n";
    out << "  ./build/bin/prep --path_to_graph=<dir>\n\n";
    out << "Required Options:\n";
    out << "  --path_to_graph   Path to a directory containing snap.txt.\n\n";
    out << "Optional Options:\n";
    out << "  --help            Show this help text.\n\n";
    out << "Positional Compatibility:\n";
    out << "  You may also pass <path_to_graph> as the first positional argument.\n\n";
    out << "Examples:\n";
    out << "  ./build/bin/prep --path_to_graph=./dataset/wiki\n";
    out << "  ./build/bin/prep ./dataset/wiki\n";
    return out.str();
}
} // namespace

int main(int argc, char * argv[]){
    using namespace minigraph;
    cxxopts::Options options("prep", "Preprocess a SNAP graph for GraphMini.");
    options.positional_help("<path_to_graph>");
    options.add_options()
            ("path_to_graph", "Path to a directory containing snap.txt", cxxopts::value<std::string>())
            ("help", "Show help");
    options.parse_positional({"path_to_graph"});

    cxxopts::ParseResult parsed;
    try {
        parsed = options.parse(argc, argv);
    } catch (const cxxopts::exceptions::exception &e) {
        std::cerr << "Error: " << e.what() << "\n\n" << build_prep_help(options) << std::endl;
        return 1;
    }

    if (parsed.count("help")) {
        std::cout << build_prep_help(options) << std::endl;
        return 0;
    }
    if (parsed.count("path_to_graph") == 0) {
        std::cerr << "Missing required option: --path_to_graph\n\n"
                  << build_prep_help(options) << std::endl;
        return 1;
    }

    minigraph::GraphConverter converter;
    std::filesystem::path in_dir{ parsed["path_to_graph"].as<std::string>() };
    converter.convert(in_dir);
}
