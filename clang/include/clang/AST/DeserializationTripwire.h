//===- DeserializationTripwire.h - Trailing-object read tripwire *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// DIAGNOSTIC INSTRUMENTATION -- NOT FOR UPSTREAM.
///
/// ASTReader::ReadDeclRecord registers a decl in DeclsLoaded before
/// ASTDeclReader visits it, so a reentrant GetDecl for the same ID returns a
/// partially-built decl rather than recursing forever. Decls with trailing
/// object arrays are therefore observable in a window where the array has been
/// sized but not yet written. Reading it there is undefined if the storage is
/// raw, and merely wrong if the storage was value-initialized: a profile taken
/// in the window hashes placeholders instead of the real contents, so anything
/// uniqued from that profile is filed under a key it will never produce again.
///
/// This header records which trailing arrays are currently inside that window,
/// counts reads that land in one, and appends a summary per process to
/// /tmp/clang-tripwire.log. Applying the patch is the switch; there is nothing
/// to configure and nothing to turn on.
///
/// Every site counts arms as well as hits. Without the arm count a zero is
/// ambiguous -- it could mean "never read inside the window" or "never armed,
/// so nothing was watched" -- and those are opposite conclusions. "armed 8412,
/// hit 0" is a real negative result; "armed 0, hit 0" means the instrument was
/// not looking.
///
/// The armed set lives on the ASTContext, not in the decls and not in a global.
/// Decls are bump-allocated from the context, so a decl address is only unique
/// within its own arena: once a context is destroyed, a later context can put a
/// different decl at the same address. A global set keyed by raw pointer would
/// therefore let a slot left armed by a dead context report against an
/// unrelated decl. Per-context state also keeps AST node layouts
/// byte-identical to an uninstrumented build, which matters because the
/// behaviour under investigation is sensitive to bump-allocator layout.
///
/// Arming happens in ReadDeclRecord, after the decl's DeclContext is set and
/// before ASTDeclReader::Visit runs -- not in CreateDeserialized. A decl fresh
/// out of CreateDeserialized has no DeclContext, so isArmed() could not reach
/// its context; and CreateDeserialized is called directly by unit tests, whose
/// decls never enter a deserialization window and must not be armed, since
/// nothing would ever disarm them. Only slots whose presence is settled by
/// CreateDeserialized can be armed there; anything derived from state that
/// Visit fills in is not yet answerable, so it is armed from the reader.
///
/// Nothing aborts and nothing prints during the run, so one build surveys a
/// whole compilation. For a stack trace at a firing site, put an lldb
/// breakpoint on clang::tripwire::hit; what this detects is deterministic
/// control flow, so a debugger cannot perturb it, unlike the original crash.
///
/// Counters are plain integers and the armed set is unsynchronised: cc1 builds
/// one AST at a time on one thread. Do not point this at clangd.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_AST_DESERIALIZATIONTRIPWIRE_H
#define LLVM_CLANG_AST_DESERIALIZATIONTRIPWIRE_H

#include "llvm/Support/Compiler.h"

namespace clang {

class ASTContext;
class Decl;

namespace tripwire {

/// One entry per instrumented trailing array. Nineteen arrays across sixteen
/// decl classes share eighteen entries: OpenACCDeclareDecl and
/// OpenACCRoutineDecl are read through the same accessor on their common base.
enum SiteID {
  ICS_Args,
  NTTP_ExpansionTypes,
  NTTP_PlaceholderConstraint,
  TTP_ExpansionParams,
  TemplateTypeParm_TypeConstraint,
  FriendTemplate_ParameterLists,
  ExplicitInstantiation_QualifierLoc,
  ExplicitInstantiation_ArgsAsWritten,
  CXXConstructor_InheritedConstructor,
  CXXConstructor_ExplicitSpecifier,
  UsingPack_Expansions,
  Decomposition_Bindings,
  PragmaComment_Arg,
  PragmaDetectMismatch_NameValue,
  OutlinedFunction_Params,
  Captured_Params,
  Import_IdentifierLocs,
  OpenACC_Clauses,
  NumSites
};

/// Number of slots armed across all contexts. Read on every instrumented
/// access, so the common case -- no AST read in flight -- costs one load and a
/// predictable branch. A non-zero reading only means some context has an open
/// window; isArmed() then consults the decl's own context.
extern unsigned NumArmedHint;

/// Arm whichever trailing arrays \p D has, if any. Called once per decl from
/// ReadDeclRecord, between the DeclContext assignment and Visit.
void armForDeserialization(const ASTContext &C, const Decl *D);

/// Mark (\p D, \p Slot) as sized but not yet written, crediting \p ID.
void arm(const ASTContext &C, const Decl *D, unsigned Slot, SiteID ID);

/// Mark (\p D, \p Slot) as written. Safe to call when not armed. \p Slot is 0
/// or 1: it rides in a spare low bit of the decl pointer, and PointerIntPair
/// asserts if it does not fit.
void disarm(const ASTContext &C, const Decl *D, unsigned Slot);

/// Whether (\p D, \p Slot) is inside its deserialization window. Recovers the
/// context from \p D. A decl with no DeclContext was never armed -- only the
/// reader arms, and only after setting one -- so this answers false for those
/// rather than asking for a context they do not have.
bool isArmed(const Decl *D, unsigned Slot);

/// Count a read that landed in the window. Breakpoint here for a stack trace.
void hit(SiteID ID);

} // namespace tripwire
} // namespace clang

/// Count a read of \p Ptr's trailing array \p Slot if it is still armed.
/// \p Ptr must be a Decl. \p ID names the site.
#define CLANG_TRIPWIRE(Ptr, Slot, ID)                                          \
  do {                                                                         \
    if (LLVM_UNLIKELY(::clang::tripwire::NumArmedHint != 0) &&                 \
        ::clang::tripwire::isArmed((Ptr), (Slot)))                             \
      ::clang::tripwire::hit(::clang::tripwire::ID);                           \
  } while (false)

#endif // LLVM_CLANG_AST_DESERIALIZATIONTRIPWIRE_H
