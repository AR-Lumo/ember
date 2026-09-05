// Entry point for the `ember` binary.
//
// Phase 0: the command line is parsed for real; every subcommand then
// reports that its pipeline stage is not implemented yet. Phases 1-5
// fill these in.

#include "cli.hpp"

#include "ember/codegen/codegen.hpp"

#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

int run_command(const ember::cli::Command& command) {
    using ember::cli::CommandKind;

    switch (command.kind) {
        case CommandKind::Help:
            std::cout << ember::cli::kUsage << '\n';
            return ember::cli::kExitSuccess;

        case CommandKind::Version:
            std::cout << ember::cli::version_string() << '\n';
            return ember::cli::kExitSuccess;

        case CommandKind::Build: {
            const std::filesystem::path output =
                command.output.value_or(ember::cli::default_output_path(command.input));
            return ember::cli::build_file(command.input, output, command.build,
                                          command.module_path, command.update);
        }

        case CommandKind::Run:
            return ember::cli::run_file(command.input, command.build, command.module_path,
                                        command.update);

        case CommandKind::Check:
            return ember::cli::check_file(command.input, command.module_path,
                                          command.update);

        case CommandKind::Fetch:
            return ember::cli::fetch_packages(std::filesystem::current_path(),
                                              command.update);

        case CommandKind::Interface:
            return ember::cli::write_interface(command.input, command.output,
                                               command.module_path);

        case CommandKind::Publish:
            return ember::cli::publish_package(std::filesystem::current_path(),
                                               command.dry_run);
    }

    return ember::cli::kExitSuccess;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 0) {
        ember::cli::set_compiler_path(argv[0]);
    }

    std::vector<std::string_view> args;
    args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    const ember::cli::ParseResult parsed = ember::cli::parse_args(args);

    if (const auto* error = std::get_if<ember::cli::UsageError>(&parsed)) {
        std::cerr << "error: " << error->message << "\n\n" << ember::cli::kUsage << '\n';
        return ember::cli::kExitUsage;
    }

    return run_command(std::get<ember::cli::Command>(parsed));
}
