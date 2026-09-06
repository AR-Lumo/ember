// Entry point for the `cinder` binary.
//
// Phase 0: the command line is parsed for real; every subcommand then
// reports that its pipeline stage is not implemented yet. Phases 1-5
// fill these in.

#include "cli.hpp"

#include "cinder/codegen/codegen.hpp"

#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

int run_command(const cinder::cli::Command& command) {
    using cinder::cli::CommandKind;

    switch (command.kind) {
        case CommandKind::Help:
            std::cout << cinder::cli::kUsage << '\n';
            return cinder::cli::kExitSuccess;

        case CommandKind::Version:
            std::cout << cinder::cli::version_string() << '\n';
            return cinder::cli::kExitSuccess;

        case CommandKind::Build: {
            const std::filesystem::path output =
                command.output.value_or(cinder::cli::default_output_path(command.input));
            return cinder::cli::build_file(command.input, output, command.build,
                                          command.module_path, command.update);
        }

        case CommandKind::Run:
            return cinder::cli::run_file(command.input, command.build, command.module_path,
                                        command.update);

        case CommandKind::Check:
            return cinder::cli::check_file(command.input, command.module_path,
                                          command.update);

        case CommandKind::Fetch:
            return cinder::cli::fetch_packages(std::filesystem::current_path(),
                                              command.update);

        case CommandKind::Interface:
            return cinder::cli::write_interface(command.input, command.output,
                                               command.module_path);

        case CommandKind::Publish:
            return cinder::cli::publish_package(std::filesystem::current_path(),
                                               command.dry_run);
    }

    return cinder::cli::kExitSuccess;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 0) {
        cinder::cli::set_compiler_path(argv[0]);
    }

    std::vector<std::string_view> args;
    args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    const cinder::cli::ParseResult parsed = cinder::cli::parse_args(args);

    if (const auto* error = std::get_if<cinder::cli::UsageError>(&parsed)) {
        std::cerr << "error: " << error->message << "\n\n" << cinder::cli::kUsage << '\n';
        return cinder::cli::kExitUsage;
    }

    return run_command(std::get<cinder::cli::Command>(parsed));
}
