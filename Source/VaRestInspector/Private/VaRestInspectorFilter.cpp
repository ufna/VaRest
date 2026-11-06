// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestInspectorFilter.h"
#include "VaRestHttpTypes.h"
namespace
{
	TArray<FString> TokenizeImpl(const FString& Input)
	{
		TArray<FString> Out; FString Token; bool bQuote = false;
		for (const TCHAR C : Input)
		{
			if (C == TEXT('"')) bQuote = !bQuote;
			else if (FChar::IsWhitespace(C) && !bQuote) { if (!Token.IsEmpty()) { Out.Add(MoveTemp(Token)); Token.Reset(); } }
			else Token.AppendChar(C);
		}
		if (!Token.IsEmpty()) Out.Add(MoveTemp(Token)); return Out;
	}
	bool StatusMatches(int32 Status, const FString& Value)
	{
		if (Value.Len() == 3 && FChar::ToLower(Value[1]) == TEXT('x') && FChar::ToLower(Value[2]) == TEXT('x') && FChar::IsDigit(Value[0])) return Status / 100 == Value[0] - TEXT('0');
		int32 Number = 0;
		for (const FString& Operator : {FString(TEXT(">=")), FString(TEXT("<=")), FString(TEXT(">")), FString(TEXT("<"))})
			if (Value.StartsWith(Operator) && LexTryParseString(Number, *Value.Mid(Operator.Len())))
				return Operator == TEXT(">=") ? Status >= Number : Operator == TEXT("<=") ? Status <= Number : Operator == TEXT(">") ? Status > Number : Status < Number;
		return LexTryParseString(Number, *Value) && Number == Status;
	}
	bool Quantity(const FString& TextValue, double& Value, bool bTime)
	{
		FString Text = TextValue.ToLower(); double Scale = 1;
		if (Text.RemoveFromEnd(TEXT("ms"))) Scale = .001;
		else if (bTime) Text.RemoveFromEnd(TEXT("s"));
		else if (Text.RemoveFromEnd(TEXT("k"))) Scale = 1024;
		else if (Text.RemoveFromEnd(TEXT("m"))) Scale = 1024 * 1024;
		if (!LexTryParseString(Value, *Text) || !FMath::IsFinite(Value) || Value < 0) return false;
		Value *= Scale; return FMath::IsFinite(Value);
	}
}
TArray<FString> FVaRestInspectorFilter::Tokenize(FStringView Text) { return TokenizeImpl(FString(Text.Left(4096))); }
void FVaRestInspectorFilter::SetText(FStringView Text)
{ Terms=Tokenize(Text); }
bool FVaRestInspectorFilter::Matches(const FVaRestInspectorFilterFields& Row) const
{
	for (FString Term : Terms)
	{
		const bool Negate = Term.RemoveFromStart(TEXT("-")); FString Key, Value; bool Match = false;
		if (!Term.Split(TEXT(":"), &Key, &Value)) Match = Row.Searchable.Contains(Term, ESearchCase::IgnoreCase);
		else if ((Key = Key.ToLower()) == TEXT("status")) Match = StatusMatches(Row.Status, Value);
		else if (Key == TEXT("id")) Match = LexToString(Row.Id) == Value;
		else if (Key == TEXT("is"))
		{
			Value.ToLowerInline(); Match = Value == TEXT("error") ? Row.bError : Value == TEXT("running") ? Row.bRunning : Value == TEXT("cached") ? Row.bCached
				: Value == TEXT("socket") ? Row.bSocket : Value == TEXT("sio") ? Row.bSio : Value == TEXT("queued") ? Row.bQueued : Value == TEXT("retried") ? Row.Attempts > 1 : Value == TEXT("auth") ? Row.bAuth : false;
		}
		else if (Key == TEXT("larger-than") || Key == TEXT("slower-than"))
		{ double Limit = 0; const bool Time = Key == TEXT("slower-than"); Match = Quantity(Value, Limit, Time) && (Time ? Row.Duration > Limit : double(Row.Bytes) > Limit); }
		else
		{
			const FStringView* Field = Key == TEXT("method") ? &Row.Method : Key == TEXT("client") ? &Row.Client : Key == TEXT("endpoint") ? &Row.Name
				: Key == TEXT("initiator") ? &Row.Initiator : Key == TEXT("tag") ? &Row.Tag : Key == TEXT("scope") ? &Row.Scope : nullptr;
			Match = (Key == TEXT("scope") && Row.bContextKnown && LexToString(Row.ScopeId) == Value) || (Field ? Field->Contains(Value, ESearchCase::IgnoreCase) : Row.Searchable.Contains(Term, ESearchCase::IgnoreCase));
		}
		if (Match == Negate) return false;
	}
	return true;
}
bool FVaRestInspectorFilter::Matches(const FVaRestInspectorRecordView& Row) const
{
 using EText=VaRest::Inspector::View::EText;
 FVaRestInspectorFilterFields F;
 const FString Name=Row.Kind==EVaRestInspectorRecordKind::Socket ? (Row.SocketName.IsNone() ? Row.SocketProtocol.ToString() : Row.SocketName.ToString()) : Row.Kind==EVaRestInspectorRecordKind::Marker ? FString(Row.Marker.Text(EText::MarkerText)) : Row.Started.Fixed && !Row.Started.Fixed->Scalars.Endpoint.IsNone() ? Row.Started.Fixed->Scalars.Endpoint.ToString() : FString(Row.Started.Text(EText::PathTemplate));
 const FString Tag=Row.Kind==EVaRestInspectorRecordKind::Socket ? Row.SocketProtocol.ToString() : Row.Started.Fixed ? Row.Started.Fixed->Scalars.Tag.ToString() : FString();
 const FString SocketClient=Row.SocketEntry.IsNone() ? Row.SocketProtocol.ToString() : Row.SocketEntry.ToString();
 const FStringView Initiator=!Row.Started.Text(EText::ScriptFunction).IsEmpty() ? Row.Started.Text(EText::ScriptFunction) : !Row.Started.Text(EText::SourceFile).IsEmpty() ? Row.Started.Text(EText::SourceFile) : Row.Started.Text(EText::OwnerPath);
 const FString Searchable=FString(Row.Started.Text(EText::Url))+TEXT(" ")+Name+TEXT(" ")+FString(Initiator)+TEXT(" ")+(Row.Kind==EVaRestInspectorRecordKind::Socket ? SocketClient : FString(Row.ClientLabel))+TEXT(" ")+Tag;
 F.Id=Row.RequestId; F.ScopeId=Row.ScopeId; F.Bytes=Row.WireBytesDown; F.Attempts=Row.ObservedDispatches; F.Status=Row.FinalStatus; F.Duration=Row.Total.bKnown ? Row.Total.Seconds : -1;
 F.Method=Row.Kind==EVaRestInspectorRecordKind::Socket ? (Row.SocketProtocol==TEXT("sio") || Row.SocketProtocol==TEXT("SocketIO") ? FStringView(TEXT("SIO")) : FStringView(TEXT("WS"))) : Row.Kind==EVaRestInspectorRecordKind::Marker ? FStringView(TEXT("MARK")) : Row.Started.Text(EText::Verb); F.Name=Name; F.Url=Row.Started.Text(EText::Url); F.Initiator=Initiator; F.Scope=Row.ScopeLabel; F.Client=Row.Kind==EVaRestInspectorRecordKind::Socket ? FStringView(SocketClient) : Row.ClientLabel; F.Tag=Tag; F.Searchable=Searchable;
 F.bContextKnown=Row.bContextKnown; F.bError=Row.EndState==EVaRestInspectorEndState::Error; F.bRunning=Row.EndState==EVaRestInspectorEndState::InFlight; F.bQueued=Row.EndState==EVaRestInspectorEndState::Queued;
 F.bSocket=Row.Kind==EVaRestInspectorRecordKind::Socket; F.bSio=F.bSocket && (Row.SocketProtocol==TEXT("sio") || Row.SocketProtocol==TEXT("SocketIO"));
 F.bCached=Row.Completed.Fixed && Row.Completed.Fixed->Scalars.bFromCache; F.bAuth=Row.Started.Fixed && EnumHasAnyFlags(Row.Started.Fixed->Scalars.Flags,EVaRestTraceRequestFlags::AuthRefresh);
 return Matches(F);
}
