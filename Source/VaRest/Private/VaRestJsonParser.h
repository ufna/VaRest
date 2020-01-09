// Copyright 2015-2019 Mail.Ru Group. All Rights Reserved.

#pragma once

#include "Json.h"

struct FUtf8Helper
{
	/** @See FUTF8ToTCHAR_Convert::CodepointFromUtf8 */
	static uint32 CodepointFromUtf8(const ANSICHAR*& SourceString, const uint32 SourceLengthRemaining);
};

class FJsonValueNonConstArray : public FJsonValueArray
{
public:
	FJsonValueNonConstArray(const TArray<TSharedPtr<FJsonValue>>& InArray)
		: FJsonValueArray(InArray)
	{
	}

	/** return non const array */
	TArray<TSharedPtr<FJsonValue>>& AsNonConstArray() { return Value; }
};

class FJsonValueNonConstBoolean : public FJsonValueBoolean
{
public:
	FJsonValueNonConstBoolean(bool InBool)
		: FJsonValueBoolean(InBool)
	{
	}

	/** return non const bool */
	bool& AsNonConstBool() { return Value; }
};

class FJsonValueNonConstString : public FJsonValueString
{
public:
	FJsonValueNonConstString(const FString& InString)
		: FJsonValueString(InString)
	{
	}

	/** return non const string */
	FString& AsNonConstString() { return Value; }
};

class FJsonValueNonConstNumber : public FJsonValueNumber
{
public:
	FJsonValueNonConstNumber(double InNumber)
		: FJsonValueNumber(InNumber)
	{
	}

	/** return non const number */
	double& AsNonConstNumber() { return Value; }
};

enum class EJSONNotation
{
	NONE,
	STRING,
	STRING_SPECIAL,
	SKIP,
	NUMBER,
	ARRAY,
	OBJECT,
};

enum class EJSONToken
{
	CURLY_BEGIN,
	CURLY_END,
	SQUARE_BEGIN,
	SQUARE_END,
	COMMA,
	COLON,
	ROOT,
	ERROR,
};

struct FJSONState
{
	/** Key */
	FString Key;

	/** Data */
	FString Data;

	/** Root object */
	TSharedPtr<FJsonObject> Root;

	/** Object list */
	TArray<TSharedPtr<FJsonValue>> Objects;

	/** Tokens */
	TArray<EJSONToken> Tokens;

	/** Notation */
	EJSONNotation Notation;

	/** Current char has escape */
	bool bEscape;

	/** Has error */
	int32 bError;

	/** Las quote for string */
	TCHAR Quote;

	/** Size */
	int32 Size;

	/** Default constructor */
	FJSONState();

	EJSONToken GetToken(int32 Index = 0);

	FORCEINLINE bool CheckTokens(EJSONToken T1);

	FORCEINLINE bool CheckTokens(EJSONToken T1, EJSONToken T2);

	FORCEINLINE bool CheckTokens(EJSONToken T1, EJSONToken T2, EJSONToken T3);

	FORCEINLINE void PopToken(int32 Num);

	FORCEINLINE void PopObject();

	FORCEINLINE void PopArray();

	FORCEINLINE void PopValue(bool bCheckType = true);

	FORCEINLINE FJsonValue* GetLast();

	FORCEINLINE FJsonValueObject* GetObject();

	FORCEINLINE FJsonValueNonConstArray* GetArray();

	FORCEINLINE TSharedPtr<FJsonValueObject> PushObject();

	FORCEINLINE TSharedPtr<FJsonValueObject> PushObject(TSharedPtr<FJsonObject> Object);

	FORCEINLINE TSharedPtr<FJsonValueNonConstArray> PushArray();

	FORCEINLINE TSharedPtr<FJsonValueNonConstBoolean> PushBoolean();

	FORCEINLINE TSharedPtr<FJsonValueNull> PushNull();

	FORCEINLINE TSharedPtr<FJsonValueNonConstNumber> PushNumber();

	FORCEINLINE TSharedPtr<FJsonValueNonConstString> PushString();

	FORCEINLINE void ClearData();

	FORCEINLINE void ClearKey();

	FORCEINLINE void DataToKey();

	FORCEINLINE void Error();
};

struct FJSONReader
{
	/** State */
	FJSONState State;

	/** Default constructor */
	FJSONReader();

private:
	FORCEINLINE bool IsNewLine(const TCHAR& Char);

	FORCEINLINE bool IsSpace(const TCHAR& Char);

	FORCEINLINE bool FindToken(const TCHAR& Char);

	FORCEINLINE void UpdateNotation();

	FORCEINLINE void ReadAsString(const TCHAR& Char);

	FORCEINLINE void ReadAsStringSpecial(const TCHAR& Char);

	FORCEINLINE void ReadAsNumber(const TCHAR& Char);

	FORCEINLINE void ReadBasicValue(const TCHAR& Char);

	FORCEINLINE void ReadAsArray(const TCHAR& Char);

	FORCEINLINE void ReadAsObject(const TCHAR& Char);

	FORCEINLINE void Skip(const TCHAR& Char);

public:
	bool Read(const TCHAR Char); // @Pushkin
};

struct FJSONWriter
{
	FJSONWriter();

	FORCEINLINE bool GetStartChar(const TSharedPtr<FJsonValue>& JsonValue, FString& Char);

	FORCEINLINE bool GetEndChar(const TSharedPtr<FJsonValue>& JsonValue, FString& Char);

public:
	void Write(TSharedPtr<FJsonValue> JsonValue, FArchive* Writer, bool IsLastElement); // @Pushkin
};
