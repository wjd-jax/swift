//===--- Module.h - Swift Language Module ASTs ------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines the Module class and its subclasses.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_MODULE_H
#define SWIFT_MODULE_H

#include "swift/AST/DeclContext.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/Type.h"
#include "swift/Basic/Optional.h"
#include "swift/Basic/SourceLoc.h"
#include "swift/Basic/STLExtras.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/StringMap.h"

namespace clang {
  class Module;
}

namespace swift {
  class ASTContext;
  class ASTWalker;
  class BraceStmt;
  class Component;
  class Decl;
  enum class DeclKind : uint8_t;
  class ExtensionDecl;
  class InfixOperatorDecl;
  class LinkLibrary;
  class LookupCache;
  class ModuleLoader;
  class NameAliasType;
  class NominalTypeDecl;
  class EnumElementDecl;
  class OperatorDecl;
  class PostfixOperatorDecl;
  class PrefixOperatorDecl;
  class ProtocolConformance;
  class ProtocolDecl;
  struct PrintOptions;
  class TupleType;
  class Type;
  class ValueDecl;
  class VisibleDeclConsumer;
  
  /// NLKind - This is a specifier for the kind of name lookup being performed
  /// by various query methods.
  enum class NLKind {
    UnqualifiedLookup,
    QualifiedLookup
  };

  enum class ModuleKind {
    TranslationUnit,
    BuiltinModule,
    SerializedModule,
    ClangModule
  };

/// Constants used to customize name lookup.
enum NameLookupOptions {
  /// Visit supertypes (such as superclasses or inherited protocols)
  /// and their extensions as well as the current extension.
  NL_VisitSupertypes = 0x01,

  /// Consider declarations within protocols to which the context type conforms.
  NL_ProtocolMembers = 0x02,

  /// Remove non-visible declarations from the set of results.
  NL_RemoveNonVisible = 0x04,

  /// Remove overridden declarations from the set of results.
  NL_RemoveOverridden = 0x08,

  /// For existentials involving the special \c DynamicLookup protocol,
  /// allow lookups to find members of all classes.
  NL_DynamicLookup    = 0x10,

  /// The default set of options used for qualified name lookup.
  ///
  /// FIXME: Eventually, add NL_ProtocolMembers to this, once all of the
  /// callers can handle it.
  NL_QualifiedDefault = NL_VisitSupertypes | NL_RemoveNonVisible |
                        NL_RemoveOverridden,

  /// The default set of options used for unqualified name lookup.
  NL_UnqualifiedDefault = NL_VisitSupertypes |
                          NL_RemoveNonVisible | NL_RemoveOverridden,

  /// The default set of options used for constructor lookup.
  NL_Constructor = NL_RemoveNonVisible
};

/// Describes the result of looking for the conformance of a given type
/// to a specific protocol.
enum class ConformanceKind {
  /// The type does not conform to the protocol.
  DoesNotConform,
  /// The type conforms to the protocol, with the given conformance.
  Conforms,
  /// The type is specified to conform to the protocol, but that conformance
  /// has not yet been checked.
  UncheckedConforms
};

/// The result of looking for a specific conformance.
typedef llvm::PointerIntPair<ProtocolConformance *, 2, ConformanceKind>
  LookupConformanceResult;

/// Module - A unit of modularity.  The current translation unit is a
/// module, as is an imported module.
class Module : public DeclContext {
  ModuleKind Kind;
protected:
  mutable void *LookupCachePimpl;
  Component *Comp;
public:
  ASTContext &Ctx;
  Identifier Name;
  
  //===--------------------------------------------------------------------===//
  // AST Phase of Translation
  //===--------------------------------------------------------------------===//
  
  /// ASTStage - Defines what phases of parsing and semantic analysis are
  /// complete for the given AST.  This should only be used for assertions and
  /// verification purposes.
  enum ASTStage_t {
    /// Parsing is underway.
    Parsing,
    /// Parsing has completed.
    Parsed,
    /// Name binding has completed.
    NameBound,
    /// Type checking has completed.
    TypeChecked
  } ASTStage;

protected:
  Module(ModuleKind Kind, Identifier Name, Component *C, ASTContext &Ctx)
  : DeclContext(DeclContextKind::Module, nullptr),
    Kind(Kind), LookupCachePimpl(0),
    Comp(C), Ctx(Ctx), Name(Name), ASTStage(Parsing) {
    assert(Comp != nullptr || Kind == ModuleKind::BuiltinModule);
  }

public:
  typedef ArrayRef<std::pair<Identifier, SourceLoc>> AccessPathTy;
  typedef std::pair<Module::AccessPathTy, Module*> ImportedModule;

  ModuleKind getKind() const { return Kind; }

  Component *getComponent() const {
    assert(Comp && "fetching component for the builtin module");
    return Comp;
  }
  
  /// Look up a (possibly overloaded) value set at top-level scope
  /// (but with the specified access path, which may come from an import decl)
  /// within the current module.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  void lookupValue(AccessPathTy AccessPath, Identifier Name, NLKind LookupKind, 
                   SmallVectorImpl<ValueDecl*> &Result);
  
  /// lookupVisibleDecls - Find ValueDecls in the module and pass them to the
  /// given consumer object.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  void lookupVisibleDecls(AccessPathTy AccessPath,
                          VisibleDeclConsumer &Consumer,
                          NLKind LookupKind) const;

  /// Look for the set of declarations with the given name within a type,
  /// its extensions and, optionally, its supertypes.
  ///
  /// This routine performs name lookup within a given type, its extensions
  /// and, optionally, its supertypes and their extensions. It can eliminate
  /// non-visible, hidden, and overridden declarations from the result set.
  /// It does not, however, perform any filtering based on the semantic
  /// usefulness of the results.
  ///
  /// \param type The type to look into.
  ///
  /// \param name The name to search for.
  ///
  /// \param options Options that control name lookup, based on the
  /// \c NL_* constants in \c NameLookupOptions.
  ///
  /// \param typeResolver Used to resolve types, usually for overload purposes.
  ///
  /// \param[out] decls Will be populated with the declarations found by name
  /// lookup.
  ///
  /// \returns true if anything was found.
  bool lookupQualified(Type type, Identifier name, unsigned options,
                       LazyResolver *typeResolver,
                       SmallVectorImpl<ValueDecl *> &decls);

  /// Look up an InfixOperatorDecl for the given operator
  /// name in this module (which must be NameBound) and return it, or return
  /// null if there is no operator decl. Returns Nothing if there was an error
  /// resolving the operator name (such as if there were conflicting importing
  /// operator declarations).
  Optional<InfixOperatorDecl *> lookupInfixOperator(Identifier name,
                                              SourceLoc diagLoc = SourceLoc());
  
  /// Look up an PrefixOperatorDecl for the given operator
  /// name in this module (which must be NameBound) and return it, or return
  /// null if there is no operator decl. Returns Nothing if there was an error
  /// resolving the operator name (such as if there were conflicting importing
  /// operator declarations).
  Optional<PrefixOperatorDecl *> lookupPrefixOperator(Identifier name,
                                              SourceLoc diagLoc = SourceLoc());
  /// Look up an PostfixOperatorDecl for the given operator
  /// name in this module (which must be NameBound) and return it, or return
  /// null if there is no operator decl. Returns Nothing if there was an error
  /// resolving the operator name (such as if there were conflicting importing
  /// operator declarations).
  Optional<PostfixOperatorDecl *> lookupPostfixOperator(Identifier name,
                                              SourceLoc diagLoc = SourceLoc());

  /// Finds all class members defined in this module.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  void lookupClassMembers(AccessPathTy accessPath,
                          VisibleDeclConsumer &consumer) const;

  /// Finds class members defined in this module with the given name.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  void lookupClassMember(AccessPathTy accessPath,
                         Identifier name,
                         SmallVectorImpl<ValueDecl*> &results) const;

  /// Look for the conformance of the given type to the given protocol.
  ///
  /// This routine determines whether the given \c type conforms to the given
  /// \c protocol. It only looks for explicit conformances (which are
  /// required by the language), and will return a \c ProtocolConformance*
  /// describing the conformance.
  ///
  /// During type-checking, it is possible that this routine will find an
  /// explicit declaration of conformance that has not yet been type-checked,
  /// in which case it will return note the presence of an unchecked
  /// conformance.
  ///
  /// \param type The type for which we are computing conformance.
  ///
  /// \param protocol The protocol to which we are computing conformance.
  ///
  /// \param resolver The lazy resolver.
  ///
  /// \returns The result of the conformance search, with a conformance
  /// structure when possible.
  LookupConformanceResult
  lookupConformance(Type type, ProtocolDecl *protocol, LazyResolver *resolver);

  /// Looks up which modules are re-exported by this module.
  void getImportedModules(SmallVectorImpl<ImportedModule> &modules,
                          bool includePrivate = false) const;

  /// Finds all top-level decls of this module.
  ///
  /// This does a simple local lookup, not recursively looking through imports.
  void getTopLevelDecls(SmallVectorImpl<Decl*> &Results);

  /// Finds all top-level decls that should be displayed to a client of this
  /// module.
  ///
  /// This includes types, variables, functions, and extensions.
  /// This does a simple local lookup, not recursively looking through imports.
  ///
  /// This can differ from \c getTopLevelDecls, e.g. it returns decls from a
  /// shadowed clang module.
  void getDisplayDecls(SmallVectorImpl<Decl*> &results);
  
  /// Perform an action for every module visible from this module.
  ///
  /// For most modules this means any re-exports, but for a translation unit
  /// all imports are considered.
  ///
  /// \param topLevelAccessPath If present, include the top-level module in the
  ///                           results, with the given access path.
  /// \param fn A callback of type bool(ImportedModule). Return \c false to
  ///           abort iteration.
  void forAllVisibleModules(Optional<AccessPathTy> topLevelAccessPath,
                            std::function<bool(ImportedModule)> fn);

  void forAllVisibleModules(Optional<AccessPathTy> topLevelAccessPath,
                            std::function<void(ImportedModule)> fn) {
    forAllVisibleModules(topLevelAccessPath,
                         [=](const ImportedModule &import) -> bool {
      fn(import);
      return true;
    });
  }
  
  template <typename Fn>
  void forAllVisibleModules(Optional<AccessPathTy> topLevelAccessPath,
                            const Fn &fn) {
    using RetTy = typename as_function<Fn>::type::result_type;
    std::function<RetTy(ImportedModule)> wrapped = std::cref(fn);
    forAllVisibleModules(topLevelAccessPath, wrapped);
  }
  
  using LinkLibraryCallback = std::function<void(LinkLibrary)>;

  void collectLinkLibraries(LinkLibraryCallback callback);
  
  template <typename Fn>
  void collectLinkLibraries(const Fn &fn) {
    LinkLibraryCallback wrapped = std::cref(fn);
    collectLinkLibraries(wrapped);
  }
  
  /// Returns true if the two access paths contain the same chain of
  /// identifiers.
  ///
  /// Source locations are ignored here.
  static bool isSameAccessPath(AccessPathTy lhs, AccessPathTy rhs);

  /// \brief Get the path for the file that this module came from, or an empty
  /// string if this is not applicable.
  StringRef getModuleFilename() const;

  /// \returns true if this module is the "swift" standard library module.
  bool isStdlibModule() const;

  static bool classof(const DeclContext *DC) {
    return DC->getContextKind() == DeclContextKind::Module;
  }

private:
  // Make placement new and vanilla new/delete illegal for DeclVarNames.
  void *operator new(size_t Bytes) throw() = delete;
  void operator delete(void *Data) throw() = delete;
  void *operator new(size_t Bytes, void *Mem) throw() = delete;
public:
  // Only allow allocation of Modules using the allocator in ASTContext
  // or by doing a placement new.
  void *operator new(size_t Bytes, ASTContext &C,
                     unsigned Alignment = alignof(Module));
};
  
/// TranslationUnit - This contains information about all of the decls and
/// external references in a translation unit, which is one file.
class TranslationUnit : public Module {
private:
  /// This is the list of modules that are imported by this module, with the
  /// second element of the pair declaring whether the module is reexported.
  ///
  /// This is filled in by the Name Binding phase.
  ArrayRef<std::pair<ImportedModule, bool>> Imports;

  /// The list of libraries specified as link-time dependencies at compile time.
  ArrayRef<LinkLibrary> LinkLibraries;

  /// \brief The buffer ID for the file that was imported as this TU, or -1
  /// if this TU is not an imported TU.
  int ImportBufferID = -1;

public:
  /// Kind - This is the sort of file the translation unit was parsed for, which
  /// can affect some type checking and other behavior.
  enum TUKind {
    Library,
    Main,
    REPL,
    SIL       // Came from a .sil file.
  } Kind;

  /// If this is true, then the translation unit is allowed to access the
  /// Builtin module with an explicit import.
  bool HasBuiltinModuleAccess = false;
  
  /// The list of top-level declarations for a translation unit.
  std::vector<Decl*> Decls;
  
  /// A map of operator names to InfixOperatorDecls.
  /// Populated during name binding; the mapping will be incomplete until name
  /// binding is complete.
  llvm::StringMap<InfixOperatorDecl*> InfixOperators;

  /// A map of operator names to PostfixOperatorDecls.
  /// Populated during name binding; the mapping will be incomplete until name
  /// binding is complete.
  llvm::StringMap<PostfixOperatorDecl*> PostfixOperators;

  /// A map of operator names to PrefixOperatorDecls.
  /// Populated during name binding; the mapping will be incomplete until name
  /// binding is complete.
  llvm::StringMap<PrefixOperatorDecl*> PrefixOperators;

  TranslationUnit(Identifier Name, Component *Comp, ASTContext &C, TUKind Kind)
    : Module(ModuleKind::TranslationUnit, Name, Comp, C), Kind(Kind) {
  }
  
  ArrayRef<std::pair<ImportedModule, bool>> getImports() const {
    assert(ASTStage >= Parsed || Kind == SIL);
    return Imports;
  }
  void setImports(ArrayRef<std::pair<ImportedModule, bool>> IM) {
    Imports = IM;
  }

  void setLinkLibraries(ArrayRef<LinkLibrary> libs) {
    assert(LinkLibraries.empty() && "link libraries already set");
    LinkLibraries = libs;
  }
  ArrayRef<LinkLibrary> getLinkLibraries() const {
    return LinkLibraries;
  }

  void clearLookupCache();

  void cacheVisibleDecls(SmallVectorImpl<ValueDecl *> &&globals) const;
  const SmallVectorImpl<ValueDecl *> &getCachedVisibleDecls() const;

  /// \brief The buffer ID for the file that was imported as this TU, or -1
  /// if this is not an imported TU.
  int getImportBufferID() const { return ImportBufferID; }
  void setImportBufferID(unsigned BufID) {
    assert(ImportBufferID == -1 && "Already set!");
    ImportBufferID = BufID;
  }

  /// \returns true if traversal was aborted, false otherwise.
  bool walk(ASTWalker &Walker);

  void dump() const;
  void dump(raw_ostream &os) const;
    
  /// \brief Pretty-print the entire contents of this translation unit.
  ///
  /// \param os The stream to which the contents will be printed.
  void print(raw_ostream &os);

  /// \brief Pretty-print the contents of this translation unit.
  ///
  /// \param os The stream to which the contents will be printed.
  ///
  /// \param options Options controlling the printing process.
  void print(raw_ostream &os, const PrintOptions &options);

  static bool classof(const Module *M) {
    return M->getKind() == ModuleKind::TranslationUnit;
  }

  static bool classof(const DeclContext *DC) {
    return isa<Module>(DC) && classof(cast<Module>(DC));
  }
};

  
/// BuiltinModule - This module represents the compiler's implicitly generated
/// declarations in the builtin module.
class BuiltinModule : public Module {
public:
  BuiltinModule(Identifier Name, ASTContext &Ctx)
    : Module(ModuleKind::BuiltinModule, Name, nullptr, Ctx) {
    // The Builtin module is always well formed.
    ASTStage = TypeChecked;
  }

  static bool classof(const Module *M) {
    return M->getKind() == ModuleKind::BuiltinModule;
  }
  static bool classof(const DeclContext *DC) {
    return isa<Module>(DC) && classof(cast<Module>(DC));
  }
};


/// Represents a serialized module that has been imported into Swift.
///
/// This may be a Swift module or a Clang module.
class LoadedModule : public Module {
protected:
  friend class Module;

  LoadedModule(ModuleKind Kind, Identifier name,
               std::string DebugModuleName, Component *comp,
               ASTContext &ctx, ModuleLoader &owner)
    : Module(Kind, name, comp, ctx),
      DebugModuleName(DebugModuleName) {
    // Loaded modules are always well-formed.
    ASTStage = TypeChecked;
    LookupCachePimpl = static_cast<void *>(&owner);
  }

  ModuleLoader &getOwner() const {
    return *static_cast<ModuleLoader *>(LookupCachePimpl);
  }

  std::string DebugModuleName;

public:
  /// Look up an operator declaration.
  ///
  /// \param name The operator name ("+", ">>", etc.)
  ///
  /// \param fixity One of PrefixOperator, InfixOperator, or PostfixOperator.
  OperatorDecl *lookupOperator(Identifier name, DeclKind fixity);

  /// Look up an operator declaration.
  template <typename T>
  T *lookupOperator(Identifier name) {
    // Make any non-specialized instantiations fail with a "nice" error message.
    static_assert(static_cast<T*>(nullptr),
                  "Must specify prefix, postfix, or infix operator decl");
  }

  static bool classof(const Module *M) {
    return M->getKind() == ModuleKind::SerializedModule ||
           M->getKind() == ModuleKind::ClangModule;
  }
  static bool classof(const DeclContext *DC) {
    return isa<Module>(DC) && classof(cast<Module>(DC));
  }

  /// \brief Get the debug name for the module.
  const char *getDebugModuleName() const { return DebugModuleName.c_str(); }
};

template <>
PrefixOperatorDecl *
LoadedModule::lookupOperator<PrefixOperatorDecl>(Identifier name);

template <>
PostfixOperatorDecl *
LoadedModule::lookupOperator<PostfixOperatorDecl>(Identifier name);

template <>
InfixOperatorDecl *
LoadedModule::lookupOperator<InfixOperatorDecl>(Identifier name);

} // end namespace swift

namespace llvm {
  template <>
  class DenseMapInfo<swift::Module::ImportedModule> {
    using Module = swift::Module;
  public:
    static Module::ImportedModule getEmptyKey() {
      return {{}, llvm::DenseMapInfo<Module *>::getEmptyKey()};
    }
    static Module::ImportedModule getTombstoneKey() {
      return {{}, llvm::DenseMapInfo<Module *>::getTombstoneKey()};
    }

    static unsigned getHashValue(const Module::ImportedModule &val) {
      auto pair = std::make_pair(val.first.size(), val.second);
      return llvm::DenseMapInfo<decltype(pair)>::getHashValue(pair);
    }

    static bool isEqual(const Module::ImportedModule &lhs,
                        const Module::ImportedModule &rhs) {
      return lhs.second == rhs.second &&
             Module::isSameAccessPath(lhs.first, rhs.first);
    }
  };
}

#endif
