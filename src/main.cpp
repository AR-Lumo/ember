// Entry point for the `soliton` binary.
//
// Phase 0: the command line is parsed for real; every subcommand then
// reports that its pipeline stage is not implemented yet. Phases 1-5
// fill these in.

#include "cli.hpp"

#include "soliton/codegen/codegen.hpp"

#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

int run_command(const soliton::cli::Command& command) {
    using soliton::cli::CommandKind;

    switch (command.kind) {
        case CommandKind::Help:
            std::cout << soliton::cli::kUsage << '\n';
            return soliton::cli::kExitSuccess;

        case CommandKind::Version:
            std::cout << soliton::cli::version_string() << '\n';
            return soliton::cli::kExitSuccess;

        case CommandKind::Build: {
            const std::filesystem::path output =
                command.output.value_or(soliton::cli::default_output_path(command.input));
            return soliton::cli::build_file(command.input, output, command.build,
                                          command.module_path, command.update);
        }

        case CommandKind::Run:
            return soliton::cli::run_file(command.input, command.build, command.module_path,
                                        command.update);

        case CommandKind::Check:
            return soliton::cli::check_file(command.input, command.module_path,
                                          command.update);

        case CommandKind::Fetch:
            return soliton::cli::fetch_packages(std::filesystem::current_path(),
                                              command.update);

        case CommandKind::Interface:
            return soliton::cli::write_interface(command.input, command.output,
                                               command.module_path);

        case CommandKind::Publish:
            return soliton::cli::publish_package(std::filesystem::current_path(),
                                               command.dry_run);
    }

    return soliton::cli::kExitSuccess;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 0) {
        soliton::cli::set_compiler_path(argv[0]);
    }

    std::vector<std::string_view> args;
    args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    const soliton::cli::ParseResult parsed = soliton::cli::parse_args(args);

    if (const auto* error = std::get_if<soliton::cli::UsageError>(&parsed)) {
        std::cerr << "error: " << error->message << "\n\n" << soliton::cli::kUsage << '\n';
        return soliton::cli::kExitUsage;
    }

    return run_command(std::get<soliton::cli::Command>(parsed));
}
