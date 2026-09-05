// ember::ast - interface files.
//
// An interface is a module's public surface with the implementations
// taken out: what a program needs in order to *use* a module, without
// needing the module's source to build against.
//
//     // textkit.emi
//     pub struct Point { pub x: int, pub y: int, }
//     pub fn shout(text: string) -> String;
//     pub fn largest<T>(a: T, b: T) -> T {
//         if a > b {
//             return a;
//         }
//         return b;
//     }
//
// Two things are worth explaining about that.
//
// **The generic keeps its body.** Ember monomorphizes, so a copy of
// `largest<int>` has to be generated wherever it is first used, and
// generating it needs the body. That is the same bargain C++ strikes by
// putting templates in headers, and it has the same consequence: a
// generic's implementation is part of its interface, and changing it
// changes what everyone compiles.
//
// **The interface is cut out of the source rather than printed from the
// tree.** Every item already knows the span it came from, so a
// signature is the text from the start of the item to the start of its
// body, and a generic is the text of the whole thing. Nothing is
// re-rendered, so nothing can be rendered *wrong* - the output is the
// author's own Ember, and it parses because it already did.

#ifndef EMBER_AST_INTERFACE_HPP
#define EMBER_AST_INTERFACE_HPP

#include "ember/ast/diagnostic.hpp"
#include "ember/ast/nodes.hpp"
#include "ember/ast/span.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ember::ast {

/// The extension an interface file carries.
inline constexpr std::string_view kInterfaceExtension = "emi";

struct InterfaceResult {
    /// The interface, as Ember source.
    std::string contents;
    std::vector<Diagnostic> diagnostics;

    bool ok() const noexcept { return diagnostics.empty(); }
};

/// Render the public interface of `program`, which must have been parsed
/// from `source`.
///
/// Only `pub` items appear. A module with nothing public produces an
/// interface with nothing in it, which is a valid module that offers
/// nothing - and is worth noticing, so it is reported.
InterfaceResult write_interface(const Program& program, const SourceFile& source);

}  // namespace ember::ast

#endif  // EMBER_AST_INTERFACE_HPP
