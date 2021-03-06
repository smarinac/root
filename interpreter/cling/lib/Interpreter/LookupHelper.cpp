//------------------------------------------------------------------------------
// CLING - the C++ LLVM-based InterpreterG :)
// author:  Vassil Vassilev <vvasilev@cern.ch>
//
// This file is dual-licensed: you can choose to license it under the University
// of Illinois Open Source License or the GNU Lesser General Public License. See
// LICENSE.TXT for details.
//------------------------------------------------------------------------------

#include "cling/Interpreter/LookupHelper.h"

#include "TransactionUnloader.h"
#include "cling/Interpreter/Interpreter.h"

#include "clang/AST/ASTContext.h"
#include "clang/Parse/Parser.h"
#include "clang/Parse/RAIIObjectsForParser.h"
#include "clang/Sema/Scope.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Overload.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/Template.h"
#include "clang/Sema/TemplateDeduction.h"

using namespace clang;

namespace cling {

  ///\brief Cleanup Parser state after a failed lookup.
  ///
  /// After a failed lookup we need to discard the remaining unparsed input,
  /// restore the original state of the incremental parsing flag, clear any
  /// pending diagnostics, restore the suppress diagnostics flag, and restore
  /// the spell checking language options.
  ///
  class ParserStateRAII {
  private:
    Parser* P;
    Preprocessor& PP;
    bool ResetIncrementalProcessing;
    bool OldSuppressAllDiagnostics;
    bool OldSpellChecking;
    DestroyTemplateIdAnnotationsRAIIObj CleanupTemplateIds;

  public:
    ParserStateRAII(Parser& p)
      : P(&p), PP(p.getPreprocessor()),
        ResetIncrementalProcessing(p.getPreprocessor()
                                   .isIncrementalProcessingEnabled()),
        OldSuppressAllDiagnostics(p.getPreprocessor().getDiagnostics()
                                  .getSuppressAllDiagnostics()),
        OldSpellChecking(p.getPreprocessor().getLangOpts().SpellChecking),
        CleanupTemplateIds(p)
    {
    }

    ~ParserStateRAII()
    {
      //
      // Advance the parser to the end of the file, and pop the include stack.
      //
      // Note: Consuming the EOF token will pop the include stack.
      //
      P->SkipUntil(tok::eof);
      PP.enableIncrementalProcessing(ResetIncrementalProcessing);
      // Doesn't reset the diagnostic mappings
      P->getActions().getDiagnostics().Reset(/*soft=*/true);
      PP.getDiagnostics().setSuppressAllDiagnostics(OldSuppressAllDiagnostics);
      const_cast<LangOptions&>(PP.getLangOpts()).SpellChecking =
         OldSpellChecking;
    }
  };

  ///\brief Class to help with the custom allocation of clang::Expr
  ///
  struct ExprAlloc {
     char fBuffer[sizeof(clang::OpaqueValueExpr)];
  };

  // pin *tor here so that we can have clang::Parser defined and be able to call
  // the dtor on the OwningPtr
  LookupHelper::LookupHelper(clang::Parser* P, Interpreter* interp)
    : m_Parser(P), m_Interpreter(interp) {}

  LookupHelper::~LookupHelper() {}

  QualType LookupHelper::findType(llvm::StringRef typeName,
                                  DiagSetting diagOnOff) const {
    //
    //  Our return value.
    //
    QualType TheQT;

    if (typeName.empty()) return TheQT;

    // Could trigger deserialization of decls.
    Interpreter::PushTransactionRAII RAII(m_Interpreter);

    // Use P for shortness
    Parser& P = *m_Parser;
    ParserStateRAII ResetParserState(P);
    prepareForParsing(typeName, llvm::StringRef("lookup.type.by.name.file"),
                      diagOnOff);
    //
    //  Try parsing the type name.
    //
    clang::ParsedAttributes Attrs(P.getAttrFactory());

    TypeResult Res(P.ParseTypeName(0,Declarator::TypeNameContext,clang::AS_none,
                                   0,&Attrs));
    if (Res.isUsable()) {
      // Accept it only if the whole name was parsed.
      if (P.NextToken().getKind() == clang::tok::eof) {
        TypeSourceInfo* TSI = 0;
        TheQT = clang::Sema::GetTypeFromParser(Res.get(), &TSI);
      }
    }
    return TheQT;
  }

  const Decl* LookupHelper::findScope(llvm::StringRef className,
                                      DiagSetting diagOnOff,
                                      const Type** resultType /* = 0 */,
                                      bool instantiateTemplate/*=true*/) const {
    //
    //  Some utilities.
    //
    // Use P for shortness
    Parser& P = *m_Parser;
    Sema& S = P.getActions();
    Preprocessor& PP = P.getPreprocessor();
    ASTContext& Context = S.getASTContext();

    // The user wants to see the template instantiation, existing or not.
    // Here we might not have an active transaction to handle
    // the caused instantiation decl.
    Interpreter::PushTransactionRAII pushedT(m_Interpreter);

    ParserStateRAII ResetParserState(P);
    prepareForParsing(className.str() + "::",
                      llvm::StringRef("lookup.class.by.name.file"), diagOnOff);
    //
    //  Our return values.
    //
    const Type* TheType = 0;
    const Type** setResultType = &TheType;
    if (resultType)
      setResultType = resultType;
    *setResultType = 0;

    Decl* TheDecl = 0;

    //
    //  Prevent failing on an assert in TryAnnotateCXXScopeToken.
    //
    if (!P.getCurToken().is(clang::tok::identifier)
        && !P.getCurToken().is(clang::tok::coloncolon)
        && !(P.getCurToken().is(clang::tok::annot_template_id)
             && P.NextToken().is(clang::tok::coloncolon))
        && !P.getCurToken().is(clang::tok::kw_decltype)) {
      // error path
      return TheDecl;
    }
    //
    //  Try parsing the name as a nested-name-specifier.
    //
    if (P.TryAnnotateCXXScopeToken(false)) {
      // error path
      return TheDecl;
    }
    if (P.getCurToken().getKind() == tok::annot_cxxscope) {
      CXXScopeSpec SS;
      S.RestoreNestedNameSpecifierAnnotation(P.getCurToken().getAnnotationValue(),
                                             P.getCurToken().getAnnotationRange(),
                                             SS);
      if (SS.isValid()) {
        NestedNameSpecifier* NNS = SS.getScopeRep();
        NestedNameSpecifier::SpecifierKind Kind = NNS->getKind();
        // Only accept the parse if we consumed all of the name.
        if (P.NextToken().getKind() == clang::tok::eof) {
          //
          //  Be careful, not all nested name specifiers refer to classes
          //  and namespaces, and those are the only things we want.
          //
          switch (Kind) {
            case NestedNameSpecifier::Identifier: {
                // Dependent type.
                // We do not accept these.
              }
              break;
            case NestedNameSpecifier::Namespace: {
                // Namespace.
                NamespaceDecl* NSD = NNS->getAsNamespace();
                NSD = NSD->getCanonicalDecl();
                TheDecl = NSD;
              }
              break;
            case NestedNameSpecifier::NamespaceAlias: {
                // Namespace alias.
                // Note: In the future, should we return the alias instead?
                NamespaceAliasDecl* NSAD = NNS->getAsNamespaceAlias();
                NamespaceDecl* NSD = NSAD->getNamespace();
                NSD = NSD->getCanonicalDecl();
                TheDecl = NSD;
              }
              break;
            case NestedNameSpecifier::TypeSpec:
                // Type name.
                // Intentional fall-though
            case NestedNameSpecifier::TypeSpecWithTemplate: {
                // Type name qualified with "template".
                // Note: Do we need to check for a dependent type here?
                NestedNameSpecifier *prefix = NNS->getPrefix();
                if (prefix) {
                   QualType temp
                     = Context.getElaboratedType(ETK_None,prefix,
                                                 QualType(NNS->getAsType(),0));
                   *setResultType = temp.getTypePtr();
                } else {
                   *setResultType = NNS->getAsType();
                }
                const TagType* TagTy = (*setResultType)->getAs<TagType>();
                if (TagTy) {
                  // It is a class, struct, or union.
                  TagDecl* TD = TagTy->getDecl();
                  if (TD) {
                    TheDecl = TD->getDefinition();
                    if (!TheDecl && instantiateTemplate) {

                      // Make sure it is not just forward declared, and
                      // instantiate any templates.
                      if (!S.RequireCompleteDeclContext(SS, TD)) {
                        // Success, type is complete, instantiations have
                        // been done.
                        TheDecl = TD->getDefinition();
                        if (TheDecl->isInvalidDecl()) {
                          // if the decl is invalid try to clean up
                          TransactionUnloader U(&S, /*CodeGenerator*/0,
                                                /*ExecutionEngine*/0);
                          U.UnloadDecl(TheDecl);
                          return 0;
                        }
                      } else {
                        // We cannot instantiate the scope: not a valid decl.
                        return 0;
                      }
                    }
                  }
                }
              }
              break;
            case clang::NestedNameSpecifier::Global: {
                // Name was just "::" and nothing more.
                TheDecl = Context.getTranslationUnitDecl();
              }
              break;
          }
          return TheDecl;
        }
      }
    }
    //
    //  Cleanup after failed parse as a nested-name-specifier.
    //
    P.SkipUntil(clang::tok::eof);
    // Doesn't reset the diagnostic mappings
    S.getDiagnostics().Reset(/*soft=*/true);
    //
    //  Setup to reparse as a type.
    //

    llvm::MemoryBuffer* SB =
      llvm::MemoryBuffer::getMemBufferCopy(className.str() + "\n",
                                           "lookup.type.file");
    clang::FileID FID = S.getSourceManager().createFileIDForMemBuffer(SB);
    PP.EnterSourceFile(FID, 0, clang::SourceLocation());
    PP.Lex(const_cast<clang::Token&>(P.getCurToken()));

    //
    //  Now try to parse the name as a type.
    //
    if (P.TryAnnotateTypeOrScopeToken(false, false)) {
      // error path
      return TheDecl;
    }
    if (P.getCurToken().getKind() == tok::annot_typename) {
      ParsedType T = P.getTypeAnnotation(const_cast<Token&>(P.getCurToken()));
      // Only accept the parse if we consumed all of the name.
      if (P.NextToken().getKind() == clang::tok::eof)
        if (!T.get().isNull()) {
          TypeSourceInfo *TSI = 0;
          clang::QualType QT = clang::Sema::GetTypeFromParser(T, &TSI);
          if (const TagType* TT = QT->getAs<TagType>()) {
            TheDecl = TT->getDecl()->getDefinition();
            *setResultType = QT.getTypePtr();
          }
        }
    }
    return TheDecl;
  }

  const ClassTemplateDecl* LookupHelper::findClassTemplate(llvm::StringRef Name,
                                                           DiagSetting diagOnOff) const {
    //
    //  Find a class template decl given its name.
    //

    if (Name.empty()) return 0;

    // Humm ... this seems to do the trick ... or does it? or is there a better way?

    // Use P for shortness
    Parser& P = *m_Parser;
    Sema& S = P.getActions();
    ASTContext& Context = S.getASTContext();
    ParserStateRAII ResetParserState(P);
    prepareForParsing(Name.str(),
                      llvm::StringRef("lookup.class.by.name.file"), diagOnOff);

    //
    //  Prevent failing on an assert in TryAnnotateCXXScopeToken.
    //
    if (!P.getCurToken().is(clang::tok::identifier)
        && !P.getCurToken().is(clang::tok::coloncolon)
        && !(P.getCurToken().is(clang::tok::annot_template_id)
             && P.NextToken().is(clang::tok::coloncolon))
        && !P.getCurToken().is(clang::tok::kw_decltype)) {
      // error path
      return 0;
    }

    //
    //  Now try to parse the name as a type.
    //
    if (P.TryAnnotateTypeOrScopeToken(false, false)) {
      // error path
      return 0;
    }
    DeclContext *where = 0;
    if (P.getCurToken().getKind() == tok::annot_cxxscope) {
      CXXScopeSpec SS;
      S.RestoreNestedNameSpecifierAnnotation(P.getCurToken().getAnnotationValue(),
                                             P.getCurToken().getAnnotationRange(),
                                             SS);
      if (SS.isValid()) {
        P.ConsumeToken();
        if (!P.getCurToken().is(clang::tok::identifier)) {
          return 0;
        }
        NestedNameSpecifier *nested = SS.getScopeRep();
        if (!nested) return 0;
        switch (nested->getKind()) {
        case NestedNameSpecifier::Global:
          where = Context.getTranslationUnitDecl();
          break;
        case NestedNameSpecifier::Namespace:
          where = nested->getAsNamespace();
          break;
        case NestedNameSpecifier::NamespaceAlias:
        case NestedNameSpecifier::Identifier:
           return 0;
        case NestedNameSpecifier::TypeSpec:
        case NestedNameSpecifier::TypeSpecWithTemplate:
          {
            const Type *ntype = nested->getAsType();
            where = ntype->getAsCXXRecordDecl();
            if (!where) return 0;
            break;
          }
        };
      }
    } else if (P.getCurToken().is(clang::tok::identifier)) {
      // We have a single indentifier, let's look for it in the
      // the global scope.
      where = Context.getTranslationUnitDecl();
    }
    if (where) {
      // Great we now have a scope and something to search for,let's go ahead.
      DeclContext::lookup_result R
        = where->lookup(P.getCurToken().getIdentifierInfo());
      for (DeclContext::lookup_iterator I = R.begin(), E = R.end();
           I != E; ++I) {
        ClassTemplateDecl *theDecl = dyn_cast<ClassTemplateDecl>(*I);
        if (theDecl)
          return theDecl;
      }
    }
    return 0;
  }

  const ValueDecl* LookupHelper::findDataMember(const clang::Decl* scopeDecl,
                                                llvm::StringRef dataName,
                                                DiagSetting diagOnOff) const {
    // Lookup a data member based on its Decl(Context), name.

    Parser& P = *m_Parser;
    Sema& S = P.getActions();
    Preprocessor& PP = S.getPreprocessor();

    IdentifierInfo *dataII = &PP.getIdentifierTable().get(dataName);
    DeclarationName decl_name( dataII );

    const clang::DeclContext *dc = llvm::cast<clang::DeclContext>(scopeDecl);

    DeclContext::lookup_result lookup = const_cast<clang::DeclContext*>(dc)->lookup(decl_name);
    for (DeclContext::lookup_iterator I = lookup.begin(), E = lookup.end();
         I != E; ++I) {
      const ValueDecl *result = dyn_cast<ValueDecl>(*I);
      if (result && !isa<FunctionDecl>(result))
        return result;
    }

    return 0;
  }

  static
  DeclContext* getContextAndSpec(CXXScopeSpec &SS,
                                 const Decl* scopeDecl,
                                 ASTContext& Context, Sema &S) {
    //
    //  Convert the passed decl into a nested name specifier,
    //  a scope spec, and a decl context.
    //
    NestedNameSpecifier* classNNS = 0;
    if (const NamespaceDecl* NSD = dyn_cast<NamespaceDecl>(scopeDecl)) {
      classNNS = NestedNameSpecifier::Create(Context, 0,
                                             const_cast<NamespaceDecl*>(NSD));
    }
    else if (const RecordDecl* RD = dyn_cast<RecordDecl>(scopeDecl)) {
      const Type* T = Context.getRecordType(RD).getTypePtr();
      classNNS = NestedNameSpecifier::Create(Context, 0, false, T);
    }
    else if (llvm::isa<TranslationUnitDecl>(scopeDecl)) {
      classNNS = NestedNameSpecifier::GlobalSpecifier(Context);
    }
    else {
      // Not a namespace or class, we cannot use it.
      return 0;
    }
    DeclContext* foundDC = dyn_cast<DeclContext>(const_cast<Decl*>(scopeDecl));
    //
    //  Some validity checks on the passed decl.
    //
    if (foundDC->isDependentContext()) {
      // Passed decl is a template, we cannot use it.
      return 0;
    }
    SS.MakeTrivial(Context, classNNS, SourceRange());
    if (S.RequireCompleteDeclContext(SS, foundDC)) {
      // Forward decl or instantiation failure, we cannot use it.
      return 0;
    }
    if (scopeDecl->isInvalidDecl()) {
      // if the decl is invalid try to clean up
      TransactionUnloader U(&S, /*CodeGenerator*/0, /*ExecutionEngine*/0);
      U.UnloadDecl(const_cast<Decl*>(scopeDecl));
      return 0;
    }

    return foundDC;
  }

  static bool FuncArgTypesMatch(const ASTContext& C,
                                const llvm::SmallVectorImpl<Expr*> &GivenArgs,
                                const FunctionProtoType* FPT) {
    // FIXME: What if FTP->arg_size() != GivenArgTypes.size()?
    FunctionProtoType::param_type_iterator ATI = FPT->param_type_begin();
    FunctionProtoType::param_type_iterator E = FPT->param_type_end();
    llvm::SmallVectorImpl<Expr*>::const_iterator GAI = GivenArgs.begin();
    for (; ATI && (ATI != E); ++ATI, ++GAI) {
      if ((*GAI)->isLValue()) {
        // If the user specified a reference we may have transform it into
        // an LValue non reference (See getExprProto) to have it in a form
        // useful for the lookup.  So we are a bit sloppy per se here (maybe)
        const ReferenceType *RefType = (*ATI)->getAs<ReferenceType>();
        if (RefType) {
          if (!C.hasSameType(RefType->getPointeeType(),(*GAI)->getType()))
            return false;
        } else if (!C.hasSameType(*ATI,(*GAI)->getType())) {
          return false;
        }
      } else if (!C.hasSameType(*ATI, (*GAI)->getType() )) {
        return false;
      }
    }
    return true;
  }

  static bool IsOverload(const ASTContext& C,
                         const TemplateArgumentListInfo* FuncTemplateArgs,
                         const llvm::SmallVectorImpl<Expr*> &GivenArgs,
                         const FunctionDecl* FD) {

    //FunctionTemplateDecl* FTD = FD->getDescribedFunctionTemplate();
    QualType FQT = C.getCanonicalType(FD->getType());
    if (llvm::isa<FunctionNoProtoType>(FQT.getTypePtr())) {
      // A K&R-style function (no prototype), is considered to match the args.
      return false;
    }
    const FunctionProtoType* FPT = llvm::cast<FunctionProtoType>(FQT);
    if ((GivenArgs.size() != FPT->getNumParams()) ||
        //(GivenArgsAreEllipsis != FPT->isVariadic()) ||
        !FuncArgTypesMatch(C, GivenArgs, FPT)) {
      return true;
    }
    return false;
  }

  static
  const FunctionDecl* overloadFunctionSelector(DeclContext* foundDC,
                                               bool objectIsConst,
                                  const llvm::SmallVectorImpl<Expr*> &GivenArgs,
                                     LookupResult &Result,
                                     DeclarationNameInfo &FuncNameInfo,
                              const TemplateArgumentListInfo* FuncTemplateArgs,
                                     ASTContext& Context, Parser &P, Sema &S) {
    //
    //  Our return value.
    //
    FunctionDecl* TheDecl = 0;

    //
    //  If we are looking up a member function, construct
    //  the implicit object argument.
    //
    //  Note: For now this is always a non-CV qualified lvalue.
    //
    QualType ClassType;
    Expr::Classification ObjExprClassification;
    if (CXXRecordDecl* CRD = dyn_cast<CXXRecordDecl>(foundDC)) {
      if (objectIsConst)
        ClassType = Context.getTypeDeclType(CRD).getCanonicalType().withConst();
      else ClassType = Context.getTypeDeclType(CRD).getCanonicalType();
      OpaqueValueExpr ObjExpr(SourceLocation(),
                              ClassType, VK_LValue);
      ObjExprClassification = ObjExpr.Classify(Context);
    }

    //
    //  Construct the overload candidate set.
    //
    OverloadCandidateSet Candidates(FuncNameInfo.getLoc());
    for (LookupResult::iterator I = Result.begin(), E = Result.end();
         I != E; ++I) {
      NamedDecl* ND = *I;
      if (FunctionDecl* FD = dyn_cast<FunctionDecl>(ND)) {
        if (isa<CXXMethodDecl>(FD) &&
            !cast<CXXMethodDecl>(FD)->isStatic() &&
            !isa<CXXConstructorDecl>(FD)) {
          // Class method, not static, not a constructor, so has
          // an implicit object argument.
          CXXMethodDecl* MD = cast<CXXMethodDecl>(FD);
          if (FuncTemplateArgs && (FuncTemplateArgs->size() != 0)) {
            // Explicit template args were given, cannot use a plain func.
            continue;
          }
          S.AddMethodCandidate(MD, I.getPair(), MD->getParent(),
                               /*ObjectType=*/ClassType,
                               /*ObjectClassification=*/ObjExprClassification,
                   llvm::makeArrayRef<Expr*>(GivenArgs.data(), GivenArgs.size()),
                                   Candidates);
        }
        else {
          const FunctionProtoType* Proto = dyn_cast<FunctionProtoType>(
            FD->getType()->getAs<clang::FunctionType>());
          if (!Proto) {
            // Function has no prototype, cannot do overloading.
            continue;
          }
          if (FuncTemplateArgs && (FuncTemplateArgs->size() != 0)) {
            // Explicit template args were given, cannot use a plain func.
             continue;
          }
          S.AddOverloadCandidate(FD, I.getPair(),
                   llvm::makeArrayRef<Expr*>(GivenArgs.data(), GivenArgs.size()),
                                 Candidates);
        }
      }
      else if (FunctionTemplateDecl* FTD =
               dyn_cast<FunctionTemplateDecl>(ND)) {
        if (isa<CXXMethodDecl>(FTD->getTemplatedDecl()) &&
            !cast<CXXMethodDecl>(FTD->getTemplatedDecl())->isStatic() &&
            !isa<CXXConstructorDecl>(FTD->getTemplatedDecl())) {
          // Class method template, not static, not a constructor, so has
          // an implicit object argument.
          S.AddMethodTemplateCandidate(FTD, I.getPair(),
                                      cast<CXXRecordDecl>(FTD->getDeclContext()),
                         const_cast<TemplateArgumentListInfo*>(FuncTemplateArgs),
                                       /*ObjectType=*/ClassType,
                                  /*ObjectClassification=*/ObjExprClassification,
                   llvm::makeArrayRef<Expr*>(GivenArgs.data(), GivenArgs.size()),
                                       Candidates);
        }
        else {
          S.AddTemplateOverloadCandidate(FTD, I.getPair(),
                const_cast<TemplateArgumentListInfo*>(FuncTemplateArgs),
                llvm::makeArrayRef<Expr*>(GivenArgs.data(), GivenArgs.size()),
                Candidates, /*SuppressUserConversions=*/false);
        }
      }
      else {
        // Is there any other cases?
      }
    }
    //
    //  Find the best viable function from the set.
    //
    {
       OverloadCandidateSet::iterator Best;
       OverloadingResult OR = Candidates.BestViableFunction(S,
                                                            Result.getNameLoc(),
                                                            Best);
       if (OR == OR_Success) {
          TheDecl = Best->Function;
          // We prefer to get the canonical decl for consistency and ease
          // of comparison.
          TheDecl = TheDecl->getCanonicalDecl();
          if (TheDecl->isTemplateInstantiation() && !TheDecl->isDefined())
            S.InstantiateFunctionDefinition(SourceLocation(), TheDecl,
                                            true /*recursive instantiation*/);
          if (TheDecl->isInvalidDecl()) {
            // if the decl is invalid try to clean up
            TransactionUnloader U(&S, /*CodeGenerator*/0, /*ExecutionEngine*/0);
            U.UnloadDecl(const_cast<FunctionDecl*>(TheDecl));
            return 0;
          }
       }
    }
    return TheDecl;
  }

  static
  const FunctionDecl* matchFunctionSelector(DeclContext* foundDC,
                                            bool objectIsConst,
                                  const llvm::SmallVectorImpl<Expr*> &GivenArgs,
                                     LookupResult &Result,
                                     DeclarationNameInfo &FuncNameInfo,
                              const TemplateArgumentListInfo* FuncTemplateArgs,
                                     ASTContext& Context, Parser &P, Sema &S) {
    //
    //  Our return value.
    //
    const FunctionDecl* TheDecl = overloadFunctionSelector(foundDC, objectIsConst,
                                                           GivenArgs, Result,
                                                           FuncNameInfo,
                                                           FuncTemplateArgs,
                                                           Context,P,S);

    if (TheDecl) {
      if ( IsOverload(Context, FuncTemplateArgs, GivenArgs, TheDecl) ) {
        return 0;
      } else {
        // Double check const-ness.
        if (const clang::CXXMethodDecl *md =
            llvm::dyn_cast<clang::CXXMethodDecl>(TheDecl)) {
          if (md->getTypeQualifiers() & clang::Qualifiers::Const) {
            if (!objectIsConst) {
              TheDecl = 0;
            }
          } else {
            if (objectIsConst) {
              TheDecl = 0;
            }
          }
        }
      }
    }
    return TheDecl;
  }

  static bool ParseWithShortcuts(DeclContext* foundDC, CXXScopeSpec &SS,
                                 llvm::StringRef funcName,
                                 Parser &P, Sema &S,
                                 UnqualifiedId &FuncId,
                                 LookupHelper::DiagSetting diagOnOff) {

    // Use a very simple parse step that dectect whether the name search (which
    // is already supposed to be an unqualified name) is a simple identifier,
    // a constructor name or a destructor name.  In those 3 cases, we can easily
    // create the UnqualifiedId object that would have resulted from the 'real'
    // parse.  By using this direct creation of the UnqualifiedId, we avoid the
    // 'permanent' cost associated with creating a memory buffer and the
    // associated FileID.

    // If the name is a template or an operator, we revert to the regular parse
    // (and its associated permanent cost).

    // In the operator case, the additional work is in the case of a conversion
    // operator where we would need to 'quickly' parse the type itself (if want
    // to avoid the permanent cost).

    // In the case with the template the problem gets a bit worse as we need to
    // handle potentially arbitrary spaces and ordering
    // ('const int' vs 'int  const', etc.)

    if (funcName.size() == 0) return false;
    Preprocessor& PP = S.getPreprocessor();

    // See if we can avoid creating the buffer, for now we just look for
    // simple indentifier, constructor and destructor.


    if (funcName.size() > 8 && strncmp(funcName.data(),"operator",8) == 0
               &&(   funcName[8] == ' ' || funcName[8] == '*'
                  || funcName[8] == '%' || funcName[8] == '&'
                  || funcName[8] == '|' || funcName[8] == '/'
                  || funcName[8] == '+' || funcName[8] == '-'
                  || funcName[8] == '(' || funcName[8] == '['
                  || funcName[8] == '=' || funcName[8] == '!'
                  || funcName[8] == '<' || funcName[8] == '>'
                  || funcName[8] == '-' || funcName[8] == '^')
               ) {
      // We have called:
      //   setOperatorFunctionId (SourceLocation OperatorLoc,
      //                          OverloadedOperatorKind Op,
      //                          SourceLocation SymbolLocations[3])
      // or
      //   setConversionFunctionId (SourceLocation OperatorLoc,
      //                            ParsedType Ty, SourceLocation EndLoc)
    } else if (funcName.find('<') != StringRef::npos) {
      // We might have a template name,
      //   setTemplateId (TemplateIdAnnotation *TemplateId)
      // or
      //   setConstructorTemplateId (TemplateIdAnnotation *TemplateId)
    } else if (funcName[0] == '~') {
       // Destructor.
       // Let's see if this is our contructor.
       TagDecl *decl = llvm::dyn_cast<TagDecl>(foundDC);
       if (decl) {
          // We have a class or struct or something.
          if (funcName.substr(1).equals(decl->getName())) {
             ParsedType PT;
             QualType QT( decl->getTypeForDecl(), 0 );
             PT.set(QT);
             FuncId.setDestructorName(SourceLocation(),PT,SourceLocation());
             return true;
          }
       }
       // So it starts with ~ but is not followed by the name of
       // a class or at least not the one that is the declaration context,
       // let's try a real parsing, to see if we can do better.
    } else {
       // We either have a simple type or a constructor name
       TagDecl *decl = llvm::dyn_cast<TagDecl>(foundDC);
       if (decl) {
          // We have a class or struct or something.
          if (funcName.equals(decl->getName())) {
             ParsedType PT;
             QualType QT( decl->getTypeForDecl(), 0 );
             PT.set(QT);
             FuncId.setConstructorName(PT,SourceLocation(),SourceLocation());
          } else {
             IdentifierInfo *TypeInfoII = &PP.getIdentifierTable().get(funcName);
             FuncId.setIdentifier (TypeInfoII, SourceLocation() );
          }
          return true;
       } else {
          // We have a namespace like context, it can't be a constructor
          IdentifierInfo *TypeInfoII = &PP.getIdentifierTable().get(funcName);
          FuncId.setIdentifier (TypeInfoII, SourceLocation() );
          return true;
       }
    }

    //
    //  Setup to reparse as a type.
    //
    //
    //  Create a fake file to parse the function name.
    //
    // FIXME:, TODO: Cleanup that complete mess.
    {
      PP.getDiagnostics().setSuppressAllDiagnostics(diagOnOff ==
                                                   LookupHelper::NoDiagnostics);
      llvm::MemoryBuffer* SB
           = llvm::MemoryBuffer::getMemBufferCopy(funcName.str()
                                                + "\n", "lookup.funcname.file");
      clang::FileID FID = S.getSourceManager().createFileIDForMemBuffer(SB);
      PP.EnterSourceFile(FID, /*DirLookup=*/0, clang::SourceLocation());
      PP.Lex(const_cast<clang::Token&>(P.getCurToken()));
    }


    //
    //  Parse the function name.
    //
    SourceLocation TemplateKWLoc;
    if (P.ParseUnqualifiedId(SS, /*EnteringContext*/false,
                             /*AllowDestructorName*/true,
                             /*AllowConstructorName*/true,
                             ParsedType(), TemplateKWLoc,
                             FuncId)) {
      // Failed parse, cleanup.
      return false;
    }
    return true;
  }


  template <typename T>
  T findFunction(DeclContext* foundDC, CXXScopeSpec &SS,
                 llvm::StringRef funcName,
                 const llvm::SmallVectorImpl<Expr*> &GivenArgs,
                 bool objectIsConst,
                 ASTContext& Context, Parser &P, Sema &S,
                 T (*functionSelector)(DeclContext* foundDC,
                                       bool objectIsConst,
                                  const llvm::SmallVectorImpl<Expr*> &GivenArgs,
                                       LookupResult &Result,
                                       DeclarationNameInfo &FuncNameInfo,
                              const TemplateArgumentListInfo* FuncTemplateArgs,
                                       ASTContext& Context, Parser &P, Sema &S),
                 LookupHelper::DiagSetting diagOnOff
                 ) {
    // Given the correctly types arguments, etc. find the function itself.

    //
    //  Make the class we are looking up the function
    //  in the current scope to please the constructor
    //  name lookup.  We do not need to do this otherwise,
    //  and may be able to remove it in the future if
    //  the way constructors are looked up changes.
    //
    DeclContext* OldEntity = P.getCurScope()->getEntity();
    DeclContext* TUCtx = Context.getTranslationUnitDecl();
    P.getCurScope()->setEntity(TUCtx);
    P.EnterScope(Scope::DeclScope);
    P.getCurScope()->setEntity(foundDC);
    P.EnterScope(Scope::DeclScope);
    Sema::ContextRAII SemaContext(S, foundDC);
    S.EnterDeclaratorContext(P.getCurScope(), foundDC);

    UnqualifiedId FuncId;
    ParserStateRAII ResetParserState(P);
    if (!ParseWithShortcuts(foundDC,SS,funcName,P,S,FuncId, diagOnOff)) {
      // Failed parse, cleanup.
      // Destroy the scope we created first, and
      // restore the original.
      S.ExitDeclaratorContext(P.getCurScope());
      P.ExitScope();
      P.ExitScope();
      P.getCurScope()->setEntity(OldEntity);
      // Then exit.
      return 0;
    }

    //
    //  Get any template args in the function name.
    //
    TemplateArgumentListInfo FuncTemplateArgsBuffer;
    DeclarationNameInfo FuncNameInfo;
    const TemplateArgumentListInfo* FuncTemplateArgs;
    S.DecomposeUnqualifiedId(FuncId, FuncTemplateArgsBuffer, FuncNameInfo,
                             FuncTemplateArgs);

    //
    //  Lookup the function name in the given class now.
    //
    DeclarationName FuncName = FuncNameInfo.getName();
    SourceLocation FuncNameLoc = FuncNameInfo.getLoc();
    LookupResult Result(S, FuncName, FuncNameLoc, Sema::LookupMemberName,
                        Sema::NotForRedeclaration);
    Result.suppressDiagnostics();
    if (!S.LookupQualifiedName(Result, foundDC)) {
      // Lookup failed.
      // Destroy the scope we created first, and
      // restore the original.
      S.ExitDeclaratorContext(P.getCurScope());
      P.ExitScope();
      P.ExitScope();
      P.getCurScope()->setEntity(OldEntity);
      // Then cleanup and exit.
      return 0;
    }

    //
    //  Destroy the scope we created, and restore the original.
    //
    S.ExitDeclaratorContext(P.getCurScope());
    P.ExitScope();
    P.ExitScope();
    P.getCurScope()->setEntity(OldEntity);
    //
    //  Check for lookup failure.
    //
    if (Result.getResultKind() != LookupResult::Found &&
        Result.getResultKind() != LookupResult::FoundOverloaded) {
       // Lookup failed.
       return 0;
    }
    return functionSelector(foundDC,objectIsConst,GivenArgs,
                            Result,
                            FuncNameInfo,
                            FuncTemplateArgs,
                            Context, P, S);
  }

  static
  bool getExprProto(llvm::SmallVectorImpl<ExprAlloc> &ExprMemory,
                    llvm::SmallVectorImpl<Expr*> &GivenArgs,
                    const llvm::SmallVectorImpl<QualType> &GivenTypes) {
    //
    //  Create the array of Expr from the array of Types.
    //

    typedef llvm::SmallVectorImpl<QualType>::const_iterator iterator;
    for(iterator iter = GivenTypes.begin(), end = GivenTypes.end();
        iter != end;
        ++iter) {
      const clang::QualType QT = iter->getCanonicalType();
      {
        ExprValueKind VK = VK_RValue;
        if (QT->getAs<LValueReferenceType>()) {
          VK = VK_LValue;
        }
        clang::QualType NonRefQT(QT.getNonReferenceType());
        unsigned int slot = ExprMemory.size();
        ExprMemory.resize(slot+1);
        Expr* val = new (&ExprMemory[slot]) OpaqueValueExpr(SourceLocation(),
                                                            NonRefQT, VK);
        GivenArgs.push_back(val);
      }
    }
    return true;
  }

  static
  bool ParseProto(llvm::SmallVectorImpl<ExprAlloc> &ExprMemory,
                  llvm::SmallVectorImpl<Expr*> &GivenArgs,
                  ASTContext& Context, Parser &P,Sema &S) {
    //
    //  Parse the prototype now.
    //

    unsigned int nargs = 0;
    while (P.getCurToken().isNot(tok::eof)) {
      TypeResult Res(P.ParseTypeName());
      if (!Res.isUsable()) {
        // Bad parse, done.
        return false;
      }
      TypeSourceInfo *TSI = 0;
      clang::QualType QT = clang::Sema::GetTypeFromParser(Res.get(), &TSI);
      QT = QT.getCanonicalType();
      {
        ExprValueKind VK = VK_RValue;
        if (QT->getAs<LValueReferenceType>()) {
          VK = VK_LValue;
        }
        clang::QualType NonRefQT(QT.getNonReferenceType());
        ExprMemory.resize(++nargs);
        new (&ExprMemory[nargs-1]) OpaqueValueExpr(TSI->getTypeLoc().getLocStart(),
                                                   NonRefQT, VK);
      }
      // Type names should be comma separated.
      // FIXME: Here if we have type followed by name won't work. Eg int f, ...
      if (!P.getCurToken().is(clang::tok::comma)) {
        break;
      }
      // Eat the comma.
      P.ConsumeToken();
    }
    for(unsigned int slot = 0; slot < nargs; ++slot) {
       Expr* val = (OpaqueValueExpr*)( &ExprMemory[slot] );
       GivenArgs.push_back(val);
    }
    if (P.getCurToken().isNot(tok::eof)) {
      // We did not consume all of the prototype, bad parse.
      return false;
    }
    //
    //  Cleanup after prototype parse.
    //
    P.SkipUntil(clang::tok::eof);
    // Doesn't reset the diagnostic mappings
    S.getDiagnostics().Reset(/*soft=*/true);

    return true;
  }

  static
  const FunctionTemplateDecl* findFunctionTemplateSelector(DeclContext* ,
                                                       bool /* objectIsConst */,
                                            const llvm::SmallVectorImpl<Expr*> &,
                                                           LookupResult &Result,
                                                          DeclarationNameInfo &,
                           const TemplateArgumentListInfo* ExplicitTemplateArgs,
                                                          ASTContext&, Parser &,
                                                           Sema &S) {
    //
    //  Check for lookup failure.
    //
    if (Result.empty())
      return 0;
    if (Result.isSingleResult())
      return dyn_cast<FunctionTemplateDecl>(Result.getFoundDecl());
    else {
      for (LookupResult::iterator I = Result.begin(), E = Result.end();
           I != E; ++I) {
        NamedDecl* ND = *I;
        FunctionTemplateDecl *MethodTmpl =dyn_cast<FunctionTemplateDecl>(ND);
        if (MethodTmpl) {
          return MethodTmpl;
        }
      }
      return 0;
    }
  }

  const FunctionTemplateDecl*
  LookupHelper::findFunctionTemplate(const clang::Decl* scopeDecl,
                                     llvm::StringRef templateName,
                                     DiagSetting diagOnOff,
                                     bool objectIsConst) const {
    // Lookup a function template based on its Decl(Context), name.

    //FIXME: remove code duplication with findFunctionArgs() and friends.

    assert(scopeDecl && "Decl cannot be null");
    //
    //  Some utilities.
    //
    Parser& P = *m_Parser;
    Sema& S = P.getActions();
    ASTContext& Context = S.getASTContext();

    //
    //  Convert the passed decl into a nested name specifier,
    //  a scope spec, and a decl context.
    //
    //  Do this 'early' to save on the expansive parser setup,
    //  in case of failure.
    //
    CXXScopeSpec SS;
    DeclContext* foundDC = getContextAndSpec(SS,scopeDecl,Context,S);
    if (!foundDC) return 0;

    ParserStateRAII ResetParserState(P);
    llvm::SmallVector<Expr*, 4> GivenArgs;

    Interpreter::PushTransactionRAII pushedT(m_Interpreter);
    return findFunction(foundDC, SS,
                        templateName, GivenArgs, objectIsConst,
                        Context, P, S, findFunctionTemplateSelector,
                        diagOnOff);
  }

  static
  const FunctionDecl* findAnyFunctionSelector(DeclContext* ,
                           bool /* objectIsConst */,
                           const llvm::SmallVectorImpl<Expr*> &,
                           LookupResult &Result,
                           DeclarationNameInfo &,
                           const TemplateArgumentListInfo* ExplicitTemplateArgs,
                           ASTContext&, Parser &, Sema &S) {
    //
    //  Check for lookup failure.
    //
    if (Result.empty())
      return 0;
    if (Result.isSingleResult())
      return dyn_cast<FunctionDecl>(Result.getFoundDecl());
    else {
      NamedDecl *aResult = *(Result.begin());
      FunctionDecl *res = dyn_cast<FunctionDecl>(aResult);
      if (res) return res;
      FunctionTemplateDecl *MethodTmpl =dyn_cast<FunctionTemplateDecl>(aResult);
      if (MethodTmpl) {
        if (!ExplicitTemplateArgs || ExplicitTemplateArgs->size()==0) {
          // Not argument was specified, any instantiation will do.

          if (MethodTmpl->spec_begin() != MethodTmpl->spec_end()) {
             return *( MethodTmpl->spec_begin() );
          }
        }
        // pick a specialization that result match the given arguments
        SourceLocation loc;
        sema::TemplateDeductionInfo Info(loc);
        FunctionDecl *fdecl = 0;
        Sema::TemplateDeductionResult Result
          = S.DeduceTemplateArguments(MethodTmpl,
                    const_cast<TemplateArgumentListInfo*>(ExplicitTemplateArgs),
                                      fdecl,
                                      Info);
        if (Result) {
          // Deduction failure.
          return 0;
        } else {
          // Instantiate the function if needed.
          if (!fdecl->isDefined())
            S.InstantiateFunctionDefinition(loc, fdecl,
                                            true /*recursive instantiation*/);
          if (fdecl->isInvalidDecl()) {
            // if the decl is invalid try to clean up
            TransactionUnloader U(&S, /*CodeGenerator*/0, /*ExecutionEngine*/0);
            U.UnloadDecl(fdecl);
            return 0;
          }
          return fdecl;
        }
      }
      return 0;
    }
  }

  const FunctionDecl* LookupHelper::findAnyFunction(const clang::Decl*scopeDecl,
                                                    llvm::StringRef funcName,
                                                    DiagSetting diagOnOff,
                                                    bool objectIsConst) const {

    //FIXME: remove code duplication with findFunctionArgs() and friends.

    assert(scopeDecl && "Decl cannot be null");
    //
    //  Some utilities.
    //
    Parser& P = *m_Parser;
    Sema& S = P.getActions();
    ASTContext& Context = S.getASTContext();

    //
    //  Convert the passed decl into a nested name specifier,
    //  a scope spec, and a decl context.
    //
    //  Do this 'early' to save on the expansive parser setup,
    //  in case of failure.
    //
    CXXScopeSpec SS;
    DeclContext* foundDC = getContextAndSpec(SS,scopeDecl,Context,S);
    if (!foundDC) return 0;

    ParserStateRAII ResetParserState(P);
    llvm::SmallVector<Expr*, 4> GivenArgs;

    Interpreter::PushTransactionRAII pushedT(m_Interpreter);
    return findFunction(foundDC, SS,
                        funcName, GivenArgs, objectIsConst,
                        Context, P, S, findAnyFunctionSelector,
                        diagOnOff);
  }

  const FunctionDecl*
  LookupHelper::findFunctionProto(const Decl* scopeDecl,
                                  llvm::StringRef funcName,
                                 const llvm::SmallVectorImpl<QualType>& funcProto,
                                  DiagSetting diagOnOff, bool objectIsConst) const {
    assert(scopeDecl && "Decl cannot be null");
    //
    //  Some utilities.
    //
    // Use P for shortness
    Parser& P = *m_Parser;
    Sema& S = P.getActions();
    ASTContext& Context = S.getASTContext();

    //
    //  Convert the passed decl into a nested name specifier,
    //  a scope spec, and a decl context.
    //
    //  Do this 'early' to save on the expansive parser setup,
    //  in case of failure.
    //
    CXXScopeSpec SS;
    DeclContext* foundDC = getContextAndSpec(SS,scopeDecl,Context,S);
    if (!foundDC) return 0;

    llvm::SmallVector<ExprAlloc, 4> ExprMemory;
    llvm::SmallVector<Expr*, 4> GivenArgs;
    if (!funcProto.empty()) {
      if (!getExprProto(ExprMemory, GivenArgs, funcProto) ) {
        return 0;
      }
    }

    //
    //  Parse the prototype now.
    //
    ParserStateRAII ResetParserState(P);
    prepareForParsing("", llvm::StringRef("func.prototype.file"), diagOnOff);
    Interpreter::PushTransactionRAII pushedT(m_Interpreter);
    return findFunction(foundDC, SS,
                        funcName, GivenArgs, objectIsConst,
                        Context, P, S,
                        overloadFunctionSelector,
                        diagOnOff);
  }

  const FunctionDecl* LookupHelper::findFunctionProto(const Decl* scopeDecl,
                                                      llvm::StringRef funcName,
                                                      llvm::StringRef funcProto,
                                                      DiagSetting diagOnOff,
                                                      bool objectIsConst) const{
    assert(scopeDecl && "Decl cannot be null");
    //
    //  Some utilities.
    //
    // Use P for shortness
    Parser& P = *m_Parser;
    Sema& S = P.getActions();
    ASTContext& Context = S.getASTContext();

    //
    //  Convert the passed decl into a nested name specifier,
    //  a scope spec, and a decl context.
    //
    //  Do this 'early' to save on the expansive parser setup,
    //  in case of failure.
    //
    CXXScopeSpec SS;
    DeclContext* foundDC = getContextAndSpec(SS,scopeDecl,Context,S);
    if (!foundDC) return 0;

    //
    //  Parse the prototype now.
    //
    ParserStateRAII ResetParserState(P);
    prepareForParsing(funcProto, llvm::StringRef("func.prototype.file"), diagOnOff);

    llvm::SmallVector<ExprAlloc, 4> ExprMemory;
    llvm::SmallVector<Expr*, 4> GivenArgs;
    if (!funcProto.empty()) {
      if (!ParseProto(ExprMemory, GivenArgs,Context,P,S) ) {
        return 0;
      }
    }

    Interpreter::PushTransactionRAII pushedT(m_Interpreter);
    return findFunction(foundDC, SS,
                        funcName, GivenArgs, objectIsConst,
                        Context, P, S,
                        overloadFunctionSelector,
                        diagOnOff);
  }

  const FunctionDecl*
  LookupHelper::matchFunctionProto(const Decl* scopeDecl,
                                   llvm::StringRef funcName,
                                   llvm::StringRef funcProto,
                                   DiagSetting diagOnOff,
                                   bool objectIsConst) const {
    assert(scopeDecl && "Decl cannot be null");
    //
    //  Some utilities.
    //
    // Use P for shortness
    Parser& P = *m_Parser;
    Sema& S = P.getActions();
    ASTContext& Context = S.getASTContext();

    //
    //  Convert the passed decl into a nested name specifier,
    //  a scope spec, and a decl context.
    //
    //  Do this 'early' to save on the expansive parser setup,
    //  in case of failure.
    //
    CXXScopeSpec SS;
    DeclContext* foundDC = getContextAndSpec(SS,scopeDecl,Context,S);
    if (!foundDC) return 0;

    //
    //  Parse the prototype now.
    //
    ParserStateRAII ResetParserState(P);
    prepareForParsing(funcProto, llvm::StringRef("func.prototype.file"), diagOnOff);

    llvm::SmallVector<ExprAlloc, 4> ExprMemory;
    llvm::SmallVector<Expr*, 4> GivenArgs;
    if (!funcProto.empty()) {
      if (!ParseProto(ExprMemory,GivenArgs,Context,P,S) ) {
        return 0;
      }
    }

    Interpreter::PushTransactionRAII pushedT(m_Interpreter);
    return findFunction(foundDC, SS,
                        funcName, GivenArgs, objectIsConst,
                        Context, P, S,
                        matchFunctionSelector,
                        diagOnOff);
  }

  const FunctionDecl*
  LookupHelper::matchFunctionProto(const Decl* scopeDecl,
                                   llvm::StringRef funcName,
                                const llvm::SmallVectorImpl<QualType>& funcProto,
                                   DiagSetting diagOnOff,
                                   bool objectIsConst) const {
    assert(scopeDecl && "Decl cannot be null");
    //
    //  Some utilities.
    //
    // Use P for shortness
    Parser& P = *m_Parser;
    Sema& S = P.getActions();
    ASTContext& Context = S.getASTContext();

    //
    //  Convert the passed decl into a nested name specifier,
    //  a scope spec, and a decl context.
    //
    //  Do this 'early' to save on the expansive parser setup,
    //  in case of failure.
    //
    CXXScopeSpec SS;
    DeclContext* foundDC = getContextAndSpec(SS,scopeDecl,Context,S);
    if (!foundDC) return 0;


    llvm::SmallVector<ExprAlloc, 4> ExprMemory;
    llvm::SmallVector<Expr*, 4> GivenArgs;
    if (!funcProto.empty()) {
      if (!getExprProto(ExprMemory, GivenArgs, funcProto) ) {
        return 0;
      }
    }

    //
    //  Parse the prototype now.
    //
    ParserStateRAII ResetParserState(P);
    prepareForParsing("", llvm::StringRef("func.prototype.file"), diagOnOff);
    Interpreter::PushTransactionRAII pushedT(m_Interpreter);
    return findFunction(foundDC, SS,
                        funcName, GivenArgs, objectIsConst,
                        Context, P, S,
                        matchFunctionSelector,
                        diagOnOff);
  }

  static
  bool ParseArgs(llvm::SmallVectorImpl<Expr*> &GivenArgs,
                 ASTContext& Context, Parser &P, Sema &S) {

    //
    //  Parse the arguments now.
    //

    PrintingPolicy Policy(Context.getPrintingPolicy());
    Policy.SuppressTagKeyword = true;
    Policy.SuppressUnwrittenScope = true;
    Policy.SuppressInitializers = true;
    Policy.AnonymousTagLocations = false;
    std::string proto;
    {
      bool first_time = true;
      while (P.getCurToken().isNot(tok::eof)) {
        ExprResult Res = P.ParseAssignmentExpression();
        if (Res.isUsable()) {
          Expr* expr = Res.release();
          GivenArgs.push_back(expr);
          if (first_time) {
            first_time = false;
          }
          else {
            proto += ',';
          }
          std::string empty;
          llvm::raw_string_ostream tmp(empty);
          expr->printPretty(tmp, /*PrinterHelper=*/0, Policy,
                            /*Indentation=*/0);
          proto += tmp.str();
        }
        if (!P.getCurToken().is(tok::comma)) {
          break;
        }
        P.ConsumeToken();
      }
    }
    // For backward compatibility with CINT accept (for now?) a trailing close
    // parenthesis.
    if (P.getCurToken().isNot(tok::eof) && P.getCurToken().isNot(tok::r_paren) ) {
      // We did not consume all of the arg list, bad parse.
      return false;
    }
    //
    //  Cleanup after the arg list parse.
    //
    P.SkipUntil(clang::tok::eof);
    // Doesn't reset the diagnostic mappings
    S.getDiagnostics().Reset(/*soft=*/true);
    return true;
  }

  const FunctionDecl*
  LookupHelper::findFunctionArgs(const Decl* scopeDecl,
                                 llvm::StringRef funcName,
                                 llvm::StringRef funcArgs,
                                 DiagSetting diagOnOff,
                                 bool objectIsConst) const {
    assert(scopeDecl && "Decl cannot be null");
    //
    //  Some utilities.
    //
    // Use P for shortness
    Parser& P = *m_Parser;
    Sema& S = P.getActions();
    ASTContext& Context = S.getASTContext();

    //
    //  Convert the passed decl into a nested name specifier,
    //  a scope spec, and a decl context.
    //
    //  Do this 'early' to save on the expansive parser setup,
    //  in case of failure.
    //
    CXXScopeSpec SS;
    DeclContext* foundDC = getContextAndSpec(SS,scopeDecl,Context,S);
    if (!foundDC) return 0;

    //
    //  Parse the arguments now.
    //
    ParserStateRAII ResetParserState(P);
    prepareForParsing(funcArgs, llvm::StringRef("func.args.file"), diagOnOff);

    llvm::SmallVector<Expr*, 4> GivenArgs;
    if (!funcArgs.empty()) {
      if (!ParseArgs(GivenArgs,Context,P,S) ) {
        return 0;
      }
    }

    Interpreter::PushTransactionRAII pushedT(m_Interpreter);
    return findFunction(foundDC, SS,
                        funcName, GivenArgs, objectIsConst,
                        Context, P, S, overloadFunctionSelector,
                        diagOnOff);
  }

  void LookupHelper::findArgList(llvm::StringRef argList,
                                 llvm::SmallVectorImpl<Expr*>& argExprs,
                                 DiagSetting diagOnOff) const {
    if (argList.empty()) return;

    //
    //  Some utilities.
    //
    // Use P for shortness
    Parser& P = *m_Parser;
    ParserStateRAII ResetParserState(P);
    prepareForParsing(argList, llvm::StringRef("arg.list.file"), diagOnOff);
    //
    //  Parse the arguments now.
    //
    {
      bool hasUnusableResult = false;
      while (P.getCurToken().isNot(tok::eof)) {
        ExprResult Res = P.ParseAssignmentExpression();
        if (Res.isUsable()) {
          argExprs.push_back(Res.release());
        }
        else {
          hasUnusableResult = true;
          break;
        }
        if (!P.getCurToken().is(tok::comma)) {
          break;
        }
        P.ConsumeToken();
      }
      if (hasUnusableResult)
        // if one of the arguments is not usable return empty.
        argExprs.clear();
    }
  }

  void LookupHelper::prepareForParsing(llvm::StringRef code,
                                       llvm::StringRef bufferName,
                                       DiagSetting diagOnOff) const {
    Parser& P = *m_Parser;
    Sema& S = P.getActions();
    Preprocessor& PP = P.getPreprocessor();
    //
    //  Tell the diagnostic engine to ignore all diagnostics.
    //
    PP.getDiagnostics().setSuppressAllDiagnostics(diagOnOff == NoDiagnostics);
    //
    //  Tell the parser to not attempt spelling correction.
    //
    const_cast<LangOptions&>(PP.getLangOpts()).SpellChecking = 0;
    //
    //  Turn on ignoring of the main file eof token.
    //
    //  Note: We need this because token readahead in the following
    //        routine calls ends up parsing it multiple times.
    //
    if (!PP.isIncrementalProcessingEnabled()) {
      PP.enableIncrementalProcessing();
    }
    if (!code.empty()) {
      //
      //  Create a fake file to parse the type name.
      //
      llvm::MemoryBuffer* SB
         = llvm::MemoryBuffer::getMemBufferCopy(code.str() + "\n",
                                                bufferName.str());
      FileID FID = S.getSourceManager().createFileIDForMemBuffer(SB);
      //
      //  Switch to the new file the way #include does.
      //
      //  Note: To switch back to the main file we must consume an eof token.
      //
      PP.EnterSourceFile(FID, /*DirLookup=*/0, SourceLocation());
      PP.Lex(const_cast<Token&>(P.getCurToken()));
    }
  }

  static
  bool hasFunctionSelector(DeclContext* ,
                           bool /* objectIsConst */,
                           const llvm::SmallVectorImpl<Expr*> &,
                           LookupResult &Result,
                           DeclarationNameInfo &,
                           const TemplateArgumentListInfo* ,
                           ASTContext&, Parser &, Sema &) {
    //
    //  Check for lookup failure.
    //
    if (Result.empty())
      return false;
    if (Result.isSingleResult())
      return isa<FunctionDecl>(Result.getFoundDecl());
    // We have many - those must be functions.
    return true;
  }

  bool LookupHelper::hasFunction(const clang::Decl* scopeDecl,
                                 llvm::StringRef funcName,
                                 DiagSetting diagOnOff) const {

    //FIXME: remove code duplication with findFunctionArgs() and friends.

    assert(scopeDecl && "Decl cannot be null");
    //
    //  Some utilities.
    //
    Parser& P = *m_Parser;
    Sema& S = P.getActions();
    ASTContext& Context = S.getASTContext();

    //
    //  Convert the passed decl into a nested name specifier,
    //  a scope spec, and a decl context.
    //
    //  Do this 'early' to save on the expansive parser setup,
    //  in case of failure.
    //
    CXXScopeSpec SS;
    DeclContext* foundDC = getContextAndSpec(SS,scopeDecl,Context,S);
    if (!foundDC) return 0;

    ParserStateRAII ResetParserState(P);
    llvm::SmallVector<Expr*, 4> GivenArgs;

    Interpreter::PushTransactionRAII pushedT(m_Interpreter);
    return findFunction(foundDC, SS,
                        funcName, GivenArgs, false /* objectIsConst */,
                        Context, P, S, hasFunctionSelector,
                        diagOnOff);
  }
} // end namespace cling
