//===- DeserializationTripwire.cpp - Trailing-object read tripwire --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// DIAGNOSTIC INSTRUMENTATION -- NOT FOR UPSTREAM. See the header for what this
// is measuring, why the armed set is owned by the ASTContext, why arming
// happens in the reader rather than in CreateDeserialized, and why arms are
// counted alongside hits.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/DeserializationTripwire.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclOpenACC.h"
#include "clang/AST/DeclTemplate.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

using namespace clang;

/// Where every process appends its summary. A build driver discards stderr
/// from anything that passes, so the survey has to land somewhere durable, and
/// a fixed path means nothing has to be threaded through lit's environment
/// allow-list to reach the cc1 processes it spawns.
static const char *const TripwireLogPath = "/tmp/clang-tripwire.log";

namespace {

struct Site {
  const char *Name;
  unsigned long long Armed;
  unsigned long long Hits;
};

Site Sites[tripwire::NumSites] = {
    {"ImplicitConceptSpecializationDecl::Args", 0, 0},
    {"NonTypeTemplateParmDecl::ExpansionTypes", 0, 0},
    {"NonTypeTemplateParmDecl::PlaceholderConstraint", 0, 0},
    {"TemplateTemplateParmDecl::ExpansionParams", 0, 0},
    {"TemplateTypeParmDecl::TypeConstraint", 0, 0},
    {"FriendTemplateDecl::TemplateParameterLists", 0, 0},
    {"ExplicitInstantiationDecl::QualifierLoc", 0, 0},
    {"ExplicitInstantiationDecl::ArgsAsWritten", 0, 0},
    {"CXXConstructorDecl::InheritedConstructor", 0, 0},
    {"CXXConstructorDecl::ExplicitSpecifier", 0, 0},
    {"UsingPackDecl::Expansions", 0, 0},
    {"DecompositionDecl::Bindings", 0, 0},
    {"PragmaCommentDecl::Arg", 0, 0},
    {"PragmaDetectMismatchDecl::NameValue", 0, 0},
    {"OutlinedFunctionDecl::Params", 0, 0},
    {"CapturedDecl::Params", 0, 0},
    {"ImportDecl::IdentifierLocs", 0, 0},
    {"OpenACCConstructDecl::Clauses", 0, 0},
};

bool SummaryRegistered = false;

void writeSummary() {
  llvm::SmallString<1024> Buf;
  llvm::raw_svector_ostream OS(Buf);
  OS << "\n=== deserialization tripwire (pid " << int(::getpid()) << ") ===\n";
  for (const Site &S : Sites)
    if (S.Armed != 0)
      OS << "  armed " << S.Armed << "\thit " << S.Hits << "\t" << S.Name
         << "\n";

  int FD = ::open(TripwireLogPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (FD < 0) {
    llvm::errs() << Buf;
    return;
  }
  // One write for the whole record: O_APPEND stops concurrent writers from
  // overwriting each other, but does not keep a record built from several
  // writes contiguous, and a parallel build has many cc1 processes exiting at
  // once.
  const char *P = Buf.data();
  size_t N = Buf.size();
  while (N != 0) {
    ssize_t Written = ::write(FD, P, N);
    if (Written <= 0)
      break;
    P += Written;
    N -= size_t(Written);
  }
  ::close(FD);
}

} // namespace

unsigned clang::tripwire::NumArmedHint = 0;

void clang::tripwire::arm(const ASTContext &C, const Decl *D, unsigned Slot,
                          SiteID ID) {
  if (!C.TripwireArmedSlots.insert({D, Slot}).second)
    return;
  ++NumArmedHint;
  ++Sites[ID].Armed;
  if (!SummaryRegistered) {
    SummaryRegistered = true;
    std::atexit(writeSummary);
  }
}

void clang::tripwire::armForDeserialization(const ASTContext &C,
                                            const Decl *D) {
  // A chain rather than a switch on getKind(): every class here is a leaf, so
  // each test is one integer compare, and this reads as the inventory it is.
  //
  // Every predicate below must be answerable before ASTDeclReader::Visit runs.
  // These all read bits assigned inside CreateDeserialized. Anything derived
  // from the decl's type is not -- see NTTP_PlaceholderConstraint, armed from
  // VisitNonTypeTemplateParmDecl instead.
  if (isa<ImplicitConceptSpecializationDecl>(D)) {
    arm(C, D, 0, ICS_Args);
  } else if (const auto *NTTP = dyn_cast<NonTypeTemplateParmDecl>(D)) {
    if (NTTP->isExpandedParameterPack())
      arm(C, D, 0, NTTP_ExpansionTypes);
  } else if (const auto *TTP = dyn_cast<TemplateTemplateParmDecl>(D)) {
    if (TTP->isExpandedParameterPack())
      arm(C, D, 0, TTP_ExpansionParams);
  } else if (const auto *TTPD = dyn_cast<TemplateTypeParmDecl>(D)) {
    if (TTPD->hasTypeConstraint())
      arm(C, D, 0, TemplateTypeParm_TypeConstraint);
  } else if (isa<FriendTemplateDecl>(D)) {
    // Its EmptyShell constructor asserts a non-zero list count, so a
    // deserialized FriendTemplateDecl always has trailing storage.
    arm(C, D, 0, FriendTemplate_ParameterLists);
  } else if (const auto *EID = dyn_cast<ExplicitInstantiationDecl>(D)) {
    if (EID->hasTrailingQualifier())
      arm(C, D, 0, ExplicitInstantiation_QualifierLoc);
    if (EID->hasTrailingArgsAsWritten())
      arm(C, D, 1, ExplicitInstantiation_ArgsAsWritten);
  } else if (const auto *CD = dyn_cast<CXXConstructorDecl>(D)) {
    if (CD->isInheritingConstructor())
      arm(C, D, 0, CXXConstructor_InheritedConstructor);
    if (CD->hasTrailingExplicitSpecifier())
      arm(C, D, 1, CXXConstructor_ExplicitSpecifier);
  } else if (isa<UsingPackDecl>(D)) {
    arm(C, D, 0, UsingPack_Expansions);
  } else if (isa<DecompositionDecl>(D)) {
    arm(C, D, 0, Decomposition_Bindings);
  } else if (isa<PragmaCommentDecl>(D)) {
    arm(C, D, 0, PragmaComment_Arg);
  } else if (isa<PragmaDetectMismatchDecl>(D)) {
    arm(C, D, 0, PragmaDetectMismatch_NameValue);
  } else if (isa<OutlinedFunctionDecl>(D)) {
    arm(C, D, 0, OutlinedFunction_Params);
  } else if (isa<CapturedDecl>(D)) {
    arm(C, D, 0, Captured_Params);
  } else if (isa<ImportDecl>(D)) {
    arm(C, D, 0, Import_IdentifierLocs);
  } else if (isa<OpenACCDeclareDecl>(D) || isa<OpenACCRoutineDecl>(D)) {
    arm(C, D, 0, OpenACC_Clauses);
  }
}

void clang::tripwire::disarm(const ASTContext &C, const Decl *D,
                             unsigned Slot) {
  if (C.TripwireArmedSlots.erase({D, Slot}))
    --NumArmedHint;
}

bool clang::tripwire::isArmed(const Decl *D, unsigned Slot) {
  // Only the reader arms, and only after assigning a DeclContext, so a decl
  // without one cannot be armed. Checking is not merely defensive: decls built
  // by calling CreateDeserialized directly -- as the unit tests do -- reach
  // instrumented accessors with no context to ask.
  if (!D->getDeclContext())
    return false;
  return D->getASTContext().TripwireArmedSlots.contains({D, Slot});
}

void clang::tripwire::hit(SiteID ID) { ++Sites[ID].Hits; }
