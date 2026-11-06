// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "VaRestSioSocket.h"
#include "VaRestSioLog.h"
#include "UObject/GCObject.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#include <charconv>
#include <tuple>
#include <type_traits>
#include <utility>

namespace VaRest::Sio::Typed
{
template <typename F> struct TSignature { static constexpr bool Valid = false; using Arguments = std::tuple<>; };
template <typename R, typename... A> struct TSignature<R(*)(A...)> { static constexpr bool Valid = true; using Arguments = std::tuple<A...>; };
template <typename R, typename... A> struct TSignature<R(*)(A...) noexcept> : TSignature<R(*)(A...)> {};
template <typename R, typename C, typename... A> struct TSignature<R(C::*)(A...) const> : TSignature<R(*)(A...)> {};
template <typename R, typename C, typename... A> struct TSignature<R(C::*)(A...)> : TSignature<R(*)(A...)> {};
template <typename R, typename C, typename... A> struct TSignature<R(C::*)(A...) const noexcept> : TSignature<R(*)(A...)> {};
template <typename R, typename C, typename... A> struct TSignature<R(C::*)(A...) noexcept> : TSignature<R(*)(A...)> {};
template <typename F, typename = void> struct TCallable : TSignature<std::decay_t<F>> {};
template <typename F> struct TCallable<F, std::void_t<decltype(&std::decay_t<F>::operator())>> : TSignature<decltype(&std::decay_t<F>::operator())> {};

/** Metadata is prepared on GT, outside collection. Collection never asks reflection to construct a descriptor. */
template <typename T, bool bStruct = VaRest::Json::bHasScriptStruct<T> && !std::is_same_v<T, FVaRestJson>> struct TObjectRoots
{ void Collect(T&, FReferenceCollector&) {} };
template <typename T> struct TObjectRoots<T, true>
{
	TStrongObjectPtr<const UScriptStruct> Type{VaRest::Json::GetScriptStruct<T>()};
	void Collect(T& Value, FReferenceCollector& Collector) { if (Type.Get()) Collector.AddPropertyReferencesWithStructARO(Type.Get(), &Value); }
};
template <typename T, typename A> struct TObjectRoots<TArray<T, A>, false>
{
	TObjectRoots<T> Element;
	void Collect(TArray<T, A>& Values, FReferenceCollector& Collector) { for (auto& Value : Values) Element.Collect(Value, Collector); }
};
template <typename T, typename K, typename A> struct TObjectRoots<TSet<T, K, A>, false>
{
	TObjectRoots<T> Element;
	void Collect(TSet<T, K, A>& Values, FReferenceCollector& Collector) { for (const auto& Value : Values) Element.Collect(const_cast<T&>(Value), Collector); }
};
template <typename K, typename V, typename A, typename F> struct TObjectRoots<TMap<K, V, A, F>, false>
{
	TObjectRoots<V> Element;
	void Collect(TMap<K, V, A, F>& Values, FReferenceCollector& Collector) { for (auto& Pair : Values) Element.Collect(Pair.Value, Collector); }
};
template <typename T> struct TObjectRoots<TOptional<T>, false>
{
	TObjectRoots<T> Element;
	void Collect(TOptional<T>& Value, FReferenceCollector& Collector) { if (Value.IsSet()) Element.Collect(Value.GetValue(), Collector); }
};
class FInvocationRoots final : public FGCObject
{
public:
	explicit FInvocationRoots(TFunctionRef<void(FReferenceCollector&)> InCollect) : Collect(InCollect) { check(IsInGameThread()); }
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override { Collect(Collector); }
	virtual FString GetReferencerName() const override { return TEXT("VaRest.SocketIO.NativeHandler"); }
private:
	TFunctionRef<void(FReferenceCollector&)> Collect;
};
inline bool Issue(FVaRestJsonDecodeReport& Report, bool bStrict, FStringView Path, FStringView Message)
{
	FVaRestJsonError Error; Error.Path = FString(Path); Error.Message = FString(Message);
	if (bStrict) Report.Errors.Add(MoveTemp(Error)); else Report.Warnings.Add(MoveTemp(Error)); return !bStrict;
}
inline void AppendReport(FVaRestJsonDecodeReport& Into, FVaRestJsonDecodeReport From, FStringView Prefix)
{
	for (auto& Item : From.Errors) { Item.Path = FString(Prefix) + Item.Path; Into.Errors.Add(MoveTemp(Item)); }
	for (auto& Item : From.Warnings) { Item.Path = FString(Prefix) + Item.Path; Into.Warnings.Add(MoveTemp(Item)); }
	Into.bBudgetExceeded |= From.bBudgetExceeded;
}

template <typename T> bool NumberFromString(FString Text, T& Out)
{
	Text.TrimStartAndEndInline();
	for (const TCHAR Ch : Text) if (Ch > 127 || Ch == 0) return false;
	const FTCHARToUTF8 Utf8(*Text, Text.Len()); const char* Begin = Utf8.Get(); const char* End = Begin + Utf8.Length();
	if (Begin != End && *Begin == '+') ++Begin;
	if (Begin == End) return false;
	T ParsedValue{}; const auto Parsed = std::from_chars(Begin, End, ParsedValue);
	if (Parsed.ec != std::errc() || Parsed.ptr != End) return false;
	if constexpr (std::is_floating_point_v<T>) if (!FMath::IsFinite(ParsedValue)) return false;
	Out = ParsedValue;
	return true;
}

template <typename T> bool Read(const FVaRestJson& Json, T& Value, const FVaRestSioEvent& Event,
	const FVaRestJsonConvertOptions& Options, FVaRestJsonDecodeReport& Report, FStringView Path, int32 Depth = 1)
{
	using U = std::remove_cvref_t<T>;
	if (Depth > VaRest::Json::MaxDepth) return Issue(Report, true, Path, TEXTVIEW("Socket.IO argument exceeds the nesting limit"));
	if (!Json.IsValid()) return Issue(Report, Options.bStrict, Path, TEXTVIEW("missing Socket.IO argument; kept its default"));
	if constexpr (std::is_same_v<U, FVaRestJson>) { Value = Json; return true; }
	else if constexpr (VaRest::Json::bHasScriptStruct<U>)
	{
		FVaRestJsonDecodeReport Native;
		const bool bRead = FVaRestJsonConvert::JsonToStruct(Json, VaRest::Json::GetScriptStruct<U>(), &Value, Options, &Native);
		AppendReport(Report, MoveTemp(Native), Path); return bRead;
	}
	else if constexpr (TArgumentOptional<U>::Value)
	{
		if (Json.IsNull()) { Value.Reset(); return true; }
		typename TArgumentOptional<U>::Element Item{}; TObjectRoots<typename TArgumentOptional<U>::Element> Metadata;
		auto Collect = [&](FReferenceCollector& Collector) { Metadata.Collect(Item, Collector); }; FInvocationRoots Root(Collect);
		if (!Read(Json, Item, Event, Options, Report, Path, Depth)) return false; Value = MoveTemp(Item); return true;
	}
	else if constexpr (TArgumentArray<U>::Value || TArgumentSet<U>::Value)
	{
		if constexpr (std::is_same_v<U, TArray<uint8>>)
		{
			const auto Id = Json.Get(TEXT("num")).TryGet<int32>(); const auto Placeholder = Json.Get(TEXT("_placeholder"));
			if (Placeholder.IsBool() && Placeholder.AsBool() && Id.IsSet())
			{
				const auto& Attachments = Event.GetNativeMessage().Attachments;
				if (!Attachments.IsValidIndex(Id.GetValue()) || !Attachments[Id.GetValue()]) return Issue(Report, true, Path, TEXTVIEW("invalid Socket.IO attachment placeholder"));
				Value = *Attachments[Id.GetValue()]; return true;
			}
		}
		if (!Json.IsArray()) return Issue(Report, Options.bStrict, Path, TEXTVIEW("expected a Socket.IO array argument; kept its default"));
		using Element = typename std::conditional_t<TArgumentArray<U>::Value, TArgumentArray<U>, TArgumentSet<U>>::Element;
		if (Options.MaxOutputBytes > 0 && Json.Num() > Options.MaxOutputBytes / FMath::Max<int64>(1, sizeof(Element)))
		{ Report.bBudgetExceeded = true; return Issue(Report, true, Path, TEXTVIEW("Socket.IO argument materialization exceeds its byte budget")); }
		Value.Reset(); TObjectRoots<Element> Metadata;
		for (int32 Index = 0; Index < Json.Num(); ++Index)
		{
			Element Item{}; auto Collect = [&](FReferenceCollector& Collector) { Metadata.Collect(Item, Collector); }; FInvocationRoots Root(Collect);
			if (!Read(Json.At(Index), Item, Event, Options, Report, FString(Path) + FString::Printf(TEXT("[%d]"), Index), Depth + 1)) return false;
			Value.Add(MoveTemp(Item));
		}
		return true;
	}
	else if constexpr (TArgumentMap<U>::Value)
	{
		static_assert(std::is_same_v<typename TArgumentMap<U>::Key, FString>, "Socket.IO map handlers require FString keys");
		if (!Json.IsObject()) return Issue(Report, Options.bStrict, Path, TEXTVIEW("expected a Socket.IO object argument; kept its default"));
		using Element = typename TArgumentMap<U>::Mapped; Value.Reset(); TObjectRoots<Element> Metadata;
		for (const auto Member : Json.Members())
		{
			Element Item{}; auto Collect = [&](FReferenceCollector& Collector) { Metadata.Collect(Item, Collector); }; FInvocationRoots Root(Collect);
			if (!Read(Member.Value, Item, Event, Options, Report, FString(Path) + TEXT(".") + Member.GetKey(), Depth + 1)) return false;
			Value.Add(Member.GetKey(), MoveTemp(Item));
		}
		return true;
	}
	else
	{
		if (auto Exact = Json.TryGet<U>(); Exact.IsSet()) { Value = MoveTemp(Exact.GetValue()); return true; }
		if (!Options.bStrict)
		{
			if constexpr (std::is_arithmetic_v<U> && !std::is_same_v<U, bool>)
			{ if (Json.IsString() && NumberFromString(Json.AsString(), Value)) { Issue(Report, false, Path, TEXTVIEW("coerced Socket.IO string to number")); return true; } }
			else if constexpr (std::is_same_v<U, FString>)
			{ if (Json.IsNumber() || Json.IsBool()) { Value = Json.ToString(); Issue(Report, false, Path, TEXTVIEW("coerced Socket.IO scalar to string")); return true; } }
			else if constexpr (std::is_same_v<U, bool>)
			{
				if (Json.IsNumber()) { Value = Json.AsDouble() != 0.0; Issue(Report, false, Path, TEXTVIEW("coerced Socket.IO number to bool")); return true; }
				if (Json.IsString() && (Json.AsString().Equals(TEXT("true"), ESearchCase::IgnoreCase) || Json.AsString().Equals(TEXT("false"), ESearchCase::IgnoreCase)))
				{ Value = Json.AsString().Equals(TEXT("true"), ESearchCase::IgnoreCase); Issue(Report, false, Path, TEXTVIEW("coerced Socket.IO string to bool")); return true; }
			}
		}
		return Issue(Report, Options.bStrict, Path, TEXTVIEW("Socket.IO argument type mismatch; kept its default"));
	}
}

template <typename A, bool bStruct = VaRest::Json::bHasScriptStruct<std::remove_cvref_t<A>>
	&& !std::is_same_v<std::remove_cvref_t<A>, FVaRestJson> && !std::is_same_v<std::remove_cvref_t<A>, FVaRestSioAck>> struct TSlot
{
	using ValueType = std::remove_cvref_t<A>;
	TObjectRoots<ValueType> Metadata;
	ValueType Data{};
	ValueType& Get() { return Data; }
	void Collect(FReferenceCollector& Collector) { Metadata.Collect(Data, Collector); }
	bool Decode(const FVaRestSioEvent& Event, int32 Index, const FVaRestJsonConvertOptions& Options, FVaRestJsonDecodeReport& Report)
	{
		if constexpr (std::is_same_v<ValueType, FVaRestSioAck>) { Data = Event.Ack; return true; }
		else return Read(Event.Args.At(Index), Data, Event, Options, Report, FString::Printf(TEXT("[%d]"), Index));
	}
};
template <typename A> struct TSlot<A, true>
{
	using ValueType = std::remove_cvref_t<A>;
	TStrongObjectPtr<const UScriptStruct> Type{VaRest::Json::GetScriptStruct<ValueType>()};
	TUniquePtr<FStructOnScope> Memory;
	ValueType& Get() { return *reinterpret_cast<ValueType*>(Memory->GetStructMemory()); }
	void Collect(FReferenceCollector& Collector) { if (Memory) Collector.AddPropertyReferencesWithStructARO(Type.Get(), Memory->GetStructMemory()); }
	bool Decode(const FVaRestSioEvent& Event, int32 Index, const FVaRestJsonConvertOptions& Options, FVaRestJsonDecodeReport& Report)
	{
		if (!Type.Get() || (Options.MaxOutputBytes > 0 && Type->GetStructureSize() > Options.MaxOutputBytes))
		{ Report.bBudgetExceeded = true; return Issue(Report, true, {}, TEXTVIEW("Socket.IO native argument exceeds its materialization budget")); }
		Memory = MakeUnique<FStructOnScope>(Type.Get());
		FVaRestJsonDecodeReport Native;
		const auto Json = Event.Args.At(Index);
		if (!Json.IsValid()) return Issue(Report, Options.bStrict, FString::Printf(TEXT("[%d]"), Index), TEXTVIEW("missing Socket.IO argument; kept its native default"));
		const bool bRead = FVaRestJsonConvert::JsonToStruct(Json, Type.Get(), Memory->GetStructMemory(), Options, &Native);
		AppendReport(Report, MoveTemp(Native), FString::Printf(TEXT("[%d]"), Index)); return bRead;
	}
};
/** A const byte-array parameter borrows the immutable native attachment without copying it. */
template <> struct TSlot<const TArray<uint8>&, false>
{
	TArray<uint8> Owned;
	const TArray<uint8>* Borrowed = nullptr;
	const TArray<uint8>& Get() { return Borrowed ? *Borrowed : Owned; }
	void Collect(FReferenceCollector&) {}
	bool Decode(const FVaRestSioEvent& Event, int32 Index, const FVaRestJsonConvertOptions& Options, FVaRestJsonDecodeReport& Report)
	{
		const auto Json = Event.Args.At(Index), Placeholder = Json.Get(TEXT("_placeholder")); const auto Id = Json.Get(TEXT("num")).TryGet<int32>();
		const auto& Attachments = Event.GetNativeMessage().Attachments;
		if (Placeholder.IsBool() && Placeholder.AsBool() && Id.IsSet() && Attachments.IsValidIndex(Id.GetValue()) && Attachments[Id.GetValue()])
		{ Borrowed = Attachments[Id.GetValue()].Get(); return true; }
		return Read(Json, Owned, Event, Options, Report, FString::Printf(TEXT("[%d]"), Index));
	}
};

template <typename Arguments, typename F, size_t... I> void Invoke(FVaRestSocketWeak Weak, TFunction<bool()>& OwnerAlive,
	const FVaRestSioEvent& Event, F& Function, std::index_sequence<I...>)
{
	check(IsInGameThread()); auto Socket = Weak.Pin(); if (!Socket.IsValid() || !OwnerAlive()) return;
	const auto Pin = VaRest::WebSocket::FSocketTypedAccess::Pin(Socket, Event.Namespace);
	auto Current = [&]() { return OwnerAlive() && VaRest::WebSocket::FSocketTypedAccess::Current(Socket, Pin); };
	if (!Current()) return;
	if constexpr (sizeof...(I) == 1 && (std::is_same_v<std::remove_cvref_t<std::tuple_element_t<I, Arguments>>, FVaRestSioEvent> && ...)) Function(Event);
	else if constexpr (sizeof...(I) == 1 && (std::is_same_v<std::remove_cvref_t<std::tuple_element_t<I, Arguments>>, FVaRestSocketMessage> && ...)) Function(Event.GetNativeMessage());
	else
	{
		static_assert(((!std::is_lvalue_reference_v<std::tuple_element_t<I, Arguments>> || std::is_const_v<std::remove_reference_t<std::tuple_element_t<I, Arguments>>>
			|| std::is_same_v<std::remove_cvref_t<std::tuple_element_t<I, Arguments>>, FVaRestSioAck>) && ...), "Socket.IO handler values are by value/const reference; only a trailing FVaRestSioAck& is mutable");
		static_assert((((!std::is_same_v<std::remove_cvref_t<std::tuple_element_t<I, Arguments>>, FVaRestSioAck> || I + 1 == sizeof...(I))) && ...), "FVaRestSioAck must be the last handler argument");
		std::tuple<TSlot<std::tuple_element_t<I, Arguments>>...> Values;
		auto Collect = [&](FReferenceCollector& Collector) { (std::get<I>(Values).Collect(Collector), ...); }; FInvocationRoots Root(Collect);
		auto Options = VaRest::WebSocket::FSocketTypedAccess::Options(Socket);
		if (Options.MaxOutputBytes <= 0) Options.MaxOutputBytes = 16 * 1024 * 1024;
		Options.BytesResolver = VaRest::WebSocket::MakePlaceholderBytesReader(Event.GetNativeMessage().Attachments);
		FVaRestJsonDecodeReport Report;
		const bool bRead = ((Current() && std::get<I>(Values).Decode(Event, static_cast<int32>(I), Options, Report)) && ...);
		if (!Current()) return;
		if (!bRead) { VaRest::WebSocket::FSocketTypedAccess::Failed(Socket, Report); return; }
		if (!Report.Warnings.IsEmpty()) { UE_LOG(LogVaRestSio, Warning, TEXT("Socket.IO handler materialized arguments with %d decode warning(s)"), Report.Warnings.Num()); }
		Function(std::get<I>(Values).Get()...);
	}
}
template <typename F> TFunction<void(const FVaRestSioEvent&)> Handler(FVaRestSocketWeak Socket, TFunction<bool()> OwnerAlive, F&& Function)
{
	using Signature = TCallable<F>; using Arguments = typename Signature::Arguments;
	static_assert(Signature::Valid, "Name the Socket.IO handler parameter types; generic or overloaded callables require an explicit adapter");
	static_assert(std::tuple_size_v<Arguments> <= 32, "Socket.IO handlers support at most 32 positional parameters");
	return [Socket, OwnerAlive = MoveTemp(OwnerAlive), Function = std::decay_t<F>(Forward<F>(Function))](const FVaRestSioEvent& Event) mutable
	{ Invoke<Arguments>(Socket, OwnerAlive, Event, Function, std::make_index_sequence<std::tuple_size_v<Arguments>>()); };
}
inline TFunction<bool()> OwnerGuard(const UObject* Owner)
{ return [Weak = TWeakObjectPtr<const UObject>(Owner), bOwned = Owner != nullptr]() { return !bOwned || Weak.IsValid(); }; }
} // namespace VaRest::Sio::Typed

template <typename F> FVaRestSioNamespace FVaRestSioNamespace::On(FStringView Event, const UObject* Owner, F&& Function)
{ check(IsInGameThread()); return On(Event, Owner, VaRest::Sio::Typed::Handler(Socket.Native.AsWeak(), VaRest::Sio::Typed::OwnerGuard(Owner), Forward<F>(Function))); }
template <typename F> FVaRestSioNamespace FVaRestSioNamespace::Once(FStringView Event, const UObject* Owner, F&& Function)
{ check(IsInGameThread()); return Once(Event, Owner, VaRest::Sio::Typed::Handler(Socket.Native.AsWeak(), VaRest::Sio::Typed::OwnerGuard(Owner), Forward<F>(Function))); }
template <typename F> FVaRestSioNamespace FVaRestSioNamespace::On(FStringView Event, F&& Function) { return On(Event, nullptr, Forward<F>(Function)); }
template <typename F> FVaRestSioSocket FVaRestSioSocket::On(FStringView Event, const UObject* Owner, F&& Function)
{ DefaultNamespace().On(Event, Owner, Forward<F>(Function)); return *this; }
template <typename F> FVaRestSioSocket FVaRestSioSocket::Once(FStringView Event, const UObject* Owner, F&& Function)
{ DefaultNamespace().Once(Event, Owner, Forward<F>(Function)); return *this; }
template <typename F> FVaRestSioSocket FVaRestSioSocket::On(FStringView Event, F&& Function) { return On(Event, nullptr, Forward<F>(Function)); }

namespace VaRest::Sio::Typed
{
/** GTK materialization of the pipe's existing DOM. This does not enable a frozen worker decode plan. */
template <typename T, typename F> TFunction<void(const FVaRestSioEvent&)> PositionalHandler(FVaRestSocketWeak Weak, TFunction<bool()> OwnerAlive, F&& Function)
{
	static_assert(VaRest::Json::bHasScriptStruct<T> && !std::is_same_v<T, FVaRestJson>, "OnArgs requires a USTRUCT with converted positional fields");
	return [Weak, OwnerAlive = MoveTemp(OwnerAlive), Function = std::decay_t<F>(Forward<F>(Function))](const FVaRestSioEvent& Event) mutable
	{
		check(IsInGameThread()); auto Socket = Weak.Pin(); if (!Socket.IsValid() || !OwnerAlive()) return;
		const auto Pin = VaRest::WebSocket::FSocketTypedAccess::Pin(Socket, Event.Namespace);
		auto Current = [&]() { return OwnerAlive() && VaRest::WebSocket::FSocketTypedAccess::Current(Socket, Pin); };
		if (!Current()) return;
		auto Options = VaRest::WebSocket::FSocketTypedAccess::Options(Socket);
		if (Options.MaxOutputBytes <= 0) Options.MaxOutputBytes = 16 * 1024 * 1024;
		Options.BytesResolver = VaRest::WebSocket::MakePlaceholderBytesReader(Event.GetNativeMessage().Attachments);
		TSlot<const T&> Value;
		auto Collect = [&](FReferenceCollector& Collector) { Value.Collect(Collector); }; FInvocationRoots Root(Collect);
		FVaRestJsonDecodeReport Report;
		if (!Value.Type.Get() || Value.Type->GetStructureSize() > Options.MaxOutputBytes) return;
		Value.Memory = MakeUnique<FStructOnScope>(Value.Type.Get());
		if (!Current()) return;
		TArray<const FProperty*> Fields; FVaRestJsonError FieldsError;
		if (!VaRest::Json::TryGetConvertedFieldsInternal(Value.Type.Get(), Fields, Options, &FieldsError)) Report.Errors.Add(MoveTemp(FieldsError));
		for (int32 Index = 0; Index < Fields.Num() && Report.Errors.IsEmpty(); ++Index)
		{
			if (!Current()) return;
			const auto Json = Event.Args.At(Index);
			if (!Json.IsValid()) { Issue(Report, Options.bStrict, FString::Printf(TEXT("[%d]"), Index), TEXTVIEW("missing positional Socket.IO field; kept its native default")); continue; }
			FVaRestJsonDecodeReport FieldReport;
			FVaRestJsonConvert::JsonToProperty(Json, Fields[Index], Fields[Index]->ContainerPtrToValuePtr<void>(Value.Memory->GetStructMemory()), Options, &FieldReport);
			AppendReport(Report, MoveTemp(FieldReport), FString::Printf(TEXT("[%d]"), Index));
		}
		if (!Current()) return;
		if (!Report.Errors.IsEmpty()) { VaRest::WebSocket::FSocketTypedAccess::Failed(Socket, Report); return; }
		if (!Report.Warnings.IsEmpty()) { UE_LOG(LogVaRestSio, Warning, TEXT("Socket.IO positional handler materialized with %d decode warning(s)"), Report.Warnings.Num()); }
		if constexpr (std::is_invocable_v<F&, const T&, FVaRestSioAck&>) { auto Ack = Event.Ack; Function(Value.Get(), Ack); }
		else { static_assert(std::is_invocable_v<F&, const T&>, "OnArgs handlers take const T& and optionally a trailing FVaRestSioAck&"); Function(Value.Get()); }
	};
}
}
template <typename T, typename F> FVaRestSioNamespace FVaRestSioNamespace::OnArgs(FStringView Event, const UObject* Owner, F&& Function)
{ check(IsInGameThread()); return On(Event, Owner, VaRest::Sio::Typed::PositionalHandler<T>(Socket.Native.AsWeak(), VaRest::Sio::Typed::OwnerGuard(Owner), Forward<F>(Function))); }
template <typename T, typename F> FVaRestSioSocket FVaRestSioSocket::OnArgs(FStringView Event, const UObject* Owner, F&& Function)
{ DefaultNamespace().OnArgs<T>(Event, Owner, Forward<F>(Function)); return *this; }

template <typename F> FVaRestSioNamespace FVaRestSioNamespace::Once(FStringView Event, F&& Function) { return Once(Event, nullptr, Forward<F>(Function)); }
template <typename F> FVaRestSioSocket FVaRestSioSocket::Once(FStringView Event, F&& Function) { return Once(Event, nullptr, Forward<F>(Function)); }
template <typename T, typename F> FVaRestSioNamespace FVaRestSioNamespace::OnArgs(FStringView Event, F&& Function) { return OnArgs<T>(Event, nullptr, Forward<F>(Function)); }
template <typename T, typename F> FVaRestSioSocket FVaRestSioSocket::OnArgs(FStringView Event, F&& Function) { return OnArgs<T>(Event, nullptr, Forward<F>(Function)); }
