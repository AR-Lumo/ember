// soliton::parser - token stream -> AST.
//
// Recursive descent for items and statements, Pratt parsing for
// expressions against the §3 precedence table:
//
//     ||  ->  &&  ->  == !=  ->  < > <= >=  ->  + -  ->  * / %
//        ->  unary - !  ->  call/method/index/field
//
// All binary operators are left-associative; the unary operators bind
// tighter than every binary one and looser than any postfix.
//
// Two places where the parser goes beyond the published grammar, both
// deliberate and both flagged in the Phase 2 notes:
//
//   * `array_literal = "[" [ expression { "," expression } ] "]"` is
//     added to `expression`. §3 has array types `[T; N]` and index
//     expressions but no way to build an array, so `[T; N]` values would
//     be uninhabited and §5's `len(arr)` would have nothing to consume.
//     The form follows Rust (§9: ergonomics).
//
//   * Struct literals are not parsed in the condition of an `if` or
//     `while`, because `if x { }` would otherwise be ambiguous between
//     "if x, then block" and "if the struct literal x{}". This is the
//     same restriction Rust imposes, and for the same reason; wrap the
//     literal in parentheses to use one there.

#ifndef SOLITON_PARSER_PARSER_HPP
#define SOLITON_PARSER_PARSER_HPP

#include "soliton/ast/diagnostic.hpp"
#include "soliton/ast/nodes.hpp"
#include "soliton/ast/span.hpp"
#include "soliton/lexer/token.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace soliton::parser {

/// Which build phase has implemented this stage so far.
inline constexpr int kImplementedPhase = 2;

/// Name of this pipeline stage, used in driver messages.
std::string_view stage_name() noexcept;

struct ParseResult {
    /// Never null. On failure it holds whatever items were recovered,
    /// so later stages can still report against a partial tree.
    std::unique_ptr<ast::Program> program;
    std::vector<ast::Diagnostic> diagnostics;

    bool ok() const noexcept { return diagnostics.empty(); }
};

/// Parse a token stream. `source` is used only to build diagnostics.
ParseResult parse(const std::vector<lexer::Token>& tokens, const ast::SourceFile& source);

/// Lex and parse in one step, merging diagnostics from both stages. When
/// lexing fails the parser is not run: a bad token stream produces
/// cascading nonsense errors that bury the real ones.
ParseResult parse_source(const ast::SourceFile& source);

/// One file of a multi-file program.
struct Module {
    /// As other files name it in an `import`. Empty for the entry
    /// module, which nothing can import.
    std::string name;
    std::filesystem::path path;
    ast::FileId file = ast::kMainFile;
    std::unique_ptr<ast::Program> program;
    /// The modules this one imported, in source order.
    std::vector<std::string> imports;
    /// True when this was loaded from a `.sni` rather than a `.sn`: a
    /// description of a module rather than the module. There is nothing
    /// in it to compile, and whoever supplied it has to supply the
    /// object too.
    bool is_interface = false;

    bool is_entry() const noexcept { return name.empty(); }
};

struct LoadResult {
    /// The entry module first, then every module reachable from it.
    std::vector<Module> modules;
    std::vector<ast::Diagnostic> diagnostics;

    bool ok() const noexcept { return diagnostics.empty(); }
};

/// Where an imported module may be found, beyond the directory of the
/// file that imported it. Searched in order.
using ModulePath = std::vector<std::filesystem::path>;

/// Read, lex and parse `entry` and everything it imports, transitively.
///
/// A module named `geometry` is looked for first as `geometry.sn` beside
/// the file that imported it, so a program's own modules always win: a
/// vendored package can never quietly take over a name already in use.
/// Then each directory of `search` is tried two ways - `geometry.sn` for
/// a module that is one file, and `geometry/geometry.sn` for one that
/// ships as a directory. The second form is what lets a package be more
/// than a single file, since its own private modules are then siblings
/// and resolve by the first rule.
///
/// Each module is loaded once however many modules import it, so a cycle
/// between two modules terminates and is legal: names are resolved after
/// every module is parsed, so neither has to come first. Two different
/// files claiming the same module name is an error rather than a race.
/// `entry_name` names the entry file's own module. It is empty for a
/// program, whose entry module nothing can import and whose symbols are
/// therefore unprefixed. Compiling a *library* has to pass its name,
/// because a library's symbols are the ones its consumers will link
/// against - and those carry the module prefix.
LoadResult load_program(const std::filesystem::path& entry, ast::SourceMap& sources,
                        const ModulePath& search = {}, const std::string& entry_name = {});

}  // namespace soliton::parser

#endif  // SOLITON_PARSER_PARSER_HPP
