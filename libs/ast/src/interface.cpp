#include "ember/ast/interface.hpp"

#include <algorithm>

namespace ember::ast {
namespace {

/// The text an item occupies, exactly as it was written.
std::string_view text_of(const SourceFile& source, Span span) { return source.text_of(span); }

/// A function without its body: everything from `fn` up to the `{`,
/// with a `;` in place of the block.
///
/// Trailing whitespace goes too, so `fn f() -> int ;` does not come out
/// with the gap the block used to sit behind.
std::string signature_of(const SourceFile& source, const FunctionDecl& function) {
    const Span head{function.span.start, function.body.span.start, function.span.file};
    std::string text{text_of(source, head)};

    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' ||
                             text.back() == '\r')) {
        text.pop_back();
    }
    return text + ";";
}

/// Whether a generic's body has to travel with it.
///
/// It does, always: monomorphization generates a copy at each use, and
/// there is nothing to copy without the body.
bool needs_body(const FunctionDecl& function) { return function.is_generic(); }

/// Every named type a type expression mentions, however deeply.
void named_types_of(const TypeRef* type, std::vector<const TypeRef*>& into) {
    if (type == nullptr) {
        return;
    }
    if (type->kind == TypeKind::Named) {
        into.push_back(type);
    }
    named_types_of(type->element.get(), into);
    for (const TypeRefPtr& argument : type->type_args) {
        named_types_of(argument.get(), into);
    }
    for (const TypeRefPtr& parameter : type->params) {
        named_types_of(parameter.get(), into);
    }
    named_types_of(type->result.get(), into);
}

/// Every named type in a function's signature.
std::vector<const TypeRef*> signature_types(const FunctionDecl& function) {
    std::vector<const TypeRef*> types;
    for (const Param& param : function.params) {
        named_types_of(param.type.get(), types);
    }
    named_types_of(function.return_type.get(), types);
    return types;
}

}  // namespace

InterfaceResult write_interface(const Program& program, const SourceFile& source) {
    InterfaceResult result;

    // What a caller will be able to name. A `pub` signature mentioning
    // anything else is a promise the caller cannot read: legal inside
    // the module, meaningless outside it. Caught here, where it can be
    // said once and pointed at, rather than as a pile of unknown-name
    // errors from re-checking what was written.
    std::vector<std::string> exported;
    std::vector<std::string> generic_params;
    for (const ItemPtr& item : program.items) {
        if (const auto* structure = node_cast<StructDecl>(item.get())) {
            if (structure->is_public) {
                exported.push_back(structure->name);
            }
        }
    }

    const auto is_reachable = [&](const TypeRef& type) {
        if (!type.module.empty()) {
            return true;  // another module's, and its own business
        }
        // `Vec` and `String` are the compiler's, and reach everywhere.
        // They parse as ordinary named types, so they have to be named
        // here or every signature using one looks like a leak.
        if (type.name == "Vec" || type.name == "String") {
            return true;
        }
        return std::find(exported.begin(), exported.end(), type.name) != exported.end() ||
               std::find(generic_params.begin(), generic_params.end(), type.name) !=
                   generic_params.end();
    };

    const auto check_signature = [&](const FunctionDecl& function, const std::string& what) {
        generic_params.clear();
        for (const GenericParam& parameter : function.generic_params) {
            generic_params.push_back(parameter.name);
        }
        for (const TypeRef* type : signature_types(function)) {
            if (is_reachable(*type)) {
                continue;
            }
            result.diagnostics.push_back(
                Diagnostic::error("`" + what + "` cannot be part of an interface", type->span,
                                  "`" + type->name + "` is not `pub`")
                    .with_note("a caller outside this module cannot name `" + type->name +
                               "`, so it could not call `" + what + "` even with the "
                               "declaration in front of it")
                    .with_note("make `" + type->name + "` public, or keep `" + what +
                               "` private"));
        }
    };

    for (const ItemPtr& item : program.items) {
        // An `impl` block carries no `pub` of its own - its methods do -
        // so it is looked at before the visibility gate rather than
        // after it.
        if (const auto* block = node_cast<ImplBlock>(item.get())) {
            for (const std::unique_ptr<FunctionDecl>& method : block->methods) {
                if (method->is_public) {
                    check_signature(*method, block->type_name + "::" + method->name);
                }
            }
            continue;
        }
        if (!item->is_public) {
            continue;
        }
        if (const auto* function = node_cast<FunctionDecl>(item.get())) {
            check_signature(*function, function->name);
        }
    }
    if (!result.diagnostics.empty()) {
        return result;
    }

    std::string out =
        "// Interface for `" +
        (program.module.empty() ? std::string{"this program"} : program.module) +
        "`, written by ember.\n"
        "//\n"
        "// The public surface of the module, with the implementations taken\n"
        "// out. A generic keeps its body, because monomorphizing one needs it.\n";

    bool anything = false;
    for (const ItemPtr& item : program.items) {
        if (const auto* block = node_cast<ImplBlock>(item.get())) {
            // An impl block is rebuilt around whichever of its methods
            // are public, since the block itself carries no `pub`.
            std::string methods;
            for (const std::unique_ptr<FunctionDecl>& method : block->methods) {
                if (!method->is_public) {
                    continue;
                }
                methods += "    ";
                methods += needs_body(*method) ? std::string{text_of(source, method->span)}
                                               : signature_of(source, *method);
                methods += "\n";
            }
            if (!methods.empty()) {
                out += "\nimpl " + block->type_name + " {\n" + methods + "}\n";
                anything = true;
            }
            continue;
        }

        if (!item->is_public) {
            continue;
        }

        if (const auto* function = node_cast<FunctionDecl>(item.get())) {
            out += "\n";
            out += needs_body(*function) ? std::string{text_of(source, function->span)}
                                         : signature_of(source, *function);
            out += "\n";
            anything = true;
            continue;
        }

        // A struct is all interface: its fields decide its layout, and
        // its layout is what a caller has to agree with. A `const` is
        // its value, which a caller may fold. Both go out whole.
        out += "\n";
        out += text_of(source, item->span);
        out += "\n";
        anything = true;
    }

    if (!anything) {
        result.diagnostics.push_back(Diagnostic::error(
            "this module has no public interface", Span{0, 0, source.id()},
            "nothing in it is declared `pub`"));
        return result;
    }

    result.contents = std::move(out);
    return result;
}

}  // namespace ember::ast
