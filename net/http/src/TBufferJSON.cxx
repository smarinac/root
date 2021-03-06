//
// Author: Sergey Linev  4.03.2014

/*************************************************************************
 * Copyright (C) 1995-2004, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

//________________________________________________________________________
//
// Class for serializing/deserializing object to/from xml.
// It redefines most of TBuffer class function to convert simple types,
// array of simple types and objects to/from xml.
// Instead of writing a binary data it creates a set of xml structures as
// nodes and attributes
// TBufferJSON class uses streaming mechanism, provided by ROOT system,
// therefore most of ROOT and user classes can be stored to xml. There are
// limitations for complex objects like TTree, which can not be yet converted to xml.
//________________________________________________________________________


#include "TBufferJSON.h"

#include "Compression.h"

#include "TArrayI.h"
#include "TObjArray.h"
#include "TROOT.h"
#include "TClass.h"
#include "TClassTable.h"
#include "TDataType.h"
#include "TDataMember.h"
#include "TExMap.h"
#include "TMethodCall.h"
#include "TStreamerInfo.h"
#include "TStreamerElement.h"
#include "TProcessID.h"
#include "TFile.h"
#include "TMemberStreamer.h"
#include "TStreamer.h"
#include "TStreamerInfoActions.h"
#include "RVersion.h"
#include "TClonesArray.h"

#ifdef R__VISUAL_CPLUSPLUS
#define FLong64    "%I64d"
#define FULong64   "%I64u"
#else
#define FLong64    "%lld"
#define FULong64   "%llu"
#endif

ClassImp(TBufferJSON);


const char *TBufferJSON::fgFloatFmt = "%e";


// TJSONStackObj is used to keep stack of object hierarchy,
// stored in TBuffer. For instance, data for parent class(es)
// stored in subnodes, but initial object node will be kept.

class TJSONStackObj : public TObject {
public:
   TStreamerInfo    *fInfo;           //!
   TStreamerElement *fElem;           //! element in streamer info
   Int_t             fElemNumber;     //! number of streamer element in streamer info
   Bool_t            fIsStreamerInfo; //!
   Bool_t            fIsElemOwner;    //!
   Bool_t            fIsBaseClass;    //! indicate if element is base-class, ignored by post processing
   Bool_t            fIsPostProcessed;//! indicate that value is written
   Bool_t            fIsObjStarted;   //! indicate that object writing started, should be closed in postprocess
   Bool_t            fIsArray;        //! indicate if array object is used
   TObjArray         fValues;         //! raw values
   Int_t             fLevel;          //! indent level

   TJSONStackObj() :
      TObject(),
      fInfo(0),
      fElem(0),
      fElemNumber(0),
      fIsStreamerInfo(kFALSE),
      fIsElemOwner(kFALSE),
      fIsBaseClass(kFALSE),
      fIsPostProcessed(kFALSE),
      fIsObjStarted(kFALSE),
      fIsArray(kFALSE),
      fValues(),
      fLevel(0)
   {
      fValues.SetOwner(kTRUE);
   }

   virtual ~TJSONStackObj()
   {
      if (fIsElemOwner) delete fElem;
   }

   Bool_t IsStreamerInfo() const
   {
      return fIsStreamerInfo;
   }
   Bool_t IsStreamerElement() const
   {
      return !fIsStreamerInfo && (fElem != 0);
   }

   void PushValue(TString &v)
   {
      fValues.Add(new TObjString(v));
      v.Clear();
   }
};


//______________________________________________________________________________
TBufferJSON::TBufferJSON() :
   TBuffer(TBuffer::kWrite),
   fOutBuffer(),
   fValue(),
   fJsonrMap(),
   fJsonrCnt(0),
   fStack(),
   fExpectedChain(kFALSE),
   fCompact(0),
   fSemicolon(" : "),
   fArraySepar(", ")
{
   // Creates buffer object to serialize data into json.

   fBufSize = 1000000000;

   SetParent(0);
   SetBit(kCannotHandleMemberWiseStreaming);
   //SetBit(kTextBasedStreaming);

   fOutBuffer.Capacity(10000);
   fValue.Capacity(1000);
}

//______________________________________________________________________________
TBufferJSON::~TBufferJSON()
{
   // destroy xml buffer

   fStack.Delete();
}

//______________________________________________________________________________
TString TBufferJSON::ConvertToJSON(const TObject *obj, Int_t compact)
{
   // converts object, inherited from TObject class, to JSON string

   return ConvertToJSON(obj, (obj ? obj->IsA() : 0), compact);
}

//______________________________________________________________________________
void TBufferJSON::SetCompact(int level)
{
   // Set level of space/newline compression
   //   0 - no any compression
   //   1 - exclude spaces in the begin
   //   2 - remove newlines
   //   3 - exclude spaces as much as possible

   fCompact = level;
   fSemicolon = fCompact > 2 ? ":" : " : ";
   fArraySepar = fCompact > 2 ? "," : ", ";
}


//______________________________________________________________________________
TString TBufferJSON::ConvertToJSON(const void *obj, const TClass *cl,
                                   Int_t compact)
{
   // converts any type of object to JSON string
   // following values of compact
   //   0 - no any compression
   //   1 - exclude spaces in the begin
   //   2 - remove newlines
   //   3 - exclude spaces as much as possible

   TBufferJSON buf;

   buf.SetCompact(compact);

   return buf.JsonWriteAny(obj, cl);
}

//______________________________________________________________________________
TString TBufferJSON::ConvertToJSON(const void *ptr, TDataMember *member,
                                   Int_t compact)
{
   // converts selected data member into json

   if ((ptr == 0) || (member == 0)) return TString("null");

   TClass *mcl = (member->IsBasic() || member->IsSTLContainer()) ? 0 :
                 gROOT->GetClass(member->GetTypeName());

   if ((mcl != 0) && (mcl != TString::Class()) &&
         (mcl->GetBaseClassOffset(TArray::Class()) != 0))
      return TBufferJSON::ConvertToJSON(ptr, mcl, compact);

   TBufferJSON buf;
   buf.SetCompact(compact);

   return buf.JsonWriteMember(ptr, member, mcl);
}

//______________________________________________________________________________
TString TBufferJSON::JsonWriteAny(const void *obj, const TClass *cl)
{
   // Convert object of any class to JSON structures
   // Returns string with converted object

   fOutBuffer.Clear();

   JsonWriteObject(obj, cl);

   return fOutBuffer;
}

//______________________________________________________________________________
TString TBufferJSON::JsonWriteMember(const void *ptr, TDataMember *member,
                                     TClass *memberClass)
{
   // Convert single data member to JSON structures
   // Returns string with converted member

   if (member == 0) return "null";

   if (gDebug > 2)
      Info("JsonWriteMember", "Write member %s type %s ndim %d\n",
           member->GetName(), member->GetTrueTypeName(), member->GetArrayDim());

   PushStack(0);
   fValue.Clear();

   if (member->IsBasic()) {

      Int_t tid = member->GetDataType() ? member->GetDataType()->GetType() : kNoType_t;

      if (ptr == 0) {
         fValue = "null";
      } else if (member->GetArrayDim() == 0) {
         switch (tid) {
            case kChar_t:
               JsonWriteBasic(*((Char_t *)ptr));
               break;
            case kShort_t:
               JsonWriteBasic(*((Short_t *)ptr));
               break;
            case kInt_t:
               JsonWriteBasic(*((Int_t *)ptr));
               break;
            case kLong_t:
               JsonWriteBasic(*((Long_t *)ptr));
               break;
            case kFloat_t:
               JsonWriteBasic(*((Float_t *)ptr));
               break;
            case kCounter:
               JsonWriteBasic(*((Int_t *)ptr));
               break;
            case kCharStar:
               WriteCharP((Char_t *)ptr);
               break;
            case kDouble_t:
               JsonWriteBasic(*((Double_t *)ptr));
               break;
            case kDouble32_t:
               JsonWriteBasic(*((Double_t *)ptr));
               break;
            case kchar:
               JsonWriteBasic(*((char *)ptr));
               break;
            case kUChar_t:
               JsonWriteBasic(*((UChar_t *)ptr));
               break;
            case kUShort_t:
               JsonWriteBasic(*((UShort_t *)ptr));
               break;
            case kUInt_t:
               JsonWriteBasic(*((UInt_t *)ptr));
               break;
            case kULong_t:
               JsonWriteBasic(*((ULong_t *)ptr));
               break;
            case kBits:
               JsonWriteBasic(*((UInt_t *)ptr));
               break;
            case kLong64_t:
               JsonWriteBasic(*((Long64_t *)ptr));
               break;
            case kULong64_t:
               JsonWriteBasic(*((ULong64_t *)ptr));
               break;
            case kBool_t:
               JsonWriteBasic(*((Bool_t *)ptr));
               break;
            case kFloat16_t:
               JsonWriteBasic(*((Float_t *)ptr));
               break;
            case kOther_t:
            case kNoType_t:
            case kVoid_t:
               break;
         }
      } else if ((member->GetArrayDim() == 1) || (fCompact > 0)) {

         Int_t n = member->GetMaxIndex(0);
         for (Int_t ndim = 1; ndim < member->GetArrayDim(); ndim++)
            n *= member->GetMaxIndex(ndim);

         switch (tid) {
            case kChar_t:
               WriteFastArray((Char_t *)ptr, n);
               break;
            case kShort_t:
               WriteFastArray((Short_t *)ptr, n);
               break;
            case kInt_t:
               WriteFastArray((Int_t *)ptr, n);
               break;
            case kLong_t:
               WriteFastArray((Long_t *)ptr, n);
               break;
            case kFloat_t:
               WriteFastArray((Float_t *)ptr, n);
               break;
            case kCounter:
               WriteFastArray((Int_t *)ptr, n);
               break;
            case kCharStar:
               WriteFastArray((Char_t *)ptr, n);
               break;
            case kDouble_t:
               WriteFastArray((Double_t *)ptr, n);
               break;
            case kDouble32_t:
               WriteFastArray((Double_t *)ptr, n);
               break;
            case kchar:
               WriteFastArray((char *)ptr, n);
               break;
            case kUChar_t:
               WriteFastArray((UChar_t *)ptr, n);
               break;
            case kUShort_t:
               WriteFastArray((UShort_t *)ptr, n);
               break;
            case kUInt_t:
               WriteFastArray((UInt_t *)ptr, n);
               break;
            case kULong_t:
               WriteFastArray((ULong_t *)ptr, n);
               break;
            case kBits:
               WriteFastArray((UInt_t *)ptr, n);
               break;
            case kLong64_t:
               WriteFastArray((Long64_t *)ptr, n);
               break;
            case kULong64_t:
               WriteFastArray((ULong64_t *)ptr, n);
               break;
            case kBool_t:
               WriteFastArray((Bool_t *)ptr, n);
               break;
            case kFloat16_t:
               WriteFastArray((Float_t *)ptr, n);
               break;
            case kOther_t:
            case kNoType_t:
            case kVoid_t:
               break;
         }
      } else {
         // here generic code to write n-dimensional array

         TArrayI indexes(member->GetArrayDim() - 1);
         indexes.Reset(0);

         Int_t cnt = 0;
         while (cnt >= 0) {
            if (indexes[cnt] >= member->GetMaxIndex(cnt)) {
               fOutBuffer.Append(" ]");
               indexes[cnt] = 0;
               cnt--;
               if (cnt >= 0) indexes[cnt]++;
               continue;
            }

            if (indexes[cnt] > 0)
               fOutBuffer.Append(fArraySepar);
            else
               fOutBuffer.Append("[ ");

            if (++cnt == indexes.GetSize()) {
               Int_t shift = 0;
               for (Int_t k = 0; k < indexes.GetSize(); k++) {
                  shift = shift * member->GetMaxIndex(k) + indexes[k];
               }

               Int_t len = member->GetMaxIndex(indexes.GetSize());
               shift *= len;

               fValue.Clear();

               WriteFastArray((Int_t *)ptr + shift, len);

               switch (tid) {
                  case kChar_t:
                     WriteFastArray((Char_t *)ptr + shift, len);
                     break;
                  case kShort_t:
                     WriteFastArray((Short_t *)ptr + shift, len);
                     break;
                  case kInt_t:
                     WriteFastArray((Int_t *)ptr + shift, len);
                     break;
                  case kLong_t:
                     WriteFastArray((Long_t *)ptr + shift, len);
                     break;
                  case kFloat_t:
                     WriteFastArray((Float_t *)ptr + shift, len);
                     break;
                  case kCounter:
                     WriteFastArray((Int_t *)ptr + shift, len);
                     break;
                  case kCharStar:
                     WriteFastArray((Char_t *)ptr + shift, len);
                     break;
                  case kDouble_t:
                     WriteFastArray((Double_t *)ptr + shift, len);
                     break;
                  case kDouble32_t:
                     WriteFastArray((Double_t *)ptr + shift, len);
                     break;
                  case kchar:
                     WriteFastArray((char *)ptr + shift, len);
                     break;
                  case kUChar_t:
                     WriteFastArray((UChar_t *)ptr + shift, len);
                     break;
                  case kUShort_t:
                     WriteFastArray((UShort_t *)ptr + shift, len);
                     break;
                  case kUInt_t:
                     WriteFastArray((UInt_t *)ptr + shift, len);
                     break;
                  case kULong_t:
                     WriteFastArray((ULong_t *)ptr + shift, len);
                     break;
                  case kBits:
                     WriteFastArray((UInt_t *)ptr + shift, len);
                     break;
                  case kLong64_t:
                     WriteFastArray((Long64_t *)ptr + shift, len);
                     break;
                  case kULong64_t:
                     WriteFastArray((ULong64_t *)ptr + shift, len);
                     break;
                  case kBool_t:
                     WriteFastArray((Bool_t *)ptr + shift, len);
                     break;
                  case kFloat16_t:
                     WriteFastArray((Float_t *)ptr + shift, len);
                     break;
                  case kOther_t:
                  case kNoType_t:
                  case kVoid_t:
                     fValue = "null";
                     break;
               }

               fOutBuffer.Append(fValue);
               cnt--;
               indexes[cnt]++;
            }
         }

         fValue = fOutBuffer;
      }
   } else if (memberClass == TString::Class()) {
      TString *str = (TString *) ptr;
      fValue.Append("\"");
      if (str != 0) fValue.Append(*str);
      fValue.Append("\"");
   } else if (memberClass->GetBaseClassOffset(TArray::Class()) == 0) {
      TArray *arr = (TArray *) ptr;
      if ((arr != 0) && (arr->GetSize() > 0)) {
         arr->Streamer(*this);
         // WriteFastArray(arr->GetArray(), arr->GetSize());
         if (Stack()->fValues.GetLast() > 0) {
            Warning("TBufferJSON", "When streaming TArray, more than 1 object in the stack, use second item");
            fValue = Stack()->fValues.At(1)->GetName();
         }
      } else
         fValue = "[]";
   }

   PopStack();

   if (fValue.Length() == 0) return "not supported";

   return fValue;
}

//______________________________________________________________________________
void TBufferJSON::WriteObject(const TObject *obj)
{
   // Convert object into xml structures.
   // !!! Should be used only by TBufferJSON itself.
   // Use ConvertToXML() methods to convert your object to xml
   // Redefined here to avoid gcc 3.x warning

   Info("WriteObject", "Object %p", obj);

   WriteObjectAny(obj, TObject::Class());
}

//______________________________________________________________________________
TJSONStackObj *TBufferJSON::PushStack(Int_t inclevel)
{
   // add new level to the structures stack

   TJSONStackObj *curr = Stack();
   TJSONStackObj *stack = new TJSONStackObj();
   stack->fLevel = (curr ? curr->fLevel : 0) + inclevel;
   fStack.Add(stack);
   return stack;
}

//______________________________________________________________________________
TJSONStackObj *TBufferJSON::PopStack()
{
   // remove one level from xml stack

   TObject *last = fStack.Last();
   if (last != 0) {
      fStack.Remove(last);
      delete last;
      fStack.Compress();
   }
   return dynamic_cast<TJSONStackObj *>(fStack.Last());
}

//______________________________________________________________________________
TJSONStackObj *TBufferJSON::Stack(Int_t depth)
{
   // return xml stack object of specified depth

   TJSONStackObj *stack = 0;
   if (depth <= fStack.GetLast())
      stack = dynamic_cast<TJSONStackObj *>(fStack.At(fStack.GetLast() - depth));
   return stack;
}

//______________________________________________________________________________
void TBufferJSON::AppendOutput(const char *line0, const char *line1)
{
   // Info("AppendOutput","  '%s' '%s'", line0, line1?line1 : "---");

   if (line0 != 0) fOutBuffer.Append(line0);

   if (line1 != 0) {
      if (fCompact < 2) fOutBuffer.Append("\n");

      if (strlen(line1) > 0) {
         if (fCompact < 1) {
            TJSONStackObj *stack = Stack();
            if ((stack != 0) && (stack->fLevel > 0))
               fOutBuffer.Append(' ', stack->fLevel);
         }
         fOutBuffer.Append(line1);
      }
   }
}

//______________________________________________________________________________
void TBufferJSON::JsonStartElement()
{
   TJSONStackObj *stack = Stack();
   if ((stack != 0) && stack->IsStreamerElement() && !stack->fIsObjStarted) {

      if ((fValue.Length() > 0) || (stack->fValues.GetLast() >= 0))
         Error("JsonWriteObject", "Non-empty value buffer when start writing object");

      stack->fIsPostProcessed = kTRUE;
      stack->fIsObjStarted = kTRUE;

      if (!stack->fIsBaseClass) {
         AppendOutput(",", "\"");
         AppendOutput(stack->fElem->GetName());
         AppendOutput("\"");
         AppendOutput(fSemicolon.Data());
      }
   }
}

//______________________________________________________________________________
void TBufferJSON::JsonWriteObject(const void *obj, const TClass *cl)
{
   // Write object to buffer
   // If object was written before, only pointer will be stored
   // Return pointer to top xml node, representing object

   // static int  cnt = 0;

   if (!cl) obj = 0;

   //if (cnt++>100) return;

   if (gDebug > 1)
      Info("JsonWriteObject", "Object %p class %s", obj, cl ? cl->GetName() : "null");

   // special handling for TArray classes - they should appear not as object but JSON array
   Bool_t isarray = strncmp("TArray", (cl ? cl->GetName() : ""), 6) == 0;
   if (isarray) isarray = ((TClass *)cl)->GetBaseClassOffset(TArray::Class()) == 0;
   Bool_t iscollect = !isarray && (cl != 0) && (((TClass *)cl)->GetBaseClassOffset(TCollection::Class()) == 0);

//   if (Stack() && Stack()->fElem && iscollect)
//      Info("Write","Collection %s of class %s", Stack()->fElem->GetName(), cl->GetName());

   // special case for TString - it is saved as string in JSON
   Bool_t iststring = !isarray && !iscollect && (cl == TString::Class()) && (fStack.GetLast() >= 0);

   if (!isarray) {
      JsonStartElement();
   }

   if (obj == 0) {
      AppendOutput("null");
      return;
   }

   TJSONStackObj *stack = 0;

   // for array and string different handling - they not recognized at the end as objects in JSON
   if (!isarray && !iststring) {
      // add element name which should correspond to the object

      std::map<const void *, unsigned>::const_iterator iter = fJsonrMap.find(obj);

      if (iter != fJsonrMap.end()) {
         AppendOutput(Form("\"$ref:%u\"", iter->second));
         return;
      }

      // if (fJsonrCnt<10) Info("JsonWriteObject","Object cl:%s $ref%u", cl->GetName(), fJsonrCnt);

      fJsonrMap[obj] = fJsonrCnt++;

      stack = PushStack(2);
      AppendOutput("{", "\"_typename\"");
      AppendOutput(fSemicolon.Data());
      AppendOutput("\"JSROOTIO.");
      AppendOutput(cl->GetName());
      AppendOutput("\"");
   } else {
      stack = PushStack(0);
   }

//   if (strcmp(cl->GetName(), "TMethodCall") == 0) {
//      gDebug=5; cl->GetStreamerInfo()->ls("*");
//   }

   if (gDebug > 3)
      Info("JsonWriteObject", "Starting object %p write for class: %s",
           obj, cl->GetName());

   stack->fIsArray = isarray;

   if (iscollect)
      JsonStreamCollection((TCollection *) obj, cl);
   else
      ((TClass *)cl)->Streamer((void *)obj, *this);

   if (gDebug > 3)
      Info("JsonWriteObject", "Done object %p write for class: %s",
           obj, cl->GetName());

//   if (strcmp(cl->GetName(), "TMethodCall") == 0) gDebug=0;

   if (isarray) {
      if (stack->fValues.GetLast() != 0)
         Error("JsonWriteObject", "Problem when writing array");
      stack->fValues.Delete();
   } else if (iststring) {
      if (stack->fValues.GetLast() > 1)
         Error("JsonWriteObject", "Problem when writing TString");
      stack->fValues.Delete();
      AppendOutput(fValue.Data());
      fValue.Clear();
   } else {
      if (stack->fValues.GetLast() >= 0)
         Error("JsonWriteObject", "Non-empty values %d for class %s",
               stack->fValues.GetLast() + 1, cl->GetName());
   }

   PopStack();

   if (!isarray && !iststring) {
      AppendOutput(0, "}");
   }
}

//______________________________________________________________________________
void TBufferJSON::JsonStreamCollection(TCollection *col, const TClass *)
{
   // store content of collection

   AppendOutput(",", "\"name\"");
   AppendOutput(fSemicolon.Data());
   AppendOutput("\"");
   AppendOutput(col->GetName());
   AppendOutput("\",", "\"arr\"");
   AppendOutput(fSemicolon.Data());

   // collection treated as JS Array and its reference kept in the objects map
   AppendOutput("[");   // fJsonrCnt++; // account array of objects

   bool islist = col->InheritsFrom(TList::Class());
   TString sopt;
   sopt.Capacity(500);
   sopt = "[";

   TIter iter(col);
   TObject *obj;
   Bool_t first = kTRUE;
   while ((obj = iter()) != 0) {
      if (!first) {
         AppendOutput(fArraySepar.Data());
         sopt.Append(fArraySepar.Data());
      }
      if (islist) {
         sopt.Append("\"");
         sopt.Append(iter.GetOption());
         sopt.Append("\"");
      }

      JsonWriteObject(obj, obj->IsA());

      first = kFALSE;
   }

   sopt.Append("]");

   AppendOutput("]");

   if (islist) {
      AppendOutput(",", "\"opt\"");
      AppendOutput(fSemicolon.Data());
      AppendOutput(sopt.Data());
      /* fJsonrCnt++; */ // account array of options
   }
}


//______________________________________________________________________________
void TBufferJSON::IncrementLevel(TVirtualStreamerInfo *info)
{
   // Function is called from TStreamerInfo WriteBuffer and Readbuffer functions
   // and indent new level in xml structure.
   // This call indicates, that TStreamerInfo functions starts streaming
   // object data of correspondent class

   if (gDebug > 2)
      Info("IncrementLevel", "Class: %s", (info ? info->GetClass()->GetName() : "custom"));

   WorkWithClass((TStreamerInfo *)info);
}

//______________________________________________________________________________
void  TBufferJSON::WorkWithClass(TStreamerInfo *sinfo, const TClass *cl)
{
   // Prepares buffer to stream data of specified class

   fExpectedChain = kFALSE;

   if (sinfo != 0) cl = sinfo->GetClass();

   if (cl == 0) return;

   if (gDebug > 3) Info("WorkWithClass", "Class: %s", cl->GetName());

   TJSONStackObj *stack = Stack();

   if ((stack != 0) && stack->IsStreamerElement() &&
         (stack->fElem->GetType() == TStreamerInfo::kObject) &&
         !stack->fIsObjStarted) {

      stack->fIsObjStarted = kTRUE;

      fJsonrCnt++;   // count object, but do not keep reference

      AppendOutput(",", "\"");
      AppendOutput(stack->fElem->GetName());

      stack = PushStack(2);
      AppendOutput("\" : {", "\"_typename\"");
      AppendOutput(fSemicolon.Data());
      AppendOutput("\"JSROOTIO.");
      AppendOutput(cl->GetName());
      AppendOutput("\"");
   } else {
      stack = PushStack(0);
   }


   stack->fInfo = sinfo;
   stack->fIsStreamerInfo = kTRUE;
}

//______________________________________________________________________________
void TBufferJSON::DecrementLevel(TVirtualStreamerInfo *info)
{
   // Function is called from TStreamerInfo WriteBuffer and ReadBuffer functions
   // and decrease level in xml structure.

   fExpectedChain = kFALSE;

   if (gDebug > 2)
      Info("DecrementLevel", "Class: %s",
           (info ? info->GetClass()->GetName() : "custom"));

   TJSONStackObj *stack = Stack();

   if (stack->IsStreamerElement()) {
      if (gDebug > 3)
         Info("DecrementLevel", "    Perform post-processing elem: %s",
              stack->fElem->GetName());

      PerformPostProcessing(stack);

      PopStack();  // remove stack of last element

      stack = Stack();
   }

   if (stack->fInfo != (TStreamerInfo *) info)
      Error("DecrementLevel", "    Mismatch of streamer info");

   PopStack();                       // back from data of stack info

   if (gDebug > 3)
      Info("DecrementLevel", "Class: %s done",
           (info ? info->GetClass()->GetName() : "custom"));
}

//______________________________________________________________________________
void TBufferJSON::SetStreamerElementNumber(TStreamerElement *elem, Int_t comp_type)
{
   // Function is called from TStreamerInfo WriteBuffer and Readbuffer functions
   // and add/verify next element of xml structure
   // This calls allows separate data, correspondent to one class member, from another

   if (gDebug > 3)
      Info("SetStreamerElementNumber", "Element name %s", elem->GetName());

   WorkWithElement(elem, comp_type);
}

//______________________________________________________________________________
void TBufferJSON::WorkWithElement(TStreamerElement *elem, Int_t comp_type)
{
   // This is call-back from streamer which indicates
   // that class member will be streamed
   // Name of element used in JSON

   fExpectedChain = kFALSE;

   TJSONStackObj *stack = Stack();
   if (stack == 0) {
      Error("WorkWithElement", "stack is empty");
      return;
   }

   if (gDebug > 3)
      Info("WorkWithElement", "    Start element %s type %d",
           elem ? elem->GetName() : "---", elem ? elem->GetType() : -1);

   if (stack->IsStreamerElement()) {
      // this is post processing

      if (gDebug > 3)
         Info("WorkWithElement", "    Perform post-processing elem: %s",
              stack->fElem->GetName());

      PerformPostProcessing(stack);

      PopStack();                    // go level back
      stack = dynamic_cast<TJSONStackObj *>(fStack.Last());
   }

   fValue.Clear();

   if (stack == 0) {
      Error("WorkWithElement", "Lost of stack");
      return;
   }

   TStreamerInfo *info = stack->fInfo;
   if (!stack->IsStreamerInfo()) {
      Error("WorkWithElement", "Problem in Inc/Dec level");
      return;
   }
   Int_t number = info->GetElements()->IndexOf(elem);

   if (gDebug > 3)
      Info("WorkWithElement", "    Start element %s type %d",
           elem ? elem->GetName() : "---", elem ? elem->GetType() : -1);

   if (elem == 0) {
      Error("WorkWithElement", "streamer info returns elem = 0");
      return;
   }

   if (gDebug > 3)
      Info("WorkWithElement", "    Element %s type %d",
           elem ? elem->GetName() : "---", elem ? elem->GetType() : -1);

   Bool_t isBasicType = (elem->GetType() > 0) && (elem->GetType() < 20);

   fExpectedChain = isBasicType && (comp_type - elem->GetType() == TStreamerInfo::kOffsetL);

   if (fExpectedChain && (gDebug > 3))
      Info("WorkWithElement", "    Expects chain for elem %s number %d",
           elem->GetName(), number);

   TClass *base_class = 0;

   if ((elem->GetType() == TStreamerInfo::kBase) ||
         ((elem->GetType() == TStreamerInfo::kTObject) &&
          !strcmp(elem->GetName(), TObject::Class()->GetName())) ||
         ((elem->GetType() == TStreamerInfo::kTNamed) &&
          !strcmp(elem->GetName(), TNamed::Class()->GetName())))
      base_class = elem->GetClassPointer();

   if (base_class && (gDebug > 3))
      Info("WorkWithElement", "   Expects base class %s with standard streamer",
           base_class->GetName());

   stack = PushStack(0);
   stack->fElem = (TStreamerElement *) elem;
   stack->fElemNumber = number;
   stack->fIsElemOwner = (number < 0);
   stack->fIsBaseClass = (base_class != 0);
}

//______________________________________________________________________________
void TBufferJSON::ClassBegin(const TClass *cl, Version_t)
{
   // Special function for custom streamers.
   // Should be called in the beginning

   WorkWithClass(0, cl);
}

//______________________________________________________________________________
void TBufferJSON::ClassEnd(const TClass *)
{
   // Special function for custom streamers.
   // Should be called at the end

   DecrementLevel(0);
}

//______________________________________________________________________________
void TBufferJSON::ClassMember(const char *name, const char *typeName,
                              Int_t arrsize1, Int_t arrsize2)
{
   // Special function for custom streamers.
   // Provides possibility to mark name and type of stored data

   if (typeName == 0) typeName = name;

   if ((name == 0) || (strlen(name) == 0)) {
      Error("ClassMember", "Invalid member name");
      return;
   }

   TString tname = typeName;

   Int_t typ_id = -1;

   if (strcmp(typeName, "raw:data") == 0)
      typ_id = TStreamerInfo::kMissing;

   if (typ_id < 0) {
      TDataType *dt = gROOT->GetType(typeName);
      if (dt != 0)
         if ((dt->GetType() > 0) && (dt->GetType() < 20))
            typ_id = dt->GetType();
   }

   if (typ_id < 0)
      if (strcmp(name, typeName) == 0) {
         TClass *cl = TClass::GetClass(tname.Data());
         if (cl != 0) typ_id = TStreamerInfo::kBase;
      }

   if (typ_id < 0) {
      Bool_t isptr = kFALSE;
      if (tname[tname.Length() - 1] == '*') {
         tname.Resize(tname.Length() - 1);
         isptr = kTRUE;
      }
      TClass *cl = TClass::GetClass(tname.Data());
      if (cl == 0) {
         Error("ClassMember", "Invalid class specifier %s", typeName);
         return;
      }

      if (cl->IsTObject())
         typ_id = isptr ? TStreamerInfo::kObjectp : TStreamerInfo::kObject;
      else
         typ_id = isptr ? TStreamerInfo::kAnyp : TStreamerInfo::kAny;

      if ((cl == TString::Class()) && !isptr)
         typ_id = TStreamerInfo::kTString;
   }

   TStreamerElement *elem = 0;

   if (typ_id == TStreamerInfo::kMissing) {
      elem = new TStreamerElement(name, "title", 0, typ_id, "raw:data");
   } else  if (typ_id == TStreamerInfo::kBase) {
      TClass *cl = TClass::GetClass(tname.Data());
      if (cl != 0) {
         TStreamerBase *b = new TStreamerBase(tname.Data(), "title", 0);
         b->SetBaseVersion(cl->GetClassVersion());
         elem = b;
      }
   } else if ((typ_id > 0) && (typ_id < 20)) {
      elem = new TStreamerBasicType(name, "title", 0, typ_id, typeName);
   } else if ((typ_id == TStreamerInfo::kObject) ||
              (typ_id == TStreamerInfo::kTObject) ||
              (typ_id == TStreamerInfo::kTNamed)) {
      elem = new TStreamerObject(name, "title", 0, tname.Data());
   } else if (typ_id == TStreamerInfo::kObjectp) {
      elem = new TStreamerObjectPointer(name, "title", 0, tname.Data());
   } else if (typ_id == TStreamerInfo::kAny) {
      elem = new TStreamerObjectAny(name, "title", 0, tname.Data());
   } else if (typ_id == TStreamerInfo::kAnyp) {
      elem = new TStreamerObjectAnyPointer(name, "title", 0, tname.Data());
   } else if (typ_id == TStreamerInfo::kTString) {
      elem = new TStreamerString(name, "title", 0);
   }

   if (elem == 0) {
      Error("ClassMember", "Invalid combination name = %s type = %s",
            name, typeName);
      return;
   }

   if (arrsize1 > 0) {
      elem->SetArrayDim(arrsize2 > 0 ? 2 : 1);
      elem->SetMaxIndex(0, arrsize1);
      if (arrsize2 > 0)
         elem->SetMaxIndex(1, arrsize2);
   }

   // we indicate that there is no streamerinfo
   WorkWithElement(elem, -1);
}

//______________________________________________________________________________
void TBufferJSON::PerformPostProcessing(TJSONStackObj *stack,
                                        const TStreamerElement *elem)
{
   // Function is converts TObject and TString structures to more compact representation

   if ((elem == 0) && stack->fIsPostProcessed) return;
   if (elem == 0) elem = stack->fElem;
   if (elem == 0) return;

   if (gDebug > 3)
      Info("PerformPostProcessing", "Start element %s type %s",
           elem->GetName(), elem->GetTypeName());

   stack->fIsPostProcessed = kTRUE;

   // when element was written as separate object, close only braces and exit
   if (stack->fIsObjStarted) {
      AppendOutput("", "}");
      return;
   }

   const char *typname = stack->fIsBaseClass ? elem->GetName() : elem->GetTypeName();
   Bool_t isTObject = (elem->GetType() == TStreamerInfo::kTObject) || (strcmp("TObject", typname) == 0);
   Bool_t isTString = elem->GetType() == TStreamerInfo::kTString;
   Bool_t isOffsetPArray = (elem->GetType() > TStreamerInfo::kOffsetP) && (elem->GetType() < TStreamerInfo::kOffsetP + 20);

   Bool_t isTArray = (strncmp("TArray", typname, 6) == 0);

   if (!stack->fIsBaseClass && (elem != 0)) {
      AppendOutput(",", "\"");
      AppendOutput(elem->GetName());
      AppendOutput("\"");
      AppendOutput(fSemicolon.Data());
   }

   if (isTString) {
      // in principle, we should just remove all kind of string length information

      if (gDebug > 3)
         Info("PerformPostProcessing", "reformat string value = '%s'", fValue.Data());

      stack->fValues.Delete();
   } else if (isOffsetPArray) {
      // basic array with [fN] comment

      if ((stack->fValues.GetLast() < 0) && (fValue == "0")) {
         fValue = "[]";
      } else if ((stack->fValues.GetLast() == 0) &&
                 (strcmp(stack->fValues.Last()->GetName(), "1") == 0)) {
         stack->fValues.Delete();
      } else {
         Error("PerformPostProcessing", "Wrong values for kOffsetP type %s name %s",
               typname, (elem ? elem->GetName() : "---"));
         stack->fValues.Delete();
         fValue = "[]";
      }
   } else if (isTObject) {
      if (stack->fValues.GetLast() != 0) {
         if (gDebug > 0)
            Error("PerformPostProcessing", "When storing TObject, more than 2 items are stored");
         AppendOutput(",", "\"dummy\"");
         AppendOutput(fSemicolon.Data());
      } else {
         AppendOutput(",", "\"fUniqueID\"");
         AppendOutput(fSemicolon.Data());
         AppendOutput(stack->fValues.At(0)->GetName());
         AppendOutput(",", "\"fBits\"");
         AppendOutput(fSemicolon.Data());
      }

      stack->fValues.Delete();
   } else if (isTArray) {

      if (gDebug > 3)
         Info("PerformPostProcessing", "TArray postprocessing");

      // work around for TArray classes - remove first element with array length

      // only for base class insert fN and fArray tags

      if (stack->fIsBaseClass && (stack->fValues.GetLast() == 0)) {
         AppendOutput(",", "\"fN\"");
         AppendOutput(fSemicolon.Data());
         AppendOutput(stack->fValues.At(0)->GetName());
         AppendOutput(",", "\"fArray\"");
         AppendOutput(fSemicolon.Data());
      }

      stack->fValues.Delete();
   }

   if (stack->fIsBaseClass && !isTArray && !isTObject) {
      if ((fValue.Length() != 0) && (gDebug > 0))
         Error("PerformPostProcessing", "Non-empty value for base class");
      return;
   }

   if (stack->fValues.GetLast() >= 0) {
      AppendOutput("{ ");
      fJsonrCnt++;   // count object, but do not keep reference
      TString sbuf;
      for (Int_t n = 0; n <= stack->fValues.GetLast(); n++) {
         sbuf.Form("\"elem%d\"%s", n, fSemicolon.Data());
         AppendOutput(sbuf.Data());
         AppendOutput(stack->fValues.At(n)->GetName());
         AppendOutput(fArraySepar.Data());
      }
      sbuf.Form("\"elem%d\"%s", stack->fValues.GetLast() + 1, fSemicolon.Data());
      AppendOutput(sbuf.Data());
   }

   if (fValue.Length() == 0) {
      AppendOutput("null");
   } else {
      AppendOutput(fValue.Data());
      fValue.Clear();
   }

   if (stack->fValues.GetLast() >= 0)
      AppendOutput("}");
}

//______________________________________________________________________________
TClass *TBufferJSON::ReadClass(const TClass *, UInt_t *)
{
   // suppressed function of TBuffer

   return 0;
}

//______________________________________________________________________________
void TBufferJSON::WriteClass(const TClass *)
{
   // suppressed function of TBuffer

}

//______________________________________________________________________________
Int_t TBufferJSON::CheckByteCount(UInt_t /*r_s */, UInt_t /*r_c*/,
                                  const TClass * /*cl*/)
{
   // suppressed function of TBuffer

   return 0;
}

//______________________________________________________________________________
Int_t  TBufferJSON::CheckByteCount(UInt_t, UInt_t, const char *)
{
   // suppressed function of TBuffer

   return 0;
}

//______________________________________________________________________________
void TBufferJSON::SetByteCount(UInt_t, Bool_t)
{
   // suppressed function of TBuffer

}

//______________________________________________________________________________
void TBufferJSON::SkipVersion(const TClass *cl)
{
   // Skip class version from I/O buffer.
   ReadVersion(0, 0, cl);
}

//______________________________________________________________________________
Version_t TBufferJSON::ReadVersion(UInt_t *start, UInt_t *bcnt,
                                   const TClass * /*cl*/)
{
   // read version value from buffer

   Version_t res = 0;

   if (start) *start = 0;
   if (bcnt) *bcnt = 0;

   if (gDebug > 3) Info("ReadVersion", "Version = %d", res);

   return res;
}

//______________________________________________________________________________
UInt_t TBufferJSON::WriteVersion(const TClass * /*cl*/, Bool_t /* useBcnt */)
{
   // Ignored in TBufferJSON

   return 0;
}

//______________________________________________________________________________
void *TBufferJSON::ReadObjectAny(const TClass *)
{
   // Read object from buffer. Only used from TBuffer

   return 0;
}

//______________________________________________________________________________
void TBufferJSON::SkipObjectAny()
{
   // Skip any kind of object from buffer
   // Actually skip only one node on current level of xml structure
}

//______________________________________________________________________________
void TBufferJSON::WriteObjectClass(const void *actualObjStart,
                                   const TClass *actualClass)
{
   // Write object to buffer. Only used from TBuffer

   if (gDebug > 3)
      Info("WriteObject", "Class %s", (actualClass ? actualClass->GetName() : " null"));

   JsonWriteObject(actualObjStart, actualClass);
}

#define TJSONPushValue()                                 \
   if (fValue.Length() > 0) Stack()->PushValue(fValue);


// macro to read array, which include size attribute
#define TBufferJSON_ReadArray(tname, vname)              \
   {                                                        \
      if (!vname) return 0;                                 \
      return 1;                                             \
   }

//______________________________________________________________________________
void TBufferJSON::ReadFloat16(Float_t *, TStreamerElement * /*ele*/)
{
   // read a Float16_t from the buffer
}

//______________________________________________________________________________
void TBufferJSON::ReadDouble32(Double_t *, TStreamerElement * /*ele*/)
{
   // read a Double32_t from the buffer
}

//______________________________________________________________________________
void TBufferJSON::ReadWithFactor(Float_t *, Double_t /* factor */,
                                 Double_t /* minvalue */)
{
   // Read a Double32_t from the buffer when the factor and minimun value have
   // been specified
   // see comments about Double32_t encoding at TBufferFile::WriteDouble32().
   // Currently TBufferJSON does not optimize space in this case.

}

//______________________________________________________________________________
void TBufferJSON::ReadWithNbits(Float_t *, Int_t /* nbits */)
{
   // Read a Float16_t from the buffer when the number of bits is specified
   // (explicitly or not)
   // see comments about Float16_t encoding at TBufferFile::WriteFloat16().
   // Currently TBufferJSON does not optimize space in this case.

}

//______________________________________________________________________________
void TBufferJSON::ReadWithFactor(Double_t *, Double_t /* factor */,
                                 Double_t /* minvalue */)
{
   // Read a Double32_t from the buffer when the factor and minimun value have
   // been specified
   // see comments about Double32_t encoding at TBufferFile::WriteDouble32().
   // Currently TBufferJSON does not optimize space in this case.

}

//______________________________________________________________________________
void TBufferJSON::ReadWithNbits(Double_t *, Int_t /* nbits */)
{
   // Read a Double32_t from the buffer when the number of bits is specified
   // (explicitly or not)
   // see comments about Double32_t encoding at TBufferFile::WriteDouble32().
   // Currently TBufferJSON does not optimize space in this case.

}

//______________________________________________________________________________
void TBufferJSON::WriteFloat16(Float_t *f, TStreamerElement * /*ele*/)
{
   // write a Float16_t to the buffer

   TJSONPushValue();

   JsonWriteBasic(*f);
}

//______________________________________________________________________________
void TBufferJSON::WriteDouble32(Double_t *d, TStreamerElement * /*ele*/)
{
   // write a Double32_t to the buffer

   TJSONPushValue();

   JsonWriteBasic(*d);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadArray(Bool_t *&b)
{
   // Read array of Bool_t from buffer

   TBufferJSON_ReadArray(Bool_t, b);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadArray(Char_t *&c)
{
   // Read array of Char_t from buffer

   TBufferJSON_ReadArray(Char_t, c);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadArray(UChar_t *&c)
{
   // Read array of UChar_t from buffer

   TBufferJSON_ReadArray(UChar_t, c);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadArray(Short_t *&h)
{
   // Read array of Short_t from buffer

   TBufferJSON_ReadArray(Short_t, h);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadArray(UShort_t *&h)
{
   // Read array of UShort_t from buffer

   TBufferJSON_ReadArray(UShort_t, h);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadArray(Int_t *&i)
{
   // Read array of Int_t from buffer

   TBufferJSON_ReadArray(Int_t, i);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadArray(UInt_t *&i)
{
   // Read array of UInt_t from buffer

   TBufferJSON_ReadArray(UInt_t, i);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadArray(Long_t *&l)
{
   // Read array of Long_t from buffer

   TBufferJSON_ReadArray(Long_t, l);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadArray(ULong_t *&l)
{
   // Read array of ULong_t from buffer

   TBufferJSON_ReadArray(ULong_t, l);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadArray(Long64_t *&l)
{
   // Read array of Long64_t from buffer

   TBufferJSON_ReadArray(Long64_t, l);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadArray(ULong64_t *&l)
{
   // Read array of ULong64_t from buffer

   TBufferJSON_ReadArray(ULong64_t, l);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadArray(Float_t *&f)
{
   // Read array of Float_t from buffer

   TBufferJSON_ReadArray(Float_t, f);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadArray(Double_t *&d)
{
   // Read array of Double_t from buffer

   TBufferJSON_ReadArray(Double_t, d);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadArrayFloat16(Float_t *&f, TStreamerElement * /*ele*/)
{
   // Read array of Float16_t from buffer

   TBufferJSON_ReadArray(Float_t, f);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadArrayDouble32(Double_t *&d, TStreamerElement * /*ele*/)
{
   // Read array of Double32_t from buffer

   TBufferJSON_ReadArray(Double_t, d);
}

// macro to read array from xml buffer
#define TBufferJSON_ReadStaticArray(vname)               \
   {                                                        \
      if (!vname) return 0;                                 \
      return 1;                                             \
   }

//______________________________________________________________________________
Int_t TBufferJSON::ReadStaticArray(Bool_t *b)
{
   // Read array of Bool_t from buffer

   TBufferJSON_ReadStaticArray(b);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadStaticArray(Char_t *c)
{
   // Read array of Char_t from buffer

   TBufferJSON_ReadStaticArray(c);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadStaticArray(UChar_t *c)
{
   // Read array of UChar_t from buffer

   TBufferJSON_ReadStaticArray(c);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadStaticArray(Short_t *h)
{
   // Read array of Short_t from buffer

   TBufferJSON_ReadStaticArray(h);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadStaticArray(UShort_t *h)
{
   // Read array of UShort_t from buffer

   TBufferJSON_ReadStaticArray(h);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadStaticArray(Int_t *i)
{
   // Read array of Int_t from buffer

   TBufferJSON_ReadStaticArray(i);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadStaticArray(UInt_t *i)
{
   // Read array of UInt_t from buffer

   TBufferJSON_ReadStaticArray(i);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadStaticArray(Long_t *l)
{
   // Read array of Long_t from buffer

   TBufferJSON_ReadStaticArray(l);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadStaticArray(ULong_t *l)
{
   // Read array of ULong_t from buffer

   TBufferJSON_ReadStaticArray(l);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadStaticArray(Long64_t *l)
{
   // Read array of Long64_t from buffer

   TBufferJSON_ReadStaticArray(l);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadStaticArray(ULong64_t *l)
{
   // Read array of ULong64_t from buffer

   TBufferJSON_ReadStaticArray(l);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadStaticArray(Float_t *f)
{
   // Read array of Float_t from buffer

   TBufferJSON_ReadStaticArray(f);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadStaticArray(Double_t *d)
{
   // Read array of Double_t from buffer

   TBufferJSON_ReadStaticArray(d);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadStaticArrayFloat16(Float_t *f, TStreamerElement * /*ele*/)
{
   // Read array of Float16_t from buffer

   TBufferJSON_ReadStaticArray(f);
}

//______________________________________________________________________________
Int_t TBufferJSON::ReadStaticArrayDouble32(Double_t *d, TStreamerElement * /*ele*/)
{
   // Read array of Double32_t from buffer

   TBufferJSON_ReadStaticArray(d);
}

// macro to read content of array, which not include size of array
// macro also treat situation, when instead of one single array chain
// of several elements should be produced
#define TBufferJSON_ReadFastArray(vname)                 \
   {                                                        \
      if (n <= 0) return;                                   \
      if (!vname) return;                                   \
   }

//______________________________________________________________________________
void TBufferJSON::ReadFastArray(Bool_t *b, Int_t n)
{
   // read array of Bool_t from buffer

   TBufferJSON_ReadFastArray(b);
}

//______________________________________________________________________________
void TBufferJSON::ReadFastArray(Char_t *c, Int_t n)
{
   // read array of Char_t from buffer

   TBufferJSON_ReadFastArray(c);
}

//______________________________________________________________________________
void TBufferJSON::ReadFastArrayString(Char_t *c, Int_t n)
{
   // read array of Char_t from buffer

   TBufferJSON_ReadFastArray(c);
}

//______________________________________________________________________________
void TBufferJSON::ReadFastArray(UChar_t *c, Int_t n)
{
   // read array of UChar_t from buffer

   TBufferJSON_ReadFastArray(c);
}

//______________________________________________________________________________
void TBufferJSON::ReadFastArray(Short_t *h, Int_t n)
{
   // read array of Short_t from buffer

   TBufferJSON_ReadFastArray(h);
}

//______________________________________________________________________________
void TBufferJSON::ReadFastArray(UShort_t *h, Int_t n)
{
   // read array of UShort_t from buffer

   TBufferJSON_ReadFastArray(h);
}

//______________________________________________________________________________
void TBufferJSON::ReadFastArray(Int_t *i, Int_t n)
{
   // read array of Int_t from buffer

   TBufferJSON_ReadFastArray(i);
}

//______________________________________________________________________________
void TBufferJSON::ReadFastArray(UInt_t *i, Int_t n)
{
   // read array of UInt_t from buffer

   TBufferJSON_ReadFastArray(i);
}

//______________________________________________________________________________
void TBufferJSON::ReadFastArray(Long_t *l, Int_t n)
{
   // read array of Long_t from buffer

   TBufferJSON_ReadFastArray(l);
}

//______________________________________________________________________________
void TBufferJSON::ReadFastArray(ULong_t *l, Int_t n)
{
   // read array of ULong_t from buffer

   TBufferJSON_ReadFastArray(l);
}

//______________________________________________________________________________
void TBufferJSON::ReadFastArray(Long64_t *l, Int_t n)
{
   // read array of Long64_t from buffer

   TBufferJSON_ReadFastArray(l);
}

//______________________________________________________________________________
void TBufferJSON::ReadFastArray(ULong64_t *l, Int_t n)
{
   // read array of ULong64_t from buffer

   TBufferJSON_ReadFastArray(l);
}

//______________________________________________________________________________
void TBufferJSON::ReadFastArray(Float_t *f, Int_t n)
{
   // read array of Float_t from buffer

   TBufferJSON_ReadFastArray(f);
}

//______________________________________________________________________________
void TBufferJSON::ReadFastArray(Double_t *d, Int_t n)
{
   // read array of Double_t from buffer

   TBufferJSON_ReadFastArray(d);
}

//______________________________________________________________________________
void TBufferJSON::ReadFastArrayFloat16(Float_t *f, Int_t n,
                                       TStreamerElement * /*ele*/)
{
   // read array of Float16_t from buffer

   TBufferJSON_ReadFastArray(f);
}

//______________________________________________________________________________
void TBufferJSON::ReadFastArrayWithFactor(Float_t *f, Int_t n,
      Double_t /* factor */,
      Double_t /* minvalue */)
{
   // read array of Float16_t from buffer

   TBufferJSON_ReadFastArray(f);
}

//______________________________________________________________________________
void TBufferJSON::ReadFastArrayWithNbits(Float_t *f, Int_t n, Int_t /*nbits*/)
{
   // read array of Float16_t from buffer

   TBufferJSON_ReadFastArray(f);
}

//______________________________________________________________________________
void TBufferJSON::ReadFastArrayDouble32(Double_t *d, Int_t n,
                                        TStreamerElement * /*ele*/)
{
   // read array of Double32_t from buffer

   TBufferJSON_ReadFastArray(d);
}

//______________________________________________________________________________
void TBufferJSON::ReadFastArrayWithFactor(Double_t *d, Int_t n,
      Double_t /* factor */,
      Double_t /* minvalue */)
{
   // read array of Double32_t from buffer

   TBufferJSON_ReadFastArray(d);
}

//______________________________________________________________________________
void TBufferJSON::ReadFastArrayWithNbits(Double_t *d, Int_t n, Int_t /*nbits*/)
{
   // read array of Double32_t from buffer

   TBufferJSON_ReadFastArray(d);
}

//______________________________________________________________________________
void TBufferJSON::ReadFastArray(void * /*start*/, const TClass * /*cl*/,
                                Int_t /*n*/, TMemberStreamer * /*s*/,
                                const TClass * /*onFileClass*/)
{
   // redefined here to avoid warning message from gcc

}

//______________________________________________________________________________
void TBufferJSON::ReadFastArray(void ** /*startp*/, const TClass * /*cl*/,
                                Int_t /*n*/, Bool_t /*isPreAlloc*/,
                                TMemberStreamer * /*s*/,
                                const TClass * /*onFileClass*/)
{
   // redefined here to avoid warning message from gcc

}


#define TJSONWriteArrayContent(vname, arrsize)        \
   {                                                     \
      fValue.Append("["); /* fJsonrCnt++; */             \
      for (Int_t indx=0;indx<arrsize;indx++) {           \
         if (indx>0) fValue.Append(fArraySepar.Data());  \
         JsonWriteBasic(vname[indx]);                    \
      }                                                  \
      fValue.Append("]");                                \
   }

// macro to write array, which include size
#define TBufferJSON_WriteArray(vname)                 \
   {                                                     \
      TJSONPushValue();                                  \
      TJSONWriteArrayContent(vname, n);                  \
   }

//______________________________________________________________________________
void TBufferJSON::WriteArray(const Bool_t *b, Int_t n)
{
   // Write array of Bool_t to buffer

   TBufferJSON_WriteArray(b);
}

//______________________________________________________________________________
void TBufferJSON::WriteArray(const Char_t *c, Int_t n)
{
   // Write array of Char_t to buffer

   TBufferJSON_WriteArray(c);
}

//______________________________________________________________________________
void TBufferJSON::WriteArray(const UChar_t *c, Int_t n)
{
   // Write array of UChar_t to buffer

   TBufferJSON_WriteArray(c);
}

//______________________________________________________________________________
void TBufferJSON::WriteArray(const Short_t *h, Int_t n)
{
   // Write array of Short_t to buffer

   TBufferJSON_WriteArray(h);
}

//______________________________________________________________________________
void TBufferJSON::WriteArray(const UShort_t *h, Int_t n)
{
   // Write array of UShort_t to buffer

   TBufferJSON_WriteArray(h);
}

//______________________________________________________________________________
void TBufferJSON::WriteArray(const Int_t *i, Int_t n)
{
   // Write array of Int_ to buffer

   TBufferJSON_WriteArray(i);
}

//______________________________________________________________________________
void TBufferJSON::WriteArray(const UInt_t *i, Int_t n)
{
   // Write array of UInt_t to buffer

   TBufferJSON_WriteArray(i);
}

//______________________________________________________________________________
void TBufferJSON::WriteArray(const Long_t *l, Int_t n)
{
   // Write array of Long_t to buffer

   TBufferJSON_WriteArray(l);
}

//______________________________________________________________________________
void TBufferJSON::WriteArray(const ULong_t *l, Int_t n)
{
   // Write array of ULong_t to buffer

   TBufferJSON_WriteArray(l);
}

//______________________________________________________________________________
void TBufferJSON::WriteArray(const Long64_t *l, Int_t n)
{
   // Write array of Long64_t to buffer

   TBufferJSON_WriteArray(l);
}

//______________________________________________________________________________
void TBufferJSON::WriteArray(const ULong64_t *l, Int_t n)
{
   // Write array of ULong64_t to buffer

   TBufferJSON_WriteArray(l);
}

//______________________________________________________________________________
void TBufferJSON::WriteArray(const Float_t *f, Int_t n)
{
   // Write array of Float_t to buffer

   TBufferJSON_WriteArray(f);
}

//______________________________________________________________________________
void TBufferJSON::WriteArray(const Double_t *d, Int_t n)
{
   // Write array of Double_t to buffer

   TBufferJSON_WriteArray(d);
}

//______________________________________________________________________________
void TBufferJSON::WriteArrayFloat16(const Float_t *f, Int_t n,
                                    TStreamerElement * /*ele*/)
{
   // Write array of Float16_t to buffer

   TBufferJSON_WriteArray(f);
}

//______________________________________________________________________________
void TBufferJSON::WriteArrayDouble32(const Double_t *d, Int_t n,
                                     TStreamerElement * /*ele*/)
{
   // Write array of Double32_t to buffer

   TBufferJSON_WriteArray(d);
}

// write array without size attribute
// macro also treat situation, when instead of one single array
// chain of several elements should be produced
#define TBufferJSON_WriteFastArray(vname)                                 \
   {                                                                         \
      TJSONPushValue();                                                      \
      if (n <= 0) { /*fJsonrCnt++;*/ fValue.Append("[]"); return; }          \
      TStreamerElement* elem = Stack(0)->fElem;                              \
      if ((elem != 0) && (elem->GetType()>TStreamerInfo::kOffsetL) &&        \
            (elem->GetType() < TStreamerInfo::kOffsetP) &&                     \
            (elem->GetArrayLength() != n)) fExpectedChain = kTRUE;             \
      if (fExpectedChain) {                                                  \
         TStreamerInfo* info = Stack(1)->fInfo;                              \
         Int_t startnumber = Stack(0)->fElemNumber;                          \
         fExpectedChain = kFALSE;                                            \
         Int_t index(0);                                          \
         while (index<n) {                                                   \
            elem = (TStreamerElement*)info->GetElements()->At(startnumber++); \
            if (elem->GetType()<TStreamerInfo::kOffsetL) {                    \
               JsonWriteBasic(vname[index]);                                   \
               PerformPostProcessing(Stack(0), elem);                          \
               index++;                                                        \
            }                                                                 \
            else {                                                            \
               Int_t elemlen = elem->GetArrayLength();                         \
               TJSONWriteArrayContent((vname+index), elemlen);                 \
               index+=elemlen;                                                 \
               PerformPostProcessing(Stack(0), elem);                          \
            }                                                                 \
         }                                                                   \
      }                                                                      \
      else {                                                                 \
         TJSONWriteArrayContent(vname, n);                                   \
      }                                                                      \
   }

//______________________________________________________________________________
void TBufferJSON::WriteFastArray(const Bool_t *b, Int_t n)
{
   // Write array of Bool_t to buffer

   TBufferJSON_WriteFastArray(b);
}

//______________________________________________________________________________
void TBufferJSON::WriteFastArray(const Char_t *c, Int_t n)
{
   // Write array of Char_t to buffer
   // If array does not include any special characters,
   // it will be reproduced as CharStar node with string as attribute

   Bool_t usedefault = fExpectedChain;
   const Char_t *buf = c;
   if (!usedefault)
      for (int i = 0; i < n; i++) {
         if (*buf < 27) {
            usedefault = kTRUE;
            break;
         }
         buf++;
      }
   if (usedefault) {
      // TODO - write as array of characters
      TBufferJSON_WriteFastArray(c);
   } else {
      TJSONPushValue();

      // special case - not a zero-reminated string
      fValue.Append("\"");
      if ((c != 0) && (n > 0)) fValue.Append(c, n);
      fValue.Append("\"");
   }
}

//______________________________________________________________________________
void TBufferJSON::WriteFastArrayString(const Char_t *c, Int_t n)
{
   // Write array of Char_t to buffer

   WriteFastArray(c, n);
}


//______________________________________________________________________________
void TBufferJSON::WriteFastArray(const UChar_t *c, Int_t n)
{
   // Write array of UChar_t to buffer

   TBufferJSON_WriteFastArray(c);
}

//______________________________________________________________________________
void TBufferJSON::WriteFastArray(const Short_t *h, Int_t n)
{
   // Write array of Short_t to buffer

   TBufferJSON_WriteFastArray(h);
}

//______________________________________________________________________________
void TBufferJSON::WriteFastArray(const UShort_t *h, Int_t n)
{
   // Write array of UShort_t to buffer

   TBufferJSON_WriteFastArray(h);
}

//______________________________________________________________________________
void TBufferJSON::WriteFastArray(const Int_t *i, Int_t n)
{
   // Write array of Int_t to buffer

   TBufferJSON_WriteFastArray(i);
}

//______________________________________________________________________________
void TBufferJSON::WriteFastArray(const UInt_t *i, Int_t n)
{
   // Write array of UInt_t to buffer

   TBufferJSON_WriteFastArray(i);
}

//______________________________________________________________________________
void TBufferJSON::WriteFastArray(const Long_t *l, Int_t n)
{
   // Write array of Long_t to buffer

   TBufferJSON_WriteFastArray(l);
}

//______________________________________________________________________________
void TBufferJSON::WriteFastArray(const ULong_t *l, Int_t n)
{
   // Write array of ULong_t to buffer

   TBufferJSON_WriteFastArray(l);
}

//______________________________________________________________________________
void TBufferJSON::WriteFastArray(const Long64_t *l, Int_t n)
{
   // Write array of Long64_t to buffer

   TBufferJSON_WriteFastArray(l);
}

//______________________________________________________________________________
void TBufferJSON::WriteFastArray(const ULong64_t *l, Int_t n)
{
   // Write array of ULong64_t to buffer

   TBufferJSON_WriteFastArray(l);
}

//______________________________________________________________________________
void TBufferJSON::WriteFastArray(const Float_t *f, Int_t n)
{
   // Write array of Float_t to buffer

   TBufferJSON_WriteFastArray(f);
}

//______________________________________________________________________________
void TBufferJSON::WriteFastArray(const Double_t *d, Int_t n)
{
   // Write array of Double_t to buffer

   TBufferJSON_WriteFastArray(d);
}

//______________________________________________________________________________
void TBufferJSON::WriteFastArrayFloat16(const Float_t *f, Int_t n,
                                        TStreamerElement * /*ele*/)
{
   // Write array of Float16_t to buffer

   TBufferJSON_WriteFastArray(f);
}

//______________________________________________________________________________
void TBufferJSON::WriteFastArrayDouble32(const Double_t *d, Int_t n,
      TStreamerElement * /*ele*/)
{
   // Write array of Double32_t to buffer

   TBufferJSON_WriteFastArray(d);
}

//______________________________________________________________________________
void  TBufferJSON::WriteFastArray(void *start, const TClass *cl, Int_t n,
                                  TMemberStreamer *streamer)
{
   // Recall TBuffer function to avoid gcc warning message

   if (gDebug > 2)
      Info("WriteFastArray", "void *start");

   if (streamer) {
      JsonStartElement();
      (*streamer)(*this, start, 0);
      return;
   }

   char *obj = (char *)start;
   if (!n) n = 1;
   int size = cl->Size();

   if (n > 1) {
      JsonStartElement();
      AppendOutput("[");
      /* fJsonrCnt++; */ // count array, but do not add to references
   }

   for (Int_t j = 0; j < n; j++, obj += size) {
      // ((TClass*)cl)->Streamer(obj,*this);

      if (j > 0) AppendOutput(fArraySepar.Data());

      JsonWriteObject(obj, cl);
   }

   if (n > 1) {
      AppendOutput(" ]");
   }

}

//______________________________________________________________________________
Int_t TBufferJSON::WriteFastArray(void **start, const TClass *cl, Int_t n,
                                  Bool_t isPreAlloc, TMemberStreamer *streamer)
{
   // Recall TBuffer function to avoid gcc warning message

   if (gDebug > 2)
      Info("WriteFastArray", "void **startp cl %s n %d streamer %p",
           cl->GetName(), n, streamer);

   if (streamer) {
      JsonStartElement();
      (*streamer)(*this, (void *)start, 0);
      return 0;
   }

   Int_t res = 0;

   if (n > 1) {
      JsonStartElement();
      AppendOutput("[");
      /* fJsonrCnt++; */ // count array, but do not add to references
   }

   if (!isPreAlloc) {

      for (Int_t j = 0; j < n; j++) {
         if (j > 0) AppendOutput(fArraySepar.Data());
         res |= WriteObjectAny(start[j], cl);
      }

   } else {
      //case //-> in comment

      for (Int_t j = 0; j < n; j++) {
         if (j > 0) AppendOutput(fArraySepar.Data());

         if (!start[j]) start[j] = ((TClass *)cl)->New();
         // ((TClass*)cl)->Streamer(start[j],*this);
         JsonWriteObject(start[j], cl);
      }
   }

   if (n > 1) {
      AppendOutput("]");
   }

   return res;
}

//______________________________________________________________________________
void TBufferJSON::StreamObject(void *obj, const type_info &typeinfo,
                               const TClass * /* onFileClass */)
{
   // stream object to/from buffer

   StreamObject(obj, TClass::GetClass(typeinfo));
}

//______________________________________________________________________________
void TBufferJSON::StreamObject(void *obj, const char *className,
                               const TClass * /* onFileClass */)
{
   // stream object to/from buffer

   StreamObject(obj, TClass::GetClass(className));
}

void TBufferJSON::StreamObject(TObject *obj)
{
   // stream object to/from buffer

   StreamObject(obj, obj ? obj->IsA() : TObject::Class());
}

//______________________________________________________________________________
void TBufferJSON::StreamObject(void *obj, const TClass *cl,
                               const TClass * /* onfileClass */)
{
   // stream object to/from buffer

   if (gDebug > 3)
      Info("StreamObject", "Class: %s", (cl ? cl->GetName() : "none"));

   JsonWriteObject(obj, cl);
}

//______________________________________________________________________________
void TBufferJSON::ReadBool(Bool_t &)
{
   // Reads Bool_t value from buffer
}

//______________________________________________________________________________
void TBufferJSON::ReadChar(Char_t &)
{
   // Reads Char_t value from buffer
}

//______________________________________________________________________________
void TBufferJSON::ReadUChar(UChar_t &)
{
   // Reads UChar_t value from buffer
}

//______________________________________________________________________________
void TBufferJSON::ReadShort(Short_t &)
{
   // Reads Short_t value from buffer
}

//______________________________________________________________________________
void TBufferJSON::ReadUShort(UShort_t &)
{
   // Reads UShort_t value from buffer
}

//______________________________________________________________________________
void TBufferJSON::ReadInt(Int_t &)
{
   // Reads Int_t value from buffer
}

//______________________________________________________________________________
void TBufferJSON::ReadUInt(UInt_t &)
{
   // Reads UInt_t value from buffer
}

//______________________________________________________________________________
void TBufferJSON::ReadLong(Long_t &)
{
   // Reads Long_t value from buffer
}

//______________________________________________________________________________
void TBufferJSON::ReadULong(ULong_t &)
{
   // Reads ULong_t value from buffer
}

//______________________________________________________________________________
void TBufferJSON::ReadLong64(Long64_t &)
{
   // Reads Long64_t value from buffer
}

//______________________________________________________________________________
void TBufferJSON::ReadULong64(ULong64_t &)
{
   // Reads ULong64_t value from buffer
}

//______________________________________________________________________________
void TBufferJSON::ReadFloat(Float_t &)
{
   // Reads Float_t value from buffer
}

//______________________________________________________________________________
void TBufferJSON::ReadDouble(Double_t &)
{
   // Reads Double_t value from buffer
}

//______________________________________________________________________________
void TBufferJSON::ReadCharP(Char_t *)
{
   // Reads array of characters from buffer
}

//______________________________________________________________________________
void TBufferJSON::ReadTString(TString & /*s*/)
{
   // Reads a TString
}


//______________________________________________________________________________
void TBufferJSON::WriteBool(Bool_t b)
{
   // Writes Bool_t value to buffer

   TJSONPushValue();

   JsonWriteBasic(b);
}

//______________________________________________________________________________
void TBufferJSON::WriteChar(Char_t c)
{
   // Writes Char_t value to buffer

   TJSONPushValue();

   JsonWriteBasic(c);
}

//______________________________________________________________________________
void TBufferJSON::WriteUChar(UChar_t c)
{
   // Writes UChar_t value to buffer

   TJSONPushValue();

   JsonWriteBasic(c);
}

//______________________________________________________________________________
void TBufferJSON::WriteShort(Short_t h)
{
   // Writes Short_t value to buffer

   TJSONPushValue();

   JsonWriteBasic(h);
}

//______________________________________________________________________________
void TBufferJSON::WriteUShort(UShort_t h)
{
   // Writes UShort_t value to buffer

   TJSONPushValue();

   JsonWriteBasic(h);
}

//______________________________________________________________________________
void TBufferJSON::WriteInt(Int_t i)
{
   // Writes Int_t value to buffer

   TJSONPushValue();

   JsonWriteBasic(i);
}

//______________________________________________________________________________
void TBufferJSON::WriteUInt(UInt_t i)
{
   // Writes UInt_t value to buffer

   TJSONPushValue();

   JsonWriteBasic(i);
}

//______________________________________________________________________________
void TBufferJSON::WriteLong(Long_t l)
{
   // Writes Long_t value to buffer

   TJSONPushValue();

   JsonWriteBasic(l);
}

//______________________________________________________________________________
void TBufferJSON::WriteULong(ULong_t l)
{
   // Writes ULong_t value to buffer

   TJSONPushValue();

   JsonWriteBasic(l);
}

//______________________________________________________________________________
void TBufferJSON::WriteLong64(Long64_t l)
{
   // Writes Long64_t value to buffer

   TJSONPushValue();

   JsonWriteBasic(l);
}

//______________________________________________________________________________
void TBufferJSON::WriteULong64(ULong64_t l)
{
   // Writes ULong64_t value to buffer

   TJSONPushValue();

   JsonWriteBasic(l);
}

//______________________________________________________________________________
void TBufferJSON::WriteFloat(Float_t f)
{
   // Writes Float_t value to buffer

   TJSONPushValue();

   JsonWriteBasic(f);
}

//______________________________________________________________________________
void TBufferJSON::WriteDouble(Double_t d)
{
   // Writes Double_t value to buffer

   TJSONPushValue();

   JsonWriteBasic(d);
}

//______________________________________________________________________________
void TBufferJSON::WriteCharP(const Char_t *c)
{
   // Writes array of characters to buffer

   TJSONPushValue();

   fValue.Append("\"");
   fValue.Append(c);
   fValue.Append("\"");
}

//______________________________________________________________________________
void TBufferJSON::WriteTString(const TString &s)
{
   // Writes a TString
   Info("WriteTString", "Write string value");

   TJSONPushValue();

   fValue.Append("\"");
   fValue.Append(s);
   fValue.Append("\"");
}

//______________________________________________________________________________
void TBufferJSON::JsonWriteBasic(Char_t value)
{
   // converts Char_t to string and add xml node to buffer

   char buf[50];
   snprintf(buf, sizeof(buf), "%d", value);
   fValue.Append(buf);
}

//______________________________________________________________________________
void TBufferJSON::JsonWriteBasic(Short_t value)
{
   // converts Short_t to string and add xml node to buffer

   char buf[50];
   snprintf(buf, sizeof(buf), "%hd", value);
   fValue.Append(buf);
}

//______________________________________________________________________________
void TBufferJSON::JsonWriteBasic(Int_t value)
{
   // converts Int_t to string and add xml node to buffer

   char buf[50];
   snprintf(buf, sizeof(buf), "%d", value);
   fValue.Append(buf);
}

//______________________________________________________________________________
void TBufferJSON::JsonWriteBasic(Long_t value)
{
   // converts Long_t to string and add xml node to buffer

   char buf[50];
   snprintf(buf, sizeof(buf), "%ld", value);
   fValue.Append(buf);
}

//______________________________________________________________________________
void TBufferJSON::JsonWriteBasic(Long64_t value)
{
   // converts Long64_t to string and add xml node to buffer

   char buf[50];
   snprintf(buf, sizeof(buf), FLong64, value);
   fValue.Append(buf);
}

//______________________________________________________________________________
void TBufferJSON::JsonWriteBasic(Float_t value)
{
   // converts Float_t to string and add xml node to buffer

   char buf[200];
   snprintf(buf, sizeof(buf), fgFloatFmt, value);
   fValue.Append(buf);
}

//______________________________________________________________________________
void TBufferJSON::JsonWriteBasic(Double_t value)
{
   // converts Double_t to string and add xml node to buffer

   char buf[1000];
   snprintf(buf, sizeof(buf), fgFloatFmt, value);
   fValue.Append(buf);
}

//______________________________________________________________________________
void TBufferJSON::JsonWriteBasic(Bool_t value)
{
   // converts Bool_t to string and add xml node to buffer

   fValue.Append(value ? "true" : "false");
}

//______________________________________________________________________________
void TBufferJSON::JsonWriteBasic(UChar_t value)
{
   // converts UChar_t to string and add xml node to buffer

   char buf[50];
   snprintf(buf, sizeof(buf), "%u", value);
   fValue.Append(buf);
}

//______________________________________________________________________________
void TBufferJSON::JsonWriteBasic(UShort_t value)
{
   // converts UShort_t to string and add xml node to buffer

   char buf[50];
   snprintf(buf, sizeof(buf), "%hu", value);
   fValue.Append(buf);
}

//______________________________________________________________________________
void TBufferJSON::JsonWriteBasic(UInt_t value)
{
   // converts UInt_t to string and add xml node to buffer

   char buf[50];
   snprintf(buf, sizeof(buf), "%u", value);
   fValue.Append(buf);
}

//______________________________________________________________________________
void TBufferJSON::JsonWriteBasic(ULong_t value)
{
   // converts ULong_t to string and add xml node to buffer

   char buf[50];
   snprintf(buf, sizeof(buf), "%lu", value);
   fValue.Append(buf);
}

//______________________________________________________________________________
void TBufferJSON::JsonWriteBasic(ULong64_t value)
{
   // converts ULong64_t to string and add xml node to buffer

   char buf[50];
   snprintf(buf, sizeof(buf), FULong64, value);
   fValue.Append(buf);
}

//______________________________________________________________________________
void TBufferJSON::SetFloatFormat(const char *fmt)
{
   // set printf format for float/double members, default "%e"

   if (fmt == 0) fmt = "%e";
   fgFloatFmt = fmt;
}

//______________________________________________________________________________
const char *TBufferJSON::GetFloatFormat()
{
   // return current printf format for float/double members, default "%e"

   return fgFloatFmt;
}

//______________________________________________________________________________
Int_t TBufferJSON::ApplySequence(const TStreamerInfoActions::TActionSequence &sequence,
                                 void *obj)
{
   // Read one collection of objects from the buffer using the StreamerInfoLoopAction.
   // The collection needs to be a split TClonesArray or a split vector of pointers.

   TVirtualStreamerInfo *info = sequence.fStreamerInfo;
   IncrementLevel(info);

   if (gDebug) {
      //loop on all active members
      TStreamerInfoActions::ActionContainer_t::const_iterator end = sequence.fActions.end();
      for (TStreamerInfoActions::ActionContainer_t::const_iterator iter = sequence.fActions.begin();
            iter != end; ++iter) {
         // Idea: Try to remove this function call as it is really needed only for XML streaming.
         SetStreamerElementNumber((*iter).fConfiguration->fCompInfo->fElem,(*iter).fConfiguration->fCompInfo->fType);
         (*iter).PrintDebug(*this, obj);
         (*iter)(*this, obj);
      }
   } else {
      //loop on all active members
      TStreamerInfoActions::ActionContainer_t::const_iterator end = sequence.fActions.end();
      for (TStreamerInfoActions::ActionContainer_t::const_iterator iter = sequence.fActions.begin();
            iter != end;  ++iter) {
         // Idea: Try to remove this function call as it is really needed only for XML streaming.
         SetStreamerElementNumber((*iter).fConfiguration->fCompInfo->fElem,(*iter).fConfiguration->fCompInfo->fType);
         (*iter)(*this, obj);
      }
   }
   DecrementLevel(info);
   return 0;
}

//______________________________________________________________________________
Int_t TBufferJSON::ApplySequenceVecPtr(const TStreamerInfoActions::TActionSequence &sequence,
                                       void *start_collection, void *end_collection)
{
   // Read one collection of objects from the buffer using the StreamerInfoLoopAction.
   // The collection needs to be a split TClonesArray or a split vector of pointers.

   TVirtualStreamerInfo *info = sequence.fStreamerInfo;
   IncrementLevel(info);

   if (gDebug) {
      //loop on all active members
      TStreamerInfoActions::ActionContainer_t::const_iterator end = sequence.fActions.end();
      for (TStreamerInfoActions::ActionContainer_t::const_iterator iter = sequence.fActions.begin();
            iter != end; ++iter) {
         // Idea: Try to remove this function call as it is really needed only for XML streaming.
         SetStreamerElementNumber((*iter).fConfiguration->fCompInfo->fElem,(*iter).fConfiguration->fCompInfo->fType);
         (*iter).PrintDebug(*this, *(char **)start_collection); // Warning: This limits us to TClonesArray and vector of pointers.
         (*iter)(*this, start_collection, end_collection);
      }
   } else {
      //loop on all active members
      TStreamerInfoActions::ActionContainer_t::const_iterator end = sequence.fActions.end();
      for (TStreamerInfoActions::ActionContainer_t::const_iterator iter = sequence.fActions.begin();
            iter != end; ++iter) {
         // Idea: Try to remove this function call as it is really needed only for XML streaming.
         SetStreamerElementNumber((*iter).fConfiguration->fCompInfo->fElem,(*iter).fConfiguration->fCompInfo->fType);
         (*iter)(*this, start_collection, end_collection);
      }
   }
   DecrementLevel(info);
   return 0;
}

//______________________________________________________________________________
Int_t TBufferJSON::ApplySequence(const TStreamerInfoActions::TActionSequence &sequence,
                                 void *start_collection, void *end_collection)
{
   // Read one collection of objects from the buffer using the StreamerInfoLoopAction.

   TVirtualStreamerInfo *info = sequence.fStreamerInfo;
   IncrementLevel(info);

   TStreamerInfoActions::TLoopConfiguration *loopconfig = sequence.fLoopConfig;
   if (gDebug) {

      // Get the address of the first item for the PrintDebug.
      // (Performance is not essential here since we are going to print to
      // the screen anyway).
      void *arr0 = loopconfig->GetFirstAddress(start_collection, end_collection);
      // loop on all active members
      TStreamerInfoActions::ActionContainer_t::const_iterator end = sequence.fActions.end();
      for (TStreamerInfoActions::ActionContainer_t::const_iterator iter = sequence.fActions.begin();
            iter != end; ++iter) {
         // Idea: Try to remove this function call as it is really needed only for XML streaming.
         SetStreamerElementNumber((*iter).fConfiguration->fCompInfo->fElem,(*iter).fConfiguration->fCompInfo->fType);
         (*iter).PrintDebug(*this, arr0);
         (*iter)(*this, start_collection, end_collection, loopconfig);
      }
   } else {
      //loop on all active members
      TStreamerInfoActions::ActionContainer_t::const_iterator end = sequence.fActions.end();
      for (TStreamerInfoActions::ActionContainer_t::const_iterator iter = sequence.fActions.begin();
            iter != end; ++iter) {
         // Idea: Try to remove this function call as it is really needed only for XML streaming.
         SetStreamerElementNumber((*iter).fConfiguration->fCompInfo->fElem,(*iter).fConfiguration->fCompInfo->fType);
         (*iter)(*this, start_collection, end_collection, loopconfig);
      }
   }
   DecrementLevel(info);
   return 0;
}


//______________________________________________________________________________
Int_t TBufferJSON::WriteClones(TClonesArray *a, Int_t /*nobjects*/)
{
   // Interface to TStreamerInfo::WriteBufferClones.

   Info("WriteClones", "Not yet tested");

   if (a != 0)
      JsonStreamCollection(a, a->IsA());

   return 0;
}

namespace {
   struct DynamicType {
      // Helper class to enable typeid on any address
      // Used in code similar to:
      //    typeid( * (DynamicType*) void_ptr );
      virtual ~DynamicType() {}
   };
}

//______________________________________________________________________________
Int_t TBufferJSON::WriteObjectAny(const void *obj, const TClass *ptrClass)
{
   // Write object to I/O buffer.
   // This function assumes that the value in 'obj' is the value stored in
   // a pointer to a "ptrClass". The actual type of the object pointed to
   // can be any class derived from "ptrClass".
   // Return:
   //  0: failure
   //  1: success
   //  2: truncated success (i.e actual class is missing. Only ptrClass saved.)

   if (!obj) {
      WriteObjectClass(0, 0);
      return 1;
   }

   if (!ptrClass) {
      Error("WriteObjectAny", "ptrClass argument may not be 0");
      return 0;
   }

   TClass *clActual = ptrClass->GetActualClass(obj);

   if (clActual == 0) {
      // The ptrClass is a class with a virtual table and we have no
      // TClass with the actual type_info in memory.

      DynamicType *d_ptr = (DynamicType *)obj;
      Warning("WriteObjectAny",
              "An object of type %s (from type_info) passed through a %s pointer was truncated (due a missing dictionary)!!!",
              typeid(*d_ptr).name(), ptrClass->GetName());
      WriteObjectClass(obj, ptrClass);
      return 2;
   } else if (clActual && (clActual != ptrClass)) {
      const char *temp = (const char *) obj;
      temp -= clActual->GetBaseClassOffset(ptrClass);
      WriteObjectClass(temp, clActual);
      return 1;
   } else {
      WriteObjectClass(obj, ptrClass);
      return 1;
   }
}

//______________________________________________________________________________
Int_t TBufferJSON::WriteClassBuffer(const TClass *cl, void *pointer)
{
   // Function called by the Streamer functions to serialize object at p
   // to buffer b. The optional argument info may be specified to give an
   // alternative StreamerInfo instead of using the default StreamerInfo
   // automatically built from the class definition.
   // For more information, see class TStreamerInfo.


   //build the StreamerInfo if first time for the class
   TStreamerInfo *sinfo = (TStreamerInfo *)const_cast<TClass *>(cl)->GetCurrentStreamerInfo();
   if (sinfo == 0) {
      const_cast<TClass *>(cl)->BuildRealData(pointer);
      sinfo = new TStreamerInfo(const_cast<TClass *>(cl));
      const_cast<TClass *>(cl)->SetCurrentStreamerInfo(sinfo);

#if ROOT_VERSION_CODE >= ROOT_VERSION(5,99,1)
      const_cast<TClass *>(cl)->RegisterStreamerInfo(sinfo);
#else
      cl->GetStreamerInfos()->AddAtAndExpand(sinfo, cl->GetClassVersion());
#endif

      if (gDebug > 0)
         printf("Creating StreamerInfo for class: %s, version: %d\n",
                cl->GetName(), cl->GetClassVersion());
      sinfo->Build();
   } else if (!sinfo->IsCompiled()) {
      const_cast<TClass *>(cl)->BuildRealData(pointer);
      sinfo->BuildOld();
   }

   //write the class version number and reserve space for the byte count
   // UInt_t R__c = WriteVersion(cl, kTRUE);

   //NOTE: In the future Philippe wants this to happen via a custom action
   TagStreamerInfo(sinfo);
   ApplySequence(*(sinfo->GetWriteObjectWiseActions()), (char *)pointer);

   //write the byte count at the start of the buffer
   // SetByteCount(R__c, kTRUE);

   if (gDebug > 2)
      printf(" TBufferJSON::WriteClassBuffer for class: %s version %d\n",
             cl->GetName(), cl->GetClassVersion());
   return 0;
}
