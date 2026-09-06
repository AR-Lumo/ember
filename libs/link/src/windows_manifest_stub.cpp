/// A manifest merger that reports it is unavailable.
///
/// lld's COFF driver references `llvm::windows_manifest` unconditionally
/// - the calls are behind a run-time check, but the symbols are not, so
/// they have to resolve. LLVM's own implementation of them lives in
/// LLVMWindowsManifest, which links libxml2.
///
/// That library cannot be linked statically here. LLVM was compiled
/// against libxml2's *shared* headers, so its references are to
/// `__imp_xmlReadMemory` and friends - the DLL import thunks - which a
/// static libxml2.a does not define. Linking the DLL instead drags in
/// libxml2-16.dll, and behind it libiconv-2.dll and zlib1.dll, which
/// puts back exactly the dependency on an MSYS2 prefix that carrying our
/// own linker was meant to remove.
///
/// So: the same thing LLVM itself does when built with
/// LLVM_ENABLE_LIBXML2=OFF. `isAvailable()` answers false and the merge
/// path is never entered. Cinder links executables through lld's MinGW
/// driver, which does not merge manifests - the default manifest comes
/// in as `default-manifest.o` on the link line, an ordinary object.
#include "llvm/WindowsManifest/WindowsManifestMerger.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <system_error>

namespace llvm {
namespace windows_manifest {

char WindowsManifestError::ID = 0;

WindowsManifestError::WindowsManifestError(const Twine& Msg) : Msg(Msg.str()) {}

void WindowsManifestError::log(raw_ostream& OS) const { OS << Msg; }

// Declared in the header as a private nested type and never defined
// there; the unique_ptr member needs it complete to be destroyed.
class WindowsManifestMerger::WindowsManifestMergerImpl {};

bool isAvailable() { return false; }

WindowsManifestMerger::WindowsManifestMerger() = default;

WindowsManifestMerger::~WindowsManifestMerger() = default;

Error WindowsManifestMerger::merge(MemoryBufferRef) {
    return createStringError(std::errc::function_not_supported,
                             "this build has no XML support, so manifests cannot be merged");
}

std::unique_ptr<MemoryBuffer> WindowsManifestMerger::getMergedManifest() { return nullptr; }

}  // namespace windows_manifest
}  // namespace llvm
