//------------------------------------------------------------------------------
// CLING - the C++ LLVM-based InterpreterG :)
//
// This file is dual-licensed: you can choose to license it under the University
// of Illinois Open Source License or the GNU Lesser General Public License. See
// LICENSE.TXT for details.
//------------------------------------------------------------------------------

// RUN: cat %s | %cling -I %S -Xclang -verify
// Test incompleteType
//XFAIL: *
//Becasue functionality is disabled now

#include "cling/Interpreter/Interpreter.h"
gCling->EnableAutoloading();

.rawInput 1
class __attribute__((annotate("Def.h"))) C;
//expected-warning + {{}}
//expected-note + {{}}
.rawInput 0

C c;
//expected-error {{}}

