//===- unittests/AST/DeclTest.cpp --- Declaration tests -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Unit tests for Decl nodes in the AST.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/Decl.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ExprConcepts.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/TypeBase.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Basic/ABI.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/LLVM.h"
#include "clang/Basic/Linkage.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Testing/Annotations/Annotations.h"
#include "gtest/gtest.h"
#include <cassert>
#include <memory>
#include <string>

using namespace clang::ast_matchers;
using namespace clang::tooling;
using namespace clang;

TEST(Decl, CleansUpAPValues) {
  MatchFinder Finder;
  std::unique_ptr<FrontendActionFactory> Factory(
      newFrontendActionFactory(&Finder));

  // This is a regression test for a memory leak in APValues for structs that
  // allocate memory. This test only fails if run under valgrind with full leak
  // checking enabled.
  std::vector<std::string> Args(1, "-std=c++11");
  Args.push_back("-fno-ms-extensions");
  ASSERT_TRUE(runToolOnCodeWithArgs(
      Factory->create(),
      "struct X { int a; }; constexpr X x = { 42 };"
      "union Y { constexpr Y(int a) : a(a) {} int a; }; constexpr Y y = { 42 };"
      "constexpr int z[2] = { 42, 43 };"
      "constexpr int __attribute__((vector_size(16))) v1 = {};"
      "\n#ifdef __SIZEOF_INT128__\n"
      "constexpr __uint128_t large_int = 0xffffffffffffffff;"
      "constexpr __uint128_t small_int = 1;"
      "\n#endif\n"
      "constexpr double d1 = 42.42;"
      "constexpr long double d2 = 42.42;"
      "constexpr _Complex long double c1 = 42.0i;"
      "constexpr _Complex long double c2 = 42.0;"
      "template<int N> struct A : A<N-1> {};"
      "template<> struct A<0> { int n; }; A<50> a;"
      "constexpr int &r = a.n;"
      "constexpr int A<50>::*p = &A<50>::n;"
      "void f() { foo: bar: constexpr int k = __builtin_constant_p(0) ?"
      "                         (char*)&&foo - (char*)&&bar : 0; }",
      Args));

  // FIXME: Once this test starts breaking we can test APValue::needsCleanup
  // for ComplexInt.
  ASSERT_FALSE(runToolOnCodeWithArgs(
      Factory->create(),
      "constexpr _Complex __uint128_t c = 0xffffffffffffffff;",
      Args));
}

TEST(Decl, AsmLabelAttr) {
  // Create two method decls: `f` and `g`.
  StringRef Code = R"(
    struct S {
      void f() {}
    };
  )";
  auto AST =
      tooling::buildASTFromCodeWithArgs(Code, {"-target", "i386-apple-darwin"});
  ASTContext &Ctx = AST->getASTContext();
  assert(Ctx.getTargetInfo().getUserLabelPrefix() == StringRef("_") &&
         "Expected target to have a global prefix");
  DiagnosticsEngine &Diags = AST->getDiagnostics();

  const auto *DeclS =
      selectFirst<CXXRecordDecl>("d", match(cxxRecordDecl().bind("d"), Ctx));
  NamedDecl *DeclF = *DeclS->method_begin();

  DeclF->addAttr(AsmLabelAttr::Create(Ctx, "foo"));

  // Mangle the decl names.
  std::string MangleF;
  std::unique_ptr<ItaniumMangleContext> MC(
      ItaniumMangleContext::create(Ctx, Diags));
  {
    llvm::raw_string_ostream OS_F(MangleF);
    MC->mangleName(DeclF, OS_F);
  }

  ASSERT_EQ(MangleF, "\x01"
                     "foo");
}

TEST(Decl, AsmLabelAttr_LLDB) {
  StringRef Code = R"(
    struct S {
      void f() {}
      S() = default;
      ~S() = default;
    };
  )";
  auto AST =
      tooling::buildASTFromCodeWithArgs(Code, {"-target", "i386-apple-darwin"});
  ASTContext &Ctx = AST->getASTContext();
  assert(Ctx.getTargetInfo().getUserLabelPrefix() == StringRef("_") &&
         "Expected target to have a global prefix");
  DiagnosticsEngine &Diags = AST->getDiagnostics();

  const auto *DeclS =
      selectFirst<CXXRecordDecl>("d", match(cxxRecordDecl().bind("d"), Ctx));

  auto *DeclF = *DeclS->method_begin();
  auto *Ctor = *DeclS->ctor_begin();
  auto *Dtor = DeclS->getDestructor();

  ASSERT_TRUE(DeclF);
  ASSERT_TRUE(Ctor);
  ASSERT_TRUE(Dtor);

  DeclF->addAttr(AsmLabelAttr::Create(Ctx, "$__lldb_func::123:123:_Z1fv"));
  Ctor->addAttr(AsmLabelAttr::Create(Ctx, "$__lldb_func::123:123:S"));
  Dtor->addAttr(AsmLabelAttr::Create(Ctx, "$__lldb_func::123:123:~S"));

  std::unique_ptr<ItaniumMangleContext> MC(
      ItaniumMangleContext::create(Ctx, Diags));

  {
    std::string Mangled;
    llvm::raw_string_ostream OS_Mangled(Mangled);
    MC->mangleName(DeclF, OS_Mangled);

    ASSERT_EQ(Mangled, "\x01$__lldb_func::123:123:_Z1fv");
  };

  {
    std::string Mangled;
    llvm::raw_string_ostream OS_Mangled(Mangled);
    MC->mangleName(GlobalDecl(Ctor, CXXCtorType::Ctor_Complete), OS_Mangled);

    ASSERT_EQ(Mangled, "\x01$__lldb_func:C0:123:123:S");
  };

  {
    std::string Mangled;
    llvm::raw_string_ostream OS_Mangled(Mangled);
    MC->mangleName(GlobalDecl(Ctor, CXXCtorType::Ctor_Base), OS_Mangled);

    ASSERT_EQ(Mangled, "\x01$__lldb_func:C1:123:123:S");
  };

  {
    std::string Mangled;
    llvm::raw_string_ostream OS_Mangled(Mangled);
    MC->mangleName(GlobalDecl(Dtor, CXXDtorType::Dtor_Deleting), OS_Mangled);

    ASSERT_EQ(Mangled, "\x01$__lldb_func:D0:123:123:~S");
  };

  {
    std::string Mangled;
    llvm::raw_string_ostream OS_Mangled(Mangled);
    MC->mangleName(GlobalDecl(Dtor, CXXDtorType::Dtor_Base), OS_Mangled);

    ASSERT_EQ(Mangled, "\x01$__lldb_func:D2:123:123:~S");
  };
}

TEST(Decl, AsmLabelAttr_LLDB_Inherit) {
  StringRef Code = R"(
    struct Base {
      Base(int x) {}
    };

    struct Derived : Base {
      using Base::Base;
    } d(5);
  )";
  auto AST =
      tooling::buildASTFromCodeWithArgs(Code, {"-target", "i386-apple-darwin"});
  ASTContext &Ctx = AST->getASTContext();
  assert(Ctx.getTargetInfo().getUserLabelPrefix() == StringRef("_") &&
         "Expected target to have a global prefix");
  DiagnosticsEngine &Diags = AST->getDiagnostics();

  const auto *Ctor = selectFirst<CXXConstructorDecl>(
      "ctor",
      match(cxxConstructorDecl(isInheritingConstructor()).bind("ctor"), Ctx));

  const_cast<CXXConstructorDecl *>(Ctor)->addAttr(
      AsmLabelAttr::Create(Ctx, "$__lldb_func::123:123:Derived"));

  std::unique_ptr<ItaniumMangleContext> MC(
      ItaniumMangleContext::create(Ctx, Diags));

  {
    std::string Mangled;
    llvm::raw_string_ostream OS_Mangled(Mangled);
    MC->mangleName(GlobalDecl(Ctor, CXXCtorType::Ctor_Complete), OS_Mangled);

    ASSERT_EQ(Mangled, "\x01$__lldb_func:CI0:123:123:Derived");
  };

  {
    std::string Mangled;
    llvm::raw_string_ostream OS_Mangled(Mangled);
    MC->mangleName(GlobalDecl(Ctor, CXXCtorType::Ctor_Base), OS_Mangled);

    ASSERT_EQ(Mangled, "\x01$__lldb_func:CI1:123:123:Derived");
  };
}

TEST(Decl, MangleDependentSizedArray) {
  StringRef Code = R"(
    template <int ...N>
    int A[] = {N...};

    template <typename T, int N>
    struct S {
      T B[N];
    };
  )";
  auto AST =
      tooling::buildASTFromCodeWithArgs(Code, {"-target", "i386-apple-darwin"});
  ASTContext &Ctx = AST->getASTContext();
  assert(Ctx.getTargetInfo().getUserLabelPrefix() == StringRef("_") &&
         "Expected target to have a global prefix");
  DiagnosticsEngine &Diags = AST->getDiagnostics();

  const auto *DeclA =
      selectFirst<VarDecl>("A", match(varDecl().bind("A"), Ctx));
  const auto *DeclB =
      selectFirst<FieldDecl>("B", match(fieldDecl().bind("B"), Ctx));

  std::string MangleA, MangleB;
  llvm::raw_string_ostream OS_A(MangleA), OS_B(MangleB);
  std::unique_ptr<ItaniumMangleContext> MC(
      ItaniumMangleContext::create(Ctx, Diags));

  MC->mangleCanonicalTypeName(DeclA->getType(), OS_A);
  MC->mangleCanonicalTypeName(DeclB->getType(), OS_B);

  ASSERT_EQ(MangleA, "_ZTSA_i");
  ASSERT_EQ(MangleB, "_ZTSAT0__T_");
}

TEST(Decl, ConceptDecl) {
  llvm::StringRef Code(R"(
    template<class T>
    concept integral = __is_integral(T);
  )");

  auto AST = tooling::buildASTFromCodeWithArgs(Code, {"-std=c++20"});
  ASTContext &Ctx = AST->getASTContext();

  const auto *Decl =
      selectFirst<ConceptDecl>("decl", match(conceptDecl().bind("decl"), Ctx));
  ASSERT_TRUE(Decl != nullptr);
  EXPECT_EQ(Decl->getName(), "integral");
}

TEST(Decl, EnumDeclRange) {
  llvm::Annotations Code(R"(
    typedef int Foo;
    [[enum Bar : Foo]];)");
  auto AST = tooling::buildASTFromCodeWithArgs(Code.code(), /*Args=*/{});
  ASTContext &Ctx = AST->getASTContext();
  const auto &SM = Ctx.getSourceManager();

  const auto *Bar =
      selectFirst<TagDecl>("Bar", match(enumDecl().bind("Bar"), Ctx));
  auto BarRange =
      Lexer::getAsCharRange(Bar->getSourceRange(), SM, Ctx.getLangOpts());
  EXPECT_EQ(SM.getFileOffset(BarRange.getBegin()), Code.range().Begin);
  EXPECT_EQ(SM.getFileOffset(BarRange.getEnd()), Code.range().End);
}

TEST(Decl, IsInExportDeclContext) {
  llvm::Annotations Code(R"(
    export module m;
    export template <class T>
    void f() {})");
  auto AST =
      tooling::buildASTFromCodeWithArgs(Code.code(), /*Args=*/{"-std=c++20"});
  ASTContext &Ctx = AST->getASTContext();

  const auto *f =
      selectFirst<FunctionDecl>("f", match(functionDecl().bind("f"), Ctx));
  EXPECT_TRUE(f->isInExportDeclContext());
}

TEST(Decl, InConsistLinkageForTemplates) {
  llvm::Annotations Code(R"(
    export module m;
    export template <class T>
    void f() {}

    template <>
    void f<int>() {}

    export template <class T>
    class C {};

    template<>
    class C<int> {};
    )");

  auto AST =
      tooling::buildASTFromCodeWithArgs(Code.code(), /*Args=*/{"-std=c++20"});
  ASTContext &Ctx = AST->getASTContext();

  llvm::SmallVector<ast_matchers::BoundNodes, 2> Funcs =
      match(functionDecl().bind("f"), Ctx);

  EXPECT_EQ(Funcs.size(), 2U);
  const FunctionDecl *TemplateF = Funcs[0].getNodeAs<FunctionDecl>("f");
  const FunctionDecl *SpecializedF = Funcs[1].getNodeAs<FunctionDecl>("f");
  EXPECT_EQ(TemplateF->getLinkageInternal(),
            SpecializedF->getLinkageInternal());

  llvm::SmallVector<ast_matchers::BoundNodes, 1> ClassTemplates =
      match(classTemplateDecl().bind("C"), Ctx);
  llvm::SmallVector<ast_matchers::BoundNodes, 1> ClassSpecializations =
      match(classTemplateSpecializationDecl().bind("C"), Ctx);

  EXPECT_EQ(ClassTemplates.size(), 1U);
  EXPECT_EQ(ClassSpecializations.size(), 1U);
  const NamedDecl *TemplatedC = ClassTemplates[0].getNodeAs<NamedDecl>("C");
  const NamedDecl *SpecializedC = ClassSpecializations[0].getNodeAs<NamedDecl>("C");
  EXPECT_EQ(TemplatedC->getLinkageInternal(),
            SpecializedC->getLinkageInternal());
}

TEST(Decl, ModuleAndInternalLinkage) {
  llvm::Annotations Code(R"(
    export module M;
    static int a;
    static int f(int x);

    int b;
    int g(int x);)");

  auto AST =
      tooling::buildASTFromCodeWithArgs(Code.code(), /*Args=*/{"-std=c++20"});
  ASTContext &Ctx = AST->getASTContext();

  const auto *a =
      selectFirst<VarDecl>("a", match(varDecl(hasName("a")).bind("a"), Ctx));
  const auto *f = selectFirst<FunctionDecl>(
      "f", match(functionDecl(hasName("f")).bind("f"), Ctx));

  EXPECT_EQ(a->getFormalLinkage(), Linkage::Internal);
  EXPECT_EQ(f->getFormalLinkage(), Linkage::Internal);

  const auto *b =
      selectFirst<VarDecl>("b", match(varDecl(hasName("b")).bind("b"), Ctx));
  const auto *g = selectFirst<FunctionDecl>(
      "g", match(functionDecl(hasName("g")).bind("g"), Ctx));

  EXPECT_EQ(b->getFormalLinkage(), Linkage::Module);
  EXPECT_EQ(g->getFormalLinkage(), Linkage::Module);
}

TEST(Decl, GetNonTransparentDeclContext) {
  llvm::Annotations Code(R"(
    export module m3;
    export template <class> struct X {
      template <class Self> friend void f(Self &&self) {
        (Self&)self;
      }
    };)");

  auto AST =
      tooling::buildASTFromCodeWithArgs(Code.code(), /*Args=*/{"-std=c++20"});
  ASTContext &Ctx = AST->getASTContext();

  auto *f = selectFirst<FunctionDecl>(
      "f", match(functionDecl(hasName("f")).bind("f"), Ctx));

  EXPECT_TRUE(f->getNonTransparentDeclContext()->isFileContext());
}

TEST(Decl, MemberFunctionInModules) {
  llvm::Annotations Code(R"(
    module;
    class G {
      void bar() {}
    };
    export module M;
    class A {
      void foo() {}
    };
    )");

  auto AST =
      tooling::buildASTFromCodeWithArgs(Code.code(), /*Args=*/{"-std=c++20"});
  ASTContext &Ctx = AST->getASTContext();

  auto *foo = selectFirst<FunctionDecl>(
      "foo", match(functionDecl(hasName("foo")).bind("foo"), Ctx));

  // The function defined within a class definition is not implicitly inline
  // if it is not attached to global module
  EXPECT_FALSE(foo->isInlined());

  auto *bar = selectFirst<FunctionDecl>(
      "bar", match(functionDecl(hasName("bar")).bind("bar"), Ctx));

  // In global module, the function defined within a class definition is
  // implicitly inline.
  EXPECT_TRUE(bar->isInlined());
}

TEST(Decl, MemberFunctionInHeaderUnit) {
  llvm::Annotations Code(R"(
    class foo {
    public:
      int memFn() {
        return 43;
      }
    };
    )");

  auto AST = tooling::buildASTFromCodeWithArgs(
      Code.code(), {"-std=c++20", " -xc++-user-header ", "-emit-header-unit"});
  ASTContext &Ctx = AST->getASTContext();

  auto *memFn = selectFirst<FunctionDecl>(
      "memFn", match(functionDecl(hasName("memFn")).bind("memFn"), Ctx));

  EXPECT_TRUE(memFn->isInlined());
}

TEST(Decl, FriendFunctionWithinClassInHeaderUnit) {
  llvm::Annotations Code(R"(
    class foo {
      int value;
    public:
      foo(int v) : value(v) {}

      friend int getFooValue(foo f) {
        return f.value;
      }
    };
    )");

  auto AST = tooling::buildASTFromCodeWithArgs(
      Code.code(), {"-std=c++20", " -xc++-user-header ", "-emit-header-unit"});
  ASTContext &Ctx = AST->getASTContext();

  auto *getFooValue = selectFirst<FunctionDecl>(
      "getFooValue",
      match(functionDecl(hasName("getFooValue")).bind("getFooValue"), Ctx));

  EXPECT_TRUE(getFooValue->isInlined());
}

TEST(Decl, FunctionDeclBitsShouldNotOverlapWithCXXConstructorDeclBits) {
  llvm::Annotations Code(R"(
    struct A {
      A() : m() {}
      int m;
    };

    A f() { return A(); }
    )");

  auto AST = tooling::buildASTFromCodeWithArgs(Code.code(), {"-std=c++14"});
  ASTContext &Ctx = AST->getASTContext();

  auto HasCtorInit =
      hasAnyConstructorInitializer(cxxCtorInitializer(isMemberInitializer()));
  auto ImpMoveCtor =
      cxxConstructorDecl(isMoveConstructor(), isImplicit(), HasCtorInit)
          .bind("MoveCtor");

  auto *ToImpMoveCtor =
      selectFirst<CXXConstructorDecl>("MoveCtor", match(ImpMoveCtor, Ctx));

  EXPECT_TRUE(ToImpMoveCtor->getNumCtorInitializers() == 1);
  EXPECT_FALSE(ToImpMoveCtor->FriendConstraintRefersToEnclosingTemplate());
}

TEST(Decl, NoProtoFunctionDeclAttributes) {
  llvm::Annotations Code(R"(
    void f();
    )");

  auto AST = tooling::buildASTFromCodeWithArgs(
      Code.code(),
      /*Args=*/{"-target", "i386-apple-darwin", "-x", "objective-c",
                "-std=c89"});
  ASTContext &Ctx = AST->getASTContext();

  auto *f = selectFirst<FunctionDecl>(
      "f", match(functionDecl(hasName("f")).bind("f"), Ctx));

  const auto *FPT = f->getType()->getAs<FunctionNoProtoType>();

  // Functions without prototypes always have 0 initialized qualifiers
  EXPECT_FALSE(FPT->isConst());
  EXPECT_FALSE(FPT->isVolatile());
  EXPECT_FALSE(FPT->isRestrict());
}

TEST(Decl, ImplicitlyDeclaredAllocationFunctionsInModules) {
  // C++ [basic.stc.dynamic.general]p2:
  //   The library provides default definitions for the global allocation
  //   and deallocation functions. Some global allocation and deallocation
  //   functions are replaceable ([new.delete]); these are attached to the
  //   global module ([module.unit]).

  llvm::Annotations Code(R"(
    export module base;

    export struct Base {
        virtual void hello() = 0;
        virtual ~Base() = default;
    };
  )");

  auto AST =
      tooling::buildASTFromCodeWithArgs(Code.code(), /*Args=*/{"-std=c++20"});
  ASTContext &Ctx = AST->getASTContext();

  // void* operator new(std::size_t);
  auto *SizedOperatorNew = selectFirst<FunctionDecl>(
      "operator new",
      match(functionDecl(hasName("operator new"), parameterCountIs(1),
                         hasParameter(0, hasType(isUnsignedInteger())))
                .bind("operator new"),
            Ctx));
  ASSERT_TRUE(SizedOperatorNew->getOwningModule());
  EXPECT_TRUE(SizedOperatorNew->isFromExplicitGlobalModule());

  // void* operator new(std::size_t, std::align_val_t);
  auto *SizedAlignedOperatorNew = selectFirst<FunctionDecl>(
      "operator new",
      match(functionDecl(
                hasName("operator new"), parameterCountIs(2),
                hasParameter(0, hasType(isUnsignedInteger())),
                hasParameter(1, hasType(enumDecl(hasName("std::align_val_t")))))
                .bind("operator new"),
            Ctx));
  ASSERT_TRUE(SizedAlignedOperatorNew->getOwningModule());
  EXPECT_TRUE(SizedAlignedOperatorNew->isFromExplicitGlobalModule());

  // void* operator new[](std::size_t);
  auto *SizedArrayOperatorNew = selectFirst<FunctionDecl>(
      "operator new[]",
      match(functionDecl(hasName("operator new[]"), parameterCountIs(1),
                         hasParameter(0, hasType(isUnsignedInteger())))
                .bind("operator new[]"),
            Ctx));
  ASSERT_TRUE(SizedArrayOperatorNew->getOwningModule());
  EXPECT_TRUE(SizedArrayOperatorNew->isFromExplicitGlobalModule());

  // void* operator new[](std::size_t, std::align_val_t);
  auto *SizedAlignedArrayOperatorNew = selectFirst<FunctionDecl>(
      "operator new[]",
      match(functionDecl(
                hasName("operator new[]"), parameterCountIs(2),
                hasParameter(0, hasType(isUnsignedInteger())),
                hasParameter(1, hasType(enumDecl(hasName("std::align_val_t")))))
                .bind("operator new[]"),
            Ctx));
  ASSERT_TRUE(SizedAlignedArrayOperatorNew->getOwningModule());
  EXPECT_TRUE(
      SizedAlignedArrayOperatorNew->isFromExplicitGlobalModule());

  // void operator delete(void*) noexcept;
  auto *Delete = selectFirst<FunctionDecl>(
      "operator delete",
      match(functionDecl(
                hasName("operator delete"), parameterCountIs(1),
                hasParameter(0, hasType(pointerType(pointee(voidType())))))
                .bind("operator delete"),
            Ctx));
  ASSERT_TRUE(Delete->getOwningModule());
  EXPECT_TRUE(Delete->isFromExplicitGlobalModule());

  // void operator delete(void*, std::align_val_t) noexcept;
  auto *AlignedDelete = selectFirst<FunctionDecl>(
      "operator delete",
      match(functionDecl(
                hasName("operator delete"), parameterCountIs(2),
                hasParameter(0, hasType(pointerType(pointee(voidType())))),
                hasParameter(1, hasType(enumDecl(hasName("std::align_val_t")))))
                .bind("operator delete"),
            Ctx));
  ASSERT_TRUE(AlignedDelete->getOwningModule());
  EXPECT_TRUE(AlignedDelete->isFromExplicitGlobalModule());

  // Sized deallocation is not enabled by default. So we skip it here.

  // void operator delete[](void*) noexcept;
  auto *ArrayDelete = selectFirst<FunctionDecl>(
      "operator delete[]",
      match(functionDecl(
                hasName("operator delete[]"), parameterCountIs(1),
                hasParameter(0, hasType(pointerType(pointee(voidType())))))
                .bind("operator delete[]"),
            Ctx));
  ASSERT_TRUE(ArrayDelete->getOwningModule());
  EXPECT_TRUE(ArrayDelete->isFromExplicitGlobalModule());

  // void operator delete[](void*, std::align_val_t) noexcept;
  auto *AlignedArrayDelete = selectFirst<FunctionDecl>(
      "operator delete[]",
      match(functionDecl(
                hasName("operator delete[]"), parameterCountIs(2),
                hasParameter(0, hasType(pointerType(pointee(voidType())))),
                hasParameter(1, hasType(enumDecl(hasName("std::align_val_t")))))
                .bind("operator delete[]"),
            Ctx));
  ASSERT_TRUE(AlignedArrayDelete->getOwningModule());
  EXPECT_TRUE(AlignedArrayDelete->isFromExplicitGlobalModule());
}

TEST(Decl, TemplateArgumentDefaulted) {
  llvm::Annotations Code(R"cpp(
    template<typename T1, typename T2>
    struct Alloc {};

    template <typename T1,
              typename T2 = double,
              int      T3 = 42,
              typename T4 = Alloc<T1, T2>>
    struct Foo {
    };

    Foo<char, int, 42, Alloc<char, int>> X;
  )cpp");

  auto AST =
      tooling::buildASTFromCodeWithArgs(Code.code(), /*Args=*/{"-std=c++20"});
  ASTContext &Ctx = AST->getASTContext();

  auto const *CTSD = selectFirst<ClassTemplateSpecializationDecl>(
      "id",
      match(classTemplateSpecializationDecl(hasName("Foo")).bind("id"), Ctx));
  ASSERT_NE(CTSD, nullptr);
  auto const &ArgList = CTSD->getTemplateArgs();

  EXPECT_FALSE(ArgList.get(0).getIsDefaulted());
  EXPECT_FALSE(ArgList.get(1).getIsDefaulted());
  EXPECT_TRUE(ArgList.get(2).getIsDefaulted());
  EXPECT_TRUE(ArgList.get(3).getIsDefaulted());
}

TEST(Decl, InstantiatedDependentFriendTemplate) {
  StringRef Code = R"cpp(
    template <class T> struct A {
      template <class U> struct B;
    };

    template <class T> struct C {
      template <class U> friend struct A<T>::B;
    };

    template struct C<int>;
  )cpp";

  auto AST = tooling::buildASTFromCodeWithArgs(Code, {"-std=c++20"});
  ASTContext &Ctx = AST->getASTContext();
  const auto *CTemplate = selectFirst<ClassTemplateDecl>(
      "c", match(classTemplateDecl(hasName("C")).bind("c"), Ctx));
  ASSERT_NE(CTemplate, nullptr);

  const FriendTemplateDecl *DependentFriend = nullptr;
  for (const Decl *D : CTemplate->getTemplatedDecl()->decls()) {
    if (const auto *FTD = dyn_cast<FriendTemplateDecl>(D)) {
      DependentFriend = FTD;
      break;
    }
  }
  ASSERT_NE(DependentFriend, nullptr);
  ASSERT_NE(DependentFriend->getFriendType(), nullptr);
  EXPECT_TRUE(DependentFriend->getFriendTemplateName().isDependent());

  const auto *CSpecialization = selectFirst<ClassTemplateSpecializationDecl>(
      "c", match(classTemplateSpecializationDecl(hasName("C")).bind("c"), Ctx));
  ASSERT_NE(CSpecialization, nullptr);

  const FriendTemplateDecl *InstFriend = nullptr;
  for (const Decl *D : CSpecialization->decls()) {
    if (const auto *FTD = dyn_cast<FriendTemplateDecl>(D)) {
      InstFriend = FTD;
      break;
    }
  }
  ASSERT_NE(InstFriend, nullptr);
  ASSERT_EQ(InstFriend->getFriendType(), nullptr);
  ASSERT_FALSE(InstFriend->getFriendTemplateName().isNull());

  const FriendDecl *Friend = InstFriend;
  EXPECT_EQ(Friend->getFriendDecl(),
            InstFriend->getFriendTemplateName().getAsTemplateDecl());
  EXPECT_EQ(InstFriend->getSourceRange().getEnd(), InstFriend->getLocation());
}

TEST(Decl, InstantiatedDependentFriendTemplateParameters) {
  StringRef Code = R"cpp(
    template <class T> struct A {
      template <class U> struct B;
    };

    template <class V> struct C {
      template <class U>
      friend struct A<U>::template B<V>;
    };

    template struct C<int>;
  )cpp";

  auto AST = tooling::buildASTFromCodeWithArgs(Code, {"-std=c++20"});
  ASTContext &Ctx = AST->getASTContext();
  const auto *CSpecialization = selectFirst<ClassTemplateSpecializationDecl>(
      "c", match(classTemplateSpecializationDecl(hasName("C")).bind("c"), Ctx));
  ASSERT_NE(CSpecialization, nullptr);
  ASSERT_NE(CSpecialization->friend_begin(), CSpecialization->friend_end());

  const auto *Friend =
      dyn_cast<FriendTemplateDecl>(*CSpecialization->friend_begin());
  ASSERT_NE(Friend, nullptr);
  ASSERT_EQ(Friend->getTemplateParameterLists().size(), 1u);
  ASSERT_NE(Friend->getFriendType(), nullptr);

  TemplateSpecializationTypeLoc FriendTL =
      Friend->getFriendType()
          ->getTypeLoc()
          .castAs<TemplateSpecializationTypeLoc>();
  const Type *QualifierType =
      FriendTL.getQualifierLoc().getNestedNameSpecifier().getAsType();
  ASSERT_NE(QualifierType, nullptr);
  const auto *TST = QualifierType->getAs<TemplateSpecializationType>();
  ASSERT_NE(TST, nullptr);
  const auto *TTP =
      TST->template_arguments()[0].getAsType()->getAs<TemplateTypeParmType>();
  ASSERT_NE(TTP, nullptr);
  EXPECT_EQ(TTP->getDecl(),
            Friend->getTemplateParameterLists().front()->getParam(0));
}

TEST(Decl, InvalidFunctionFriendIsRetained) {
  StringRef Code = R"cpp(
    int f();
    struct A {
      friend void f();
    };
  )cpp";

  IgnoringDiagConsumer Diags;
  auto AST = tooling::buildASTFromCodeWithArgs(
      Code, {"-std=c++20"}, "input.cc", "clang-tool",
      std::make_shared<PCHContainerOperations>(),
      tooling::getClangStripDependencyFileAdjuster(),
      tooling::FileContentMappings(), &Diags);
  ASTContext &Ctx = AST->getASTContext();
  const auto *Record = selectFirst<CXXRecordDecl>(
      "a", match(cxxRecordDecl(hasName("A"), isDefinition()).bind("a"), Ctx));
  ASSERT_NE(Record, nullptr);
  ASSERT_NE(Record->friend_begin(), Record->friend_end());

  const FriendDecl *Friend = *Record->friend_begin();
  EXPECT_TRUE(Friend->isInvalidDecl());
  ASSERT_NE(Friend->getFriendDecl(), nullptr);
  EXPECT_TRUE(Friend->getFriendDecl()->isInvalidDecl());
}

TEST(Decl, CXXDestructorDeclsShouldHaveWellFormedNameInfoRanges) {
  // GH71161
  llvm::Annotations Code(R"cpp(
template <typename T> struct Resource {
  ~Resource(); // 1
};
template <typename T>
Resource<T>::~Resource() {} // 2,3

void instantiate_template() {
  Resource<int> x;
}
)cpp");

  auto AST = tooling::buildASTFromCode(Code.code());
  ASTContext &Ctx = AST->getASTContext();

  const auto &SM = Ctx.getSourceManager();
  auto GetNameInfoRange = [&SM](const BoundNodes &Match) {
    const auto *D = Match.getNodeAs<CXXDestructorDecl>("dtor");
    return D->getNameInfo().getSourceRange().printToString(SM);
  };

  auto Matches = match(findAll(cxxDestructorDecl().bind("dtor")),
                       *Ctx.getTranslationUnitDecl(), Ctx);
  ASSERT_EQ(Matches.size(), 3U);
  EXPECT_EQ(GetNameInfoRange(Matches[0]), "<input.cc:3:3, col:4>");
  EXPECT_EQ(GetNameInfoRange(Matches[1]), "<input.cc:6:14, col:15>");
  EXPECT_EQ(GetNameInfoRange(Matches[2]), "<input.cc:6:14, col:15>");
}

TEST(Decl, getQualifiedNameAsString) {
  llvm::Annotations Code(R"cpp(
namespace x::y {
  template <class T> class Foo { Foo() {} };
}
)cpp");

  auto AST = tooling::buildASTFromCode(Code.code());
  ASTContext &Ctx = AST->getASTContext();

  auto const *FD = selectFirst<CXXConstructorDecl>(
      "ctor", match(cxxConstructorDecl().bind("ctor"), Ctx));
  ASSERT_NE(FD, nullptr);
  ASSERT_EQ(FD->getQualifiedNameAsString(), "x::y::Foo::Foo<T>");
}

TEST(Decl, NoWrittenArgsInImplicitlyInstantiatedVarSpec) {
  const char *Code = R"cpp(
    template <typename>
    int VarTpl;

    void fn() {
      (void)VarTpl<char>;
    }
  )cpp";

  auto AST = tooling::buildASTFromCode(Code);
  ASTContext &Ctx = AST->getASTContext();

  const auto *VTSD = selectFirst<VarTemplateSpecializationDecl>(
      "id", match(varDecl(isTemplateInstantiation()).bind("id"), Ctx));
  ASSERT_NE(VTSD, nullptr);
  EXPECT_EQ(VTSD->getTemplateArgsAsWritten(), nullptr);
}

TEST(Decl, ObjCMethodDeclNameForDiagnostic) {
  const char *Code = R"objc(
    @protocol MyProtocol
    - (void)myProtocolMethod;
    @end

    @interface MyClass
    - (void)myMethod:(int)x;
    + (void)myClassMethod;
    @end

    @interface MyClass (MyCategory)
    - (void)myCategoryMethod;
    @end

    @interface MyClass ()
    - (void)myExtensionMethod;
    @end
  )objc";

  auto AST = tooling::buildASTFromCodeWithArgs(Code, {"-x", "objective-c"});
  ASTContext &Ctx = AST->getASTContext();

  auto const *IM = selectFirst<ObjCMethodDecl>(
      "im", match(objcMethodDecl(hasName("myMethod:")).bind("im"), Ctx));
  ASSERT_NE(IM, nullptr);

  std::string IMQualifiedName;
  llvm::raw_string_ostream IMQualifiedOS(IMQualifiedName);
  IM->getNameForDiagnostic(IMQualifiedOS, Ctx.getPrintingPolicy(),
                           /*Qualified=*/true);
  EXPECT_EQ(IMQualifiedOS.str(), "-[MyClass myMethod:]");

  std::string IMUnqualifiedName;
  llvm::raw_string_ostream IMUnqualifiedOS(IMUnqualifiedName);
  IM->getNameForDiagnostic(IMUnqualifiedOS, Ctx.getPrintingPolicy(),
                           /*Qualified=*/false);
  EXPECT_EQ(IMUnqualifiedOS.str(), "myMethod:");

  auto const *CM = selectFirst<ObjCMethodDecl>(
      "cm", match(objcMethodDecl(hasName("myClassMethod")).bind("cm"), Ctx));
  ASSERT_NE(CM, nullptr);

  std::string CMQualifiedName;
  llvm::raw_string_ostream CMQualifiedOS(CMQualifiedName);
  CM->getNameForDiagnostic(CMQualifiedOS, Ctx.getPrintingPolicy(),
                           /*Qualified=*/true);
  EXPECT_EQ(CMQualifiedOS.str(), "+[MyClass myClassMethod]");

  std::string CMUnqualifiedName;
  llvm::raw_string_ostream CMUnqualifiedOS(CMUnqualifiedName);
  CM->getNameForDiagnostic(CMUnqualifiedOS, Ctx.getPrintingPolicy(),
                           /*Qualified=*/false);
  EXPECT_EQ(CMUnqualifiedOS.str(), "myClassMethod");

  auto const *PM = selectFirst<ObjCMethodDecl>(
      "pm", match(objcMethodDecl(hasName("myProtocolMethod")).bind("pm"), Ctx));
  ASSERT_NE(PM, nullptr);

  std::string PMQualifiedName;
  llvm::raw_string_ostream PMQualifiedOS(PMQualifiedName);
  PM->getNameForDiagnostic(PMQualifiedOS, Ctx.getPrintingPolicy(),
                           /*Qualified=*/true);
  EXPECT_EQ(PMQualifiedOS.str(), "-[MyProtocol myProtocolMethod]");

  auto const *CatM = selectFirst<ObjCMethodDecl>(
      "catm",
      match(objcMethodDecl(hasName("myCategoryMethod")).bind("catm"), Ctx));
  ASSERT_NE(CatM, nullptr);

  std::string CatMQualifiedName;
  llvm::raw_string_ostream CatMQualifiedOS(CatMQualifiedName);
  CatM->getNameForDiagnostic(CatMQualifiedOS, Ctx.getPrintingPolicy(),
                             /*Qualified=*/true);
  EXPECT_EQ(CatMQualifiedOS.str(), "-[MyClass myCategoryMethod]");

  auto const *ExtM = selectFirst<ObjCMethodDecl>(
      "extm",
      match(objcMethodDecl(hasName("myExtensionMethod")).bind("extm"), Ctx));
  ASSERT_NE(ExtM, nullptr);

  std::string ExtMQualifiedName;
  llvm::raw_string_ostream ExtMQualifiedOS(ExtMQualifiedName);
  ExtM->getNameForDiagnostic(ExtMQualifiedOS, Ctx.getPrintingPolicy(),
                             /*Qualified=*/true);
  EXPECT_EQ(ExtMQualifiedOS.str(), "-[MyClass myExtensionMethod]");
}

TEST(Decl, ObjCPropertyDeclNameForDiagnostic) {
  const char *Code = R"objc(
    @protocol MyProtocol
    @property int myProtocolProp;
    @end

    @interface MyClass
    @property int myProp;
    @property(class) int myClassProp;
    @end

    @interface MyClass (MyCategory)
    @property int myCategoryProp;
    @end

    @interface MyClass ()
    @property int extensionProp;
    @end
  )objc";

  auto AST = tooling::buildASTFromCodeWithArgs(Code, {"-x", "objective-c"});
  ASTContext &Ctx = AST->getASTContext();

  auto const *P = selectFirst<ObjCPropertyDecl>(
      "p", match(objcPropertyDecl(hasName("myProp")).bind("p"), Ctx));
  ASSERT_NE(P, nullptr);

  std::string PQualifiedName;
  llvm::raw_string_ostream PQualifiedOS(PQualifiedName);
  P->getNameForDiagnostic(PQualifiedOS, Ctx.getPrintingPolicy(),
                          /*Qualified=*/true);
  EXPECT_EQ(PQualifiedOS.str(), "-[MyClass myProp]");

  std::string PUnqualifiedName;
  llvm::raw_string_ostream PUnqualifiedOS(PUnqualifiedName);
  P->getNameForDiagnostic(PUnqualifiedOS, Ctx.getPrintingPolicy(),
                          /*Qualified=*/false);
  EXPECT_EQ(PUnqualifiedOS.str(), "myProp");

  auto const *CP = selectFirst<ObjCPropertyDecl>(
      "cp", match(objcPropertyDecl(hasName("myClassProp")).bind("cp"), Ctx));
  ASSERT_NE(CP, nullptr);

  std::string CPQualifiedName;
  llvm::raw_string_ostream CPQualifiedOS(CPQualifiedName);
  CP->getNameForDiagnostic(CPQualifiedOS, Ctx.getPrintingPolicy(),
                           /*Qualified=*/true);
  EXPECT_EQ(CPQualifiedOS.str(), "+[MyClass myClassProp]");

  std::string CPUnqualifiedName;
  llvm::raw_string_ostream CPUnqualifiedOS(CPUnqualifiedName);
  CP->getNameForDiagnostic(CPUnqualifiedOS, Ctx.getPrintingPolicy(),
                           /*Qualified=*/false);
  EXPECT_EQ(CPUnqualifiedOS.str(), "myClassProp");

  auto const *PP = selectFirst<ObjCPropertyDecl>(
      "pp", match(objcPropertyDecl(hasName("myProtocolProp")).bind("pp"), Ctx));
  ASSERT_NE(PP, nullptr);

  std::string PPQualifiedName;
  llvm::raw_string_ostream PPQualifiedOS(PPQualifiedName);
  PP->getNameForDiagnostic(PPQualifiedOS, Ctx.getPrintingPolicy(),
                           /*Qualified=*/true);
  EXPECT_EQ(PPQualifiedOS.str(), "-[MyProtocol myProtocolProp]");

  auto const *CatP = selectFirst<ObjCPropertyDecl>(
      "catp",
      match(objcPropertyDecl(hasName("myCategoryProp")).bind("catp"), Ctx));
  ASSERT_NE(CatP, nullptr);

  std::string CatPQualifiedName;
  llvm::raw_string_ostream CatPQualifiedOS(CatPQualifiedName);
  CatP->getNameForDiagnostic(CatPQualifiedOS, Ctx.getPrintingPolicy(),
                             /*Qualified=*/true);
  // We expect MyClass if getter is available, or if fallback looks through
  // categories. Let's see what happens.
  EXPECT_EQ(CatPQualifiedOS.str(), "-[MyClass myCategoryProp]");

  auto const *ExtP = selectFirst<ObjCPropertyDecl>(
      "extp",
      match(objcPropertyDecl(hasName("extensionProp")).bind("extp"), Ctx));
  ASSERT_NE(ExtP, nullptr);

  std::string ExtPQualifiedName;
  llvm::raw_string_ostream ExtPQualifiedOS(ExtPQualifiedName);
  ExtP->getNameForDiagnostic(ExtPQualifiedOS, Ctx.getPrintingPolicy(),
                             /*Qualified=*/true);
  EXPECT_EQ(ExtPQualifiedOS.str(), "-[MyClass extensionProp]");
}

namespace {

/// Collects every ConceptSpecializationExpr in a translation unit.
struct ConceptSpecializationExprCollector
    : RecursiveASTVisitor<ConceptSpecializationExprCollector> {
  SmallVector<const ConceptSpecializationExpr *, 8> Exprs;

  bool VisitConceptSpecializationExpr(ConceptSpecializationExpr *E) {
    Exprs.push_back(E);
    return true;
  }

  bool VisitTemplateTypeParmDecl(TemplateTypeParmDecl *D) {
    if (const TypeConstraint *TC = D->getTypeConstraint())
      if (auto *CSE = dyn_cast_or_null<ConceptSpecializationExpr>(
              TC->getImmediatelyDeclaredConstraint()))
        Exprs.push_back(CSE);
    return true;
  }
};

} // namespace

// Pins the postcondition of the fix for #191361: CreateDeserialized() publishes
// NumTemplateArgs, but ASTDeclReader does not write the trailing arguments
// until setTemplateArguments() runs at the end of
// VisitImplicitConceptSpecializationDecl(). Re-entrant deserialization can
// reach the decl in that window -- through a ConceptSpecializationExpr, into
// StmtProfiler -- so the storage has to be well defined rather than raw
// bump-allocator memory.
TEST(ImplicitConceptSpecializationDecl,
     DeserializedArgumentsAreValueInitialized) {
  std::unique_ptr<ASTUnit> AST = buildASTFromCodeWithArgs("", {"-std=c++20"});
  ASSERT_TRUE(AST);

  auto *D = ImplicitConceptSpecializationDecl::CreateDeserialized(
      AST->getASTContext(), GlobalDeclID(), /*NumTemplateArgs=*/3);
  ArrayRef<TemplateArgument> Args = D->getTemplateArguments();

  ASSERT_EQ(Args.size(), 3u);
  for (const TemplateArgument &Arg : Args)
    EXPECT_TRUE(Arg.isNull())
        << "trailing arguments must be value-initialized before "
           "setTemplateArguments() runs";
}

// The value-initialization must not survive the real write.
TEST(ImplicitConceptSpecializationDecl, SetTemplateArgumentsOverwritesStorage) {
  std::unique_ptr<ASTUnit> AST = buildASTFromCodeWithArgs("", {"-std=c++20"});
  ASSERT_TRUE(AST);
  ASTContext &Ctx = AST->getASTContext();

  auto *D = ImplicitConceptSpecializationDecl::CreateDeserialized(
      Ctx, GlobalDeclID(), /*NumTemplateArgs=*/2);

  TemplateArgument Written[] = {TemplateArgument(Ctx.IntTy),
                                TemplateArgument(Ctx.CharTy)};
  D->setTemplateArguments(Written);

  ArrayRef<TemplateArgument> Args = D->getTemplateArguments();
  ASSERT_EQ(Args.size(), 2u);
  for (const TemplateArgument &Arg : Args)
    EXPECT_FALSE(Arg.isNull());
  EXPECT_TRUE(Args[0].getAsType()->isIntegerType());
}

// The zero-argument case: getTemplateArguments() must be empty and must not
// touch the trailing storage at all.
TEST(ImplicitConceptSpecializationDecl, DeserializedWithNoArguments) {
  std::unique_ptr<ASTUnit> AST = buildASTFromCodeWithArgs("", {"-std=c++20"});
  ASSERT_TRUE(AST);

  auto *D = ImplicitConceptSpecializationDecl::CreateDeserialized(
      AST->getASTContext(), GlobalDeclID(), /*NumTemplateArgs=*/0);
  EXPECT_TRUE(D->getTemplateArguments().empty());
}

namespace {

/// StmtProfiler::VisitConceptSpecializationExpr() identifies an
/// ImplicitConceptSpecializationDecl whose trailing arguments have not been
/// deserialized yet by the fact that they are Null in every position -- see the
/// FIXME in that class's EmptyShell constructor, and the assertions in
/// ASTContext::getFunctionTypeInternal() that depend on it. That is only sound
/// if a converted concept-id argument is never Null.
///
/// The cases below are the product of the two axes along which
/// Sema::CheckTemplateArgumentList() builds a converted list from a written
/// one, so that the coverage claim can be checked against that function rather
/// than taken on faith:
///
///   * the kind of the concept's template parameter (type, non-type, template
///     template, and packs of each). This does not map one-to-one onto
///     TemplateArgument kinds: a class-type non-type parameter converts to a
///     Declaration argument backed by a TemplateParamObjectDecl, the same kind
///     a pointer parameter produces. See
///     ObservedArgumentKindsCoverTheEnumeration for the kinds actually
///     observed;
///   * how each position is filled: written explicitly, supplied by default,
///     deduced implicitly (the leading argument of a type-constraint), or
///     absorbed into a pack, including an empty one.
///
/// Error recovery is sampled rather than enumerated; see
/// ConvertedArgumentsSurviveErrorRecovery.
class ConvertedConceptArguments : public ::testing::Test {
protected:
  std::unique_ptr<ASTUnit> AST;
  ConceptSpecializationExprCollector Collector;

  void parse(StringRef Code) {
    AST = tooling::buildASTFromCodeWithArgs(Code, {"-std=c++20"});
    ASSERT_TRUE(AST);
    Collector.Exprs.clear();
    Collector.TraverseDecl(AST->getASTContext().getTranslationUnitDecl());
    ASSERT_FALSE(Collector.Exprs.empty())
        << "no ConceptSpecializationExpr was produced; the test code is not "
           "exercising what it claims to";
  }

  static void expectNoNull(ArrayRef<TemplateArgument> Args) {
    for (const TemplateArgument &Arg : Args) {
      EXPECT_FALSE(Arg.isNull())
          << "a converted concept-id argument was Null; an unwritten argument "
             "list is Null in every position, so this is what the detection in "
             "StmtProfiler::VisitConceptSpecializationExpr uses to identify a "
             "decl that has not been deserialized yet";
      if (Arg.getKind() == TemplateArgument::Pack)
        expectNoNull(Arg.pack_elements());
    }
  }

  /// Checks the invariant on every specialization decl in the parse, and
  /// additionally that no argument list is Null throughout -- the exact
  /// condition StmtProfiler tests.
  void expectInvariantHolds() {
    for (const ConceptSpecializationExpr *E : Collector.Exprs) {
      const auto *D = E->getSpecializationDecl();
      ASSERT_NE(D, nullptr);
      expectNoNull(D->getTemplateArguments());
    }
  }

  llvm::DenseSet<int> observedKinds() {
    llvm::DenseSet<int> Kinds;
    for (const ConceptSpecializationExpr *E : Collector.Exprs)
      if (const auto *D = E->getSpecializationDecl())
        for (const TemplateArgument &Arg : D->getTemplateArguments()) {
          Kinds.insert(Arg.getKind());
          if (Arg.getKind() == TemplateArgument::Pack)
            for (const TemplateArgument &Elem : Arg.pack_elements())
              Kinds.insert(Elem.getKind());
        }
    return Kinds;
  }
};

} // namespace

// --- Axis 1: parameter kind ------------------------------------------------

TEST_F(ConvertedConceptArguments, TypeParameter) {
  parse(R"cpp(
    template <class T> concept C = true;
    static_assert(C<int>);
  )cpp");
  expectInvariantHolds();
}

TEST_F(ConvertedConceptArguments, IntegralNonTypeParameter) {
  parse(R"cpp(
    template <int N> concept C = N > 0;
    static_assert(C<1>);
  )cpp");
  expectInvariantHolds();
}

TEST_F(ConvertedConceptArguments, PointerAndNullPtrNonTypeParameters) {
  parse(R"cpp(
    extern int G;
    template <int *P> concept CPtr = true;
    template <decltype(nullptr) P> concept CNull = true;
    static_assert(CPtr<&G>);
    static_assert(CNull<nullptr>);
  )cpp");
  expectInvariantHolds();
}

// A class-type non-type parameter. The converted argument is a Declaration
// backed by a TemplateParamObjectDecl -- not a StructuralValue, which is not
// reachable from a concept-id this way.
TEST_F(ConvertedConceptArguments, ClassTypeNonTypeParameter) {
  parse(R"cpp(
    struct Val { int X; };
    template <Val V> concept C = true;
    static_assert(C<Val{1}>);
  )cpp");
  expectInvariantHolds();
}

TEST_F(ConvertedConceptArguments, TemplateTemplateParameter) {
  parse(R"cpp(
    template <class> struct S {};
    template <template <class> class TT> concept C = true;
    static_assert(C<S>);
  )cpp");
  expectInvariantHolds();
}

// A concept-id in a dependent context: the converted arguments are still
// expressions rather than values.
TEST_F(ConvertedConceptArguments, DependentArguments) {
  parse(R"cpp(
    template <int N> concept C = N > 0;
    template <int N> void f() requires C<N> {}
    void g() { f<1>(); }
  )cpp");
  expectInvariantHolds();
}

// --- Axis 2: how a position is filled --------------------------------------

TEST_F(ConvertedConceptArguments, DefaultedParameter) {
  // F<int> converts to {int, int}: the written list is shorter than the
  // converted one.
  parse(R"cpp(
    template <class T> concept C = true;
    template <class T, class U = int> concept F = C<U>;
    static_assert(F<int>);
  )cpp");
  expectInvariantHolds();
}

TEST_F(ConvertedConceptArguments, ImplicitLeadingArgumentOfTypeConstraint) {
  // The constraint on f's parameter converts to {T}, with the first argument
  // supplied implicitly rather than written.
  parse(R"cpp(
    template <class T> concept C = true;
    template <C T> void f(T);
    void g() { f(0); }
  )cpp");
  expectInvariantHolds();
}

TEST_F(ConvertedConceptArguments, ParameterPack) {
  parse(R"cpp(
    template <class T> concept C = true;
    template <class... Ts> concept E = (C<Ts> && ...);
    static_assert(E<int, char, long>);
  )cpp");
  expectInvariantHolds();
}

// The closest a well-formed converted list gets to "nothing here": a Pack
// argument with no elements. It is a Pack, not a Null.
TEST_F(ConvertedConceptArguments, EmptyParameterPack) {
  parse(R"cpp(
    template <class... Ts> concept E = true;
    static_assert(E<>);
  )cpp");
  expectInvariantHolds();
}

TEST_F(ConvertedConceptArguments, NestedConceptIdsGetTheirOwnDecls) {
  parse(R"cpp(
    template <class T> concept C = true;
    template <class T, class U> concept D = C<T> && C<U>;
    static_assert(D<int, char>);
  )cpp");
  expectInvariantHolds();
}

// --- Coverage --------------------------------------------------------------

// Turns the enumeration above from a claim in a comment into something the
// test verifies: if a future Clang stops producing one of these kinds for the
// code below, the coverage assumed by the other tests has silently shrunk.
TEST_F(ConvertedConceptArguments, ObservedArgumentKindsCoverTheEnumeration) {
  parse(R"cpp(
    extern int G;
    template <class> struct S {};
    struct Val { int X; };

    template <class T> concept CType = true;
    template <int N> concept CInt = N > 0;
    template <int *P> concept CPtr = true;
    template <Val V> concept CVal = true;
    template <template <class> class TT> concept CTmpl = true;
    template <class... Ts> concept CPack = true;

    static_assert(CType<int>);
    static_assert(CInt<1>);
    static_assert(CPtr<&G>);
    static_assert(CVal<Val{1}>);
    static_assert(CTmpl<S>);
    static_assert(CPack<int, char>);
  )cpp");
  expectInvariantHolds();

  llvm::DenseSet<int> Kinds = observedKinds();
  // StructuralValue is deliberately absent: CVal below is a class-type non-type
  // parameter, and its converted argument is a Declaration backed by a
  // TemplateParamObjectDecl. Declaration therefore covers both CPtr and CVal.
  EXPECT_TRUE(Kinds.contains(TemplateArgument::Type));
  EXPECT_TRUE(Kinds.contains(TemplateArgument::Integral));
  EXPECT_TRUE(Kinds.contains(TemplateArgument::Declaration));
  EXPECT_TRUE(Kinds.contains(TemplateArgument::Template));
  EXPECT_TRUE(Kinds.contains(TemplateArgument::Pack));
  EXPECT_FALSE(Kinds.contains(TemplateArgument::Null))
      << "a converted concept-id argument was Null";
}

// --- Error recovery (sampled, not enumerated) ------------------------------

// Whether a partially built converted list can ever be attached to an
// ImplicitConceptSpecializationDecl is a question about Sema's recovery paths
// that this test samples rather than settles. If any of these produce a decl
// at all, its arguments must still satisfy the invariant.
//
// NOTE: this parse is expected to emit diagnostics, which will appear in the
// test output. If buildASTFromCodeWithArgs returns null on a failed parse,
// split this into per-case parses and skip the ones that do.
TEST_F(ConvertedConceptArguments, ConvertedArgumentsSurviveErrorRecovery) {
  AST = tooling::buildASTFromCodeWithArgs(R"cpp(
    template <class T, class U> concept C = true;
    template <int N> concept CInt = true;
    struct Incomplete;

    static_assert(C<int>);          // too few arguments
    static_assert(C<int, int, int>);// too many arguments
    static_assert(CInt<Incomplete>);// wrong argument kind
  )cpp",
                                          {"-std=c++20"});
  if (!AST)
    GTEST_SKIP() << "the ill-formed parse produced no AST";

  Collector.Exprs.clear();
  Collector.TraverseDecl(AST->getASTContext().getTranslationUnitDecl());
  expectInvariantHolds();
}
