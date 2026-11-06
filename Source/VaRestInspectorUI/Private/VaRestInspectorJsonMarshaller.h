// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "Framework/Text/PlainTextLayoutMarshaller.h"
#include "Framework/Text/SyntaxHighlighterTextLayoutMarshaller.h"
#include "Framework/Text/SlateTextLayout.h"
#include "Framework/Text/SlateTextRun.h"

#if WITH_FANCY_TEXT
/** Line tokens keep escape-aware JSON classification in the marshaller, with no regex or rich markup. */
class FVaRestInspectorJsonLineTokenizer final : public ISyntaxTokenizer
{
public:
 void Process(TArray<FTokenizedLine>& Out,const FString& Input) override
 {
  TArray<FTextRange> Ranges; FTextRange::CalculateLineRangesFromString(Input,Ranges); Out.Reset(Ranges.Num());
  for (const auto& Range:Ranges) { auto& Line=Out.AddDefaulted_GetRef(); Line.Range=Range; Line.Tokens.Emplace(ETokenType::Literal,Range); }
 }
};
using FVaRestInspectorJsonMarshallerBase=FSyntaxHighlighterTextLayoutMarshaller;
#else
using FVaRestInspectorJsonMarshallerBase=FPlainTextLayoutMarshaller;
#endif

/** Colour native text runs only. Captured strings never become rich-text markup. */
class FVaRestInspectorJsonMarshaller final : public FVaRestInspectorJsonMarshallerBase
{
public:
 FVaRestInspectorJsonMarshaller()
#if WITH_FANCY_TEXT
  : FVaRestInspectorJsonMarshallerBase(MakeShared<FVaRestInspectorJsonLineTokenizer>())
#endif
 { }
 void Enable(bool Value)
 {
  if (bEnabled==Value) return; bEnabled=Value;
#if WITH_FANCY_TEXT
  EnableSyntaxHighlighting(Value);
#else
  MakeDirty();
#endif
 }
 bool RequiresLiveUpdate() const override { return false; } // read-only text changes drive layout refresh
 void SetText(const FString& Source,FTextLayout& Layout) override
 {
  if (!bEnabled || Source.Len()>64*1024) { FPlainTextLayoutMarshaller::SetText(Source,Layout); return; }
#if WITH_FANCY_TEXT
  FSyntaxHighlighterTextLayoutMarshaller::SetText(Source,Layout);
#else
  FPlainTextLayoutMarshaller::SetText(Source,Layout);
#endif
 }
#if WITH_FANCY_TEXT
protected:
 void ParseTokens(const FString& Source,FTextLayout& Layout,TArray<ISyntaxTokenizer::FTokenizedLine> TokenizedLines) override
 {
  if (TokenizedLines.Num()>20000) { FPlainTextLayoutMarshaller::SetText(Source,Layout); return; }
  const auto Normal=static_cast<FSlateTextLayout&>(Layout).GetDefaultTextStyle();
  auto Key=Normal,Value=Normal,Number=Normal,Literal=Normal;
  Key.SetColorAndOpacity(FLinearColor(.4f,.75f,1.f)); Value.SetColorAndOpacity(FLinearColor(.65f,.85f,.45f));
  Number.SetColorAndOpacity(FLinearColor(1.f,.72f,.35f)); Literal.SetColorAndOpacity(FLinearColor(.8f,.6f,.95f));
  TArray<FTextLayout::FNewLineData> Lines; Lines.Reserve(TokenizedLines.Num()); int32 RunCount=0;
  for (int32 LineIndex=0;LineIndex<TokenizedLines.Num();++LineIndex)
  {
   const auto Range=TokenizedLines[LineIndex].Range; auto Text=MakeShared<FString>(Source.Mid(Range.BeginIndex,Range.Len()));
   TArray<TSharedRef<IRun>> Runs; const int32 Size=Text->Len(); int32 At=0;
   while (At<Size)
   {
    const int32 Begin=At; const FTextBlockStyle* Style=&Normal; const TCHAR C=(*Text)[At++];
    // Reserve a plain run for this suffix and each remaining line before admitting more styled runs.
    if (RunCount>=32768-(TokenizedLines.Num()-LineIndex)) At=Size;
    else if (C==TCHAR('"'))
    {
     while (At<Size) { const TCHAR Next=(*Text)[At++]; if (Next==TCHAR('\\') && At<Size) ++At; else if (Next==TCHAR('"')) break; }
     int32 Next=At; while (Next<Size && FChar::IsWhitespace((*Text)[Next])) ++Next;
     Style=Next<Size && (*Text)[Next]==TCHAR(':') ? &Key : &Value;
    }
    else if (FChar::IsDigit(C) || C==TCHAR('-'))
    {
     while (At<Size && (FChar::IsDigit((*Text)[At]) || (*Text)[At]==TCHAR('.') || (*Text)[At]==TCHAR('e') || (*Text)[At]==TCHAR('E') || (*Text)[At]==TCHAR('+') || (*Text)[At]==TCHAR('-'))) ++At;
     Style=&Number;
    }
    else if (FChar::IsAlpha(C)) { while (At<Size && FChar::IsAlpha((*Text)[At])) ++At; Style=&Literal; }
    else if (FChar::IsWhitespace(C)) { while (At<Size && FChar::IsWhitespace((*Text)[At])) ++At; }
    Runs.Add(FSlateTextRun::Create(FRunInfo(),Text,*Style,FTextRange(Begin,At))); ++RunCount;
   }
   if (!Size) { Runs.Add(FSlateTextRun::Create(FRunInfo(),Text,Normal)); ++RunCount; }
   Lines.Emplace(MoveTemp(Text),MoveTemp(Runs));
  }
  Layout.AddLines(Lines);
 }
#endif
private:
 bool bEnabled=false;
};
