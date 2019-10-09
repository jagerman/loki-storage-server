#pragma once

#include <boost/program_options.hpp>
#include <string>

namespace loki {

struct command_line_options {
    uint16_t port;
    uint16_t lokid_rpc_port = 22023;
    bool force_start = false;
    bool print_version = false;
    bool print_help = false;
    bool testnet = false;
    std::string ip;
    std::string log_level = "info";
    std::string lokid_key_path;
    std::string data_dir;
};

class command_line_parser {
  public:
    void parse_args(int argc, char* argv[]);
    bool early_exit() const;

    const command_line_options& get_options() const;
    void print_usage() const;

  private:
    boost::program_options::options_description desc_;
    command_line_options options_;
    std::string binary_name_;
};

} // namespace loki
